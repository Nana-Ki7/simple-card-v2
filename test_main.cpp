#include "game.h"
#include <cstdio>
#include <cassert>
using namespace game;

int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("  PASS %s\n", name); else { printf("  FAIL %s\n", name); ++fails; } } while (0)

int main() {
    printf("== 牌型识别 ==\n");
    CHECK(pattern_of({0}).type == Pattern::SINGLE, "单张");
    CHECK(pattern_of({0,13}).type == Pattern::PAIR, "对子");
    CHECK(pattern_of({0,13,26}).type == Pattern::TRIPLE, "三条");
    CHECK(pattern_of({0,13,26,39}).type == Pattern::BOMB, "炸弹");
    // 顺子 3-4-5-6-7 (club3,dia4,heart5,spade6,...)
    std::vector<u8> st = {0, 14, 28, 42, 4};  // 3♣,4♦,5♥,6♠,7♣
    CHECK(pattern_of(st).type == Pattern::STRAIGHT, "5连顺");
    // 三带二: 三张8(♣8=5,♦8=18,♥8=31) + 一对4(♣4=1,♠4=40)
    CHECK(pattern_of({5, 18, 31, 1, 40}).type == Pattern::FULL_HOUSE, "三带二");
    // 四带两单
    CHECK(pattern_of({0,13,26,39, 1, 2}).type == Pattern::FOUR_TWO_SINGLE, "四带两单");
    // 2 不入顺: 3..7 换成 J..A,2? 测试 10-J-Q-K-A
    printf("== 压牌 ==\n");
    Pattern p5 = pattern_of(st);
    CHECK(!beats(p5, p5), "同点不能互压");
    // 8-9-10-J-Q
    Pattern p6 = pattern_of({1*13+5, 2*13+6, 3*13+7, 0*13+8, 1*13+9});
    CHECK(beats(p5, p6), "顺子顶牌大者压");
    Pattern bomb = pattern_of({0,13,26,39});
    CHECK(beats(p6, bomb) && !beats(bomb, p6), "炸弹压顺子/顺子压不了炸弹");
    // 炸弹间
    Pattern bomb2 = pattern_of({1,14,27,40}); // 4 炸
    Pattern bomb3 = pattern_of({2,15,28,41}); // 5 炸
    CHECK(beats(bomb2, bomb3) && !beats(bomb3, bomb2), "炸弹比点数");
    printf("== 发牌确定性 ==\n");
    GameState a = deal(42), b = deal(42);
    bool same = true;
    for (int i=0;i<4;i++) same = same && (a.hands[i]==b.hands[i]);
    CHECK(same, "同 seed 同局");
    // 检查 52 张牌正好发完且每人 13
    int tot = 0; for (int i=0;i<4;i++) tot += (int)a.hands[i].size();
    CHECK(tot == 52 && a.hands[0].size()==13, "52张/4人/13张");
    // 首出者持 ♣3
    CHECK(a.hands[a.turn_seat][0] == 0 || std::find(a.hands[a.turn_seat].begin(), a.hands[a.turn_seat].end(), (u8)0) != a.hands[a.turn_seat].end(), "♣3 持有者先出");

    printf("== 自动模拟 100 局（贪心: 领出最小单张/跟最小能压或PASS） ==\n");
    int complete = 0, key_win = 0;
    for (unsigned seed = 1; seed <= 100; seed++) {
        GameState g = deal(seed);
        int guard = 0;
        while (g.phase == Phase::PLAYING && guard++ < 2000) {
            CardAction act;
            act.seat = g.turn_seat;
            // 找手牌最小合法动作
            bool acted = false;
            if (!g.last_play.pat.valid()) {
                // 领出: 出最小单张
                act.kind = CardAction::PLAY;
                act.cards = {g.hands[g.turn_seat][0]};
            } else {
                auto moves = legal_moves(g.hands[g.turn_seat], &g.last_play.pat);
                if (!moves.empty()) {
                    act.kind = CardAction::PLAY;
                    act.cards = moves[0];  // legal_moves 已过滤能压的
                } else {
                    act.kind = CardAction::PASS;
                }
            }
            auto r = apply_action(g, act);
            if (!r.ok()) { printf("  异常: seed=%u seat=%d code=%d\n", seed, act.seat, (int)r.code); break; }
            g = r.next;
            for (auto& e : r.events) if (e.kind == Event::GAME_OVER) {
                complete++;
                if (e.winner_team == kTeamKey) key_win++;
            }
        }
        if (g.phase != Phase::FINISHED) printf("  未终局: seed=%u moves=%d\n", seed, guard);
    }
    printf("  完成 %d/100 局, 关键队胜 %d 局\n", complete, key_win);

    printf("\n%s  (%d 失败)\n", fails==0 ? "ALL TESTS PASSED" : "SOME FAILED", fails);
    return fails ? 1 : 0;
}
