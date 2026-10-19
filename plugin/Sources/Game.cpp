#include <3ds.h>
#include "Game.hpp"
#include <CTRPluginFramework.hpp>
#include <cmath>

using namespace CTRPluginFramework;

namespace game {

// Verified on Alpha Sapphire 1.4 (title 000400000011C500, update v1.4).
constexpr u32 PLAYER_VTABLE = 0x005DEDC0;
constexpr u32 KNOWN_ADDR    = 0x08C6E880;
constexpr u32 SCAN_START    = 0x08000000;
constexpr u32 SCAN_END      = 0x09000000;
constexpr u32 PAGE          = 0x1000;

constexpr u32 OFF_MAP    = 0x04;
constexpr u32 OFF_FACING = 0x06;
constexpr u32 OFF_AUX    = 0x08;
constexpr u32 OFF_X      = 0x14;
constexpr u32 OFF_Z      = 0x18;
constexpr u32 OFF_Y      = 0x1C;

static u32 s_obj = 0;

static bool Plausible(float v) {
    return std::isfinite(v) && std::fabs(v) < 100000.0f;
}

static bool LooksLikePlayer(u32 addr) {
    u32 vt = 0;
    if (!Process::Read32(addr, vt) || vt != PLAYER_VTABLE) return false;
    u16 map = 0;
    float x = 0, y = 0;
    if (!Process::Read16(addr + OFF_MAP, map) || map > 2000) return false;
    if (!Process::ReadFloat(addr + OFF_X, x) || !Plausible(x)) return false;
    if (!Process::ReadFloat(addr + OFF_Y, y) || !Plausible(y)) return false;
    return true;
}

constexpr u64 RESCAN_MS = 2000; // full heap scans are slow; don't repeat them every frame

bool Locate() {
    static u64 lastScan = 0;
    if (s_obj && LooksLikePlayer(s_obj)) return true;
    if (LooksLikePlayer(KNOWN_ADDR)) { s_obj = KNOWN_ADDR; return true; }
    if (osGetTime() - lastScan < RESCAN_MS) return false;
    lastScan = osGetTime();

    // Fall back to scanning the heap. The plugin runs inside the game process,
    // so mapped pages can be read directly; unmapped pages are skipped.
    for (u32 page = SCAN_START; page < SCAN_END; page += PAGE) {
        if (!Process::CheckAddress(page)) continue;
        const volatile u32* p = reinterpret_cast<const volatile u32*>(page);
        for (u32 i = 0; i < PAGE / 4; i++) {
            if (p[i] == PLAYER_VTABLE && LooksLikePlayer(page + i * 4)) {
                s_obj = page + i * 4;
                return true;
            }
        }
    }
    s_obj = 0;
    return false;
}

PlayerState Read() {
    PlayerState s;
    if (!s_obj || !LooksLikePlayer(s_obj)) return s;
    Process::Read16(s_obj + OFF_MAP, s.map);
    Process::Read16(s_obj + OFF_FACING, s.facing);
    Process::Read32(s_obj + OFF_AUX, s.aux);
    Process::ReadFloat(s_obj + OFF_X, s.x);
    Process::ReadFloat(s_obj + OFF_Z, s.z);
    Process::ReadFloat(s_obj + OFF_Y, s.y);
    s.valid = true;
    ReadPlayerDir(s.dirX, s.dirY, &s.anim);
    s.model = PlayerModel();
    return s;
}

u32 ObjectAddress() { return s_obj; }

// ---- Move models ----
// Array of 32 slots, 0xAB0 apart, each starting with vtable 0x005DF560 (verified AS 1.4).

constexpr u32 MM_VTABLE = 0x005DF560;
constexpr u32 MM_STRIDE = 0xAB0;
constexpr u32 MM_FLAGS4 = 0x04;   // bit 0 = slot in use
constexpr u32 MM_FLAGS8 = 0x08;   // bit 0x400 = moving (pushes model transform)
constexpr u32 MM_ID     = 0x0C;
constexpr u32 MM_MODEL  = 0x10;
constexpr u32 MM_POS    = 0xEC;   // f32 x, z, y (record starts at 0xE8)
constexpr u32 MM_PREV   = 0x114;  // committed copy of x, z, y
constexpr u32 MOVING    = 0x400;
constexpr u32 MM_ANIM   = 0x58;   // u8 animation id, set by step actions via FUN_003F1C20
// Grid records (0x2C bytes, f32 tile x @+4, tile y @+0xC; tile = floor(world / 18)) used for
// character-vs-character collision: previous, current and next tile. Spawn fills all three.
// Verified: the player is blocked by a spawned trainer only at the tile these records hold.
constexpr u32 MM_TILE_PREV = 0x64;
constexpr u32 MM_TILE_CUR  = 0x90;
constexpr u32 MM_TILE_NEXT = 0xBC;
constexpr float TILE = 18.0f;
// Facing: cos at these offsets, sin at the paired ones.
constexpr u32 MM_COS[] = { 0x1C, 0x28, 0x34, 0x48 };
constexpr u32 MM_SIN[] = { 0x24, 0x30, 0x3C, 0x40 };

static u32 s_mmBase = 0;

static bool IsSlot(u32 addr) {
    u32 vt = 0;
    return Process::Read32(addr, vt) && vt == MM_VTABLE;
}

bool LocateMoveModels() {
    static u64 lastScan = 0;
    if (s_mmBase && IsSlot(s_mmBase) && IsSlot(s_mmBase + MM_STRIDE)) return true;
    s_mmBase = 0;
    if (osGetTime() - lastScan < RESCAN_MS) return false;
    lastScan = osGetTime();
    for (u32 page = SCAN_START; page < SCAN_END; page += PAGE) {
        if (!Process::CheckAddress(page)) continue;
        const volatile u32* p = reinterpret_cast<const volatile u32*>(page);
        for (u32 i = 0; i < PAGE / 4; i++) {
            u32 a = page + i * 4;
            // The first slot is the one whose predecessor isn't a slot.
            if (p[i] == MM_VTABLE && IsSlot(a + MM_STRIDE) && !IsSlot(a - MM_STRIDE)) {
                s_mmBase = a;
                return true;
            }
        }
    }
    return false;
}

static bool ReadSlot(u32 a, MoveModel& m) {
    u32 flags4 = 0;
    if (!IsSlot(a) || !Process::Read32(a + MM_FLAGS4, flags4) || !(flags4 & 1)) return false;
    m.addr = a;
    Process::Read16(a + MM_ID, m.id);
    Process::Read16(a + MM_MODEL, m.model);
    Process::ReadFloat(a + MM_POS, m.x);
    Process::ReadFloat(a + MM_POS + 4, m.z);
    Process::ReadFloat(a + MM_POS + 8, m.y);
    Process::ReadFloat(a + MM_COS[0], m.dirX);
    Process::ReadFloat(a + MM_SIN[0], m.dirY);
    return true;
}

int ListMoveModels(MoveModel* out, int max) {
    if (!LocateMoveModels()) return 0;
    int n = 0;
    for (int i = 0; i < MOVE_MODEL_SLOTS && n < max; i++) {
        if (ReadSlot(s_mmBase + i * MM_STRIDE, out[n])) n++;
    }
    return n;
}

bool ReadPlayerDir(float& dirX, float& dirY, u8* anim) {
    if (!LocateMoveModels()) return false;
    for (int i = 0; i < MOVE_MODEL_SLOTS; i++) {
        MoveModel m;
        if (ReadSlot(s_mmBase + i * MM_STRIDE, m) && m.id == PLAYER_MM_ID) {
            dirX = m.dirX;
            dirY = m.dirY;
            if (anim) Process::Read8(m.addr + MM_ANIM, *anim);
            return true;
        }
    }
    return false;
}

bool Puppet(u32 a, float x, float z, float y, float dirX, float dirY) {
    u32 flags4 = 0, flags8 = 0;
    if (!IsSlot(a) || !Process::Read32(a + MM_FLAGS4, flags4) || !(flags4 & 1)) return false;
    Process::WriteFloat(a + MM_POS, x);
    Process::WriteFloat(a + MM_POS + 4, z);
    Process::WriteFloat(a + MM_POS + 8, y);
    Process::WriteFloat(a + MM_PREV, x);
    Process::WriteFloat(a + MM_PREV + 4, z);
    Process::WriteFloat(a + MM_PREV + 8, y);
    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len > 0.001f) {
        for (u32 off : MM_COS) Process::WriteFloat(a + off, dirX / len);
        for (u32 off : MM_SIN) Process::WriteFloat(a + off, dirY / len);
    }
    if (Process::Read32(a + MM_FLAGS8, flags8) && !(flags8 & MOVING)) {
        Process::Write32(a + MM_FLAGS8, flags8 | MOVING);
    }
    // Keep the collision tiles on the character, otherwise it blocks its spawn tile forever.
    float tx = std::floor(x / TILE), ty = std::floor(y / TILE);
    float cx = 0, cy = 0;
    Process::ReadFloat(a + MM_TILE_CUR + 4, cx);
    Process::ReadFloat(a + MM_TILE_CUR + 0xC, cy);
    if (cx != tx || cy != ty) {
        // All three on the current tile: a lagging "previous" tile would leave a ghost block.
        for (u32 rec : { MM_TILE_PREV, MM_TILE_CUR, MM_TILE_NEXT }) {
            Process::WriteFloat(a + rec + 4, tx);
            Process::WriteFloat(a + rec + 0xC, ty);
        }
    }
    return true;
}

bool SetAnim(u32 a, u8 anim) {
    u32 flags4 = 0;
    if (!IsSlot(a) || !Process::Read32(a + MM_FLAGS4, flags4) || !(flags4 & 1)) return false;
    u8 cur = 0;
    if (Process::Read8(a + MM_ANIM, cur) && cur != anim) Process::Write8(a + MM_ANIM, anim);
    return true;
}

// ---- Spawning ----
// FUN_003F584C(manager, entry*, zoneId, extra=0): fills the first free slot from a 0x30-byte
// spawn entry (u16 words: [0] id, [1] model, [2] move type, [0xE..0x10] 0xFFFF = no link,
// [0x14] tile x, [0x15] tile y). Asserts (crashes) if no slot is free — always check first.

constexpr u32 FN_SPAWN = 0x003F584C;
constexpr u32 MM_MANAGER = 0x8B4;  // move model -> manager (used by the model registry)
constexpr u32 MM_ZONE = 0x0E;

static u32 FindPlayerSlot() {
    if (!LocateMoveModels()) return 0;
    for (int i = 0; i < MOVE_MODEL_SLOTS; i++) {
        MoveModel m;
        if (ReadSlot(s_mmBase + i * MM_STRIDE, m) && m.id == PLAYER_MM_ID) return m.addr;
    }
    return 0;
}

u16 PlayerModel() {
    u32 p = FindPlayerSlot();
    u16 model = 0;
    if (p) Process::Read16(p + MM_MODEL, model);
    return model;
}

// Resident model table in the manager: u32 count @0x36C8, 0x18-byte descriptors @0x36CC
// (u16 model @0, ..., u32 archive file index @0x14), filled by the map loader from a per-map
// list (slot 0 = the player's model). Only models listed there have their 3D data loaded:
// appending a descriptor ourselves spawns an invisible character (see NOTES.md).
constexpr u32 RES_COUNT = 0x36C8;
constexpr u32 RES_TABLE = 0x36CC;
constexpr u32 RES_SIZE  = 0x18;
constexpr u32 RES_MAX   = 0x20;

bool IsModelResident(u16 model) {
    u32 player = FindPlayerSlot();
    u32 manager = 0;
    if (!player || !Process::Read32(player + MM_MANAGER, manager) || manager < 0x08000000) return false;
    u32 count = 0;
    if (!Process::Read32(manager + RES_COUNT, count) || count == 0 || count > RES_MAX) return false;
    for (u32 i = 0; i < count; i++) {
        u16 m = 0;
        if (Process::Read16(manager + RES_TABLE + i * RES_SIZE, m) && m == model) return true;
    }
    return false;
}

u32 SpawnCharacter(u16 id, u16 model, u16 tileX, u16 tileY) {
    u32 player = FindPlayerSlot();
    if (!player) return 0;

    int used = 0;
    for (int i = 0; i < MOVE_MODEL_SLOTS; i++) {
        u32 flags4 = 0;
        if (Process::Read32(s_mmBase + i * MM_STRIDE + MM_FLAGS4, flags4) && (flags4 & 1)) used++;
    }
    if (used >= MOVE_MODEL_SLOTS - 1) return 0; // keep one slot spare for the game

    u32 manager = 0;
    u16 zone = 0;
    if (!Process::Read32(player + MM_MANAGER, manager) || manager < 0x08000000) return 0;
    Process::Read16(player + MM_ZONE, zone);

    // Same shape as the entry the game builds when it spawns the player.
    u16 entry[0x18] = {0};
    entry[0] = id;
    entry[1] = model;
    entry[2] = 0;
    entry[0xE] = entry[0xF] = entry[0x10] = 0xFFFF;
    entry[0x14] = tileX;
    entry[0x15] = tileY;

    using SpawnFn = u32 (*)(u32 manager, u16* entry, u32 zone, u32 extra);
    return reinterpret_cast<SpawnFn>(FN_SPAWN)(manager, entry, zone, 0);
}

// FUN_003FB3A0(mm): releases the character's render/animation objects and frees the slot.
// Used by the game's map-exit cleanup (FUN_003F5D98) and script commands.
constexpr u32 FN_DESPAWN = 0x003FB3A0;

bool DespawnCharacter(u32 mm, u16 id) {
    MoveModel m;
    if (!ReadSlot(mm, m) || m.id != id) return false;
    using DespawnFn = void (*)(u32 mm);
    reinterpret_cast<DespawnFn>(FN_DESPAWN)(mm);
    return true;
}

constexpr u32 FN_SET_ACTION = 0x003FB1B4;
constexpr u32 MM_ACTION_LOCK = 0x9F8;   // u8 + u32 @0x9FC: setter asserts if both non-zero

bool SetAction(u32 mm, u16 code, u8 dir) {
    MoveModel m;
    if (code > 0x103 || !ReadSlot(mm, m)) return false;
    u8 lock = 0;
    u32 lockPtr = 0;
    Process::Read8(mm + MM_ACTION_LOCK, lock);
    Process::Read32(mm + MM_ACTION_LOCK + 4, lockPtr);
    if (lock && lockPtr) return false; // the game would assert
    using SetActionFn = void (*)(u32 mm, u32 code, u32 dir);
    reinterpret_cast<SetActionFn>(FN_SET_ACTION)(mm, code, dir);
    return true;
}

u32 FindCharacter(u16 id) {
    if (!LocateMoveModels()) return 0;
    for (int i = 0; i < MOVE_MODEL_SLOTS; i++) {
        MoveModel m;
        if (ReadSlot(s_mmBase + i * MM_STRIDE, m) && m.id == id) return m.addr;
    }
    return 0;
}

void FacingToDir(u16 facing, float& dirX, float& dirY) {
    // 0 up, 1 down, 2 left, 3 right (verified); 4..7 diagonals (7 = down-right verified).
    static const float D = 0.70710678f;
    static const float table[8][2] = {
        { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 }, { -D, -D }, { D, -D }, { -D, D }, { D, D },
    };
    const float* d = table[facing < 8 ? facing : 1];
    dirX = d[0];
    dirY = d[1];
}

} // namespace game
