import asyncio, json, websockets
URI = "ws://127.0.0.1:9002"
hand, me = [], 0

async def main():
    global hand, me
    async with websockets.connect(URI) as ws:
        await ws.send(json.dumps({"type":"hello","name":"真人"}))
        await ws.recv()  # welcome
        await ws.send(json.dumps({"type":"create_room","bots":3}))
        await ws.recv()  # room_state
        moves = 0
        while moves < 500:
            m = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
            tt = m.get("type")
            if tt == "your_hand":
                hand = m["hand"]; me = m.get("seat", 0)
            elif tt == "turn_update" and m.get("seat") == me:
                # 真人自动行动：领出走最小单张，跟牌 PASS（简化，便于验证 bot 流程）
                if m.get("can_pass", True) is False and hand:
                    c = min(hand); hand.remove(c)
                    await ws.send(json.dumps({"type":"play","cards":[c]}))
                else:
                    await ws.send(json.dumps({"type":"pass"}))
            elif tt == "play_made":
                moves += 1
                who = "真人" if m["seat"]==me else f"Bot{m['seat']}"
                print(f"[{moves}] {who} 出 {m['cards']} 手牌数 {m.get('hand_counts')}", flush=True)
            elif tt == "game_over":
                print("🏁 结束 winner_team=%s teams=%s order=%s" % (m.get('winner_team'), m.get('teams'), m.get('finish_order')), flush=True)
                print("内置BOT测试 PASS ✅", flush=True)
                return
            elif tt == "error":
                print("ERR:", m, flush=True)
        print("超时未结束", flush=True)

asyncio.run(main())
