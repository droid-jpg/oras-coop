// On-screen display: markers for nearby players (top) and a radar + player list (bottom).
#include <3ds.h>
#include <cmath>
#include <cstdio>
#include "Session.hpp"
#include "Game.hpp"
#include "Puppet.hpp"

using namespace CTRPluginFramework;

namespace coop {

namespace {

constexpr int TOP_W = 400, TOP_H = 240, BOT_W = 320, BOT_H = 240;
constexpr int PLAYER_SX = 200, PLAYER_SY = 148; // fallback: where our character's feet sit
constexpr float HEAD_HEIGHT = 30.0f;            // world units from feet to just above the head
constexpr float TILE = 18.0f;
constexpr float RADAR_RANGE = 20.0f * TILE;     // world units from centre to radar edge
constexpr u64 STALE_MS = 5000;
constexpr u64 CHAT_SHOW_MS = 6000;

const Color kBg(8, 14, 24, 170);
const Color kAccent(0, 210, 255, 220);
const Color kOk(0, 230, 120, 230);
const Color kBad(230, 60, 60, 230);
const Color kClear(0, 0, 0, 0);

const Color kPlayerColors[MAX_ID + 1] = {
    Color(255, 255, 255), Color(255, 90, 90), Color(90, 170, 255), Color(255, 210, 60), Color(120, 230, 120),
    Color(220, 120, 255), Color(255, 150, 60), Color(90, 230, 220), Color(255, 130, 200),
};

// Clipped rectangle so nothing is ever drawn outside the framebuffer.
void Rect(const Screen& s, int x, int y, int w, int h, const Color& c, bool fill = true) {
    int maxW = s.IsTop ? TOP_W : BOT_W, maxH = s.IsTop ? TOP_H : BOT_H;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > maxW) w = maxW - x;
    if (y + h > maxH) h = maxH - y;
    if (w <= 0 || h <= 0) return;
    s.DrawRect(x, y, w, h, c, fill);
}

void Text(const Screen& s, const std::string& t, int x, int y, const Color& c) {
    int maxW = s.IsTop ? TOP_W : BOT_W, maxH = s.IsTop ? TOP_H : BOT_H;
    if (x < 0 || y < 0 || y + 10 > maxH || x >= maxW) return;
    s.Draw(t, x, y, c, kClear);
}

int TextWidth(const std::string& t) { return (int)t.size() * 6; }

float Clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

struct View {
    game::PlayerState me;
    Remote remotes[MAX_ID + 1];
    u8 myId;
    NetStatus status;
    std::string statusText, chat, room, name;
    u64 chatTime;
    float scale;
    bool overlay;
    bool puppets;
};

// Remote players are shown INTERP_DELAY_MS in the past, between the two received states
// around that moment, so movement keeps a constant speed despite 15 Hz updates and jitter.
// Must exceed the sender's update interval (66 ms) plus typical jitter.
constexpr u32 INTERP_DELAY_MS = 120;

void Interpolate(Remote& r, u64 now, float& dirX, float& dirY) {
    if (r.histLen == 0 || !r.haveOffset) {
        r.dx = r.tx; r.dz = r.tz; r.dy = r.ty;
        return;
    }
    u32 t = (u32)((s64)(u32)now - r.clockOffset) - INTERP_DELAY_MS; // in sender ms
    const Sample* a = &r.hist[0];
    const Sample* b = nullptr;
    for (int k = 1; k < r.histLen; k++) {
        if ((s32)(r.hist[k].t - t) >= 0) { b = &r.hist[k]; break; }
        a = &r.hist[k];
    }
    if (!b || (s32)(t - a->t) < 0) {
        // Past the newest state (late packet or standing still) or before the oldest: hold.
        // No extrapolation — overshooting and snapping back looks worse than a short pause.
        r.dx = a->x; r.dz = a->z; r.dy = a->y;
        dirX = a->dirX; dirY = a->dirY;
        return;
    }
    float span = (float)(s32)(b->t - a->t);
    float u = span > 0 ? (float)(s32)(t - a->t) / span : 1.0f;
    if (std::fabs(b->x - a->x) > 10 * TILE || std::fabs(b->y - a->y) > 10 * TILE) u = 1.0f; // warp
    r.dx = a->x + (b->x - a->x) * u;
    r.dz = a->z + (b->z - a->z) * u;
    r.dy = a->y + (b->y - a->y) * u;
    // Facing: blend the vectors (renormalised by Puppet); fall back if one is unknown.
    if ((a->dirX || a->dirY) && (b->dirX || b->dirY)) {
        dirX = a->dirX + (b->dirX - a->dirX) * u;
        dirY = a->dirY + (b->dirY - a->dirY) * u;
        if (dirX * dirX + dirY * dirY < 1e-4f) { dirX = b->dirX; dirY = b->dirY; } // 180° turn
    } else {
        dirX = b->dirX; dirY = b->dirY;
    }
}

// Copies shared state and advances smoothing; done once per top-screen frame.
View Snapshot() {
    View v;
    game::Locate(); // cheap when found; otherwise scans a small slice of the heap
    v.me = game::Read();
    g_session.lock.Lock();
    g_session.local = v.me;
    u64 now = osGetTime();
    for (int i = 0; i <= MAX_ID; i++) {
        Remote& r = g_session.remotes[i];
        float dirX = r.dirX, dirY = r.dirY;
        if (r.hasState) Interpolate(r, now, dirX, dirY);
        v.remotes[i] = r;
        v.remotes[i].dirX = dirX; // display facing; the shared copy keeps the latest received
        v.remotes[i].dirY = dirY;
    }
    v.myId = g_session.myId;
    v.status = g_session.status;
    v.statusText = g_session.statusText;
    v.chat = g_session.lastChat;
    v.chatTime = g_session.lastChatTime;
    v.room = g_session.cfg.room;
    v.name = g_session.cfg.name;
    v.scale = g_session.cfg.pxPerUnit;
    v.overlay = g_session.cfg.overlay;
    v.puppets = g_session.cfg.puppets;
    g_session.lock.Unlock();
    return v;
}

View g_view;

bool Visible(const Remote& r, const game::PlayerState& me) {
    return r.active && r.hasState && me.valid && me.inField && r.map == me.map && osGetTime() - r.lastUpdate < STALE_MS;
}

void DrawStatus(const Screen& s, const View& v) {
    bool ok = v.status == NetStatus::Connected;
    std::string line = ok ? ("oras-coop  " + v.room + "  #" + std::to_string(v.myId)) : v.statusText;
    int w = TextWidth(line) + 10;
    Rect(s, 2, 2, w, 13, kBg);
    Rect(s, 2, 2, 3, 13, ok ? kOk : kBad);
    Text(s, line, 8, 3, ok ? kOk : kBad);
}

void DrawMarker(const Screen& s, int id, const Remote& r, const View& v) {
    // Feet and head through the game's camera; the flat scale is only a fallback.
    float fx, fy, hx, hy;
    int sx, sy, labelY;
    if (game::ProjectToTop(r.dx, r.dz, r.dy, fx, fy) && game::ProjectToTop(r.dx, r.dz + HEAD_HEIGHT, r.dy, hx, hy)) {
        sx = (int)(fx + 0.5f);
        sy = (int)(fy + 0.5f);
        labelY = (int)(hy + 0.5f) - 14;
    } else {
        sx = PLAYER_SX + (int)((r.dx - v.me.x) * v.scale);
        sy = PLAYER_SY + (int)((r.dy - v.me.y) * v.scale);
        labelY = sy - 36;
    }
    const Color& col = kPlayerColors[id];
    std::string label = r.name;

    bool onScreen = sx >= 8 && sx < TOP_W - 8 && sy >= 20 && sy < TOP_H - 8;
    if (onScreen) {
        if (!IsPuppeted(id)) {
            // Ground shadow + body marker at the player's feet (a real NPC stands in otherwise).
            Rect(s, sx - 6, sy - 1, 12, 3, Color(0, 0, 0, 110));
            Rect(s, sx - 4, sy - 22, 8, 21, col);
            Rect(s, sx - 4, sy - 22, 8, 21, Color::Black, false);
        }
        int lw = TextWidth(label) + 6;
        Rect(s, sx - lw / 2, labelY, lw, 12, kBg);
        Text(s, label, sx - lw / 2 + 3, labelY + 1, col);
    } else {
        // Edge arrow pointing toward an off-screen player, with distance in tiles.
        int ex = (int)Clampf((float)sx, 6, TOP_W - 7);
        int ey = (int)Clampf((float)sy, 20, TOP_H - 7);
        float tiles = std::sqrt((r.dx - v.me.x) * (r.dx - v.me.x) + (r.dy - v.me.y) * (r.dy - v.me.y)) / TILE;
        char buf[32];
        snprintf(buf, sizeof buf, "%s %dt", r.name, (int)(tiles + 0.5f));
        std::string t = buf;
        int lw = TextWidth(t) + 6;
        int lx = (int)Clampf((float)(ex - lw / 2), 2, (float)(TOP_W - lw - 2));
        int ly = ey > TOP_H / 2 ? ey - 14 : ey + 6;
        Rect(s, ex - 3, ey - 3, 7, 7, col);
        Rect(s, lx, ly, lw, 12, kBg);
        Text(s, t, lx + 3, ly + 1, col);
    }
}

void DrawTop(const Screen& s) {
    g_view = Snapshot();
    const View& v = g_view;
    UpdatePuppets(v.puppets, v.me, v.remotes, v.myId);
    DrawStatus(s, v);
    if (!v.overlay) return;
    for (int id = 1; id <= MAX_ID; id++) {
        if (id != v.myId && Visible(v.remotes[id], v.me)) DrawMarker(s, id, v.remotes[id], v);
    }
    if (!v.chat.empty() && osGetTime() - v.chatTime < CHAT_SHOW_MS) {
        int w = TextWidth(v.chat) + 8;
        Rect(s, 4, TOP_H - 16, w, 13, kBg);
        Text(s, v.chat, 8, TOP_H - 15, Color(255, 230, 90));
    }
}

void DrawBottom(const Screen& s) {
    const View& v = g_view; // filled by the top-screen pass this frame
    if (!v.overlay) return;

    // Radar: 90x90 box in the top-right corner, north up, our player in the centre.
    constexpr int R = 45, CX = BOT_W - R - 4, CY = R + 4;
    Rect(s, CX - R, CY - R, 2 * R, 2 * R, kBg);
    Rect(s, CX - R, CY - R, 2 * R, 2 * R, kAccent, false);
    Rect(s, CX - 1, CY - 1, 3, 3, Color::White);

    // Player list on the left.
    int rows = 0;
    Rect(s, 4, 4, 150, 12, kBg);
    Text(s, "Players", 8, 5, kAccent);
    for (int id = 1; id <= MAX_ID; id++) {
        const Remote& r = v.remotes[id];
        if (!r.active || id == v.myId) continue;
        bool same = Visible(r, v.me);
        char buf[48];
        if (!r.hasState) snprintf(buf, sizeof buf, "%s  (loading)", r.name);
        else if (same) snprintf(buf, sizeof buf, "%s  here", r.name);
        else snprintf(buf, sizeof buf, "%s  map %u", r.name, (unsigned)r.map);
        int y = 18 + rows * 12;
        Rect(s, 4, y, 150, 12, kBg);
        Text(s, buf, 8, y + 1, same ? kPlayerColors[id] : Color(160, 160, 160));
        rows++;

        if (same) {
            float rx = (r.dx - v.me.x) / RADAR_RANGE, ry = (r.dy - v.me.y) / RADAR_RANGE;
            int px = CX + (int)(Clampf(rx, -1, 1) * (R - 3));
            int py = CY + (int)(Clampf(ry, -1, 1) * (R - 3));
            Rect(s, px - 2, py - 2, 5, 5, kPlayerColors[id]);
        }
    }
    if (rows == 0) {
        Rect(s, 4, 18, 150, 12, kBg);
        Text(s, "Nobody else here yet", 8, 19, Color(160, 160, 160));
    }
}

} // namespace

bool OverlayCallback(const Screen& screen) {
    if (screen.IsTop) DrawTop(screen);
    else DrawBottom(screen);
    return true;
}

} // namespace coop
