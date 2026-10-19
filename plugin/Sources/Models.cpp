// Friends' own trainer models (Brendan/May). Only models in the map's resident list get their
// 3D data loaded, and the list is built while a map loads. We wrap the list loader
// (FUN_003F7728) and, the moment it starts a new map's list, append descriptors for the
// player models our friends use — the game then loads them like any other map NPC.
// Verified on AS 1.4: a May trainer added this way is fully visible and animated.
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

constexpr u32 FN_LOAD_LIST = 0x003F7728; // (manager, listFile) -> 1 when the list is read
constexpr u32 LIST_STATE = 0x36AA;       // u8: 0 = start a new list, 1 = reading
constexpr u32 RES_COUNT2 = 0x36C4;       // the loader writes the count twice
constexpr u32 RES_COUNT  = 0x36C8;
constexpr u32 RES_TABLE  = 0x36CC;
constexpr u32 RES_SIZE   = 0x18;
constexpr u32 RES_MAX    = 0x20;         // FUN_003F7640 asserts above this

// Descriptor for a player model, as the game builds it for the player itself (slot 0):
// u16 model, flags 01 01 04 00 01 01 01 02 01 01, zeros, u32 archive file = model id.
void MakeDescriptor(u16 model, u32* d) {
    d[0] = 0x01010000 | model;
    d[1] = 0x01010004;
    d[2] = 0x01010201;
    d[3] = 0;
    d[4] = 0;
    d[5] = model;
}

volatile u32 s_wanted = 0; // bit 0 = May, bit 1 = Brendan; written by the game thread only
Hook s_hook;

void AppendWanted(u32 manager) {
    volatile u32* count = reinterpret_cast<volatile u32*>(manager + RES_COUNT);
    volatile u32* count2 = reinterpret_cast<volatile u32*>(manager + RES_COUNT2);
    u32 n = *count;
    if (n == 0 || n > RES_MAX) return;
    const u16 models[] = { MODEL_MAY, MODEL_BRENDAN };
    for (int b = 0; b < 2; b++) {
        if (!(s_wanted & (1u << b)) || n >= RES_MAX) continue;
        u16 model = models[b];
        bool present = false;
        for (u32 i = 0; i < n && !present; i++)
            present = *reinterpret_cast<volatile u16*>(manager + RES_TABLE + i * RES_SIZE) == model;
        if (present) continue;
        // Slot n is just past the list file's entries (the async read fills slots 1..n-1).
        u32 d[RES_SIZE / 4];
        MakeDescriptor(model, d);
        volatile u32* dst = reinterpret_cast<volatile u32*>(manager + RES_TABLE + n * RES_SIZE);
        for (u32 i = 0; i < RES_SIZE / 4; i++) dst[i] = d[i];
        n++;
    }
    *count = n;
    *count2 = n;
}

u32 LoadListHook(u32 manager, u32 listFile) {
    u8 before = *reinterpret_cast<volatile u8*>(manager + LIST_STATE);
    u32 r = HookContext::GetCurrent().OriginalFunction<u32>(manager, listFile);
    if (before == 0 && *reinterpret_cast<volatile u8*>(manager + LIST_STATE) == 1) AppendWanted(manager);
    return r;
}

} // namespace

void SetWantedModels(bool may, bool brendan) { s_wanted = (may ? 1u : 0u) | (brendan ? 2u : 0u); }

bool InstallModelHook() {
    s_hook.InitializeForMitm(FN_LOAD_LIST, (u32)LoadListHook);
    return s_hook.Enable() == HookResult::Success;
}

} // namespace game
