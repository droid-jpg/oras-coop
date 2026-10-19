// "Real models" mode: spawns a trainer (using the local player's model, which is always
// loaded) for each remote player on the current map and drives it every frame.
// This WRITES game memory and calls the game's own spawn/delete functions.
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include "Session.hpp"
#include "Game.hpp"
#include "Puppet.hpp"

namespace coop {

namespace {

constexpr u16 ID_BASE = 0xE0;          // character id = ID_BASE + remote player id
constexpr u64 MAP_SETTLE_MS = 1000;    // wait after a map change before spawning
constexpr u64 STALE_MS = 5000;

// Animation ids (mm+0x58) used by the game's step actions: 0 idle, 0x10 walk (7 frames per
// tile, the player's normal walk). The sender forwards its own id; we fall back to these.
constexpr u8 ANIM_IDLE = 0x00;
constexpr u8 ANIM_WALK = 0x10;
constexpr u8 ANIM_RUN  = 0x19;          // running-shoes clip (verified visually on the player model)
constexpr float RUN_SPEED = 105.0f;     // units/s; walking is ~77 (18 units per 7 frames at 30 fps)
constexpr u8 ANIM_MAX  = 0x30;          // ignore ids outside the on-foot range (bike, surf...)
constexpr float MOVE_EPS = 0.25f;       // world units per frame that count as moving
constexpr u64 MOVE_HOLD_MS = 200;       // keep the clip playing across small update gaps

struct Spawned {
    bool active = false;
    u32 mm = 0;
    u16 map = 0;
    float lastX = 0, lastY = 0;
    u64 lastTime = 0;
    float speed = 0;                    // smoothed display speed, units/s
    u64 movingUntil = 0;
    u8 moveAnim = 0;                    // clip chosen for the current movement (0 = pick one)
    bool fromSender = false;            // moveAnim came from the sender's anim byte
};

Spawned s_spawned[MAX_ID + 1];
u16 s_lastMap = 0xFFFF;
u64 s_mapSince = 0;

u16 CharId(int remoteId) { return (u16)(ID_BASE + remoteId); }

void Forget(Spawned& s) { s = Spawned(); }

// Walk/run clip while the display position is moving, idle otherwise. The sender's own id
// drops to 0 for a frame between steps, so remember the last locomotion clip.
u8 PickAnim(Spawned& s, const Remote& r, u64 now) {
    float mx = r.dx - s.lastX, my = r.dy - s.lastY;
    float dist = std::sqrt(mx * mx + my * my);
    u64 dt = now - s.lastTime;
    s.lastX = r.dx;
    s.lastY = r.dy;
    s.lastTime = now;
    if (dt > 0 && dt < 500) s.speed += (dist * 1000.0f / dt - s.speed) * 0.2f; // units/s, smoothed
    if (dist > MOVE_EPS) s.movingUntil = now + MOVE_HOLD_MS;
    if (now >= s.movingUntil) {
        s.moveAnim = 0;
        s.fromSender = false;
        return ANIM_IDLE;
    }
    if (r.anim != ANIM_IDLE && r.anim < ANIM_MAX) {
        s.moveAnim = r.anim;  // the sender's own clip
        s.fromSender = true;
    } else if (!s.fromSender || !s.moveAnim) {
        // Old client: pick by speed, with some hysteresis so it doesn't flicker.
        bool run = s.moveAnim == ANIM_RUN ? s.speed > RUN_SPEED * 0.85f : s.speed > RUN_SPEED;
        s.moveAnim = run ? ANIM_RUN : ANIM_WALK;
    }
    return s.moveAnim;
}

void Despawn(int id, Spawned& s, u16 currentMap) {
    // After a map change the game has already deleted it; only delete on the same map.
    if (s.active && s.map == currentMap) game::DespawnCharacter(s.mm, CharId(id));
    Forget(s);
}

} // namespace

void UpdatePuppets(bool enabled, const game::PlayerState& me, const Remote* remotes, u8 myId) {
    u64 now = osGetTime();
    if (!me.valid) {
        // Title screen, battle, or loading: the game owns the characters right now.
        for (auto& s : s_spawned) Forget(s);
        s_lastMap = 0xFFFF;
        return;
    }
    if (me.map != s_lastMap) {
        s_lastMap = me.map;
        s_mapSince = now;
        for (auto& s : s_spawned) Forget(s); // the game cleared non-persistent characters
    }
    bool settled = now - s_mapSince >= MAP_SETTLE_MS;

    for (int id = 1; id <= MAX_ID; id++) {
        Spawned& s = s_spawned[id];
        const Remote& r = remotes[id];
        bool want = enabled && id != myId && r.active && r.hasState && r.map == me.map &&
                    now - r.lastUpdate < STALE_MS;

        if (!want) {
            if (s.active) Despawn(id, s, me.map);
            continue;
        }
        if (!s.active) {
            if (!settled) continue;
            // The friend's own trainer (Brendan/May) when this map has it loaded, else our own
            // model, which is always loaded.
            u16 model = game::PlayerModel();
            if (!model) continue;
            if (game::IsPlayerModel(r.model) && r.model != model && game::IsModelResident(r.model))
                model = r.model;
            u16 tx = (u16)std::max(0.0f, r.dx / 18.0f), ty = (u16)std::max(0.0f, r.dy / 18.0f);
            u32 mm = game::SpawnCharacter(CharId(id), model, tx, ty);
            if (!mm) continue; // no free slot on this map
            s.active = true;
            s.mm = mm;
            s.map = me.map;
            s.lastX = r.dx;
            s.lastY = r.dy;
            s.lastTime = now;
        }
        float dx = r.dirX, dy = r.dirY;
        if (dx == 0 && dy == 0) game::FacingToDir(r.facing, dx, dy);
        if (!game::Puppet(s.mm, r.dx, r.dz, r.dy, dx, dy)) { Forget(s); continue; } // slot reclaimed
        game::SetAnim(s.mm, PickAnim(s, r, now));
    }
}

bool IsPuppeted(int id) {
    return id >= 0 && id <= MAX_ID && s_spawned[id].active;
}

} // namespace coop
