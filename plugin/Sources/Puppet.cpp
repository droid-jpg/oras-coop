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

struct Spawned {
    bool active = false;
    u32 mm = 0;
    u16 map = 0;
};

Spawned s_spawned[MAX_ID + 1];
u16 s_lastMap = 0xFFFF;
u64 s_mapSince = 0;

u16 CharId(int remoteId) { return (u16)(ID_BASE + remoteId); }

void Forget(Spawned& s) { s = Spawned(); }

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
            u16 model = game::PlayerModel();
            if (!model) continue;
            u16 tx = (u16)std::max(0.0f, r.dx / 18.0f), ty = (u16)std::max(0.0f, r.dy / 18.0f);
            u32 mm = game::SpawnCharacter(CharId(id), model, tx, ty);
            if (!mm) continue; // no free slot on this map
            s.active = true;
            s.mm = mm;
            s.map = me.map;
        }
        float dx = r.dirX, dy = r.dirY;
        if (dx == 0 && dy == 0) game::FacingToDir(r.facing, dx, dy);
        if (!game::Puppet(s.mm, r.dx, r.dz, r.dy, dx, dy)) Forget(s); // slot was reclaimed
        // TODO walk/run animation: re-requesting an action every frame resets its progress
        // (action 1 is the NPC "stand" action). Needs the movement-command codes used by
        // FUN_003FF4D0 (cutscene "apply movement") issued once per step — see NOTES.md.
    }
}

bool IsPuppeted(int id) {
    return id >= 0 && id <= MAX_ID && s_spawned[id].active;
}

} // namespace coop
