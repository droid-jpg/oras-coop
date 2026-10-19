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
#include "Session.hpp"
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace coop {

Session g_session;

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
            g_session.lastChat = std::string(r.name) + " joined";
            g_session.lastChatTime = osGetTime();
            g_session.greetChanged = true; // the server keeps no state: the newcomer needs ours
        }
        break;
    case T_LEAVE:
        if (id >= 1 && id <= MAX_ID) {
            Remote& r = g_session.remotes[id];
            if (r.active) {
                g_session.lastChat = std::string(r.name) + " left";
                g_session.lastChatTime = osGetTime();
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
            g_session.lastChat = std::string(g_session.remotes[id].name) + ": " + text;
            g_session.lastChatTime = osGetTime();
        }
        break;
    case T_GREET:
        if (id >= 1 && id <= MAX_ID) f.getStr(4, 40, g_session.remotes[id].greeting);
        break;
    case T_TALK:
        if (id >= 1 && id <= MAX_ID && f.get<u8>(4) == g_session.myId) {
            g_session.lastChat = std::string(g_session.remotes[id].name) + " is talking to you!";
            g_session.lastChatTime = osGetTime();
        }
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

    while (g_session.running) {
        std::string chat, greeting;
        game::PlayerState s;
        u8 talkTo = 0;
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
