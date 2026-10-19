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

// Reads the local player's facing as a unit vector (map X/Y plane).
bool ReadPlayerDir(float& dirX, float& dirY);

// Moves a move model's real model: position, facing, and the "moving" flag so the
// game pushes the new transform. Returns false if `addr` is no longer a valid slot.
bool Puppet(u32 addr, float x, float z, float y, float dirX, float dirY);

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

// Finds an in-use character by object id. Returns its address or 0.
u32 FindCharacter(u16 id);

// Reads the local player's model id (0 if unknown).
u16 PlayerModel();

// Converts the player-state facing value (0..7) to a unit vector.
void FacingToDir(u16 facing, float& dirX, float& dirY);

} // namespace game
