// ============================================================================
// server.cpp — SimpleCardGame v2 WebSocket 服务端（GLM 重写版，落地适配）
// C++17 + websocketpp(asio_no_tls) + nlohmann/json + game.h
// 编译: g++ -std=c++17 server.cpp -o server
// ============================================================================
#include "game.h"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;
using server_t = websocketpp::server<websocketpp::config::asio>;
using hdl_t = websocketpp::connection_hdl;
using msg_ptr = server_t::message_ptr;
// 统一走 websocketpp 别名（跟随配置选 boost/standalone），本环境为 get_io_service()
using io_timer = websocketpp::lib::asio::steady_timer;
using err_code = websocketpp::lib::error_code;

namespace {

constexpr uint16_t kListenPort    = 9002;
constexpr uint32_t kTurnTimeoutMs = 30000;

server_t*                g_srv = nullptr;
std::mt19937_64          g_rng{std::random_device{}()};

// ---- 兼容新旧 websocketpp/asio ----
// 新版 (standalone asio): 有 get_io_context()，无 io_service 旧名
// 旧版 (boost asio):      有 get_io_service()
template <typename S>
struct has_io_context {
    template <typename T> static auto check(int)
        -> decltype(std::declval<T&>().get_io_context(), std::true_type{});
    template <typename> static auto check(...) -> std::false_type;
    static constexpr bool value = decltype(check<S>(0))::value;
};
template <typename S>
decltype(auto) get_server_io(S& s) {
    if constexpr (has_io_context<S>::value) return s.get_io_context();
    else return s.get_io_service();
}
uint64_t                 g_room_seq = 0;

struct Player {
    std::string token;
    std::string name;
    hdl_t       hdl;
    std::string room_id;
    bool        connected = true;
};

struct Room {
    explicit Room(std::string id_) : id(std::move(id_)) {}
    std::string id;
    bool started  = false;
    bool finished = false;
    Player* seats[4] = {nullptr, nullptr, nullptr, nullptr};
    game::GameState gs;
    std::shared_ptr<io_timer> timer;
    uint64_t  timer_gen = 0;
    uint64_t  seq = 0;
};

std::unordered_map<std::string, std::unique_ptr<Player>> g_players;
std::map<hdl_t, Player*, std::owner_less<hdl_t>>         g_by_hdl;
std::unordered_map<std::string, std::unique_ptr<Room>>   g_rooms;

std::string now_hms() {
    std::time_t t = std::time(nullptr);
    std::tm tmb{};
    localtime_r(&t, &tmb);
    std::ostringstream os;
    os << std::put_time(&tmb, "%H:%M:%S");
    return os.str();
}
void log_line(const std::string& tag, const std::string& detail) {
    std::cout << '[' << now_hms() << "] [" << tag << "] " << detail << std::endl;
}
bool same_owner(const hdl_t& a, const hdl_t& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}
std::string gen_token() {
    for (;;) {
        std::ostringstream os;
        os << std::hex << std::setfill('0')
           << std::setw(16) << g_rng() << std::setw(16) << g_rng();
        std::string t = os.str();
        if (!g_players.count(t)) return t;
    }
}
std::string gen_room_id() {  // 纯数字字符串，与前端 ?room= 数字兼容
    return std::to_string(++g_room_seq);
}
std::string cards_str(const std::vector<game::u8>& cards) {
    std::ostringstream os;
    os << '[';
    for (size_t i = 0; i < cards.size(); ++i) {
        if (i) os << ',';
        os << static_cast<int>(cards[i]);
    }
    os << ']';
    return os.str();
}

void send_raw(Player* p, const std::string& payload) {
    if (!p || !p->connected) return;
    try {
        g_srv->send(p->hdl, payload, websocketpp::frame::opcode::text);
    } catch (...) {}
}
void send_json(Player* p, const json& j) { send_raw(p, j.dump()); }
void send_error(Player* p, const std::string& code, const std::string& message) {
    log_line("error", "name=" + (p ? p->name : "?") + " code=" + code + " msg=" + message);
    send_json(p, json{{"type", "error"}, {"code", code}, {"message", message}});
}
void send_error_hdl(hdl_t hdl, const std::string& code, const std::string& message) {
    log_line("error", "code=" + code + " msg=" + message);
    try {
        g_srv->send(hdl, json{{"type", "error"}, {"code", code}, {"message", message}}.dump(),
                    websocketpp::frame::opcode::text);
    } catch (...) {}
}
void broadcast(Room* room, const json& j, Player* except = nullptr) {
    const std::string payload = j.dump();
    for (Player* sp : room->seats) {
        if (!sp || sp == except) continue;
        send_raw(sp, payload);
    }
}

Room* room_of(Player* p) {
    if (!p || p->room_id.empty()) return nullptr;
    auto it = g_rooms.find(p->room_id);
    if (it == g_rooms.end()) { p->room_id.clear(); return nullptr; }
    Room* room = it->second.get();
    for (Player* sp : room->seats) if (sp == p) return room;
    p->room_id.clear();
    return nullptr;
}
int seat_of(Room* room, Player* p) {
    for (int i = 0; i < game::kNumSeats; ++i) if (room->seats[i] == p) return i;
    return -1;
}

json hand_counts_json(const game::GameState& gs) {
    json a = json::array();
    for (int i = 0; i < game::kNumSeats; ++i) a.push_back(gs.hands[i].size());
    return a;
}
json teams_json(const game::GameState& gs) {
    json a = json::array();
    for (int i = 0; i < game::kNumSeats; ++i) a.push_back(gs.teams[i]);
    return a;
}
json turn_update_msg(Room* room) {
    const game::GameState& gs = room->gs;
    return json{{"type", "turn_update"}, {"room_id", room->id}, {"seat", gs.turn_seat},
                {"round_seq", gs.round_seq}, {"timeout_ms", kTurnTimeoutMs},
                {"can_pass", gs.last_play.pat.valid()}, {"hand_counts", hand_counts_json(gs)}};
}
json room_update_msg(Room* room) {
    json players = json::array();
    for (int i = 0; i < game::kNumSeats; ++i) {
        Player* sp = room->seats[i];
        if (!sp) continue;
        players.push_back(json{{"seat", i}, {"name", sp->name}, {"connected", sp->connected}});
    }
    return json{{"type", "room_update"}, {"room_id", room->id}, {"players", players}};
}
void broadcast_room_update(Room* room) { broadcast(room, room_update_msg(room)); }

void on_turn_timeout(Room* room);
void stop_turn_timer(Room* room) { ++room->timer_gen; if (room->timer) room->timer->cancel(); }
void arm_turn_timer(Room* room, uint32_t timeout_ms) {
    stop_turn_timer(room);
    const uint64_t gen = room->timer_gen;
    const std::string room_id = room->id;
    if (!room->timer) room->timer = std::make_shared<io_timer>(get_server_io(*g_srv));
    room->timer->expires_after(std::chrono::milliseconds(timeout_ms));
    room->timer->async_wait([room_id, gen](const err_code& ec) {
        if (ec) return;
        auto it = g_rooms.find(room_id);
        if (it == g_rooms.end()) return;
        Room* r = it->second.get();
        if (r->timer_gen != gen) return;
        on_turn_timeout(r);
    });
}

void maybe_destroy_room(Room* room) {
    for (Player* sp : room->seats) if (sp) return;
    stop_turn_timer(room);
    const std::string id = room->id;
    g_rooms.erase(id);
    log_line("room_destroy", "room=" + id);
}
void destroy_player(Player* p) {
    Room* room = room_of(p);
    if (room) {
        int s = seat_of(room, p);
        if (s >= 0) { room->seats[s] = nullptr; broadcast_room_update(room); }
        maybe_destroy_room(room);
    }
    p->room_id.clear();
    g_by_hdl.erase(p->hdl);
    g_players.erase(p->token);
}

const char* accept_err_code(game::Accept a) {
    switch (a) {
        case game::Accept::ERR_WRONG_PHASE:        return "WRONG_PHASE";
        case game::Accept::ERR_BAD_SEAT:           return "NOT_YOUR_TURN";
        case game::Accept::ERR_SEAT_FINISHED:      return "WRONG_PHASE";
        case game::Accept::ERR_NOT_YOUR_TURN:      return "NOT_YOUR_TURN";
        case game::Accept::ERR_LEADER_CANNOT_PASS: return "LEADER_CANNOT_PASS";
        case game::Accept::ERR_PASSED_LOCKED:      return "PASSED_LOCKED";
        case game::Accept::ERR_EMPTY_CARDS:        return "CARD_NOT_IN_HAND";
        case game::Accept::ERR_CARD_NOT_IN_HAND:   return "CARD_NOT_IN_HAND";
        case game::Accept::ERR_INVALID_PATTERN:    return "CANNOT_BEAT";
        case game::Accept::ERR_CANNOT_BEAT:        return "CANNOT_BEAT";
    }
    return "WRONG_PHASE";
}
const char* accept_err_msg(game::Accept a) {
    switch (a) {
        case game::Accept::ERR_WRONG_PHASE:        return "wrong phase";
        case game::Accept::ERR_BAD_SEAT:           return "bad seat";
        case game::Accept::ERR_SEAT_FINISHED:      return "seat already finished";
        case game::Accept::ERR_NOT_YOUR_TURN:      return "not your turn";
        case game::Accept::ERR_LEADER_CANNOT_PASS: return "leader cannot pass";
        case game::Accept::ERR_PASSED_LOCKED:      return "already passed this round";
        case game::Accept::ERR_EMPTY_CARDS:        return "empty card list";
        case game::Accept::ERR_CARD_NOT_IN_HAND:   return "card not in hand";
        case game::Accept::ERR_INVALID_PATTERN:    return "invalid card pattern";
        case game::Accept::ERR_CANNOT_BEAT:        return "cannot beat last play";
    }
    return "unknown error";
}

void apply_and_broadcast(Room* room, const game::CardAction& act, Player* from,
                         const char* source) {
    game::ActionResult res = game::apply_action(room->gs, act);
    if (!res.ok()) {
        log_line(source, "room=" + room->id + " seat=" + std::to_string(act.seat) +
                             " rejected accept=" + std::to_string(static_cast<int>(res.code)));
        if (from) send_error(from, accept_err_code(res.code), accept_err_msg(res.code));
        return;
    }
    room->gs = res.next;
    ++room->seq;
    std::string ev_names;
    const int cur_round = room->gs.round_seq;

    for (const game::Event& ev : res.events) {
        const int ev_round = ev.round >= 0 ? ev.round : cur_round;
        switch (ev.kind) {
            case game::Event::PLAY_MADE:
                broadcast(room, json{{"type", "play_made"}, {"room_id", room->id},
                                     {"seat", ev.seat}, {"cards", ev.cards},
                                     {"round_seq", ev_round},
                                     {"hand_counts", hand_counts_json(room->gs)}});
                ev_names += "PLAY ";
                break;
            case game::Event::PASS_MADE:
                broadcast(room, json{{"type", "pass_made"}, {"room_id", room->id},
                                     {"seat", ev.seat}, {"round_seq", ev_round}});
                ev_names += "PASS ";
                break;
            case game::Event::REVEAL_H2:
                broadcast(room, json{{"type", "reveal"}, {"room_id", room->id},
                                     {"kind", "H2"}, {"seat", ev.seat}});
                ev_names += "REVEAL_H2 ";
                break;
            case game::Event::REVEAL_SK:
                broadcast(room, json{{"type", "reveal"}, {"room_id", room->id},
                                     {"kind", "SK"}, {"seat", ev.seat}});
                ev_names += "REVEAL_SK ";
                break;
            case game::Event::HAND_EMPTY:
                broadcast(room, json{{"type", "hand_empty"}, {"room_id", room->id}, {"seat", ev.seat}});
                ev_names += "HAND_EMPTY ";
                break;
            case game::Event::ROUND_END:
                broadcast(room, json{{"type", "round_end"}, {"room_id", room->id},
                                     {"leader", room->gs.turn_seat}, {"round_seq", ev_round}});
                ev_names += "ROUND_END ";
                break;
            case game::Event::GAME_OVER: {
                const int wt = ev.winner_team >= 0 ? ev.winner_team : room->gs.winner_team;
                broadcast(room, json{{"type", "game_over"}, {"room_id", room->id},
                                     {"winner_team", wt},
                                     {"finish_order", room->gs.finished_order},
                                     {"teams", teams_json(room->gs)}});
                room->finished = true;
                stop_turn_timer(room);
                ev_names += "GAME_OVER ";
                break;
            }
        }
    }
    if (room->gs.phase == game::Phase::FINISHED && !room->finished) {
        room->finished = true;
        stop_turn_timer(room);
    }
    log_line(source, "room=" + room->id + " seq=" + std::to_string(room->seq) +
                         " seat=" + std::to_string(act.seat) +
                         " kind=" + (act.kind == game::CardAction::PLAY ? "PLAY" : "PASS") +
                         " cards=" + cards_str(act.cards) + " events=[" + ev_names + "] turn=" +
                         std::to_string(room->gs.turn_seat));
    if (!room->finished) {
        broadcast(room, turn_update_msg(room));
        arm_turn_timer(room, kTurnTimeoutMs);
    }
}

void on_turn_timeout(Room* room) {
    if (room->finished || room->gs.phase != game::Phase::PLAYING) return;
    const int seat = room->gs.turn_seat;
    if (seat < 0 || seat >= game::kNumSeats || room->gs.seat_finished(seat)) return;
    game::CardAction act;
    act.seat = seat;
    const bool leader = !room->gs.last_play.pat.valid();
    if (leader) {
        act.kind = game::CardAction::PLAY;
        if (!room->gs.hands[seat].empty())
            act.cards.push_back(room->gs.hands[seat].front());
    } else {
        act.kind = game::CardAction::PASS;
    }
    log_line("timeout", "room=" + room->id + " seat=" + std::to_string(seat) +
                             (leader ? " auto_play cards=" + cards_str(act.cards) : " auto_pass"));
    apply_and_broadcast(room, act, nullptr, "timeout");
}

void start_game(Room* room, const char* reason) {
    const unsigned seed = static_cast<unsigned>(
        g_rng() ^ static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    room->gs = game::deal(seed);
    room->started = true;
    room->finished = false;
    room->seq = 0;
    log_line("game_start", "room=" + room->id + " seed=" + std::to_string(seed) +
                               " reason=" + reason);
    broadcast(room, json{{"type", "game_started"}, {"room_id", room->id},
                         {"turn_seat", room->gs.turn_seat}, {"timeout_ms", kTurnTimeoutMs}});
    for (int i = 0; i < game::kNumSeats; ++i) {
        Player* sp = room->seats[i];
        if (!sp) continue;
        send_json(sp, json{{"type", "your_hand"}, {"room_id", room->id}, {"seat", i},
                           {"hand", room->gs.hands[i]}, {"turn_seat", room->gs.turn_seat}});
    }
    broadcast(room, turn_update_msg(room));
    arm_turn_timer(room, kTurnTimeoutMs);
}

void send_reconnect_snapshot(Room* room, Player* p, int seat) {
    const game::GameState& gs = room->gs;
    p->connected = true;
    broadcast(room, json{{"type", "player_conn"}, {"room_id", room->id},
                         {"seat", seat}, {"connected", true}}, p);
    send_json(p, json{{"type", "your_hand"}, {"room_id", room->id}, {"seat", seat},
                      {"hand", gs.hands[seat]}, {"turn_seat", gs.turn_seat}, {"reconnect", true}});
    if (gs.last_play.pat.valid()) {
        send_json(p, json{{"type", "last_play_sync"}, {"seat", gs.last_play.seat},
                          {"cards", gs.last_play.cards}, {"round_seq", gs.round_seq}});
    }
    if (gs.revealed_h2 >= 0)
        send_json(p, json{{"type", "reveal"}, {"room_id", room->id}, {"kind", "H2"},
                          {"seat", gs.revealed_h2}});
    if (gs.revealed_sk >= 0)
        send_json(p, json{{"type", "reveal"}, {"room_id", room->id}, {"kind", "SK"},
                          {"seat", gs.revealed_sk}});
    if (room->finished) {
        send_json(p, json{{"type", "game_over"}, {"room_id", room->id},
                          {"winner_team", gs.winner_team},
                          {"finish_order", gs.finished_order}, {"teams", teams_json(gs)}});
    } else {
        broadcast(room, turn_update_msg(room));
    }
}

void handle_hello(hdl_t hdl, const json& msg) {
    std::string name = msg.value("name", std::string("player"));
    if (name.size() > 32) name.resize(32);
    std::string token = msg.value("token", std::string());

    Player* cur = nullptr;
    auto cur_it = g_by_hdl.find(hdl);
    if (cur_it != g_by_hdl.end()) cur = cur_it->second;

    if (cur && (token.empty() || token == cur->token)) {
        cur->connected = true;
        send_json(cur, json{{"type", "welcome"}, {"token", cur->token}, {"name", cur->name}});
        return;
    }
    Player* p = nullptr;
    if (!token.empty()) {
        auto it = g_players.find(token);
        if (it != g_players.end()) p = it->second.get();
    }
    if (cur && cur != p) {
        destroy_player(cur);
        cur = nullptr;
    }
    if (p) {
        log_line("hello", "rebind name=" + p->name);
        try {
            g_srv->close(p->hdl, websocketpp::close::status::going_away, "reconnected elsewhere");
        } catch (...) {}
        g_by_hdl.erase(p->hdl);
        p->hdl = hdl;
        p->connected = true;
        g_by_hdl[hdl] = p;
        send_json(p, json{{"type", "welcome"}, {"token", p->token}, {"name", p->name}});
        Room* room = room_of(p);
        if (room) {
            int seat = seat_of(room, p);
            if (seat >= 0) {
                broadcast_room_update(room);
                if (room->started) send_reconnect_snapshot(room, p, seat);
                else send_json(p, json{{"type", "room_state"}, {"room_id", room->id},
                                       {"seat", seat}, {"phase", "waiting"}});
            }
        }
        return;
    }
    token = gen_token();
    auto up = std::make_unique<Player>();
    up->token = token;
    up->name = name;
    up->hdl = hdl;
    up->connected = true;
    Player* raw = up.get();
    g_players.emplace(token, std::move(up));
    g_by_hdl[hdl] = raw;
    log_line("hello", "new name=" + name);
    send_json(raw, json{{"type", "welcome"}, {"token", token}, {"name", name}});
}

void handle_create_room(Player* p) {
    if (room_of(p)) { send_error(p, "ALREADY_IN_ROOM", "leave current room first"); return; }
    const std::string id = gen_room_id();
    auto room_up = std::make_unique<Room>(id);
    Room* room = room_up.get();
    g_rooms.emplace(id, std::move(room_up));
    room->seats[0] = p;
    p->room_id = id;
    log_line("create_room", "room=" + id + " name=" + p->name);
    send_json(p, json{{"type", "room_state"}, {"room_id", id}, {"seat", 0}, {"phase", "waiting"}});
    broadcast_room_update(room);
}

void handle_join_room(Player* p, const json& msg) {
    // 兼容 room_id 数字/字符串
    std::string room_id;
    if (msg.contains("room_id") && msg["room_id"].is_number())
        room_id = std::to_string(msg["room_id"].get<long long>());
    else
        room_id = msg.value("room_id", std::string());
    auto it = g_rooms.find(room_id);
    if (it == g_rooms.end()) { send_error(p, "ROOM_NOT_FOUND", "no such room"); return; }
    Room* room = it->second.get();

    const int exist_seat = seat_of(room, p);
    if (exist_seat >= 0) {
        const char* phase = (room->started && !room->finished) ? "playing" : "waiting";
        send_json(p, json{{"type", "room_state"}, {"room_id", room->id},
                          {"seat", exist_seat}, {"phase", phase}});
        if (room->started) send_reconnect_snapshot(room, p, exist_seat);
        return;
    }
    if (room_of(p)) { send_error(p, "ALREADY_IN_ROOM", "leave current room first"); return; }
    if (room->started) { send_error(p, "GAME_STARTED", "game already started in this room"); return; }

    int seat = -1;
    for (int i = 0; i < game::kNumSeats; ++i) {
        if (!room->seats[i]) { seat = i; break; }
    }
    if (seat < 0) { send_error(p, "ROOM_FULL", "room is full"); return; }

    room->seats[seat] = p;
    p->room_id = room->id;
    p->connected = true;
    log_line("join_room", "room=" + room->id + " seat=" + std::to_string(seat) + " name=" + p->name);
    send_json(p, json{{"type", "room_state"}, {"room_id", room->id},
                      {"seat", seat}, {"phase", "waiting"}});
    broadcast_room_update(room);

    bool full = true;
    for (Player* sp : room->seats) if (!sp) { full = false; break; }
    if (full) start_game(room, "room_full");
}

void handle_leave_room(Player* p) {
    Room* room = room_of(p);
    if (!room) { send_error(p, "NOT_IN_ROOM", "not in a room"); return; }
    const int seat = seat_of(room, p);
    if (room->started && !room->finished) {
        send_json(p, json{{"type", "left_room"}, {"room_id", room->id}});
        p->connected = false;
        if (seat >= 0) {
            broadcast(room, json{{"type", "player_conn"}, {"room_id", room->id},
                                 {"seat", seat}, {"connected", false}});
        }
        return;
    }
    if (seat >= 0) room->seats[seat] = nullptr;
    p->room_id.clear();
    send_json(p, json{{"type", "left_room"}, {"room_id", room->id}});
    broadcast_room_update(room);
    maybe_destroy_room(room);
}

void handle_play(Player* p, const json& msg) {
    Room* room = room_of(p);
    if (!room) { send_error(p, "NOT_IN_ROOM", "join a room first"); return; }
    const int seat = seat_of(room, p);
    if (seat < 0) { send_error(p, "NOT_IN_ROOM", "not seated"); return; }
    if (!room->started || room->finished || room->gs.phase != game::Phase::PLAYING) {
        send_error(p, "WRONG_PHASE", "no active game in this room"); return;
    }
    game::CardAction act;
    act.seat = seat;
    act.kind = game::CardAction::PLAY;
    if (msg.contains("cards") && msg["cards"].is_array()) {
        for (const auto& e : msg["cards"]) {
            if (!e.is_number_integer()) continue;
            const long long v = e.get<long long>();
            if (v >= 0 && v < 52) act.cards.push_back(static_cast<game::u8>(v));
        }
    }
    apply_and_broadcast(room, act, p, "play");
}

void handle_pass(Player* p) {
    Room* room = room_of(p);
    if (!room) { send_error(p, "NOT_IN_ROOM", "join a room first"); return; }
    const int seat = seat_of(room, p);
    if (seat < 0) { send_error(p, "NOT_IN_ROOM", "not seated"); return; }
    if (!room->started || room->finished || room->gs.phase != game::Phase::PLAYING) {
        send_error(p, "WRONG_PHASE", "no active game in this room"); return;
    }
    game::CardAction act;
    act.seat = seat;
    act.kind = game::CardAction::PASS;
    apply_and_broadcast(room, act, p, "pass");
}

void handle_rematch(Player* p) {
    Room* room = room_of(p);
    if (!room) { send_error(p, "NOT_IN_ROOM", "join a room first"); return; }
    if (!room->started) { send_error(p, "WRONG_PHASE", "no game to rematch"); return; }
    if (!room->finished) { send_error(p, "WRONG_PHASE", "game still running"); return; }
    for (Player* sp : room->seats) {
        if (!sp) { send_error(p, "WRONG_PHASE", "waiting for all players seated"); return; }
    }
    start_game(room, "rematch");
}

void dispatch_msg(hdl_t hdl, const json& body) {
    const std::string type = body.value("type", std::string());
    Player* p = nullptr;
    auto it = g_by_hdl.find(hdl);
    if (it != g_by_hdl.end()) p = it->second;

    if (type == "hello") { handle_hello(hdl, body); return; }
    if (!p) { send_error_hdl(hdl, "BAD_TOKEN", "send hello first"); return; }

    if (type == "create_room")      handle_create_room(p);
    else if (type == "join_room")   handle_join_room(p, body);
    else if (type == "leave_room")  handle_leave_room(p);
    else if (type == "play")        handle_play(p, body);
    else if (type == "pass")        handle_pass(p);
    else if (type == "rematch")     handle_rematch(p);
    else send_error(p, "BAD_TOKEN", "unknown message type: " + type);
}

void on_message(hdl_t hdl, msg_ptr msg) {
    if (!msg || msg->get_opcode() != websocketpp::frame::opcode::text) return;
    json body = json::parse(msg->get_payload(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        send_error_hdl(hdl, "BAD_TOKEN", "invalid json payload");
        return;
    }
    try {
        dispatch_msg(hdl, body);
    } catch (const std::exception& e) {
        log_line("handler_err", std::string("what=") + e.what());
        send_error_hdl(hdl, "BAD_TOKEN", "internal error");
    } catch (...) {
        send_error_hdl(hdl, "BAD_TOKEN", "internal error");
    }
}

void on_open(hdl_t) {}
void on_close(hdl_t hdl) {
    auto it = g_by_hdl.find(hdl);
    if (it == g_by_hdl.end()) return;  // 已被新连接顶替的旧 close
    Player* p = it->second;
    g_by_hdl.erase(it);
    if (!same_owner(hdl, p->hdl)) return;  // 顶替后旧连接关闭：忽略
    p->connected = false;
    Room* room = room_of(p);
    if (room) {
        const int seat = seat_of(room, p);
        if (seat >= 0) {
            broadcast(room, json{{"type", "player_conn"}, {"room_id", room->id},
                                 {"seat", seat}, {"connected", false}});
            log_line("conn_close", "name=" + p->name + " room=" + room->id +
                                       " seat=" + std::to_string(seat) + " (seat kept)");
        }
    } else {
        g_players.erase(p->token);  // 不在房间的断线玩家回收
    }
}

}  // namespace（内部符号结束，main 需外部链接）

int main() {
    static server_t srv;
    g_srv = &srv;
    srv.set_access_channels(websocketpp::log::alevel::none);
    srv.set_error_channels(websocketpp::log::elevel::none);
    srv.init_asio();
    // io 上下文通过 get_server_io(*g_srv) 按需获取（兼容新旧 websocketpp/asio）

    srv.set_open_handler(&on_open);
    srv.set_close_handler(&on_close);
    srv.set_message_handler(&on_message);

    websocketpp::lib::error_code ec;
    srv.listen(static_cast<uint16_t>(kListenPort), ec);
    if (ec) { log_line("fatal", "listen: " + ec.message()); return 1; }
    srv.start_accept(ec);
    if (ec) { log_line("fatal", "accept: " + ec.message()); return 1; }

    log_line("boot", "SimpleCardGame v2 server on 9002 (single thread)");
    srv.run();
    return 0;
}
