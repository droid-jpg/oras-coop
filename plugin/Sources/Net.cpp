// Network thread: sends our state to the relay server and applies frames from others.
#include <3ds.h>
#include <3ds/services/soc.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Session.hpp"
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace coop {

Session g_session;

void PushNotice(const std::string& text, u8 from) {
    if (g_session.noticeCount == MAX_NOTICES) { // full: drop the oldest
        for (int i = 1; i < MAX_NOTICES; i++) g_session.notices[i - 1] = g_session.notices[i];
        g_session.noticeCount--;
    }
    Notice& n = g_session.notices[g_session.noticeCount++];
    n = Notice();
    snprintf(n.text, sizeof n.text, "%s", text.c_str());
    n.from = from;
}

u32 TradeHash(const Trade& t, u8 myId) {
    const u8* first = myId < t.partner ? t.mine : t.theirs;
    const u8* second = myId < t.partner ? t.theirs : t.mine;
    u32 h = 2166136261u; // FNV-1a
    for (int i = 0; i < game::PK_SIZE; i++) h = (h ^ first[i]) * 16777619u;
    for (int i = 0; i < game::PK_SIZE; i++) h = (h ^ second[i]) * 16777619u;
    return h;
}

namespace {

constexpr u32 SOC_SIZE = 0x20000;
constexpr u64 BUSY_AFTER_MS = 2000;      // script control longer than this = busy
constexpr u64 STATE_INTERVAL_MS = 66;   // ~15/s while moving
constexpr u64 IDLE_INTERVAL_MS = 1000;  // keep-alive while idle
constexpr u64 LOOP_SLEEP_NS = 33ULL * 1000 * 1000;
constexpr u64 RETRY_DELAY_MS = 3000;

struct Guard {
    Mutex& m;
    explicit Guard(Mutex& mm) : m(mm) { m.Lock(); }
    ~Guard() { m.Unlock(); }
};

void SetStatus(NetStatus s, const std::string& text) {
    Guard g(g_session.lock);
    g_session.status = s;
    g_session.statusText = text;
}

void SleepMs(u64 ms) { svcSleepThread((s64)ms * 1000 * 1000); }

bool SocStart() {
    // Azahar/Citra emulate SOC:U and accept a null buffer; real hardware needs memory.
    if (R_SUCCEEDED(socInit((u32*)0, SOC_SIZE))) return true;
    u32* buf = (u32*)memalign(0x1000, SOC_SIZE);
    if (buf && R_SUCCEEDED(socInit(buf, SOC_SIZE))) return true;
    free(buf);
    return false;
}

class Conn {
public:
    ~Conn() { Close(); }

    bool Open(const std::string& host, u16 port, std::string& err) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { err = "socket() failed"; return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(host.c_str());
        if (connect(fd_, (sockaddr*)&addr, sizeof addr) < 0) {
            err = "can't reach " + host + ":" + std::to_string(port);
            Close();
            return false;
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        rxLen_ = 0;
        return true;
    }

    void Close() {
        if (fd_ >= 0) closesocket(fd_);
        fd_ = -1;
    }

    bool Send(const Frame& f) {
        int sent = 0;
        int spins = 0;
        while (sent < FRAME) {
            int r = send(fd_, f.b + sent, FRAME - sent, 0);
            if (r > 0) { sent += r; continue; }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++spins < 50) { SleepMs(2); continue; }
            return false;
        }
        return true;
    }

    // Returns 1 with a full frame, 0 if none yet, -1 if the connection dropped.
    int Poll(Frame& out) {
        if (rxLen_ < FRAME) {
            int r = recv(fd_, rx_ + rxLen_, sizeof rx_ - rxLen_, 0);
            if (r == 0) return -1;
            if (r < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            } else {
                rxLen_ += r;
            }
        }
        if (rxLen_ < FRAME) return 0;
        std::memcpy(out.b, rx_, FRAME);
        rxLen_ -= FRAME;
        std::memmove(rx_, rx_ + FRAME, rxLen_);
        return 1;
    }

private:
    int fd_ = -1;
    u8 rx_[FRAME * 16];
    int rxLen_ = 0;
};

void ClearRemotes() {
    Guard g(g_session.lock);
    for (auto& r : g_session.remotes) r = Remote();
    g_session.myId = 0;
    g_session.trade = Trade(); // a trade never survives a reconnect
}

bool SameBattle(const game::BattleState& a, const game::BattleState& b) {
    if (a.active != b.active) return false;
    for (int s = 0; s < 2; s++) {
        if (a.count[s] != b.count[s]) return false;
        for (int i = 0; i < a.count[s]; i++) {
            const game::BattleMon &x = a.mon[s][i], &y = b.mon[s][i];
            if (x.species != y.species || x.hp != y.hp || x.maxHp != y.maxHp || x.level != y.level) return false;
        }
    }
    return true;
}

// Notice text for a traded Pokemon: "\x01" + species + level, resolved to "Wingull Lv20" by the
// overlay (the game thread; this thread never reads game memory).
std::string Describe(const u8* pk) {
    u16 species = 0;
    u8 level = 0;
    if (!game::DescribePokemon(pk, species, level)) return "a Pokemon";
    // 7 bits per byte with the top bit set, so no byte is 0.
    char buf[8] = { 1, (char)(0x80 | (species & 0x7F)), (char)(0x80 | species >> 7), (char)level, 0 };
    return buf;
}

// "\x02" + species: resolved to the species name by the overlay, like Describe.
std::string SpeciesTag(u16 species) {
    char buf[4] = { 2, (char)(0x80 | (species & 0x7F)), (char)(0x80 | species >> 7), 0 };
    return buf;
}

// Hoenn's gym badges in the order the bits are set.
const char* const BADGE_NAMES[8] = { "Stone", "Knuckle", "Dynamo", "Heat", "Balance", "Feather", "Mind", "Rain" };

// A friend's catch, evolution or badge (lock held).
void ApplyEvent(const Frame& f, u8 id) {
    std::string name = g_session.remotes[id].name;
    u8 kind = f.get<u8>(4), value = f.get<u8>(5);
    u16 species = f.get<u16>(6), from = f.get<u16>(8);
    if ((kind == EVENT_CAUGHT || kind == EVENT_EVOLVED) && (!species || species > 721)) return;
    switch (kind) {
    case EVENT_CAUGHT:
        PushNotice(name + " caught " + SpeciesTag(species) + "!", id);
        break;
    case EVENT_EVOLVED:
        if (!from || from > 721) return;
        PushNotice(name + "'s " + SpeciesTag(from) + " evolved into " + SpeciesTag(species) + "!", id);
        break;
    case EVENT_BADGE:
        if (value >= 1 && value <= 8) PushNotice(name + " earned the " + BADGE_NAMES[value - 1] + " Badge!", id);
        break;
    default:
        break;
    }
}

// A party request or a piece of a friend's party, addressed to us (lock held).
void ApplyParty(const Frame& f, u8 id) {
    Remote& r = g_session.remotes[id];
    u8 op = f.get<u8>(4);
    if (op == PARTY_REQUEST) { g_session.partyWanted |= (u16)(1 << id); return; }
    if (op != PARTY_DATA) return;
    u8 piece = f.get<u8>(6), count = f.get<u8>(7);
    if (count > 6) return;
    if (count != r.partyCount) { r.partyCount = count; r.partyPieces = 0; r.partyComplete = false; }
    int slot = piece / PARTY_CHUNKS, chunk = piece % PARTY_CHUNKS;
    if (count && slot < count) {
        int off = chunk * PARTY_CHUNK;
        int len = game::PK_SIZE - off < PARTY_CHUNK ? game::PK_SIZE - off : PARTY_CHUNK;
        if (len > 0) std::memcpy(r.party[slot] + off, f.b + 8, len);
        r.partyPieces |= 1ULL << piece;
    }
    u64 all = count ? (1ULL << (count * PARTY_CHUNKS)) - 1 : 0;
    r.partyComplete = (r.partyPieces & all) == all;
}

// A trade step from player `id` addressed to us (lock held).
void ApplyTrade(const Frame& f, u8 id) {
    Trade& t = g_session.trade;
    std::string name = g_session.remotes[id].name;
    u8 op = f.get<u8>(4);
    if (op == TRADE_CANCEL) {
        if (t.partner == id) {
            t = Trade();
            PushNotice(name + " cancelled the trade.", id);
        }
        return;
    }
    if (t.partner && t.partner != id) return; // busy with someone else
    if (op == TRADE_OFFER) {
        constexpr int CHUNKS = (game::PK_SIZE + TRADE_CHUNK - 1) / TRADE_CHUNK;
        u8 chunk = f.get<u8>(6);
        if (chunk >= CHUNKS || f.get<u8>(7) != CHUNKS) return;
        if (!t.partner) { t = Trade(); t.partner = id; }
        if (chunk == 0) { t.theirChunks = 0; t.theirComplete = false; t.iAccepted = t.theyAccepted = false; }
        int off = chunk * TRADE_CHUNK, len = game::PK_SIZE - off < TRADE_CHUNK ? game::PK_SIZE - off : TRADE_CHUNK;
        std::memcpy(t.theirs + off, f.b + 8, len);
        t.theirChunks |= 1u << chunk;
        if (t.theirChunks == (1u << CHUNKS) - 1 && !t.theirComplete) {
            t.theirComplete = true;
            if (t.mySlot >= 0) // they answered our offer
                PushNotice(name + " offers " + Describe(t.theirs) + " for " + Describe(t.mine) + "! Talk to " + name + ".", id);
            else
                PushNotice(name + " wants to trade " + Describe(t.theirs) + "! Talk to " + name + ".", id);
        }
    } else if (op == TRADE_ACCEPT && t.partner == id) {
        t.theyAccepted = true;
        t.theirHash = f.get<u32>(8);
    } else if (op == TRADE_COMMIT && t.partner == id) {
        // The server saw both accepts for the same pair of Pokemon and told both of us at once.
        if (t.iAccepted && f.get<u32>(8) == TradeHash(t, g_session.myId)) t.pendingWrite = true;
    }
}

void Apply(const Frame& f) {
    u8 id = f.player();
    Guard g(g_session.lock);
    switch (f.type()) {
    case T_WELCOME:
        g_session.myId = id;
        g_session.status = NetStatus::Connected;
        g_session.statusText = "Connected as #" + std::to_string(id);
        g_session.greetChanged = true; // tell everyone our greeting
        break;
    case T_JOIN:
        if (id >= 1 && id <= MAX_ID && id != g_session.myId) {
            Remote& r = g_session.remotes[id];
            r = Remote();
            r.active = true;
            f.getStr(4, 16, r.name);
            PushNotice(std::string(r.name) + " joined!", id);
            g_session.greetChanged = true; // the server keeps no state: the newcomer needs ours
            g_session.partyAsk |= (u16)(1 << id); // so "Battle" is there the first time we talk
        }
        break;
    case T_LEAVE:
        if (id >= 1 && id <= MAX_ID) {
            Remote& r = g_session.remotes[id];
            if (r.active) PushNotice(std::string(r.name) + " left.", id);
            if (g_session.trade.partner == id) {
                g_session.trade = Trade();
                PushNotice("The trade was cancelled.");
            }
            r = Remote();
        }
        break;
    case T_STATE:
        if (id >= 1 && id <= MAX_ID) {
            Remote& r = g_session.remotes[id];
            r.active = true;
            bool mapChanged = !r.hasState || r.map != f.get<u16>(4);
            r.map = f.get<u16>(4);
            r.facing = f.get<u16>(6);
            r.tx = f.get<float>(8);
            r.tz = f.get<float>(12);
            r.ty = f.get<float>(16);
            r.dirX = f.get<float>(28);
            r.dirY = f.get<float>(32);
            if (!std::isfinite(r.dirX) || !std::isfinite(r.dirY)) r.dirX = r.dirY = 0;
            r.anim = f.get<u8>(36);
            r.model = f.get<u16>(38);
            u8 st = f.get<u8>(37);
            r.away = st & ST_AWAY;
            r.riding = st & ST_RIDING;
            r.surfing = st & ST_SURFING;
            if (!r.away) r.battle = game::BattleState(); // back on the field
            u64 now = osGetTime();
            if (mapChanged) {
                r.dx = r.tx; r.dz = r.tz; r.dy = r.ty;
                r.histLen = 0; // don't interpolate across maps
            }
            // Interpolation history, stamped with the sender's clock (old clients send 0:
            // use our receive time instead).
            Sample smp;
            smp.t = f.get<u32>(40);
            if (!smp.t) smp.t = (u32)now;
            smp.x = r.tx; smp.z = r.tz; smp.y = r.ty;
            smp.dirX = r.dirX; smp.dirY = r.dirY;
            bool inOrder = r.histLen == 0 || (s32)(smp.t - r.hist[r.histLen - 1].t) > 0;
            if (inOrder) {
                if (r.histLen == Remote::HISTORY) {
                    for (int i = 1; i < Remote::HISTORY; i++) r.hist[i - 1] = r.hist[i];
                    r.histLen--;
                }
                r.hist[r.histLen++] = smp;
                // Smallest (local - sender) seen is the least-delayed packet; let it creep up
                // 1 ms per packet so clock drift and route changes are absorbed.
                s64 off = (s64)(u32)now - (s64)smp.t;
                if (!r.haveOffset || off < r.clockOffset) r.clockOffset = off;
                else r.clockOffset += 1;
                r.haveOffset = true;
            }
            r.hasState = true;
            r.lastUpdate = now;
        }
        break;
    case T_CHAT:
        if (id >= 1 && id <= MAX_ID) {
            char text[40];
            f.getStr(4, 40, text);
            PushNotice(std::string(g_session.remotes[id].name) + ": " + text, id);
        }
        break;
    case T_GREET:
        if (id >= 1 && id <= MAX_ID) f.getStr(4, 40, g_session.remotes[id].greeting);
        break;
    case T_TALK:
        if (id >= 1 && id <= MAX_ID && f.get<u8>(4) == g_session.myId) {
            PushNotice(std::string(g_session.remotes[id].name) + " is talking to you!", id);
        }
        break;
    case T_BATTLE:
        if (id >= 1 && id <= MAX_ID) {
            Remote& r = g_session.remotes[id];
            u8 side = f.get<u8>(4), count = f.get<u8>(5);
            if (side > 1 || count > game::BATTLE_PARTY) break;
            if (count == 0) { r.battle = game::BattleState(); break; }
            r.battle.count[side] = count;
            for (int i = 0; i < count; i++) {
                game::BattleMon& m = r.battle.mon[side][i];
                int off = 6 + i * BATTLE_ENTRY;
                m.species = f.get<u16>(off);
                m.hp = f.get<u16>(off + 2);
                m.maxHp = f.get<u16>(off + 4);
                m.level = f.get<u8>(off + 6);
            }
            r.battle.active = r.battle.count[0] && r.battle.count[1];
            r.battleTime = osGetTime();
        }
        break;
    case T_TRADE:
        if (id >= 1 && id <= MAX_ID && f.get<u8>(5) == g_session.myId) ApplyTrade(f, id);
        break;
    case T_PARTY:
        if (id >= 1 && id <= MAX_ID && f.get<u8>(5) == g_session.myId) ApplyParty(f, id);
        break;
    case T_EVENT:
        if (id >= 1 && id <= MAX_ID && g_session.remotes[id].active) ApplyEvent(f, id);
        break;
    case T_ERROR: {
        u8 code = f.get<u8>(4);
        g_session.status = NetStatus::Failed;
        g_session.statusText = code == 1 ? "Room is full" : code == 3 ? "Server version mismatch" : "Server rejected hello";
        break;
    }
    default:
        break;
    }
}

Frame MakeState(const game::PlayerState& s, u32 seq, bool away = false) {
    Frame f(T_STATE);
    f.put<u16>(4, s.map);
    f.put<u16>(6, s.facing);
    f.put<float>(8, s.x);
    f.put<float>(12, s.z);
    f.put<float>(16, s.y);
    f.put<u32>(20, s.aux);
    f.put<u32>(24, seq);
    f.put<float>(28, s.dirX);
    f.put<float>(32, s.dirY);
    f.put<u8>(36, s.anim);
    f.put<u16>(38, s.model);
    f.put<u8>(37, (u8)((away ? ST_AWAY : 0) | (s.riding ? ST_RIDING : 0) | (s.surfing ? ST_SURFING : 0)));
    f.put<u32>(40, (u32)osGetTime()); // sender clock (ms) for receiver interpolation
    return f;
}

bool Changed(const game::PlayerState& a, const game::PlayerState& b) {
    return a.map != b.map || a.facing != b.facing || a.anim != b.anim || a.riding != b.riding ||
           a.surfing != b.surfing ||
           std::fabs(a.dirX - b.dirX) > 0.02f || std::fabs(a.dirY - b.dirY) > 0.02f ||
           std::fabs(a.x - b.x) > 0.05f || std::fabs(a.y - b.y) > 0.05f || std::fabs(a.z - b.z) > 0.05f;
}

void RunSession(const Config& cfg) {
    Conn conn;
    std::string err;
    SetStatus(NetStatus::Connecting, "Connecting to " + cfg.server + "...");
    if (!conn.Open(cfg.server, cfg.port, err)) {
        SetStatus(NetStatus::Failed, err);
        return;
    }

    Frame hello(T_HELLO);
    hello.putStr(4, 12, cfg.room.c_str());
    hello.putStr(16, 16, cfg.name.c_str());
    if (!conn.Send(hello)) { SetStatus(NetStatus::Failed, "Send failed"); return; }

    game::PlayerState last; // last state sent from the live field
    bool wasBusy = false;
    u64 suspendedSince = 0;
    u64 lastSend = 0;
    u32 seq = 0;
    game::BattleState lastBattle;
    u64 lastBattleSend = 0;
    // Our party on its way to the friends who asked for it, a few pieces per loop (server: 60 frames/s).
    static u8 outParty[6][game::PK_SIZE];
    int outCount = 0, outPiece = -1;
    u16 outTo = 0;

    while (g_session.running) {
        std::string chat, greeting;
        game::PlayerState s;
        u8 talkTo = 0;
        game::BattleState battle;
        Trade trade;
        bool tradeOffer = false, tradeAccept = false, tradeCancel = false;
        u32 tradeHash = 0;
        Event events[MAX_EVENTS];
        int eventCount = 0;
        u16 partyAsk = 0;
        {
            Guard g(g_session.lock);
            if (g_session.reconnect) break; // NetThread clears it and reconnects immediately
            chat.swap(g_session.pendingChat);
            s = g_session.local; // published by the game thread each frame
            if (g_session.greetChanged && g_session.myId) {
                greeting = g_session.cfg.greeting.empty() ? " " : g_session.cfg.greeting;
                g_session.greetChanged = false;
            }
            talkTo = g_session.pendingTalk;
            g_session.pendingTalk = 0;
            battle = g_session.localBattle;
            Trade& t = g_session.trade;
            tradeOffer = t.sendOffer; tradeAccept = t.sendAccept; tradeCancel = t.sendCancel;
            t.sendOffer = t.sendAccept = t.sendCancel = false;
            if (tradeOffer || tradeAccept || tradeCancel) trade = t;
            if (tradeAccept) tradeHash = TradeHash(t, g_session.myId);
            if (tradeCancel) t = Trade();
            partyAsk = g_session.partyAsk;
            g_session.partyAsk = 0;
            if (outPiece < 0 && g_session.partyWanted) {
                outTo = g_session.partyWanted;
                g_session.partyWanted = 0;
                outCount = g_session.localPartyCount;
                std::memcpy(outParty, g_session.localParty, sizeof outParty);
                outPiece = 0;
            }
            eventCount = g_session.eventCount;
            for (int i = 0; i < eventCount; i++) events[i] = g_session.events[i];
            g_session.eventCount = 0;
        }

        u64 now = osGetTime();
        u64 since = now - lastSend;
        bool live = s.valid && s.inField;
        // Busy: under script control for a while (battle, soaring, long cutscene) or off the field.
        // Short script moments (talking to an NPC) don't count, so friends don't flicker.
        if (live && s.suspended) { if (!suspendedSince) suspendedSince = now; }
        else suspendedSince = 0;
        bool busy = live ? suspendedSince && now - suspendedSince >= BUSY_AFTER_MS : last.valid;
        if (live || last.valid) {
            const game::PlayerState& out = live ? s : last; // off the field: our last field spot
            if (busy != wasBusy || (Changed(out, last) && since >= STATE_INTERVAL_MS) || since >= IDLE_INTERVAL_MS) {
                if (!conn.Send(MakeState(out, ++seq, busy))) break;
                if (live) last = s;
                lastSend = now;
                wasBusy = busy;
            }
        } else if (since >= IDLE_INTERVAL_MS) {
            if (!conn.Send(Frame(T_PING))) break;
            lastSend = now;
        }

        if (!chat.empty()) {
            Frame c(T_CHAT);
            c.putStr(4, 40, chat.c_str());
            if (!conn.Send(c)) break;
        }

        if (!greeting.empty()) {
            Frame gr(T_GREET);
            gr.putStr(4, 40, greeting.c_str());
            if (!conn.Send(gr)) break;
        }
        if (talkTo) {
            Frame t(T_TALK);
            t.put<u8>(4, talkTo);
            if (!conn.Send(t)) break;
        }
        bool sent = true;
        for (int i = 0; i < eventCount && sent; i++) {
            Frame e(T_EVENT);
            e.put<u8>(4, events[i].kind);
            e.put<u8>(5, events[i].value);
            e.put<u16>(6, events[i].species);
            e.put<u16>(8, events[i].from);
            sent = conn.Send(e);
        }
        if (!sent) break;
        for (int id = 1; id <= MAX_ID && sent; id++) {
            if (!(partyAsk & (1 << id))) continue;
            Frame q(T_PARTY);
            q.put<u8>(4, PARTY_REQUEST);
            q.put<u8>(5, (u8)id);
            sent = conn.Send(q);
        }
        // 8 pieces per loop (~30 loops/s) to each friend who asked; a party is at most 42 pieces.
        int total = outCount * PARTY_CHUNKS;
        for (int k = 0; k < 8 && outPiece >= 0 && sent; k++) {
            for (int id = 1; id <= MAX_ID && sent; id++) {
                if (!(outTo & (1 << id))) continue;
                Frame p(T_PARTY);
                p.put<u8>(4, PARTY_DATA);
                p.put<u8>(5, (u8)id);
                p.put<u8>(6, (u8)(total ? outPiece : 0));
                p.put<u8>(7, (u8)outCount);
                if (total) {
                    int slot = outPiece / PARTY_CHUNKS, off = (outPiece % PARTY_CHUNKS) * PARTY_CHUNK;
                    int len = game::PK_SIZE - off < PARTY_CHUNK ? game::PK_SIZE - off : PARTY_CHUNK;
                    std::memcpy(p.b + 8, outParty[slot] + off, len);
                }
                sent = conn.Send(p);
            }
            if (++outPiece >= (total ? total : 1)) outPiece = -1;
        }
        if (!sent) break;

        // Our battle, for friends who walk up to our trainer: on every change, else every 2 s.
        if (battle.active && (!SameBattle(battle, lastBattle) || now - lastBattleSend >= 2000)) {
            bool ok = true;
            for (u8 side = 0; side < 2 && ok; side++) {
                Frame b(T_BATTLE);
                b.put<u8>(4, side);
                b.put<u8>(5, battle.count[side]);
                for (int i = 0; i < battle.count[side]; i++) {
                    const game::BattleMon& m = battle.mon[side][i];
                    int off = 6 + i * BATTLE_ENTRY;
                    b.put<u16>(off, m.species);
                    b.put<u16>(off + 2, m.hp);
                    b.put<u16>(off + 4, m.maxHp);
                    b.put<u8>(off + 6, m.level);
                }
                ok = conn.Send(b);
            }
            if (!ok) break;
            lastBattle = battle;
            lastBattleSend = now;
        } else if (!battle.active && lastBattle.active) {
            if (!conn.Send(Frame(T_BATTLE))) break; // count 0: over
            lastBattle = game::BattleState();
        }

        if (tradeOffer) {
            constexpr int CHUNKS = (game::PK_SIZE + TRADE_CHUNK - 1) / TRADE_CHUNK;
            bool ok = true;
            for (int c = 0; c < CHUNKS && ok; c++) {
                Frame t(T_TRADE);
                t.put<u8>(4, TRADE_OFFER);
                t.put<u8>(5, trade.partner);
                t.put<u8>(6, (u8)c);
                t.put<u8>(7, (u8)CHUNKS);
                int off = c * TRADE_CHUNK, len = game::PK_SIZE - off < TRADE_CHUNK ? game::PK_SIZE - off : TRADE_CHUNK;
                std::memcpy(t.b + 8, trade.mine + off, len);
                ok = conn.Send(t);
            }
            if (!ok) break;
        }
        if (tradeAccept || tradeCancel) {
            Frame t(T_TRADE);
            t.put<u8>(4, tradeCancel ? TRADE_CANCEL : TRADE_ACCEPT);
            t.put<u8>(5, trade.partner);
            t.put<u32>(8, tradeHash);
            if (!conn.Send(t)) break;
        }

        Frame in;
        int r;
        while ((r = conn.Poll(in)) == 1) Apply(in);
        if (r < 0) break;

        svcSleepThread(LOOP_SLEEP_NS);
    }
    SetStatus(NetStatus::Failed, "Disconnected");
}

} // namespace

void NetThread(void*) {
    if (!SocStart()) {
        SetStatus(NetStatus::Failed, "Network init failed");
        return;
    }
    while (g_session.running) {
        Config cfg;
        {
            Guard g(g_session.lock);
            cfg = g_session.cfg;
            g_session.reconnect = false;
        }
        RunSession(cfg);
        ClearRemotes();
        for (u64 waited = 0; waited < RETRY_DELAY_MS && g_session.running; waited += 100) {
            {
                Guard g(g_session.lock);
                if (g_session.reconnect) break;
            }
            SleepMs(100);
        }
    }
    socExit();
}

} // namespace coop
