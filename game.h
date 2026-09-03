#ifndef GAME_H_
#define GAME_H_

// ============================================================================
// game.h — 扑克核心逻辑引擎（v2 重构 Phase 1）
//
// C++17 / header-only / 单文件 / 纯逻辑 / 零外部依赖 / 零 I/O
//
// 规则裁定编号对照（正文以 [R#] 引用）:
//   [R1]  52 张无王, 4 人局每人 13 张
//   [R2]  点数序 3<4<...<K<A<2; 花色不比大小, 仅用于确定性裁决 (♣<♦<♥<♠)
//   [R3]  card_id = suit*13 + (rank-3); suit 0=♣ 1=♦ 2=♥ 3=♠; rank 3..15
//   [R4]  ♥2/♠K 持有者为关键队; 同一人双持 → 1v3, 不重发
//   [R5]  同队全员出完 → 该队胜（1 人队自己出完即胜）
//   [R6]  8 种牌型; 同点不互压; 三条≠三带二; 四带二单≠四带两对; 异长顺子不互压
//   [R7]  炸弹压一切非炸弹, 炸弹间比点数; 可炸队友, 可领出
//   [R8]  首出者 = ♣3 持有者, 首手不限牌型
//   [R9]  领出者不可 PASS; PASS 后本轮锁定, 直到新一轮领出清空
//   [R10] pass_count >= (未出完人数-1) 结轮; 最后出牌者领出, 已出完则顺延
// ============================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace game {

using u8 = std::uint8_t;

// ---------------------------------------------------------------------------
// 顶部集中常量（规则数值全部硬编码于此, 可改）
// ---------------------------------------------------------------------------
constexpr int kNumSeats     = 4;    // [R1]
constexpr int kCardsPerHand = 13;   // [R1]
constexpr int kTotalCards   = 52;   // [R1]
constexpr int kRanksPerSuit = 13;   // [R3]
constexpr int kRankOffset   = 3;    // [R3] id = suit*13 + (rank - 3)
constexpr int kMinRank      = 3;    // [R2]
constexpr int kMaxRank      = 15;   // [R2] 2 最大
constexpr int kAceRank      = 14;   // [R6] A 仅作顺子高端

constexpr int kSuitClub    = 0;     // [R3] ♣<♦<♥<♠ 仅用于确定性裁决, 不比大小 [R2]
constexpr int kSuitDiamond = 1;
constexpr int kSuitHeart   = 2;
constexpr int kSuitSpade   = 3;

constexpr u8 kClubThree = 0;        // [R3][R8] ♣3 = 0*13+(3-3)
constexpr u8 kHeartTwo  = 38;       // [R3][R4] ♥2 = 2*13+(15-3)
constexpr u8 kSpadeKing = 49;       // [R3][R4] ♠K = 3*13+(13-3)

constexpr int kTeamKey   = 0;       // [R4] 关键队
constexpr int kTeamCrowd = 1;       // [R4] 平民队

constexpr int kStraightMinLen   = 5;   // [R6]
constexpr int kStraightMaxLen   = 12;  // [R6] 3..A 共 12 张
constexpr int kFullHouseLen     = 5;   // [R6]
constexpr int kFourTwoSingleLen = 6;   // [R6]
constexpr int kFourTwoPairLen   = 8;   // [R6]

// 解释性裁定: 四带两对的"两对"可否同点（即 4+4 带出另一四条）。
// 类比 [R6] "两单可同点"默认允许; 项目方如需禁止改 false 即可。
constexpr bool kFourTwoPairAllowSamePair = true;

// ---------------------------------------------------------------------------
// card_id 工具 [R3]
// ---------------------------------------------------------------------------
inline int  rank_of(u8 id) { return static_cast<int>(id) % kRanksPerSuit + kRankOffset; }
inline int  suit_of(u8 id) { return static_cast<int>(id) / kRanksPerSuit; }
inline u8   make_id(int suit, int rank) { return static_cast<u8>(suit * kRanksPerSuit + (rank - kRankOffset)); }
inline bool is_valid_id(u8 id) { return static_cast<int>(id) < kTotalCards; }

// ---------------------------------------------------------------------------
// 牌型 [R6]
// ---------------------------------------------------------------------------
struct Pattern {
  enum Type {
    INVALID = 0,
    SINGLE, PAIR, TRIPLE, FULL_HOUSE, STRAIGHT, BOMB,
    FOUR_TWO_SINGLE, FOUR_TWO_PAIR
  };
  Type type      = INVALID;
  int  main_rank = 0;  // 比大点数: 单/对/三/炸=该点; 三带二=三条点; 顺子=顶牌点; 四带*=四条点
  int  length    = 0;  // 张数

  bool valid()   const { return type != INVALID; }
  bool is_bomb() const { return type == BOMB; }
};

inline bool operator==(const Pattern& a, const Pattern& b) {
  return a.type == b.type && a.main_rank == b.main_rank && a.length == b.length;
}

namespace detail {

// [R6] 顺子: 长度 5..12, 仅 rank 3..A（滑动窗口找任意起点）, 顶牌为 main_rank
inline bool try_straight(const int* cnt, int len, Pattern& out) {
  if (len < kStraightMinLen || len > kStraightMaxLen) return false;
  for (int s = kMinRank; s + len - 1 <= kAceRank; ++s) {
    bool ok = true;
    for (int r = s; r < s + len; ++r) {
      if (cnt[r] != 1) { ok = false; break; }
    }
    if (ok) {
      out.type = Pattern::STRAIGHT;
      out.main_rank = s + len - 1;  // 顶牌比大 [R6]
      return true;
    }
  }
  return false;
}

}  // namespace detail

// [R6] 判定牌型; 非法组合（含空/非法 id/重复牌）返回 type==INVALID
inline Pattern pattern_of(const std::vector<u8>& cards) {
  Pattern p;
  p.length = static_cast<int>(cards.size());
  if (p.length <= 0 || p.length > kStraightMaxLen) return p;

  bool seen[kTotalCards] = {false};
  int cnt[16] = {0};
  for (u8 c : cards) {
    if (!is_valid_id(c) || seen[c]) return p;
    seen[c] = true;
    ++cnt[rank_of(c)];
  }
  int distinct = 0;
  for (int r = kMinRank; r <= kMaxRank; ++r)
    if (cnt[r]) ++distinct;

  switch (p.length) {
    case 1:
      p.type = Pattern::SINGLE;
      p.main_rank = rank_of(cards[0]);
      return p;
    case 2:
      if (distinct == 1) { p.type = Pattern::PAIR; p.main_rank = rank_of(cards[0]); }
      return p;
    case 3:
      if (distinct == 1) { p.type = Pattern::TRIPLE; p.main_rank = rank_of(cards[0]); }
      return p;
    case 4:
      if (distinct == 1) { p.type = Pattern::BOMB; p.main_rank = rank_of(cards[0]); }
      return p;
    case kFullHouseLen: {  // 三带二 或 5 连顺 [R6]
      if (distinct == 2) {
        int tri = -1, pr = -1;
        for (int r = kMinRank; r <= kMaxRank; ++r) {
          if (cnt[r] == 3) tri = r;
          else if (cnt[r] == 2) pr = r;
        }
        if (tri >= 0 && pr >= 0) { p.type = Pattern::FULL_HOUSE; p.main_rank = tri; return p; }
      }
      detail::try_straight(cnt, p.length, p);
      return p;
    }
    case kFourTwoSingleLen: {  // 四带两单（两单可同点）或 6 连顺 [R6]
      for (int r = kMinRank; r <= kMaxRank; ++r)
        if (cnt[r] == 4) { p.type = Pattern::FOUR_TWO_SINGLE; p.main_rank = r; return p; }
      detail::try_straight(cnt, p.length, p);
      return p;
    }
    case kFourTwoPairLen: {  // 四带两对 或 8 连顺 [R6]
      int quad = -1, quad2 = -1;
      bool ok = true;
      for (int r = kMinRank; r <= kMaxRank; ++r) {
        if (cnt[r] == 4) { if (quad < 0) quad = r; else quad2 = r; }
        else if (cnt[r] != 0 && cnt[r] != 2) ok = false;  // 带出部分须成对
      }
      if (ok && quad >= 0) {
        if (quad2 < 0) { p.type = Pattern::FOUR_TWO_PAIR; p.main_rank = quad; return p; }
        if (kFourTwoPairAllowSamePair) {
          p.type = Pattern::FOUR_TWO_PAIR;
          p.main_rank = quad > quad2 ? quad : quad2;
          return p;
        }
      }
      detail::try_straight(cnt, p.length, p);
      return p;
    }
    default:  // 7 / 9..12 张: 仅可能是顺子 [R6]
      detail::try_straight(cnt, p.length, p);
      return p;
  }
}

// ---------------------------------------------------------------------------
// 压牌判定 [R6][R7]
// ---------------------------------------------------------------------------
inline bool beats(const Pattern& last, const Pattern& move) {
  if (!last.valid() || !move.valid()) return false;
  if (move.is_bomb())
    return !last.is_bomb() || move.main_rank > last.main_rank;  // 炸弹压一切; 炸弹间比点数 [R7]
  if (last.is_bomb()) return false;                             // 非炸弹压不了炸弹 [R7]
  return last.type == move.type &&
         last.length == move.length &&
         move.main_rank > last.main_rank;
}

// ---------------------------------------------------------------------------
// 出牌合法性预检（独立工具）
// ---------------------------------------------------------------------------
inline bool is_legal_play(const std::vector<u8>& hand,
                          const std::vector<u8>& cards,
                          const Pattern* must_beat) {
  if (cards.empty() || cards.size() > hand.size()) return false;
  bool used[kTotalCards] = {false};
  for (u8 c : cards) {
    bool found = false;
    for (size_t i = 0; i < hand.size(); ++i) {
      if (!used[i] && hand[i] == c) { used[i] = true; found = true; break; }
    }
    if (!found) return false;
  }
  Pattern p = pattern_of(cards);
  if (!p.valid()) return false;
  if (must_beat && !beats(*must_beat, p)) return false;
  return true;
}

// ---------------------------------------------------------------------------
// 合法牌型枚举（返回牌型代表）
// ---------------------------------------------------------------------------
inline std::vector<std::vector<u8>> legal_moves(const std::vector<u8>& hand,
                                                const Pattern* must_beat) {
  std::vector<std::vector<u8>> out;
  auto push = [&](std::vector<u8> cs) {
    Pattern p = pattern_of(cs);
    if (!p.valid()) return;
    if (must_beat && !beats(*must_beat, p)) return;
    out.push_back(std::move(cs));
  };

  int cnt[16] = {0};
  std::vector<u8> rep[16];
  for (u8 c : hand) { const int r = rank_of(c); ++cnt[r]; rep[r].push_back(c); }

  for (int r = kMinRank; r <= kMaxRank; ++r) {
    if (cnt[r] >= 1) push({rep[r][0]});
    if (cnt[r] >= 2) push({rep[r][0], rep[r][1]});
    if (cnt[r] >= 3) push({rep[r][0], rep[r][1], rep[r][2]});
    if (cnt[r] >= 4) push({rep[r][0], rep[r][1], rep[r][2], rep[r][3]});
  }
  for (int t = kMinRank; t <= kMaxRank; ++t)
    if (cnt[t] >= 3)
      for (int pr = kMinRank; pr <= kMaxRank; ++pr)
        if (pr != t && cnt[pr] >= 2)
          push({rep[t][0], rep[t][1], rep[t][2], rep[pr][0], rep[pr][1]});
  for (int len = kStraightMinLen; len <= kStraightMaxLen; ++len)
    for (int s = kMinRank; s + len - 1 <= kAceRank; ++s) {
      bool ok = true;
      std::vector<u8> cs;
      cs.reserve(static_cast<size_t>(len));
      for (int r = s; r < s + len; ++r) {
        if (!cnt[r]) { ok = false; break; }
        cs.push_back(rep[r][0]);
      }
      if (ok) push(std::move(cs));
    }
  for (int b = kMinRank; b <= kMaxRank; ++b)
    if (cnt[b] == 4) {
      const std::vector<u8> bomb = {rep[b][0], rep[b][1], rep[b][2], rep[b][3]};
      for (int i = kMinRank; i <= kMaxRank; ++i) {
        if (i == b || !cnt[i]) continue;
        for (int j = i + 1; j <= kMaxRank; ++j) {
          if (j == b || !cnt[j]) continue;
          std::vector<u8> cs = bomb;
          cs.push_back(rep[i][0]); cs.push_back(rep[j][0]);
          push(std::move(cs));
        }
        if (cnt[i] >= 2) {
          std::vector<u8> cs = bomb;
          cs.push_back(rep[i][0]); cs.push_back(rep[i][1]);
          push(std::move(cs));
        }
      }
    }
  for (int b = kMinRank; b <= kMaxRank; ++b)
    if (cnt[b] == 4) {
      const std::vector<u8> bomb = {rep[b][0], rep[b][1], rep[b][2], rep[b][3]};
      for (int i = kMinRank; i <= kMaxRank; ++i) {
        if (i == b || cnt[i] < 2) continue;
        for (int j = i + 1; j <= kMaxRank; ++j) {
          if (j == b || cnt[j] < 2) continue;
          std::vector<u8> cs = bomb;
          cs.push_back(rep[i][0]); cs.push_back(rep[i][1]);
          cs.push_back(rep[j][0]); cs.push_back(rep[j][1]);
          push(std::move(cs));
        }
        if (kFourTwoPairAllowSamePair && cnt[i] == 4) {
          std::vector<u8> cs = bomb;
          cs.push_back(rep[i][0]); cs.push_back(rep[i][1]);
          cs.push_back(rep[i][2]); cs.push_back(rep[i][3]);
          push(std::move(cs));
        }
      }
    }
  return out;
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------
struct CardAction {
  enum Kind { PLAY = 0, PASS = 1, TIMEOUT = 2 };
  int  seat  = -1;
  Kind kind  = PASS;
  std::vector<u8> cards;
};

// ---------------------------------------------------------------------------
// 事件（轻量结构化, 不含网络/JSON）
// ---------------------------------------------------------------------------
struct Event {
  enum Kind {
    PLAY_MADE,
    PASS_MADE,
    REVEAL_H2,
    REVEAL_SK,
    HAND_EMPTY,
    ROUND_END,
    GAME_OVER
  };
  Kind kind       = PLAY_MADE;
  int  seat       = -1;
  int  round      = -1;
  std::vector<u8> cards;
  Pattern pat;
  int  winner_team = -1;
};

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
enum class Phase { DEALING = 0, PLAYING = 1, FINISHED = 2 };

struct LastPlay {
  int seat = -1;
  std::vector<u8> cards;
  Pattern pat;
};

struct GameState {
  Phase phase = Phase::DEALING;
  int teams[kNumSeats] = {-1, -1, -1, -1};
  std::vector<u8> hands[kNumSeats];
  int turn_seat  = -1;
  LastPlay last_play;
  int pass_count = 0;
  bool passed_flags[kNumSeats] = {false, false, false, false};
  int round_seq  = 0;
  int revealed_h2 = -1;
  int revealed_sk = -1;
  std::vector<int> finished_order;
  int winner_team = -1;

  bool hand_empty(int seat) const { return hands[seat].empty(); }
  bool seat_finished(int seat) const {
    for (int s : finished_order) if (s == seat) return true;
    return false;
  }
  int active_count() const { return kNumSeats - static_cast<int>(finished_order.size()); }
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
inline int next_seat(int seat) { return (seat + 1) % kNumSeats; }

inline int next_active_seat(const GameState& s, int from) {
  for (int i = 1; i <= kNumSeats; ++i) {
    const int c = (from + i) % kNumSeats;
    if (!s.seat_finished(c)) return c;
  }
  return -1;
}

inline int holder_of(const GameState& s, u8 card) {
  for (int i = 0; i < kNumSeats; ++i)
    if (std::find(s.hands[i].begin(), s.hands[i].end(), card) != s.hands[i].end())
      return i;
  return -1;
}

inline void assign_teams(GameState& s) {
  const int h2 = holder_of(s, kHeartTwo);
  const int sk = holder_of(s, kSpadeKing);
  for (int i = 0; i < kNumSeats; ++i)
    s.teams[i] = (i == h2 || i == sk) ? kTeamKey : kTeamCrowd;
}

inline bool team_done(const GameState& s, int team) {
  for (int i = 0; i < kNumSeats; ++i)
    if (s.teams[i] == team && !s.seat_finished(i)) return false;
  return true;
}

inline bool contains_card(const std::vector<u8>& cards, u8 c) {
  return std::find(cards.begin(), cards.end(), c) != cards.end();
}

inline void remove_cards(std::vector<u8>& hand, const std::vector<u8>& cards) {
  for (u8 c : cards) {
    auto it = std::find(hand.begin(), hand.end(), c);
    if (it != hand.end()) hand.erase(it);
  }
}

// ---------------------------------------------------------------------------
// 发牌 [R1][R3][R4][R8]（手写 Fisher-Yates, 同 seed 恒同局）
// ---------------------------------------------------------------------------
inline GameState deal(unsigned seed) {
  GameState s;
  std::mt19937 rng(seed);
  std::array<u8, kTotalCards> deck;
  for (int i = 0; i < kTotalCards; ++i) deck[i] = static_cast<u8>(i);
  for (int i = kTotalCards - 1; i > 0; --i) {
    const int j = static_cast<int>(rng() % static_cast<unsigned>(i + 1));
    std::swap(deck[i], deck[j]);
  }
  for (int i = 0; i < kTotalCards; ++i)
    s.hands[i / kCardsPerHand].push_back(deck[i]);
  for (int i = 0; i < kNumSeats; ++i)
    std::sort(s.hands[i].begin(), s.hands[i].end());
  assign_teams(s);
  s.turn_seat = holder_of(s, kClubThree);
  s.phase = Phase::PLAYING;
  return s;
}

// ---------------------------------------------------------------------------
// 动作裁决（唯一状态入口）
// ---------------------------------------------------------------------------
enum class Accept {
  OK = 0,
  ERR_WRONG_PHASE        = 1,
  ERR_BAD_SEAT           = 2,
  ERR_SEAT_FINISHED      = 3,
  ERR_NOT_YOUR_TURN      = 4,
  ERR_LEADER_CANNOT_PASS = 5,
  ERR_PASSED_LOCKED      = 6,
  ERR_EMPTY_CARDS        = 7,
  ERR_CARD_NOT_IN_HAND   = 8,
  ERR_INVALID_PATTERN    = 9,
  ERR_CANNOT_BEAT        = 10
};

struct ActionResult {
  Accept code = Accept::ERR_WRONG_PHASE;
  std::vector<Event> events;
  GameState next;
  bool ok() const { return code == Accept::OK; }
};

namespace detail {

inline bool maybe_end_round(GameState& s, std::vector<Event>& ev) {
  const int active = s.active_count();
  if (s.pass_count < active - 1) return false;

  const int last = s.last_play.seat;
  const int new_leader = s.seat_finished(last) ? next_active_seat(s, last) : last;
  ++s.round_seq;
  s.pass_count = 0;
  for (int i = 0; i < kNumSeats; ++i) s.passed_flags[i] = false;
  s.last_play = LastPlay{};
  s.turn_seat = new_leader;

  Event e;
  e.kind = Event::ROUND_END;
  e.seat = new_leader;
  e.round = s.round_seq;
  ev.push_back(std::move(e));
  return true;
}

}  // namespace detail

inline ActionResult apply_action(const GameState& st, const CardAction& act) {
  ActionResult r;
  r.next = st;
  std::vector<Event>& ev = r.events;
  GameState& s = r.next;

  if (st.phase != Phase::PLAYING) { r.code = Accept::ERR_WRONG_PHASE; return r; }
  if (act.seat < 0 || act.seat >= kNumSeats) { r.code = Accept::ERR_BAD_SEAT; return r; }
  if (st.seat_finished(act.seat)) { r.code = Accept::ERR_SEAT_FINISHED; return r; }
  if (act.seat != st.turn_seat)   { r.code = Accept::ERR_NOT_YOUR_TURN; return r; }

  auto emit = [&](Event::Kind k, int seat_) {
    Event e; e.kind = k; e.seat = seat_; e.round = st.round_seq;
    ev.push_back(std::move(e));
  };

  const bool is_pass = (act.kind == CardAction::PASS || act.kind == CardAction::TIMEOUT);

  if (is_pass) {
    if (!st.last_play.pat.valid()) { r.code = Accept::ERR_LEADER_CANNOT_PASS; return r; }
    s.passed_flags[act.seat] = true;
    ++s.pass_count;
    emit(Event::PASS_MADE, act.seat);
    if (!detail::maybe_end_round(s, ev))
      s.turn_seat = next_active_seat(s, act.seat);
    r.code = Accept::OK;
    return r;
  }

  if (st.passed_flags[act.seat]) { r.code = Accept::ERR_PASSED_LOCKED; return r; }
  if (act.cards.empty()) { r.code = Accept::ERR_EMPTY_CARDS; return r; }

  {
    bool used[kTotalCards] = {false};
    const std::vector<u8>& h = st.hands[act.seat];
    for (u8 c : act.cards) {
      bool found = false;
      for (size_t i = 0; i < h.size(); ++i) {
        if (!used[i] && h[i] == c) { used[i] = true; found = true; break; }
      }
      if (!found) { r.code = Accept::ERR_CARD_NOT_IN_HAND; return r; }
    }
  }
  const Pattern pat = pattern_of(act.cards);
  if (!pat.valid()) { r.code = Accept::ERR_INVALID_PATTERN; return r; }
  if (st.last_play.pat.valid() && !beats(st.last_play.pat, pat)) {
    r.code = Accept::ERR_CANNOT_BEAT; return r;
  }

  remove_cards(s.hands[act.seat], act.cards);
  s.last_play.seat  = act.seat;
  s.last_play.cards = act.cards;
  s.last_play.pat   = pat;
  s.pass_count = 0;

  emit(Event::PLAY_MADE, act.seat);
  ev.back().cards = act.cards;
  ev.back().pat   = pat;

  if (s.revealed_h2 < 0 && contains_card(act.cards, kHeartTwo)) {
    s.revealed_h2 = act.seat;
    emit(Event::REVEAL_H2, act.seat);
  }
  if (s.revealed_sk < 0 && contains_card(act.cards, kSpadeKing)) {
    s.revealed_sk = act.seat;
    emit(Event::REVEAL_SK, act.seat);
  }

  if (s.hands[act.seat].empty()) {
    s.finished_order.push_back(act.seat);
    emit(Event::HAND_EMPTY, act.seat);
    if (team_done(s, s.teams[act.seat])) {
      s.phase = Phase::FINISHED;
      s.winner_team = s.teams[act.seat];
      emit(Event::GAME_OVER, act.seat);
      ev.back().winner_team = s.winner_team;
      r.code = Accept::OK;
      return r;
    }
  }

  if (!detail::maybe_end_round(s, ev))
    s.turn_seat = next_active_seat(s, act.seat);
  r.code = Accept::OK;
  return r;
}

}  // namespace game

#endif  // GAME_H_
