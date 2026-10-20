#include <3ds.h>
#include "Game.hpp"
#include <CTRPluginFramework.hpp>
#include <cmath>
#include <cstring>

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

// Player mm+0x08 bits for the game's own UI (verified on Route 109): idle 0x00044006, a text box
// (Surf prompt) 0x0004400E, the X menu 0x0204400E. Bit 0 (script control) stays clear in both.
constexpr u32 UI_LOCKED = 0x8;
constexpr u32 UI_MENU = 0x02000000;
static u32 PlayerUiFlags();

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

// Heap scans are spread over frames: a few pages per call, resuming where the last call
// stopped, so a lost object (battle, title screen, loading) never stalls a frame.
// The whole 16 MB range takes SCAN_END-SCAN_START / (PAGE*SCAN_PAGES) calls (~2 s at 30 fps).
constexpr u32 SCAN_PAGES = 64;

struct Scanner {
    u32 next = SCAN_START;
    // Returns the first address in this call's slice whose word is `value` and passes `ok`.
    template <typename Ok> u32 Step(u32 value, Ok ok) {
        for (u32 n = 0; n < SCAN_PAGES; n++) {
            u32 page = next;
            next = next + PAGE >= SCAN_END ? SCAN_START : next + PAGE;
            if (!Process::CheckAddress(page)) continue;
            // The plugin runs inside the game process, so mapped pages can be read directly.
            const volatile u32* p = reinterpret_cast<const volatile u32*>(page);
            for (u32 i = 0; i < PAGE / 4; i++) {
                if (p[i] == value && ok(page + i * 4)) return page + i * 4;
            }
        }
        return 0;
    }
};

bool Locate() {
    static Scanner scan;
    if (s_obj && LooksLikePlayer(s_obj)) return true;
    if (LooksLikePlayer(KNOWN_ADDR)) { s_obj = KNOWN_ADDR; return true; }
    s_obj = scan.Step(PLAYER_VTABLE, LooksLikePlayer);
    return s_obj != 0;
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
    s.inField = s.model && FieldModuleLoaded() && PlayerFieldActive();
    s.surfing = s.inField && PlayerSurfing();
    s.riding = s.inField && !s.surfing && PlayerRiding();
    s.suspended = s.inField && PlayerSuspended();
    u32 ui = s.inField ? PlayerUiFlags() : 0;
    s.locked = ui & UI_LOCKED;
    s.menuOpen = ui & UI_MENU;
    return s;
}

u32 ObjectAddress() { return s_obj; }

// ---- Camera ----
// The field camera keeps a 4x4 row-major view-projection matrix (world x, height, map y, 1 ->
// clip). Row 3 is the negated view direction plus offset, so clip.w is the depth in front of
// the camera; top screen: sx = 200 * (1 + x/w), sy = 120 * (1 - y/w). Verified: projects the
// player's feet to (200, 148) in Littleroot and (200, 227) in the house (fixed camera);
// 30° vertical FOV. The camera object moves between boots; the matrix is always preceded by
// the word 0x00020003 and a pointer, then eye (+0x4C) and target (+0x58) follow it.
constexpr u32 CAMERA_TAG = 0x00020003;
constexpr u32 CAMERA_TAG_TO_MATRIX = 8;
constexpr float TOP_HALF_W = 200.0f, TOP_HALF_H = 120.0f;

static bool ReadMatrix(u32 a, float* m) {
    for (int i = 0; i < 16; i++) {
        if (!Process::ReadFloat(a + i * 4, m[i]) || !std::isfinite(m[i])) return false;
    }
    // Row 3 xyz is a unit vector (view direction); rows 0/1 carry the projection scale
    // (|row0 xyz| = |row1 xyz| * 240/400 for the top screen's aspect).
    float r3 = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
    float r0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float r1 = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    return std::fabs(r3 - 1.0f) < 0.02f && r0 > 0.3f && r0 < 20.0f && std::fabs(r0 - r1 * 0.6f) < 0.05f * r0;
}

static bool Project(const float* m, float x, float z, float y, float& sx, float& sy) {
    float cx = m[0] * x + m[1] * z + m[2] * y + m[3];
    float cy = m[4] * x + m[5] * z + m[6] * y + m[7];
    float cw = m[12] * x + m[13] * z + m[14] * y + m[15];
    if (cw < 1.0f) return false; // behind or at the camera
    sx = TOP_HALF_W * (1.0f + cx / cw);
    sy = TOP_HALF_H * (1.0f - cy / cw);
    return true;
}

// The live camera shows our own player; a stale camera from another map usually doesn't.
static bool SeesPlayer(const float* m) {
    float px, pz, py, sx, sy;
    if (!s_obj || !Process::ReadFloat(s_obj + OFF_X, px) || !Process::ReadFloat(s_obj + OFF_Z, pz) ||
        !Process::ReadFloat(s_obj + OFF_Y, py))
        return false;
    return Project(m, px, pz, py, sx, sy) && sx >= 0 && sx <= 2 * TOP_HALF_W && sy >= 0 && sy <= 2 * TOP_HALF_H;
}

static bool IsCameraTag(u32 a) {
    float m[16];
    return ReadMatrix(a + CAMERA_TAG_TO_MATRIX, m) && SeesPlayer(m);
}

bool ProjectToTop(float x, float z, float y, float& sx, float& sy) {
    static u32 cam = 0; // matrix address
    static Scanner scan;
    float m[16];
    if (!cam || !ReadMatrix(cam, m) || !SeesPlayer(m)) {
        u32 tag = scan.Step(CAMERA_TAG, IsCameraTag);
        cam = tag ? tag + CAMERA_TAG_TO_MATRIX : 0;
        if (!cam || !ReadMatrix(cam, m)) return false;
    }
    return Project(m, x, z, y, sx, sy);
}

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
constexpr float NO_TILE = -1000.0f; // tile coordinate no map uses (collision off)
// Facing: cos at these offsets, sin at the paired ones.
constexpr u32 MM_COS[] = { 0x1C, 0x28, 0x34, 0x48 };
constexpr u32 MM_SIN[] = { 0x24, 0x30, 0x3C, 0x40 };

static u32 s_mmBase = 0;

static bool IsSlot(u32 addr) {
    u32 vt = 0;
    return Process::Read32(addr, vt) && vt == MM_VTABLE;
}

// The first slot is the one whose predecessor isn't a slot.
static bool IsFirstSlot(u32 a) { return IsSlot(a + MM_STRIDE) && !IsSlot(a - MM_STRIDE); }

bool LocateMoveModels() {
    static Scanner scan;
    static u32 lastGood = 0; // the array is usually reallocated at the same address
    if (s_mmBase && IsSlot(s_mmBase) && IsSlot(s_mmBase + MM_STRIDE)) return true;
    if (lastGood && IsSlot(lastGood) && IsFirstSlot(lastGood)) { s_mmBase = lastGood; return true; }
    s_mmBase = scan.Step(MM_VTABLE, IsFirstSlot);
    if (s_mmBase) lastGood = s_mmBase;
    return s_mmBase != 0;
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

// The player's move model; cached because it is needed every frame and rarely moves.
static u32 FindPlayerSlot() {
    static u32 cached = 0;
    if (!LocateMoveModels()) return 0;
    u16 id = 0;
    u32 flags4 = 0;
    if (cached && IsSlot(cached) && Process::Read32(cached + MM_FLAGS4, flags4) && (flags4 & 1) &&
        Process::Read16(cached + MM_ID, id) && id == PLAYER_MM_ID)
        return cached;
    cached = 0;
    for (int i = 0; i < MOVE_MODEL_SLOTS; i++) {
        MoveModel m;
        if (ReadSlot(s_mmBase + i * MM_STRIDE, m) && m.id == PLAYER_MM_ID) return cached = m.addr;
    }
    return 0;
}

bool ReadPlayerDir(float& dirX, float& dirY, u8* anim) {
    u32 a = FindPlayerSlot();
    if (!a) return false;
    Process::ReadFloat(a + MM_COS[0], dirX);
    Process::ReadFloat(a + MM_SIN[0], dirY);
    if (anim) Process::Read8(a + MM_ANIM, *anim);
    return true;
}

bool Puppet(u32 a, float x, float z, float y, float dirX, float dirY, bool solid) {
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
    // Not solid: park them far off the map, so a player standing inside it can walk out.
    float tx = solid ? std::floor(x / TILE) : NO_TILE, ty = solid ? std::floor(y / TILE) : NO_TILE;
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
constexpr int SPARE_SLOTS = 4;     // free slots kept for the game (scripts spawn NPCs mid-map)
constexpr u32 MM_MANAGER = 0x8B4;  // move model -> manager (used by the model registry)
constexpr u32 MM_ZONE = 0x0E;

// Player mm+0x08 while the field runs: bit 0x2 in every field state seen (towns 0x4002/0x4006,
// routes 0x8006, biking 0x20004402, soaring 0x2000400F); the intro has 0x5. (0x4000 is NOT it:
// it is clear on routes.)
constexpr u32 FIELD_ACTIVE = 0x2;

bool PlayerFieldActive() {
    u32 p = FindPlayerSlot(), flags8 = 0;
    return p && Process::Read32(p + MM_FLAGS8, flags8) && (flags8 & FIELD_ACTIVE);
}

static u32 PlayerUiFlags() {
    u32 p = FindPlayerSlot(), flags8 = 0;
    return p && Process::Read32(p + MM_FLAGS8, flags8) ? flags8 : 0;
}

constexpr u32 RIDING = 0x01200000;   // player mm+0x04 bits set while on a bike (0x02000000 = indoors)

bool PlayerSuspended() {
    // mm+0x08 bit 0: set while soaring (0x2000400F) and in the intro (0x5); clear in menus.
    u32 p = FindPlayerSlot(), flags8 = 0;
    return p && Process::Read32(p + MM_FLAGS8, flags8) && (flags8 & 1);
}

constexpr u32 SURFING = 0x10000000;  // player mm+0x04 while surfing (also has the bike bits)

bool PlayerSurfing() {
    u32 p = FindPlayerSlot(), flags4 = 0;
    return p && Process::Read32(p + MM_FLAGS4, flags4) && (flags4 & SURFING);
}

bool PlayerRiding() {
    u32 p = FindPlayerSlot(), flags4 = 0;
    return p && Process::Read32(p + MM_FLAGS4, flags4) && (flags4 & RIDING) == RIDING;
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

// Address of `model`'s descriptor in the current map's resident table, or 0.
static u32 FindResident(u16 model) {
    u32 player = FindPlayerSlot();
    u32 manager = 0;
    if (!player || !Process::Read32(player + MM_MANAGER, manager) || manager < 0x08000000) return 0;
    u32 count = 0;
    if (!Process::Read32(manager + RES_COUNT, count) || count == 0 || count > RES_MAX) return 0;
    for (u32 i = 0; i < count; i++) {
        u16 m = 0;
        u32 d = manager + RES_TABLE + i * RES_SIZE;
        if (Process::Read16(d, m) && m == model) return d;
    }
    return 0;
}

bool IsModelResident(u16 model) { return FindResident(model) != 0; }

// The game's own descriptor read, as FUN_003F7640 does it for the player's model and FUN_003F7578
// for variable models: FUN_0019C678(model) -> descriptor file (-1 = none), then the synchronous
// read FUN_00128E4C(*(manager+0x36B4), file, out).
constexpr u32 FN_MODEL_FILE = 0x0019C678;
constexpr u32 FN_READ_FILE = 0x00128E4C;
constexpr u32 MGR_MODEL_ARCHIVE = 0x36B4;

bool ReadModelDescriptor(u16 model, u32* out) {
    if (!model || model >= MODEL_LIMIT || !FieldModuleLoaded()) return false;
    u32 player = FindPlayerSlot(), manager = 0, archive = 0;
    if (!player || !Process::Read32(player + MM_MANAGER, manager) || manager < 0x08000000 ||
        !Process::Read32(manager + MGR_MODEL_ARCHIVE, archive) || !archive)
        return false;
    s32 file = reinterpret_cast<s32 (*)(u32)>(FN_MODEL_FILE)(model);
    if (file < 0) return false;
    static u32 buf[16]; // descriptor files are 0x18 bytes
    std::memset(buf, 0, sizeof buf);
    reinterpret_cast<u32 (*)(u32, u32, u32*)>(FN_READ_FILE)(archive, (u32)file, buf);
    if (!buf[5] || buf[5] >= 0x1000) return false; // no model file in it
    buf[0] = (buf[0] & 0xFFFF0000) | model;        // as FUN_003F7578 does
    std::memcpy(out, buf, RES_SIZE);
    return true;
}

bool RivalIsMay() {
    u32 d = FindResident(MODEL_RIVAL), file = 0;
    return d && Process::Read32(d + 0x14, file) && file == RIVAL_MAY_FILE;
}

u32 SpawnCharacter(u16 id, u16 model, u16 tileX, u16 tileY) {
    if (!FieldModuleLoaded()) return 0; // battle: the field code is unloaded
    u32 player = FindPlayerSlot();
    if (!player) return 0;

    int used = 0;
    for (int i = 0; i < MOVE_MODEL_SLOTS; i++) {
        u32 flags4 = 0;
        if (Process::Read32(s_mmBase + i * MM_STRIDE + MM_FLAGS4, flags4) && (flags4 & 1)) used++;
    }
    if (used >= MOVE_MODEL_SLOTS - SPARE_SLOTS) return 0; // leave room for cutscene/script NPCs

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

// ---- Ride attachments (bike / surf mount) ----
// Mirrors FUN_003F5C2C(manager, mm, ride, slot), the game's attach step, one call at a time so we
// can see where it stops. Bike mount = DllField FUN_007980BC (ride 0x1B, form 2, mode 2); surf
// mount = DllField FUN_007958B8 (ride 1, form 7, mode 0):
//   inst   = FUN_004E3F90(mm)                 model instance (needs mm+0x08 bit 2 + mm+0xA18)
//   slot   free when inst[0x1C0 + slot] == 0  (2 slots)
//   status = stub 0x19C610(pool, ride)        pool = manager+0x36BC; 3 = ride model loaded
//   0     -> stub 0x19C618(pool, entry)       request it (entry = manager + ride*8 + 0x39E8)
//   3     -> stub 0x19C618(pool, entry); stub 0x19C620(pool, inst, ride, mm, slot) attaches
//            (DllField FUN_007352AC); stub 0x19C628(mm + 0x96C) refreshes the animation state.
constexpr u32 FN_MODEL_INST = 0x004E3F90;
constexpr u32 STUB_RIDE_STATUS = 0x0019C610, STUB_RIDE_REQUEST = 0x0019C618;
constexpr u32 STUB_RIDE_ATTACH = 0x0019C620, STUB_RIDE_REFRESH = 0x0019C628;
constexpr u32 MGR_RIDE_POOL = 0x36BC, MGR_RIDE_ENTRIES = 0x39E8;
constexpr u32 INST_RIDE_USED = 0x1C0;
constexpr u32 RIDE_MAX = 0x22;
constexpr u32 FN_SET_FORM = 0x003F31EC;  // (mm, form): mm+0x59, animation remap set (bike = 2)
constexpr u32 FN_SET_RIDE = 0x003F9C08;  // (mm, mode): mm+0xA2C (bike 2, surf 0, on foot 1)

RideDebug g_rideDebug;

int AttachRide(u32 mm, const Ride& rd) {
    if (!FieldModuleLoaded()) return g_rideDebug.result = -1;
    u32 ride = rd.model;
    RideDebug& d = g_rideDebug;
    d = RideDebug();
    MoveModel m;
    u32 manager = 0, pool = 0;
    if (ride >= RIDE_MAX || !ReadSlot(mm, m) || !Process::Read32(mm + MM_MANAGER, manager) ||
        manager < 0x08000000 || !Process::Read32(manager + MGR_RIDE_POOL, pool) || !pool)
        return d.result = -1;
    d.inst = reinterpret_cast<u32 (*)(u32)>(FN_MODEL_INST)(mm);
    if (!d.inst) return d.result = 0; // just spawned: its model isn't built yet, retry
    Process::Read8(d.inst + INST_RIDE_USED, d.used0);
    Process::Read8(d.inst + INST_RIDE_USED + 1, d.used1);
    u32 slot = !d.used0 ? 0 : !d.used1 ? 1 : 2;
    if (slot > 1) return d.result = -3;
    d.status = reinterpret_cast<u32 (*)(u32, u32)>(STUB_RIDE_STATUS)(pool, ride);
    u32 entry = manager + ride * 8 + MGR_RIDE_ENTRIES;
    if (d.status == 0) {
        reinterpret_cast<void (*)(u32, u32)>(STUB_RIDE_REQUEST)(pool, entry);
        return d.result = 0; // loading
    }
    if (d.status != 3) return d.result = 0; // still loading
    reinterpret_cast<void (*)(u32, u32)>(STUB_RIDE_REQUEST)(pool, entry);
    d.attached = reinterpret_cast<u32 (*)(u32, u32, u32, u32, u32)>(STUB_RIDE_ATTACH)(pool, d.inst, ride, mm, slot);
    reinterpret_cast<void (*)(u32)>(STUB_RIDE_REFRESH)(mm + 0x96C);
    if (!d.attached) return d.result = -4;
    // Rest of the mount that applies to any character: animation set + ride mode.
    reinterpret_cast<void (*)(u32, u32)>(FN_SET_FORM)(mm, rd.form);
    reinterpret_cast<void (*)(u32, u32)>(FN_SET_RIDE)(mm, rd.mode);
    return d.result = 1;
}

// FUN_003FB3A0(mm): releases the character's render/animation objects and frees the slot.
// Used by the game's map-exit cleanup (FUN_003F5D98) and script commands.
constexpr u32 FN_DESPAWN = 0x003FB3A0;

bool DespawnCharacter(u32 mm, u16 id) {
    if (!FieldModuleLoaded()) return false;
    MoveModel m;
    if (!ReadSlot(mm, m) || m.id != id) return false;
    using DespawnFn = void (*)(u32 mm);
    reinterpret_cast<DespawnFn>(FN_DESPAWN)(mm);
    return true;
}

constexpr u32 FN_SET_ACTION = 0x003FB1B4;
constexpr u32 MM_ACTION_LOCK = 0x9F8;   // u8 + u32 @0x9FC: setter asserts if both non-zero

bool SetAction(u32 mm, u16 code, u8 dir) {
    if (!FieldModuleLoaded()) return false;
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

bool SlotHasId(u32 mm, u16 id) {
    MoveModel m;
    return ReadSlot(mm, m) && m.id == id;
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

// ---- Place names ----
// Zone table: *(0x005F45BC) -> 0x218 entries of 0x38 bytes, resident from boot (FUN_003D99FC);
// entry +0x1C & 0x3FF = place-name line (Littleroot 170, Slateport 0xBA, Route 109 220; the
// area banner reads it through FUN_004D88E8). The place names are a resident message file
// (vtable 0x005DE400; header: u16 languages 1, u16 lines 0x164) that we decode like GetString
// FUN_00139A34 / FUN_0013F758: line i is XORed with a key starting at (i + 3) * 0x2983 and
// rotated left by 3 bits per character.
constexpr u32 ZONE_TABLE_PTR = 0x005F45BC;
constexpr u32 ZONE_COUNT = 0x218, ZONE_SIZE = 0x38, ZONE_PLACE = 0x1C;
constexpr u32 MSG_VTABLE = 0x005DE400;
constexpr u32 MSG_IN_MEMORY = 0x0C, MSG_HEADER = 0x18, MSG_SECTION = 0x1C;
constexpr u16 PLACE_LINES = 0x164;

// A resident message file with `wantLines` lines (one language).
static bool IsTextWithLines(u32 md, u16 wantLines) {
    u8 inMemory = 0;
    u32 header = 0, section = 0;
    u16 langs = 0, lines = 0;
    return Process::Read8(md + MSG_IN_MEMORY, inMemory) && inMemory &&
           Process::Read32(md + MSG_HEADER, header) && Process::CheckAddress(header) &&
           Process::Read32(md + MSG_SECTION, section) && Process::CheckAddress(section) &&
           Process::Read16(header, langs) && langs == 1 && Process::Read16(header + 2, lines) &&
           lines == wantLines;
}

static bool IsPlaceText(u32 md) { return IsTextWithLines(md, PLACE_LINES); }

// Decodes line `line` of message file `md` to ASCII.
static bool DecodeLine(u32 md, u16 line, char* out, int len) {
    u32 header = 0, section = 0, offset = 0;
    u16 chars = 0;
    bool encrypted = true;
    u32 plain = 0;
    Process::Read32(md + MSG_HEADER, header);
    Process::Read32(md + MSG_SECTION, section);
    if (Process::Read32(header + 8, plain)) encrypted = plain == 0;
    if (!Process::Read32(section + line * 8 + 4, offset) || !Process::Read16(section + line * 8 + 8, chars))
        return false;
    u32 key = ((line + 3) * 0x2983) & 0xFFFF;
    int n = 0;
    for (u16 i = 0; i < chars && n < len - 1; i++) {
        u16 c = 0;
        if (!Process::Read16(section + offset + i * 2, c)) break;
        if (encrypted) {
            c ^= (u16)key;
            key = ((key << 3) | (key >> 13)) & 0xFFFF;
        }
        if (c == 0) break;
        if (c == 0xE9 || c == 0xC9) c = 'e';        // "Pokemon"
        else if (c < 0x20 || c > 0x7E) c = '?';     // our font is ASCII
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n > 0;
}

bool PlaceName(u16 zone, char* out, int len) {
    static u32 md = 0;
    static Scanner scan;
    if (len < 2 || zone >= ZONE_COUNT) return false;
    u32 table = 0;
    u16 place = 0;
    if (!Process::Read32(ZONE_TABLE_PTR, table) || !Process::CheckAddress(table) ||
        !Process::Read16(table + zone * ZONE_SIZE + ZONE_PLACE, place))
        return false;
    place &= 0x3FF;
    if (place >= PLACE_LINES) return false;
    u32 vt = 0;
    if (!md || !Process::Read32(md, vt) || vt != MSG_VTABLE || !IsPlaceText(md)) {
        md = scan.Step(MSG_VTABLE, IsPlaceText);
        if (!md) return false;
    }
    return DecodeLine(md, place, out, len);
}

// Species names: a resident message file of 812 lines, line = national dex number (1 = the first
// species). Loaded from boot and kept through battles (same address on the field and in a battle).
constexpr u16 SPECIES_LINES = 812;
static bool IsSpeciesText(u32 md) { return IsTextWithLines(md, SPECIES_LINES); }

bool SpeciesName(u16 species, char* out, int len) {
    static u32 md = 0;
    static Scanner scan;
    if (len < 2 || species == 0 || species >= SPECIES_LINES) return false;
    u32 vt = 0;
    if (!md || !Process::Read32(md, vt) || vt != MSG_VTABLE || !IsSpeciesText(md)) {
        md = scan.Step(MSG_VTABLE, IsSpeciesText);
        if (!md) return false;
    }
    return DecodeLine(md, species, out, len);
}

// ---- Loaded game module ----
// The field (DllField) and battle (DllBattle) code are CROs loaded into the same slot at
// 0x006F3000; header +0x84 points at the module's name. In a battle DllField is gone, and so is
// code the spawn/delete/action functions call into: calling them then would crash.
constexpr u32 CRO_SLOT_NAME = 0x006F3084;
constexpr u32 DLLFIELD_NAME = 0x00802000, DLLBATTLE_NAME = 0x007DA000;

static bool ModuleIs(u32 nameAddr, const char* name) {
    u32 at = 0;
    if (!Process::Read32(CRO_SLOT_NAME, at) || at != nameAddr) return false;
    for (int i = 0; name[i]; i++) {
        u8 c = 0;
        if (!Process::Read8(nameAddr + i, c) || c != (u8)name[i]) return false;
    }
    return true;
}

bool FieldModuleLoaded() { return ModuleIs(DLLFIELD_NAME, "DllField"); }
bool BattleModuleLoaded() { return ModuleIs(DLLBATTLE_NAME, "DllBattle"); }

} // namespace game
