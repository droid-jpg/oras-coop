// Read-only access to Alpha Sapphire 1.4 player state. See ORAS-RE/NOTES.md.
#pragma once
#include <3ds/types.h>

namespace game {

struct PlayerState {
    bool valid = false;
    u16 map = 0;
    u16 facing = 0;
    float x = 0, z = 0, y = 0; // z is height
    u32 aux = 0;               // raw player+0x08, meaning unknown
    float dirX = 0, dirY = 0;  // facing unit vector from the move model (0,0 if unknown)
    u8 anim = 0;               // player move model's animation id (mm+0x58; 0 idle, 0x10 walk)
    u16 model = 0;             // player model id (0xAC Brendan, 0xAB May; 0 if unknown)
    // The field is live and running (not title/intro/loading): the player's character exists
    // and its flags have bit 0x2. During the intro the save's field is already built with the
    // player flagged 0x5; calling game functions on it then hangs the game (verified).
    bool inField = false;
    bool riding = false;       // on a bike: player mm+0x04 has 0x01200000 (and not surfing)
    bool surfing = false;      // surfing: player mm+0x04 has 0x10000000 (0x13210443 vs bike 0x03210443)
    bool suspended = false;    // under script control (mm+0x08 bit 0): soaring, cutscenes, battles?
    bool locked = false;       // input locked by the game's UI (mm+0x08 bit 0x8): text box, X menu
    bool menuOpen = false;     // X menu open (mm+0x08 bit 0x02000000), drawn on the bottom screen
};

// Finds the player object (known address first, then a heap scan for its vtable).
// Returns true if found. Cheap when the cached address is still valid.
bool Locate();

// Reads the current state; valid=false if the object can't be confirmed.
PlayerState Read();

u32 ObjectAddress();

// ---- Move models (field characters). Writing to these changes the game; see Puppet.cpp. ----

constexpr int MOVE_MODEL_SLOTS = 32;
constexpr u16 PLAYER_MM_ID = 0xFF;

struct MoveModel {
    u32 addr = 0;
    u16 id = 0;     // object id on the map (player = 0xFF)
    u16 model = 0;  // model id
    float x = 0, z = 0, y = 0;
    float dirX = 1, dirY = 0;
};

// Finds the move-model array (cheap once found). Returns false if not on a field map.
bool LocateMoveModels();

// Fills `out` with in-use slots. Returns the count.
int ListMoveModels(MoveModel* out, int max);

// Reads the local player's facing as a unit vector (map X/Y plane) and animation id.
bool ReadPlayerDir(float& dirX, float& dirY, u8* anim = nullptr);

// Sets a character's animation id (mm+0x58, the byte the game's step actions set through
// FUN_003F1C20): 0 = idle, 0x10 = walk. Only sticks on characters no action is driving.
bool SetAnim(u32 addr, u8 anim);

// Moves a move model's real model: position, facing, and the "moving" flag so the
// game pushes the new transform. Returns false if `addr` is no longer a valid slot.
// solid = blocks the local player (its collision tile follows it); pass false while it overlaps
// the player, or the player gets stuck inside it.
bool Puppet(u32 addr, float x, float z, float y, float dirX, float dirY, bool solid = true);

// Spawns a new field character through the game's own spawn function (FUN_003F584C), the
// same path the game uses for the player. `model` should be one already loaded on the map
// (e.g. the player's own model) so no disk load is needed. Returns the new move model's
// address, or 0 if there's no free slot / the player isn't on a field map.
u32 SpawnCharacter(u16 id, u16 model, u16 tileX, u16 tileY);

// Deletes a character created by SpawnCharacter via the game's own delete (FUN_003FB3A0).
// Only acts if the slot is still in use and still has `id`. Returns true if deleted.
bool DespawnCharacter(u32 mm, u16 id);

// Starts a movement/animation action on a character via the game's own setter
// (FUN_003FB1B4(mm, code, dir) → DllField FUN_0072D128). code <= 0x103; dir 9 = keep facing.
bool SetAction(u32 mm, u16 code, u8 dir);

// Attaches a ride model to a character like the game's own mount (verified: friends' trainers
// show the Mach bike / the surf Pokémon). Call every frame until it returns 1: 0 = still loading,
// < 0 = can't (see g_rideDebug). Undo = respawn the character.
struct Ride { u32 model; u8 form, mode; };  // ride model id, animation set (mm+0x59), mm+0xA2C
constexpr Ride RIDE_BIKE = { 0x1B, 2, 2 };   // DllField FUN_007980BC
constexpr Ride RIDE_SURF = { 0x01, 7, 0 };   // DllField FUN_007958B8
// Last AttachRide call, for diagnostics (model instance, slot flags, ride status, result).
struct RideDebug { u32 inst = 0; u8 used0 = 0, used1 = 0; u32 status = 0, attached = 0; int result = 0; };
extern RideDebug g_rideDebug;
int AttachRide(u32 mm, const Ride& ride);

// True if `mm` is an in-use character slot holding object id `id`.
bool SlotHasId(u32 mm, u16 id);

// Finds an in-use character by object id. Returns its address or 0.
u32 FindCharacter(u16 id);

// Projects a world point (x, height z, map y) to top-screen pixels with the game's own
// camera (view-projection matrix at a fixed heap address, validated on every call). Returns
// false if the camera isn't there (battle, menus) or the point is behind it. sx/sy may be
// off-screen.
bool ProjectToTop(float x, float z, float y, float& sx, float& sy);

// True while the field is live (see PlayerState::inField).
bool PlayerFieldActive();

// True while the player is under script control (soaring, cutscenes); clear in menus.
bool PlayerSuspended();

// True while the player surfs (mm+0x04 bit 0x10000000; form byte mm+0x59 = 7).
bool PlayerSurfing();

// True while the player rides a bike (verified with the Mach bike; flags 0x00010443 -> 0x03210443; bits 0x01200000).
bool PlayerRiding();

// Reads the local player's model id (0 if unknown).
u16 PlayerModel();

// Player character models: Brendan 0xAC, May 0xAB (DllField FUN_0070A614, by gender).
constexpr u16 MODEL_BRENDAN = 0xAC;
constexpr u16 MODEL_MAY = 0xAB;
inline bool IsPlayerModel(u16 m) { return m == MODEL_BRENDAN || m == MODEL_MAY; }

// Friends' trainer models (Models.cpp): hooks the map loader so the player models friends use
// are added to every map's resident list as it loads (their 3D data then loads normally).
bool InstallModelHook();
void SetWantedModels(bool may, bool brendan);

// Rival placeholder model: 0x7A is a "variable" model the story points at the rival, i.e. May
// for a Brendan player (archive file 0x13A, verified); resident on maps where the rival appears.
constexpr u16 MODEL_RIVAL = 0x7A;
constexpr u32 RIVAL_MAY_FILE = 0x13A;
// True if the rival placeholder is resident on this map and currently resolves to May.
bool RivalIsMay();

// True if `model` is in the current map's resident model table, i.e. the game preloaded its
// 3D data for this map, so SpawnCharacter can use it.
bool IsModelResident(u16 model);

// Writes the place name the game shows for `zone` (the map id in PlayerState), e.g. "Route 109";
// buildings give their town's name. Read from the game's own place-name text (Places.cpp).
// Returns false while that text isn't found yet (it is searched for a few pages per call).
bool PlaceName(u16 zone, char* out, int len);

// Converts the player-state facing value (0..7) to a unit vector.
void FacingToDir(u16 facing, float& dirX, float& dirY);

} // namespace game
