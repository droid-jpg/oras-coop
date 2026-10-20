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

// Dialogs in the game's own field text box (Talk.cpp): runs a small script of ours through the
// game's script system, so it looks and behaves exactly like talking to an NPC (the player waits).
//   Message:   [intro page] then `text`.
//   YesNo:     [intro] then `text` as a question with the game's Yes/No; the answer goes to the
//              result callback, which writes the reply line.
//   YesNoPick: as YesNo, and on Yes `pick` is asked with the game's choice list of `items`; the
//              index chosen (0xFF = backed out with B) goes to the result callback.
//   Menu:      `text`, then the game's choice list of `actions` right away. The action picked
//              goes to the result callback (Action; 0xFF = backed out with B). If it is
//              `battleAction`, the box closes and a trainer battle against `battleTrainer` starts
//              (the game's own _CallTrainerBattleCore, as trainer scripts call it; no whiteout on a
//              loss: that's a separate call we never make). If it is `tradeAction`, `pick` and the
//              list of `items` follow, as in YesNoPick.
enum class DialogKind : u8 { Message, YesNo, YesNoPick, Menu };
constexpr int DIALOG_ITEMS = 6;
constexpr int DIALOG_ACTIONS = 4;
struct Dialog {
    char intro[96] = {0};
    char text[96] = {0};
    DialogKind kind = DialogKind::Message;
    char pick[64] = {0};                  // YesNoPick: the question above the list
    char items[DIALOG_ITEMS][32] = {};    // YesNoPick: the choices
    int itemCount = 0;
    char actions[DIALOG_ACTIONS][32] = {}; // Menu
    int actionCount = 0;
    int battleAction = -1, tradeAction = -1;
    u16 battleTrainer = 0;
};
// Answer: the Yes/No result as the game stores it (non-zero = Yes). Slot: the party slot picked.
// Action: the Menu entry picked (0xFF = none).
enum class DialogResult : u8 { Answer, Slot, Action };
using DialogResultFn = void (*)(DialogResult what, int value, char* reply, int len);
// talkFn fills the dialog for a friend's trainer the player talks to (A in front of it; charId =
// its character id, 0xE1..0xE8). Both callbacks run on the game thread.
using TalkDialogFn = bool (*)(u16 charId, Dialog& out);
bool InstallTalkHooks(TalkDialogFn talkFn, DialogResultFn resultFn);
// 1 = started; 0 = the game is busy (another script / loading), try again later; < 0 = can't here.
int ShowGameDialog(const Dialog& d);
int ShowGameMessage(const char* text);
// True while any field script runs (ours included).
bool GameScriptRunning();

// Writes the place name the game shows for `zone` (the map id in PlayerState), e.g. "Route 109";
// buildings give their town's name. Read from the game's own place-name text (Places.cpp).
// Returns false while that text isn't found yet (it is searched for a few pages per call).
bool PlaceName(u16 zone, char* out, int len);

// Species name from the game's own text ("Wingull"); false while that text isn't found yet.
bool SpeciesName(u16 species, char* out, int len);

// Which game module is loaded in the shared CRO slot. During a battle DllField is unloaded:
// nothing that calls field code (spawn, delete, actions, rides, our text box) may run then.
bool FieldModuleLoaded();
bool BattleModuleLoaded();

// ---- Battles (Battle.cpp, read-only) ----
constexpr int BATTLE_PARTY = 6;
struct BattleMon { u16 species = 0, hp = 0, maxHp = 0; u8 level = 0; };
struct BattleState {
    bool active = false;
    u8 count[2] = { 0, 0 };              // side 0 = the player's team, 1 = the opponent
    BattleMon mon[2][BATTLE_PARTY];      // slot 0 = the Pokemon out right now
};
// Reads the running battle; false (and active=false) outside battles or on an unknown layout.
bool ReadBattle(BattleState& out);

// ---- Party (Battle.cpp) ----
// A party Pokemon as the game keeps it: the encrypted data block, then the encrypted party stats.
constexpr int PK_DATA = 0xE8, PK_STATS = 0x1C, PK_SIZE = PK_DATA + PK_STATS;
int PartyCount();
bool ReadPartySlot(int slot, u8* out);         // PK_SIZE bytes
bool WritePartySlot(int slot, const u8* in);   // replaces that slot (field only, no menu open)
bool DescribePokemon(const u8* pk, u16& species, u8& level, char* nickname = nullptr, int nickLen = 0);

// ---- Save data in memory (Save.cpp, read-only) ----
// Badges: one bit per gym, in the order they're earned in the story. False if the block doesn't
// look like the save data (an unknown game version).
bool ReadBadges(u8& out);
bool ReadMoney(u32& out);
bool WriteMoney(u32 money);
// PC boxes: 31 x 30 slots of encrypted PK6 data (PK_DATA bytes; no party stats).
constexpr int BOX_SLOTS = 31 * 30;
u32 BoxSlotEC(int slot);                     // the slot's encryption constant; 0 = empty
bool ReadBoxSlot(int slot, u8* out);         // PK_DATA bytes
// Species of encrypted PK6 data (PK_DATA bytes); false if empty or the checksum doesn't match.
bool SpeciesOf(const u8* pk, u16& species, bool* egg = nullptr);
// Sets a party Pokemon's (PK_SIZE bytes) current HP to its max and clears its status.
void HealPokemon(u8* pk);

// ---- Battles with friends' teams (FriendBattle.cpp) ----
// Hooks the game's trainer loader (FUN_00367804): while armed, the opponent's party (client 1)
// of the next trainer battle is replaced with `party` (count Pokemon, PK_SIZE bytes each).
bool InstallFriendBattleHook();
void ArmFriendBattle(const u8* party, int count);
void DisarmFriendBattle();
// True once the armed battle's party has been put in (cleared by DisarmFriendBattle).
bool FriendBattleStarted();
// Rival trainers to borrow (single battles, the game's strongest AI flags): they look like a
// friend's own character. Trainer data a/0/3/6; see re/NOTES.md.
constexpr u16 TRAINER_BRENDAN = 699, TRAINER_MAY = 906;

// Converts the player-state facing value (0..7) to a unit vector.
void FacingToDir(u16 facing, float& dirX, float& dirY);

} // namespace game
