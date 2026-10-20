// Battles with a friend's team: a trainer battle (started by our talk script, Talk.cpp) whose
// opponent's party is replaced with the friend's party as it arrived over the network. The game's
// own trainer AI plays it; the trainer is a borrowed rival (Brendan/May) so it looks like them.
//
// FUN_00367804(setup, gamedata, client, party, trainerId, heap) loads one trainer client of the
// battle setup: its trainer data, then its party (FUN_004550A0, from the trainer's Pokemon data),
// then the setup's level offset. We let it run and then overwrite the party it built. A party is 6
// pointers to Pokemon objects (all 6 allocated), u8 count @0x18 and u8 lead @0x19, the same as the
// player's party, whose objects (vtable 0x005DE89C) hold the encrypted data at +4 and the encrypted
// party stats at +8 (Battle.cpp). See re/NOTES.md, "Trainer battles and the battle setup".
//
// The trainer's name goes in too: the client's trainer data (setup+0x6C + client*4, 0x38 bytes,
// FUN_001F0558) keeps the name in a StrBuf at +0x28, which FUN_0045564C fills from the
// trainer-name text (file 22) by trainer id. We overwrite it with the friend's name.
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstring>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

constexpr u32 FN_LOAD_TRAINER = 0x00367804;
constexpr u32 OPPONENT = 1;
constexpr u32 PARTY_COUNT_OFF = 0x18, PARTY_LEAD = 0x19;
constexpr u32 MON_VTABLE = 0x005DE89C, MON_DATA = 4, MON_STATS = 8;
constexpr u32 SETUP_TRAINER = 0x6C, TRAINER_NAME = 0x28;
// StrBuf, as Talk.cpp's GetString fills it: +4 u16 buffer, +8 u16 capacity, +0xA u16 length, +0xC u8 valid.
constexpr u32 STR_BUF = 4, STR_CAP = 8, STR_LEN = 0xA, STR_VALID = 0xC;

Hook s_hook;
u8 s_party[6][PK_SIZE];
char s_name[16] = {0};
volatile int s_count = 0;
volatile bool s_armed = false, s_started = false;

bool HeapPtr(u32 a, u32& v) {
    return Process::Read32(a, v) && v >= 0x08000000 && v < 0x10000000 && (v & 3) == 0;
}

bool PutParty(u32 party) {
    int n = s_count;
    for (int i = 0; i < n; i++) {
        u32 mon = 0, vt = 0, data = 0, stats = 0;
        if (!HeapPtr(party + i * 4, mon) || !Process::Read32(mon, vt) || vt != MON_VTABLE ||
            !HeapPtr(mon + MON_DATA, data) || !HeapPtr(mon + MON_STATS, stats))
            return false;
    }
    for (int i = 0; i < n; i++) {
        u32 mon = *reinterpret_cast<u32*>(party + i * 4);
        std::memcpy(*reinterpret_cast<void**>(mon + MON_DATA), s_party[i], PK_DATA);
        std::memcpy(*reinterpret_cast<void**>(mon + MON_STATS), s_party[i] + PK_DATA, PK_STATS);
    }
    *reinterpret_cast<volatile u8*>(party + PARTY_COUNT_OFF) = (u8)n;
    *reinterpret_cast<volatile u8*>(party + PARTY_LEAD) = 0;
    return true;
}

// Writes s_name into the client's trainer name; false (name left alone) on a layout we don't know.
bool PutName(u32 setup, u32 client) {
    u32 trainer = 0, str = 0, buf = 0;
    u16 cap = 0;
    u8 valid = 0;
    if (!s_name[0] || !HeapPtr(setup + SETUP_TRAINER + client * 4, trainer) || !HeapPtr(trainer + TRAINER_NAME, str) ||
        !HeapPtr(str + STR_BUF, buf) || !Process::Read16(str + STR_CAP, cap) || !Process::Read8(str + STR_VALID, valid) ||
        !valid || cap < 2)
        return false;
    u16* out = reinterpret_cast<u16*>(buf);
    int n = 0;
    for (; s_name[n] && n < cap - 1; n++) out[n] = (u8)s_name[n];
    out[n] = 0;
    *reinterpret_cast<volatile u16*>(str + STR_LEN) = (u16)n;
    return true;
}

void LoadTrainerHook(u32 setup, u32 gamedata, u32 client, u32 party, u32 trainerId, u32 heap) {
    HookContext::GetCurrent().OriginalFunction<void>(setup, gamedata, client, party, trainerId, heap);
    if (s_armed && client == OPPONENT && party && s_count > 0 && PutParty(party)) {
        PutName(setup, client);
        s_armed = false;
        s_started = true;
    }
}

} // namespace

bool InstallFriendBattleHook() {
    s_hook.InitializeForMitm(FN_LOAD_TRAINER, (u32)LoadTrainerHook);
    return s_hook.Enable() == HookResult::Success;
}

void ArmFriendBattle(const u8* party, int count, const char* name) {
    std::strncpy(s_name, name ? name : "", sizeof s_name - 1);
    s_name[sizeof s_name - 1] = 0;
    if (count > 6) count = 6;
    for (int i = 0; i < count; i++) {
        std::memcpy(s_party[i], party + i * PK_SIZE, PK_SIZE);
        HealPokemon(s_party[i]); // a friendly battle: their team at full strength
    }
    s_count = count;
    s_started = false;
    s_armed = count > 0;
}

void DisarmFriendBattle() {
    s_armed = false;
    s_started = false;
}

bool FriendBattleStarted() { return s_started; }

} // namespace game
