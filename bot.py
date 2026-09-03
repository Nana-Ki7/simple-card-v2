#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bot.py —— 扑克牌"人机玩家"（单文件 / asyncio + websockets 15，GLM 产出）

用法：
  python3 bot.py --host 127.0.0.1 --name AI1 --mode create    # 建房
  python3 bot.py --name AI1 --mode join --room 1              # 加入指定房间
  python3 bot.py --names AI1,AI2,AI3,AI4 --mode autofill      # 4 AI 自动打一局
  python3 bot.py --selftest                                   # 本地自检牌型逻辑
可选：--port 9002 --games N（N 局后退出，默认1，0=不限）--timeout 秒
协议：ws://HOST:9002 JSON；牌 card_id 0..51，suit=id//13(0♣1♦2♥3♠)，rank=id%13+3
规则：♥2(38)/♠K(49) 关键队（双持 1v3），同队全员出完获胜
"""

import argparse
import asyncio
import json
import random
import time
from collections import Counter

import websockets

SUIT_STR = "♣♦♥♠"
RANK_STR = {11: "J", 12: "Q", 13: "K", 14: "A", 15: "2"}


def rank_of(cid):
    return cid % 13 + 3


def suit_of(cid):
    return cid // 13


def rank_str(r):
    return RANK_STR.get(r, str(r))


def card_str(cid):
    return SUIT_STR[suit_of(cid)] + rank_str(rank_of(cid))


def cards_str(cards):
    return " ".join(card_str(c) for c in sorted(cards, key=rank_of)) if cards else "(空)"


def straight_pattern(ranks):
    if len(ranks) < 5 or len(set(ranks)) != len(ranks):
        return None
    if ranks[-1] > 14 or ranks[-1] - ranks[0] != len(ranks) - 1:
        return None
    return ("straight", ranks[0], len(ranks))


def pattern_of(cards):
    n = len(cards)
    if n == 0:
        return None
    ranks = sorted(rank_of(c) for c in cards)
    cnt = Counter(ranks)
    if n == 1:
        return ("single", ranks[0], 1)
    if n == 2 and len(cnt) == 1:
        return ("pair", ranks[0], 1)
    if n == 3 and len(cnt) == 1:
        return ("triple", ranks[0], 1)
    if n == 4 and len(cnt) == 1:
        return ("bomb", ranks[0], 1)
    if n == 5 and sorted(cnt.values()) == [2, 3]:
        tri = next(r for r, c in cnt.items() if c == 3)
        return ("triple_pair", tri, 1)
    if n == 6 and sorted(cnt.values()) == [1, 1, 4]:
        four = next(r for r, c in cnt.items() if c == 4)
        return ("four_two_single", four, 1)
    if n == 8 and sorted(cnt.values()) == [2, 2, 4]:
        four = next(r for r, c in cnt.items() if c == 4)
        return ("four_two_pair", four, 1)
    return straight_pattern(ranks)


def beats(a, b):
    if a is None or b is None:
        return False
    atype, arank, alen = a
    btype, brank, blen = b
    if atype == "bomb":
        return btype != "bomb" or arank > brank
    if atype != btype:
        return False
    if atype == "straight":
        return alen == blen and arank > brank
    return arank > brank


def enumerate_beats(hand, tp):
    if not tp:
        return [], []
    ttype, trank, tlen = tp
    by_rank = {}
    for c in hand:
        by_rank.setdefault(rank_of(c), []).append(c)
    for cs in by_rank.values():
        cs.sort()
    order = sorted(by_rank)
    normals, bombs = [], []

    if ttype != "bomb":
        if ttype == "single":
            normals = [[by_rank[r][0]] for r in order if r > trank]
        elif ttype == "pair":
            normals = [by_rank[r][:2] for r in order if r > trank and len(by_rank[r]) >= 2]
        elif ttype == "triple":
            normals = [by_rank[r][:3] for r in order if r > trank and len(by_rank[r]) >= 3]
        elif ttype == "triple_pair":
            for r in order:
                if r > trank and len(by_rank[r]) >= 3:
                    kick = next((k for k in order if k != r and 2 <= len(by_rank[k]) < 4), None)
                    if kick is not None:
                        normals.append(by_rank[r][:3] + by_rank[kick][:2])
        elif ttype == "straight":
            for start in range(trank + 1, 16 - tlen):
                if all(start + i in by_rank for i in range(tlen)):
                    normals.append([by_rank[start + i][0] for i in range(tlen)])
        elif ttype == "four_two_single":
            for r in order:
                if r > trank and len(by_rank[r]) == 4:
                    kick = [k for k in order if k != r and len(by_rank[k]) < 4][:2]
                    if len(kick) == 2:
                        normals.append(by_rank[r][:4] + [by_rank[k][0] for k in kick])
        elif ttype == "four_two_pair":
            for r in order:
                if r > trank and len(by_rank[r]) == 4:
                    kick = [k for k in order if k != r and 2 <= len(by_rank[k]) < 4][:2]
                    if len(kick) == 2:
                        normals.append(by_rank[r][:4] + by_rank[kick[0]][:2] + by_rank[kick[1]][:2])

    for r in order:
        if len(by_rank[r]) == 4 and (ttype != "bomb" or r > trank):
            bombs.append(by_rank[r][:4])
    return normals, bombs


def normalize_hand_counts(hc):
    try:
        if isinstance(hc, dict):
            return {int(k): int(v) for k, v in hc.items()}
        if isinstance(hc, list):
            return {i: int(v) for i, v in enumerate(hc)}
    except (TypeError, ValueError):
        pass
    return {}


def selftest():
    assert pattern_of([0]) == ("single", 3, 1)
    assert pattern_of([0, 13]) == ("pair", 3, 1)
    assert pattern_of([0, 13, 26]) == ("triple", 3, 1)
    assert pattern_of([0, 13, 26, 39]) == ("bomb", 3, 1)
    assert pattern_of([0, 13, 26, 1, 14]) == ("triple_pair", 3, 1)
    assert pattern_of([0, 13, 26, 39, 1]) is None
    assert pattern_of([0, 1, 2, 3, 4]) == ("straight", 3, 5)
    assert pattern_of([7, 8, 9, 10, 11]) == ("straight", 10, 5)
    assert pattern_of([7, 8, 9, 10, 11, 12]) is None
    assert pattern_of([0, 13, 26, 39, 2, 3]) == ("four_two_single", 3, 1)
    assert pattern_of([0, 13, 26, 39, 2, 15]) is None
    assert pattern_of([0, 13, 26, 39, 1, 14, 2, 15]) == ("four_two_pair", 3, 1)
    assert beats(("pair", 5, 1), ("pair", 4, 1))
    assert not beats(("single", 15, 1), ("bomb", 3, 1))
    assert beats(("bomb", 3, 1), ("straight", 3, 5))
    assert beats(("bomb", 5, 1), ("bomb", 4, 1))
    assert not beats(("bomb", 4, 1), ("bomb", 5, 1))
    assert beats(("straight", 5, 5), ("straight", 4, 5))
    assert not beats(("straight", 4, 6), ("straight", 3, 5))
    assert not beats(("triple", 5, 1), ("triple_pair", 3, 1))
    normals, bombs = enumerate_beats([1, 14, 2, 15], ("pair", 3, 1))
    assert normals == [[1, 14], [2, 15]] and bombs == []
    normals, bombs = enumerate_beats([0, 13, 26, 39, 2], ("pair", 10, 1))
    assert normals == [] and bombs == [[0, 13, 26, 39]]
    normals, bombs = enumerate_beats([1, 2, 3, 4, 5], ("straight", 3, 5))
    assert normals == [[1, 2, 3, 4, 5]]
    normals, _ = enumerate_beats([1, 14, 27, 2, 15], ("triple_pair", 3, 1))
    assert normals == [[1, 14, 27, 2, 15]]
    normals, bombs = enumerate_beats([0, 13, 26, 39, 8, 21, 5, 18], ("four_two_single", 3, 1))
    assert normals == [] and bombs == [[0, 13, 26, 39]]
    print("selftest OK：牌型识别 / 大小比较 / 压牌枚举全部通过")


class Bot:
    def __init__(self, name, host, port, mode, room_id, games_target, shared=None, creator=False):
        self.name = name
        self.host, self.port = host, port
        self.mode, self.room_id = mode, room_id
        self.games_target = games_target
        self.shared, self.creator = shared, creator
        self.token = None
        self.seat = None
        self.turn_seat = None
        self.hand = []
        self.phase = "idle"
        self.table = None
        self.round_seq = None
        self.last_can_pass = True
        self.last_action = None
        self.err_retries = 0
        self.join_retries = 0
        self.fail_count = 0
        self.hand_counts = {}
        self.key_cards = {}
        self.finish_order = []
        self.games_done = 0
        self.stop_flag = False
        self.start_delay = 0.0

    def log(self, text):
        seat = self.seat if self.seat is not None else "-"
        print(f"[{self.name}|seat{seat}] {text}", flush=True)

    def should_stop(self):
        if self.stop_flag:
            return True
        if self.shared and self.shared["stop"].is_set():
            return True
        return self.games_target > 0 and self.games_done >= self.games_target

    async def send(self, ws, obj):
        await ws.send(json.dumps(obj, ensure_ascii=False))

    def by_rank(self):
        d = {}
        for c in self.hand:
            d.setdefault(rank_of(c), []).append(c)
        for cs in d.values():
            cs.sort()
        return d

    def hc_str(self):
        return " ".join(f"{s}{'*' if s == self.seat else ''}:{self.hand_counts.get(s, '?')}"
                        for s in range(4))

    def mates(self):
        if len(self.key_cards) < 2 or self.seat is None:
            return set()
        holders = set(self.key_cards.values())
        return holders if self.seat in holders else set(range(4)) - holders

    def find_straight(self, by_rank, length, avoid):
        for start in range(3, 16 - length):
            if any((start + i) in avoid for i in range(length)):
                continue
            if all((start + i) in by_rank for i in range(length)):
                return [by_rank[start + i][0] for i in range(length)]
        return None

    def choose_lead(self):
        hand = self.hand
        if not hand:
            return None
        if pattern_of(hand):
            return list(hand)
        by_rank = self.by_rank()
        bomb_ranks = {r for r, cs in by_rank.items() if len(cs) == 4}
        st = self.find_straight(by_rank, 5, bomb_ranks)
        if st:
            return st
        exact_pair = next((r for r in sorted(by_rank)
                           if r not in bomb_ranks and len(by_rank[r]) == 2), None)
        if exact_pair is not None:
            return by_rank[exact_pair][:2]
        single_rank = next((r for r in sorted(by_rank) if r not in bomb_ranks), None)
        if single_rank is not None:
            return [by_rank[single_rank][0]]
        for r in sorted(by_rank):
            if len(by_rank[r]) == 4:
                return by_rank[r][:4]
        return [min(hand, key=rank_of)]

    def choose_follow(self):
        if not self.table:
            return None
        tp = pattern_of(self.table)
        if tp is None:
            self.log(f"警告：桌面牌 {cards_str(self.table)} 无法识别，按 PASS 处理")
            return None
        normals, bombs = enumerate_beats(self.hand, tp)
        normals = [mv for mv in normals if beats(pattern_of(mv), tp)]
        n = len(self.hand)
        for mv in normals:
            if len(mv) == n:
                return mv
        for mv in bombs:
            if len(mv) == n:
                return mv
        if normals:
            return min(normals, key=lambda mv: (len(mv), pattern_of(mv)[1]))
        if bombs and (n <= 5 or random.random() < 0.5):
            return bombs[0]
        return None

    async def do_play(self, ws, cards):
        cards = list(cards)
        if any(c not in self.hand for c in cards):
            self.log(f"内部错误：打出手牌之外的牌 {cards_str(cards)}，改 PASS")
            self.last_action = "pass"
            await self.send(ws, {"type": "pass"})
            return
        self.last_action = "play"
        await self.send(ws, {"type": "play", "cards": cards})

    async def on_my_turn(self, ws, can_pass):
        if not self.hand:
            return
        if can_pass and self.table is None:
            self.log("轮到我但桌面牌缺失，保守 PASS")
            self.last_action = "pass"
            await self.send(ws, {"type": "pass"})
            return
        await asyncio.sleep(random.uniform(0.05, 0.30))
        if not can_pass:
            move = self.choose_lead()
            if move is None:
                move = [min(self.hand, key=rank_of)]
            self.log(f"领出：{cards_str(move)}　｜各家 {self.hc_str()}")
            await self.do_play(ws, move)
        else:
            move = self.choose_follow()
            if move is None:
                self.log(f"压不过 {cards_str(self.table)}，PASS")
                self.last_action = "pass"
                await self.send(ws, {"type": "pass"})
            else:
                self.log(f"跟牌：{cards_str(move)} 压 {cards_str(self.table)}")
                await self.do_play(ws, move)

    async def run(self, deadline=None):
        if self.start_delay:
            await asyncio.sleep(self.start_delay)  # 错峰启动，避免并发握手竞态
        uri = f"ws://{self.host}:{self.port}"
        backoff = 1.0
        while not self.should_stop():
            if deadline is not None and time.monotonic() >= deadline:
                self.log("总超时，退出")
                return
            try:
                left = None if deadline is None else max(1.0, deadline - time.monotonic())
                async with websockets.connect(uri, open_timeout=10,
                                              ping_interval=20, ping_timeout=20) as ws:
                    if left is not None:
                        await asyncio.wait_for(self.session(ws), timeout=left)
                    else:
                        await self.session(ws)
            except (asyncio.TimeoutError, TimeoutError):
                if deadline is not None and time.monotonic() >= deadline:
                    self.log("总超时，退出")
                    return
                self.log("连接超时，重试…")
            except Exception as e:
                if self.should_stop():
                    return
                self.fail_count += 1
                if self.fail_count >= 8:
                    self.log(f"连续 {self.fail_count} 次失败（{type(e).__name__}: {e}），放弃")
                    return
                self.log(f"连接异常（{type(e).__name__}: {e}），{backoff:.0f}s 后重连")
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 8)
            else:
                backoff, self.fail_count = 1.0, 0
        self.log("会话结束，退出")

    async def session(self, ws):
        self.log(f"已连接 ws://{self.host}:{self.port}")
        hello = {"type": "hello", "name": self.name}
        if self.token:
            hello["token"] = self.token
        await ws.send(json.dumps(hello))
        await self.await_welcome(ws)
        if self.room_id is not None:
            await self.send(ws, {"type": "join_room", "room_id": self.room_id})
            self.log(f"加入房间 {self.room_id}")
        elif self.mode == "create" or (self.mode == "autofill" and self.creator):
            await self.send(ws, {"type": "create_room"})
            self.log("已请求建房，等待 room_state…")
        else:
            await self.wait_shared_room()
            self.room_id = self.shared["room_id"]
            await self.send(ws, {"type": "join_room", "room_id": self.room_id})
            self.log(f"加入房间 {self.room_id}")
        await self.recv_loop(ws)

    async def await_welcome(self, ws):
        while True:
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            msg = json.loads(raw)
            t = msg.get("type")
            if t == "welcome":
                self.token = msg.get("token") or self.token
                print(f"[{self.name}] welcome：token={self.token}", flush=True)
                return
            if t == "error":
                raise RuntimeError(f"hello 被拒：[{msg.get('code')}] {msg.get('message')}")

    async def wait_shared_room(self):
        try:
            await asyncio.wait_for(self.shared["room_evt"].wait(), timeout=60)
        except asyncio.TimeoutError:
            raise RuntimeError("等待房主建房超时（60s）")

    async def recv_loop(self, ws):
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except (ValueError, UnicodeDecodeError):
                self.log(f"非 JSON 消息：{raw!r}")
                continue
            try:
                await self.dispatch(ws, msg)
            except websockets.ConnectionClosed:
                raise
            except Exception as e:
                self.log(f"处理 {msg.get('type')} 出错：{type(e).__name__}: {e}")
            if self.should_stop():
                return

    async def dispatch(self, ws, msg):
        t = msg.get("type")
        if t == "room_state":
            self.room_id = msg.get("room_id")
            self.seat = msg.get("seat")
            self.phase = msg.get("phase", self.phase)
            self.join_retries = 0
            self.log(f"房间 {self.room_id}｜座位 {self.seat}｜阶段 {self.phase}")
            if self.shared and self.creator and self.shared["room_id"] is None:
                self.shared["room_id"] = self.room_id
                self.shared["room_evt"].set()
        elif t == "game_started":
            self.phase = "playing"
            self.table = None
            self.round_seq = None
            self.key_cards = {}
            self.finish_order = []
            self.last_action = None
            self.turn_seat = msg.get("turn_seat")
            self.log(f"开局！seat{self.turn_seat} 先出，限时 {msg.get('timeout_ms')}ms")
        elif t == "your_hand":
            self.seat = msg.get("seat", self.seat)
            self.hand = sorted(msg.get("hand", []))
            self.turn_seat = msg.get("turn_seat")
            self.phase = "playing"
            tag = "重连同步" if msg.get("reconnect") else "发牌"
            self.log(f"{tag}：seat={self.seat}，共 {len(self.hand)} 张")
            self.log(f"  手牌：{cards_str(self.hand)}")
        elif t == "turn_update":
            seq = msg.get("round_seq")
            if seq is not None and seq != self.round_seq:
                self.round_seq = seq
                self.table = None
            hc = msg.get("hand_counts")
            if hc:
                self.hand_counts = normalize_hand_counts(hc)
            if msg.get("seat") == self.seat:
                self.err_retries = 0
                self.last_can_pass = bool(msg.get("can_pass", True))
                await self.on_my_turn(ws, self.last_can_pass)
        elif t == "play_made":
            seat, cards = msg.get("seat"), list(msg.get("cards", []))
            seq = msg.get("round_seq")
            if seq is not None and seq != self.round_seq:
                self.round_seq = seq
                self.table = None
            if seat == self.seat:
                gone = set(cards)
                self.hand = [c for c in self.hand if c not in gone]
                self.log(f"我出 {cards_str(cards)}，剩 {len(self.hand)} 张")
            else:
                self.log(f"seat{seat} 出 {cards_str(cards)}　｜各家 {self.hc_str()}")
            self.table = cards
            self.last_action = None
            hc = msg.get("hand_counts")
            if hc:
                self.hand_counts = normalize_hand_counts(hc)
        elif t == "pass_made":
            seq = msg.get("round_seq")
            if seq is not None and seq != self.round_seq:
                self.round_seq = seq
                self.table = None
            if msg.get("seat") == self.seat:
                self.last_action = None
            self.log(f"seat{msg.get('seat')} PASS")
        elif t == "round_end":
            if msg.get("round_seq") is not None:
                self.round_seq = msg["round_seq"]
            self.table = None
            self.log(f"—— 新一轮，seat{msg.get('leader')} 领出 ——")
        elif t == "reveal":
            kind, seat = msg.get("kind"), msg.get("seat")
            self.key_cards[kind] = seat
            note = ""
            if len(self.key_cards) >= 2:
                note = "（我方关键队）" if seat in self.mates() else "（对手）"
            self.log(f"reveal：{kind} 在 seat{seat}{note}")
        elif t == "hand_empty":
            seat = msg.get("seat")
            if seat not in self.finish_order:
                self.finish_order.append(seat)
            self.hand_counts[seat] = 0
            if seat == self.seat:
                self.log("★ 我出完了，进入看戏模式")
            else:
                self.log(f"seat{seat} 出完（第 {len(self.finish_order)} 位）")
        elif t == "game_over":
            self.on_game_over(msg)
        elif t == "player_conn":
            self.log(f"seat{msg.get('seat')} {'上线' if msg.get('connected') else '掉线'}")
        elif t == "error":
            await self.on_error(ws, msg)

    def on_game_over(self, msg):
        self.games_done += 1
        self.log(f"🏁 本局结束：winner_team={msg.get('winner_team')} "
                 f"finish_order={msg.get('finish_order')} teams={msg.get('teams')}")
        self.hand, self.table, self.round_seq = [], None, None
        self.key_cards, self.finish_order = {}, []
        self.last_action = None
        self.phase = "waiting"
        if self.shared:
            self.shared["stop"].set()
        if self.games_target > 0 and self.games_done >= self.games_target:
            self.stop_flag = True
            self.log(f"已完成 {self.games_done} 局，退出")
        else:
            self.log("等待下一局…")

    async def on_error(self, ws, msg):
        code, text = str(msg.get("code", "")), str(msg.get("message", ""))
        self.log(f"服务器错误 [{code}] {text}")
        if self.last_action == "play" and self.err_retries < 2:
            self.err_retries += 1
            self.last_action = None
            if self.last_can_pass:
                self.log("出牌被拒 → 改为 PASS")
                self.last_action = "pass"
                await self.send(ws, {"type": "pass"})
            elif self.hand:
                mv = [min(self.hand, key=rank_of)]
                self.log(f"出牌被拒 → 补打最小单张 {cards_str(mv)}")
                await self.do_play(ws, mv)
            return
        self.last_action = None
        if self.seat is None and "room" in (code + text).lower():
            if self.mode == "join" or (self.mode == "autofill" and not self.creator):
                if self.join_retries < 8:
                    self.join_retries += 1
                    asyncio.create_task(self.delayed_join(ws))
                else:
                    self.log("多次加入房间失败，退出")
                    self.stop_flag = True
            elif self.room_id is not None:
                self.room_id = None
                self.log("旧房间已失效，重新建房")
                await self.send(ws, {"type": "create_room"})

    async def delayed_join(self, ws):
        await asyncio.sleep(2.0)
        try:
            if self.seat is not None:
                return
            self.log(f"第 {self.join_retries} 次重试加入房间 {self.room_id}")
            await self.send(ws, {"type": "join_room", "room_id": self.room_id})
        except Exception:
            pass


def parse_args():
    p = argparse.ArgumentParser(description="扑克牌人机玩家 bot.py")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9002)
    p.add_argument("--name", default=None)
    p.add_argument("--names", default=None)
    p.add_argument("--mode", default="join", choices=["create", "join", "autofill"])
    p.add_argument("--room", type=int, default=None)
    p.add_argument("--games", type=int, default=1)
    p.add_argument("--timeout", type=int, default=0)
    p.add_argument("--selftest", action="store_true")
    return p.parse_args()


async def main():
    args = parse_args()
    if args.selftest:
        selftest()
        return
    timeout = args.timeout if args.timeout > 0 else (300 if args.mode == "autofill" else 0)
    deadline = time.monotonic() + timeout if timeout else None
    if args.mode == "autofill":
        names = [s.strip() for s in (args.names or "AI1,AI2,AI3,AI4").split(",") if s.strip()]
        if len(names) != 4:
            raise SystemExit("autofill 需要 4 个昵称：--names AI1,AI2,AI3,AI4")
        shared = {"room_id": None, "room_evt": asyncio.Event(), "stop": asyncio.Event()}
        bots = [Bot(n, args.host, args.port, args.mode, None, 1,
                    shared=shared, creator=(i == 0)) for i, n in enumerate(names)]
        for i, b in enumerate(bots):
            b.start_delay = i * 0.5  # AI1 立即，AI2 0.5s，AI3 1.0s，AI4 1.5s 后连
        print(f"== autofill：{', '.join(names)} → ws://{args.host}:{args.port} ==")
        results = await asyncio.gather(*(b.run(deadline) for b in bots), return_exceptions=True)
        for b, r in zip(bots, results):
            if isinstance(r, Exception) and not isinstance(r, asyncio.CancelledError):
                print(f"[{b.name}] 异常退出: {r!r}")
        print("== autofill 全部结束 ==")
    else:
        if not args.name:
            raise SystemExit("create/join 模式需要 --name，如 --name AI1")
        if args.mode == "join" and args.room is None:
            raise SystemExit("join 模式需要 --room，如 --room 1")
        bot = Bot(args.name, args.host, args.port, args.mode, args.room, args.games)
        await bot.run(deadline)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nCtrl-C，退出")
