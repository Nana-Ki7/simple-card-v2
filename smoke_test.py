import asyncio, json, websockets, sys

URI = "ws://127.0.0.1:9002"
room_id = None

async def bot(name, is_host):
    global room_id
    hand = []
    me = -1
    done = asyncio.Event()
    result = {}
    async with websockets.connect(URI) as ws:
        # hello
        await ws.send(json.dumps({"type":"hello","name":name}))
        welcome = json.loads(await ws.recv())
        assert welcome["type"]=="welcome", welcome
        if is_host:
            await ws.send(json.dumps({"type":"create_room"}))
        else:
            while room_id is None:
                await asyncio.sleep(0.05)
            await ws.send(json.dumps({"type":"join_room","room_id":room_id}))
        moves = 0
        while not done.is_set() and moves < 1000:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=45))
            t = msg["type"]
            if t=="room_state":
                me = msg["seat"]
                if is_host: room_id = msg["room_id"]
            elif t=="your_hand":
                hand = list(msg["hand"]); me = msg.get("seat", me)
            elif t=="turn_update" and msg.get("seat")==me:
                moves += 1
                if not hand:
                    print(f"[{name}] 警告: 手牌空却被轮到 seat={me}, 等待 game_over")
                    continue
                if msg.get("can_pass", True) is False:
                    # 领出：出最小单张
                    c = min(hand)
                    hand.remove(c)
                    await ws.send(json.dumps({"type":"play","cards":[c]}))
                else:
                    await ws.send(json.dumps({"type":"pass"}))
            elif t=="game_over":
                result = msg; done.set()
            elif t=="error":
                print(f"[{name}] error:", msg)
    return result

async def main():
    results = await asyncio.gather(bot("A", True), bot("B", False), bot("C", False), bot("D", False))
    over = [r for r in results if r]
    if over:
        r = over[0]
        print("=== 对局结束 ===")
        print("胜队:", "关键队(♥2+♠K)" if r["winner_team"]==0 else "平民队")
        print("出完顺序:", r.get("finish_order"))
        print("队伍归属(seat):", r.get("teams"))
        print("SMOKE TEST PASS ✅")
    else:
        print("没有收到 game_over，测试失败 ❌")

asyncio.run(asyncio.wait_for(main(), timeout=180))
