// Trade evolutions for our trades: the game's own check decides, and the game's own evolution
// scene (DllShinkaDemo) evolves the Pokemon in the party, as after a link trade.
//
// The link trade (FUN_003E1D48, state 4) calls FUN_004D2C68(received, given, &species, &route):
// the evolution table's trade methods (5 trade, 6 trade holding an item, 7 Karrablast <-> Shelmet,
// which needs the other Pokemon), with the Everstone check (not for Kadabra). On a hit it fills a
// 0x20-byte DllShinkaDemo param and starts the scene. After a battle, level-up evolutions
// (FUN_003E8C68) pass the player's party and slot the same way, so the scene evolves it in place.
//
// Apps run through the script manager's app-call request (FUN_003D05E4: type @+0x1C, param @+0x20),
// which field natives fill in from a running script; FUN_003CFBA8 then starts the app by type.
// No type starts DllShinkaDemo from the field, so our talk script asks for the field demo
// (CallApplicationDemo, type 0x16) and our hook on FUN_003CFBA8 starts the evolution scene instead
// (FUN_00369658, the field's DllShinkaDemo launcher). See re/NOTES.md, "Trade evolutions".
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstring>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

constexpr u32 FN_TRADE_EVOLVES = 0x004D2C68; // (mon, partner or 0, u16* species, u32* route) -> bool
constexpr u32 FN_START_APP = 0x003CFBA8;     // (scriptMgr, gameMgr): starts the requested app
constexpr u32 FN_CALL_SHINKA = 0x00369658;   // (procMgr, param): DllShinkaDemo
constexpr u32 FN_NEW_MON = 0x0011F560;       // (mem, heap, 0, 1, 0, 0): a blank Pokemon (0x10 bytes)
constexpr u32 FN_ALLOC = 0x001235A8;         // (size, heap) -> ptr
constexpr u32 SCRIPT_MGR_PTR = 0x005F46FC, MGR_HEAP = 0x04;
constexpr u32 APP_TYPE = 0x1C, APP_FIELD_DEMO = 0x16, APP_NONE = 0x01; // 0x01: no app has it
constexpr u32 GAME_PROCS = 0x14;             // game manager -> proc manager
constexpr u32 MON_VTABLE = 0x005DE89C, MON_DATA = 4, MON_STATS = 8, MON_DELETE = 8; // vtable +8: delete
constexpr u16 KARRABLAST = 588, SHELMET = 616;

// DllShinkaDemo's param, as the link trade fills it.
struct ShinkaParam {
    u32 party;      // PokeParty
    u32 index;      // slot in it
    u32 species;    // evolves into
    u32 route;      // evolution table entry (the scene handles what it uses up, e.g. the held item)
    u8 flags[4];    // 0, 0, 0, 1 after a trade (1, 1, 1, 0 after a battle: B may stop it)
    u32 myStatus;   // 0 after a trade
    u32 unused[2];
};

ShinkaParam s_param;
volatile bool s_armed = false;
bool s_hooked = false;
Hook s_hook;

bool HeapPtr(u32 a, u32& v) {
    return Process::Read32(a, v) && v >= 0x08000000 && v < 0x10000000 && (v & 3) == 0;
}

bool IsMon(u32 mon) {
    u32 vt = 0, p = 0;
    return mon && Process::Read32(mon, vt) && vt == MON_VTABLE && HeapPtr(mon + MON_DATA, p) && HeapPtr(mon + MON_STATS, p);
}

// A temporary Pokemon object holding `pk` (PK_SIZE bytes), on the script heap; 0 if that fails.
u32 NewMon(const u8* pk) {
    u32 mgr = 0, heap = 0;
    if (!HeapPtr(SCRIPT_MGR_PTR, mgr) || !HeapPtr(mgr + MGR_HEAP, heap)) return 0;
    u32 mem = reinterpret_cast<u32 (*)(u32, u32)>(FN_ALLOC)(0x10, heap);
    if (!mem) return 0;
    u32 mon = reinterpret_cast<u32 (*)(u32, u32, u32, u32, u32, u32)>(FN_NEW_MON)(mem, heap, 0, 1, 0, 0);
    if (!IsMon(mon)) return 0;
    std::memcpy(*reinterpret_cast<void**>(mon + MON_DATA), pk, PK_DATA);
    std::memcpy(*reinterpret_cast<void**>(mon + MON_STATS), pk + PK_DATA, PK_STATS);
    return mon;
}

void DeleteMon(u32 mon) {
    u32 vt = *reinterpret_cast<u32*>(mon);
    reinterpret_cast<void (*)(u32)>(*reinterpret_cast<u32*>(vt + MON_DELETE))(mon);
}

u32 StartAppHook(u32 mgr, u32 gameMgr) {
    HookContext& ctx = HookContext::GetCurrent();
    volatile u8* type = reinterpret_cast<volatile u8*>(mgr + APP_TYPE);
    u32 procs = 0;
    if (!s_armed || *type != APP_FIELD_DEMO || !HeapPtr(gameMgr + GAME_PROCS, procs))
        return ctx.OriginalFunction<u32>(mgr, gameMgr);
    s_armed = false;
    // The original's bookkeeping (it notes the time) without starting the field demo.
    *type = APP_NONE;
    u32 r = ctx.OriginalFunction<u32>(mgr, gameMgr);
    *type = APP_FIELD_DEMO;
    reinterpret_cast<void (*)(u32, u32)>(FN_CALL_SHINKA)(procs, (u32)&s_param);
    return r;
}

} // namespace

bool InstallEvolutionHook() {
    s_hook.InitializeForMitm(FN_START_APP, (u32)StartAppHook);
    s_hooked = s_hook.Enable() == HookResult::Success;
    return s_hooked;
}

bool ArmTradeEvolution(int slot, const u8* given, u16& species) {
    s_armed = false;
    u32 mon = 0;
    if (!s_hooked || slot < 0 || slot >= PartyCount() || !HeapPtr(PARTY_OBJECT + slot * 4, mon) || !IsMon(mon))
        return false;
    // Only Karrablast and Shelmet look at the other Pokemon; everything else gets none.
    u16 mine = 0, theirs = 0;
    u8 pk[PK_SIZE];
    u32 partner = 0;
    if (ReadPartySlot(slot, pk) && SpeciesOf(pk, mine) && (mine == KARRABLAST || mine == SHELMET) &&
        given && SpeciesOf(given, theirs) && (theirs == KARRABLAST || theirs == SHELMET))
        partner = NewMon(given);
    u16 into = 0;
    u32 route = 0;
    bool evolves = reinterpret_cast<bool (*)(u32, u32, u16*, u32*)>(FN_TRADE_EVOLVES)(mon, partner, &into, &route);
    if (partner) DeleteMon(partner);
    if (!evolves || !into || into > 721) return false;
    s_param = ShinkaParam();
    s_param.party = PARTY_OBJECT;
    s_param.index = (u32)slot;
    s_param.species = into;
    s_param.route = route;
    s_param.flags[3] = 1;
    species = into;
    s_armed = true;
    return true;
}

void DisarmTradeEvolution() { s_armed = false; }

bool TradeEvolutionArmed() { return s_armed; }

} // namespace game
