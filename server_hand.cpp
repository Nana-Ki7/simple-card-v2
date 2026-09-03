// ============================================================================
// server.cpp — SimpleCardGame v2 服务端（Phase 2）
// C++17 + websocketpp + nlohmann/json，单线程事件循环
// 依赖同目录 game.h（纯逻辑引擎）
//
// 编译: g++ -std=c++17 server.cpp -o server
// 运行: ./server   → 监听 ws://0.0.0.0:9002
//
// 设计要点：
//  - 服务器单一权威：所有规则判定调 game::apply_action，前端只展示
//  - 座位固定 0..3，4 人满自动发牌（确定性 seed 随机）
//  - 每房间 30s steady_timer + generation 防旧定时器误触发
//  - 断线保留座位计时照走，超时自动托管（领出走最小单张/跟牌 PASS）
//  - 重连补发全量快照（手牌 + 轮次 + 桌面）
// ============================================================================
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <iostream>
#include <algorithm>

#include "game.h"

using json = nlohmann::json;
using websocketpp::connection_hdl;
typedef websocketpp::server<websocketpp::config::asio> wsserver;

static wsserver ws_server;
static constexpr int kTurnSeconds = 30;
static constexpr int kMaxRooms = 20;

// ---------------- 会话 ----------------
struct Player {
    Player(connection_hdl h, std::string t, std::string n)
        : hdl(h), token(std::move(t)), name(std::move(n)) {}
    connection_hdl hdl;
    std::string token;
    std::string name;
    int room_id = -1;      // -1 = 不在房间
    bool connected = true;
};

// token 会话表（unique_ptr 保证地址稳定，rehash 不失效）
static std::unordered_map<std::string, std::unique_ptr<Player>> g_auth;
// hdl → token（快速查连接对应玩家）
static std::map<connection_hdl, Player*, std::owner_less<connection_hdl>> g_hdl;

// ---------------- 房间 ----------------
struct Room : std::enable_shared_from_this<Room> {
    explicit Room(int i) : id(i) {}
    int id = 0;
    bool started = false;
    bool finished = false;
    Player* seats[4] = {nullptr, nullptr, nullptr, nullptr};  // 固定座位
    game::GameState gs;                 // 仅 started 后有效
    uint64_t gen = 0;                   // turn timer generation
    std::shared_ptr<websocketpp::lib::asio::steady_timer> timer;
    int last_event_seq = 0;

    int player_count() const {
        int n = 0;
        for (auto* p : seats) if (p) ++n;
        return n;
    }
    int seat_of(Player* p) const {
        for (int i = 0; i < 4; ++i) if (seats[i] == p) return i;
        return -1;
    }
    bool all_seated() const { return seats[0] && seats[1] && seats[2] && seats[3]; }
};

static std::unordered_map<int, std::shared_ptr<Room>> g_rooms;
static int g_next_room = 1;
static int g_next_seq = 1;

static std::mt19937 g_rng(std::random_device{}());

// 前向声明（互相调用）
static void broadcast_turn(std::shared_ptr<Room> room);
static void arm_turn_timer(std::shared_ptr<Room> room);

// ---------------- 工具 ----------------
static Player* find_player(connection_hdl hdl) {
    auto it = g_hdl.find(hdl);
    return it == g_hdl.end() ? nullptr : it->second;
}

static void send_to(connection_hdl hdl, const json& msg) {
    try {
        ws_server.send(hdl, msg.dump(), websocketpp::frame::opcode::text);
    } catch (...) {}
}

static void send_error(Player* p, const std::string& code, const std::string& msg) {
    send_to(p->hdl, json{{"type", "error"}, {"code", code}, {"message", msg}});
}

// Accept 错误码 → 字符串
static std::string code_of(game::Accept a) {
    switch (a) {
        case game::Accept::ERR_WRONG_PHASE: return "WRONG_PHASE";
        case game::Accept::ERR_BAD_SEAT: return "BAD_SEAT";
        case game::Accept::ERR_SEAT_FINISHED: return "SEAT_FINISHED";
        case game::Accept::ERR_NOT_YOUR_TURN: return "NOT_YOUR_TURN";
        case game::Accept::ERR_LEADER_CANNOT_PASS: return "LEADER_CANNOT_PASS";
        case game::Accept::ERR_PASSED_LOCKED: return "PASSED_LOCKED";
        case game::Accept::ERR_EMPTY_CARDS: return "EMPTY_CARDS";
        case game::Accept::ERR_CARD_NOT_IN_HAND: return "CARD_NOT_IN_HAND";
        case game::Accept::ERR_INVALID_PATTERN: return "INVALID_PATTERN";
        case game::Accept::ERR_CANNOT_BEAT: return "CANNOT_BEAT";
        default: return "UNKNOWN";
    }
}

static std::string room_token() {
    static const char* ch = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<int> d(0, 61);
    std::string s;
    for (int i = 0; i < 40; ++i) s += ch[d(g_rng)];
    return s;
}

// 座位满时给每人发手牌并开局
static void try_start_game(std::shared_ptr<Room> room) {
    if (room->started || !room->all_seated()) return;
    room->started = true;
    room->gs = game::deal(g_rng());
    for (int seat = 0; seat < 4; ++seat) {
        Player* p = room->seats[seat];
        json hand = json::array();
        for (auto c : room->gs.hands[seat]) hand.push_back(c);
        json msg{
            {"type", "your_hand"}, {"room_id", room->id}, {"seat", seat},
            {"hand", hand}, {"turn_seat", room->gs.turn_seat}
        };
        send_to(p->hdl, msg);
    }
    json start{
        {"type", "game_started"}, {"room_id", room->id},
        {"turn_seat", room->gs.turn_seat}, {"timeout_ms", kTurnSeconds * 1000}
    };
    // 广播开局 + 轮次
    for (auto* p : room->seats) send_to(p->hdl, start);
    broadcast_turn(room);
}

// 广播当前轮次（发给所有人：能看到轮到谁）
static void broadcast_turn(std::shared_ptr<Room> room) {
    if (!room->started || room->finished) return;
    json msg{
        {"type", "turn_update"},
        {"room_id", room->id},
        {"seat", room->gs.turn_seat},
        {"round_seq", room->gs.round_seq},
        {"timeout_ms", kTurnSeconds * 1000},
        {"can_pass", room->gs.last_play.pat.valid()},  // 领出位不可 PASS
        {"hand_counts", json::array()}
    };
    for (int i = 0; i < 4; ++i)
        msg["hand_counts"].push_back(room->started ? (int)room->gs.hands[i].size() : 0);
    for (auto* p : room->seats)
        if (p) send_to(p->hdl, msg);
}

// 安排本房间回合计时（generation 防旧 timer 串扰）
static void arm_turn_timer(std::shared_ptr<Room> room);

// 广播一个引擎事件给全房间（含剩余手牌数）
static void broadcast_event(std::shared_ptr<Room> room, const game::Event& e) {
    json msg{{"type", ""}, {"room_id", room->id}, {"round_seq", e.round}};
    switch (e.kind) {
        case game::Event::PLAY_MADE: {
            msg["type"] = "play_made";
            msg["seat"] = e.seat;
            json cards = json::array();
            for (auto c : e.cards) cards.push_back(c);
            msg["cards"] = cards;
            json counts = json::array();
            for (int i = 0; i < 4; ++i) counts.push_back((int)room->gs.hands[i].size());
            msg["hand_counts"] = counts;
            break;
        }
        case game::Event::PASS_MADE:
            msg["type"] = "pass_made";
            msg["seat"] = e.seat;
            break;
        case game::Event::REVEAL_H2:
            msg["type"] = "reveal";
            msg["kind"] = "H2";
            msg["seat"] = e.seat;
            break;
        case game::Event::REVEAL_SK:
            msg["type"] = "reveal";
            msg["kind"] = "SK";
            msg["seat"] = e.seat;
            break;
        case game::Event::HAND_EMPTY:
            msg["type"] = "hand_empty";
            msg["seat"] = e.seat;
            break;
        case game::Event::ROUND_END:
            msg["type"] = "round_end";
            msg["leader"] = e.seat;
            break;
        case game::Event::GAME_OVER:
            msg["type"] = "game_over";
            msg["winner_team"] = e.winner_team;
            json order = json::array();
            for (int s : room->gs.finished_order) order.push_back(s);
            msg["finish_order"] = order;
            room->finished = true;
            // 终局揭示全员队伍
            json teams = json::array();
            for (int i = 0; i < 4; ++i) teams.push_back(room->gs.teams[i]);
            msg["teams"] = teams;
            break;
    }
    if (!msg["type"].is_null())
        for (auto* p : room->seats)
            if (p && p->connected) send_to(p->hdl, msg);
}

// 超时托管：领出走最小单张，跟牌自动 PASS
static void on_turn_timeout(std::shared_ptr<Room> room, uint64_t gen) {
    if (!room || gen != room->gen || !room->started || room->finished) return;
    const int seat = room->gs.turn_seat;
    if (seat < 0 || !room->seats[seat]) return;
    game::CardAction act;
    act.seat = seat;
    act.kind = game::CardAction::TIMEOUT;
    // 领出位（桌面无牌）→ 自动出最小单张
    if (!room->gs.last_play.pat.valid()) {
        act.kind = game::CardAction::PLAY;
        act.cards = {room->gs.hands[seat][0]};
    }
    auto res = game::apply_action(room->gs, act);
    if (res.ok()) {
        room->gs = res.next;
        for (const auto& e : res.events) broadcast_event(room, e);
        if (!room->finished) {
            broadcast_turn(room);
            arm_turn_timer(room);
        }
    } else if (room->started && !room->finished) {
        // 理论上托管不应失败；失败则重试下一位
        arm_turn_timer(room);
    }
}

static void arm_turn_timer(std::shared_ptr<Room> room) {
    if (!room->timer) {
        room->timer = std::make_shared<websocketpp::lib::asio::steady_timer>(
            ws_server.get_io_service());
    }
    room->timer->cancel();
    const uint64_t gen = ++room->gen;
    room->timer->expires_after(std::chrono::seconds(kTurnSeconds));
    room->timer->async_wait([room, gen](const websocketpp::lib::error_code& ec) {
        if (ec) return;
        on_turn_timeout(room, gen);
    });
}

// 重连/同步时补发该玩家完整快照
static void send_snapshot(Player* p, std::shared_ptr<Room> room) {
    const int seat = room->seat_of(p);
    if (seat < 0) return;
    json hand = json::array();
    for (auto c : room->gs.hands[seat]) hand.push_back(c);
    send_to(p->hdl, json{
        {"type", "your_hand"}, {"room_id", room->id}, {"seat", seat},
        {"hand", hand}, {"turn_seat", room->gs.turn_seat}, {"reconnect", true}
    });
    // 桌面最后出的牌
    if (room->gs.last_play.pat.valid()) {
        json cards = json::array();
        for (auto c : room->gs.last_play.cards) cards.push_back(c);
        send_to(p->hdl, json{
            {"type", "last_play_sync"}, {"seat", room->gs.last_play.seat},
            {"cards", cards}, {"round_seq", room->gs.round_seq}
        });
    }
    if (room->gs.revealed_h2 >= 0)
        send_to(p->hdl, json{{"type","reveal"},{"kind","H2"},{"seat",room->gs.revealed_h2}});
    if (room->gs.revealed_sk >= 0)
        send_to(p->hdl, json{{"type","reveal"},{"kind","SK"},{"seat",room->gs.revealed_sk}});
    broadcast_turn(room);  // 轮次 + 各手牌数（他会自行定位自己）
}

// ---------------- 消息处理 ----------------
static void handle_msg(connection_hdl hdl, const json& j) {
    Player* p = find_player(hdl);
    const std::string type = j.value("type", "");

    if (type == "hello") {
        std::string token = j.value("token", "");
        std::string name = j.value("name", "");
        if (token.empty() || g_auth.find(token) == g_auth.end()) {
            token = room_token();
            g_auth[token] = std::make_unique<Player>(hdl, token, name);
        }
        Player* np = g_auth[token].get();
        np->hdl = hdl;
        np->connected = true;
        if (!name.empty()) np->name = name;
        g_hdl[hdl] = np;
        send_to(hdl, json{{"type", "welcome"}, {"token", token}, {"name", np->name}});
        // 若已在房间（重连），补快照
        if (np->room_id >= 0) {
            auto it = g_rooms.find(np->room_id);
            if (it != g_rooms.end() && it->second->started) {
                send_snapshot(np, it->second);
                // 通知房间该玩家回来了
                json conn{{"type", "player_conn"}, {"room_id", np->room_id},
                          {"seat", it->second->seat_of(np)}, {"connected", true}};
                for (auto* q : it->second->seats) if (q) send_to(q->hdl, conn);
            }
        }
        return;
    }
    if (!p) { send_to(hdl, json{{"type","error"},{"code","NOT_AUTHED"},{"message","先 hello"}}); return; }

    if (type == "create_room") {
        if (p->room_id >= 0) { send_error(p, "ALREADY_IN_ROOM", "已在房间中"); return; }
        if ((int)g_rooms.size() >= kMaxRooms) { send_error(p, "ROOM_LIMIT", "房间已满"); return; }
        int rid = g_next_room++;
        auto room = std::make_shared<Room>(rid);
        g_rooms[rid] = room;
        room->seats[0] = p;
        p->room_id = rid;
        send_to(hdl, json{{"type", "room_state"}, {"room_id", rid}, {"seat", 0},
                          {"phase", "waiting"}, {"players", json::array({{"seat",0},{"name",p->name},{"connected",true}} )}});
        return;
    }
    if (type == "join_room") {
        int rid = j.value("room_id", 0);
        auto it = g_rooms.find(rid);
        if (it == g_rooms.end()) { send_error(p, "ROOM_NOT_FOUND", "房间不存在"); return; }
        auto room = it->second;
        if (p->room_id == rid && room->seat_of(p) >= 0) {  // 已在此房间（重连）
            send_to(hdl, json{{"type","room_state"},{"room_id",rid},{"seat",room->seat_of(p)},{"phase", room->started ? "playing":"waiting"}});
            if (room->started) send_snapshot(p, room);
            return;
        }
        if (p->room_id >= 0) { send_error(p, "ALREADY_IN_ROOM", "你已在其他房间"); return; }
        if (room->started) { send_error(p, "GAME_STARTED", "游戏已开始"); return; }
        if (room->player_count() >= 4) { send_error(p, "ROOM_FULL", "房间已满"); return; }
        int seat = room->player_count();
        room->seats[seat] = p;
        p->room_id = rid;
        json joined{{"type","room_state"},{"room_id",rid},{"seat",seat},{"phase","waiting"}};
        send_to(hdl, joined);
        json pl{{"type","room_update"},{"room_id",rid},{"players",json::array()}};
        for (int i = 0; i < 4; ++i)
            if (room->seats[i])
                pl["players"].push_back({{"seat",i},{"name",room->seats[i]->name},{"connected",room->seats[i]->connected}});
        for (auto* q : room->seats) if (q) send_to(q->hdl, pl);
        try_start_game(room);
        if (room->started) { broadcast_turn(room); arm_turn_timer(room); }
        return;
    }
    if (type == "leave_room") {
        if (p->room_id < 0) { send_error(p, "NOT_IN_ROOM", "不在房间"); return; }
        auto it = g_rooms.find(p->room_id);
        if (it == g_rooms.end()) { p->room_id = -1; return; }
        auto room = it->second;
        if (room->started) { send_error(p, "GAME_STARTED", "游戏已开始不能退出"); return; }
        int seat = room->seat_of(p);
        room->seats[seat] = nullptr;
        p->room_id = -1;
        send_to(hdl, json{{"type","left_room"},{"room_id",room->id}});
        if (room->player_count() == 0) g_rooms.erase(room->id);
        return;
    }
    if (type == "play" || type == "pass") {
        if (p->room_id < 0) { send_error(p, "NOT_IN_ROOM", "不在房间"); return; }
        auto it = g_rooms.find(p->room_id);
        if (it == g_rooms.end()) { p->room_id = -1; return; }
        auto room = it->second;
        if (!room->started || room->finished) { send_error(p, "BAD_PHASE", "游戏未进行中"); return; }
        int seat = room->seat_of(p);
        if (seat != room->gs.turn_seat) { send_error(p, "NOT_YOUR_TURN", "还没轮到你"); return; }
        game::CardAction act;
        act.seat = seat;
        if (type == "pass") {
            act.kind = game::CardAction::PASS;
        } else {
            if (!j.contains("cards") || !j["cards"].is_array() || j["cards"].empty()) {
                send_error(p, "BAD_CARDS", "缺少出牌"); return;
            }
            act.kind = game::CardAction::PLAY;
            for (const auto& c : j["cards"]) act.cards.push_back((game::u8)c.get<int>());
        }
        auto res = game::apply_action(room->gs, act);
        if (!res.ok()) {
            send_error(p, code_of(res.code), "出牌被拒绝");
            return;
        }
        room->gs = res.next;
        for (const auto& e : res.events) broadcast_event(room, e);
        if (!room->finished) {
            broadcast_turn(room);
            arm_turn_timer(room);
        }
        return;
    }
    send_error(p, "UNKNOWN", "未知消息 " + type);
}


static void on_open(connection_hdl) {}
static void on_close(connection_hdl hdl) {
    auto it = g_hdl.find(hdl);
    if (it == g_hdl.end()) return;
    Player* p = it->second;
    std::owner_less<connection_hdl> less;
    // 若 p->hdl 还指向另一个（新）连接，说明 close 的是旧 socket，直接忽略
    if (p->hdl.lock() && (less(p->hdl, hdl) || less(hdl, p->hdl))) {
        g_hdl.erase(it);
        return;
    }
    g_hdl.erase(it);
    p->connected = false;
    p->hdl = connection_hdl();
    if (p->room_id >= 0) {
        auto rit = g_rooms.find(p->room_id);
        if (rit != g_rooms.end()) {
            auto room = rit->second;
            json conn{{"type", "player_conn"}, {"room_id", room->id},
                      {"seat", room->seat_of(p)}, {"connected", false}};
            for (auto* q : room->seats) if (q && q->connected) send_to(q->hdl, conn);
        }
    }
}

static void on_message(connection_hdl hdl, wsserver::message_ptr msg) {
    json j;
    try { j = json::parse(msg->get_payload()); }
    catch (...) { return; }
    handle_msg(hdl, j);
}

int main() {
    ws_server.init_asio();
    ws_server.set_open_handler(&on_open);
    ws_server.set_close_handler(&on_close);
    ws_server.set_message_handler(&on_message);
    ws_server.listen(9002);
    ws_server.start_accept();
    std::cout << "SimpleCardGame v2 server listening on ws://0.0.0.0:9002\n";
    ws_server.run();
    return 0;
}
