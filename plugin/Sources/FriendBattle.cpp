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
//
// A live battle (LiveBattle.cpp) needs both games to build exactly the same battle, so the setup
// is evened out further: our own team (setup+0x4C, a copy of the party made by FUN_003B6720 just
// before) is healed like theirs, both lose their Pokemon-Amie affection (the battle gives it only
// to the player's side, FUN_00766A64), EXP is off (setup+0xA8 bit 0x2000, checked with 0x200 by
// the EXP code FUN_0075F034), and the battle style is Set (config bit 3, read by DllBattle into
// main+0x818: in Shift style only the player is asked to switch after a knock-out, FUN_0070508C).
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
constexpr u32 SETUP_OWN_PARTY = 0x4C, SETUP_FLAGS = 0xA8, SETUP_CONFIG = 0x88;
constexpr u32 FLAG_NO_EXP = 0x2000;
constexpr u32 CONFIG_BITS = 4, CONFIG_STYLE_SET = 1 << 3; // FUN_004EA27C: (config+4 & 0xF) >> 3
// StrBuf, as Talk.cpp's GetString fills it: +4 u16 buffer, +8 u16 capacity, +0xA u16 length, +0xC u8 valid.
constexpr u32 STR_BUF = 4, STR_CAP = 8, STR_LEN = 0xA, STR_VALID = 0xC;

Hook s_hook;
u8 s_party[6][PK_SIZE];
char s_name[16] = {0};
volatile int s_count = 0;
volatile bool s_armed = false, s_started = false;
bool s_live = false;
u32 s_config = 0, s_configWas = 0; // battle style changed for a live battle: put back afterwards

bool HeapPtr(u32 a, u32& v) {
    return Process::Read32(a, v) && v >= 0x08000000 && v < 0x10000000 && (v & 3) == 0;
}

bool PartyObjects(u32 party, int n) {
    for (int i = 0; i < n; i++) {
        u32 mon = 0, vt = 0, data = 0, stats = 0;
        if (!HeapPtr(party + i * 4, mon) || !Process::Read32(mon, vt) || vt != MON_VTABLE ||
            !HeapPtr(mon + MON_DATA, data) || !HeapPtr(mon + MON_STATS, stats))
            return false;
    }
    return true;
}

void ReadMon(u32 party, int i, u8* pk) {
    u32 mon = *reinterpret_cast<u32*>(party + i * 4);
    std::memcpy(pk, *reinterpret_cast<void**>(mon + MON_DATA), PK_DATA);
    std::memcpy(pk + PK_DATA, *reinterpret_cast<void**>(mon + MON_STATS), PK_STATS);
}

void WriteMon(u32 party, int i, const u8* pk) {
    u32 mon = *reinterpret_cast<u32*>(party + i * 4);
    std::memcpy(*reinterpret_cast<void**>(mon + MON_DATA), pk, PK_DATA);
    std::memcpy(*reinterpret_cast<void**>(mon + MON_STATS), pk + PK_DATA, PK_STATS);
}

bool PutParty(u32 party) {
    int n = s_count;
    if (!PartyObjects(party, n)) return false;
    int lead = -1;
    for (int i = 0; i < n; i++) {
        WriteMon(party, i, s_party[i]);
        if (lead < 0 && !IsEgg(s_party[i])) lead = i;
    }
    *reinterpret_cast<volatile u8*>(party + PARTY_COUNT_OFF) = (u8)n;
    *reinterpret_cast<volatile u8*>(party + PARTY_LEAD) = (u8)(lead < 0 ? 0 : lead);
    return true;
}

// Live battles: our own team as theirs is (healed, no affection), no EXP, Set style.
void EvenOut(u32 setup) {
    u32 own = 0;
    u8 count = 0;
    if (HeapPtr(setup + SETUP_OWN_PARTY, own) && Process::Read8(own + PARTY_COUNT_OFF, count) && count <= 6 &&
        PartyObjects(own, count)) {
        for (int i = 0; i < count; i++) {
            u8 pk[PK_SIZE];
            ReadMon(own, i, pk);
            HealPokemon(pk);
            ClearAffection(pk);
            WriteMon(own, i, pk);
        }
    }
    *reinterpret_cast<volatile u32*>(setup + SETUP_FLAGS) |= FLAG_NO_EXP;
    u32 config = 0, bits = 0;
    if (HeapPtr(setup + SETUP_CONFIG, config) && Process::Read32(config + CONFIG_BITS, bits)) {
        if (!s_config) { s_config = config; s_configWas = bits; }
        *reinterpret_cast<volatile u32*>(config + CONFIG_BITS) = bits | CONFIG_STYLE_SET;
    }
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
        if (s_live) EvenOut(setup);
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
    s_live = false;
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

void ArmLiveFriendBattle(const u8* party, int count, const char* name) {
    ArmFriendBattle(party, count, name);
    for (int i = 0; i < s_count; i++) ClearAffection(s_party[i]);
    s_live = true;
}

void DisarmFriendBattle() {
    s_armed = false;
    s_started = false;
}

void RestoreBattleStyle() {
    if (!s_config) return;
    u32 bits = 0;
    if (Process::Read32(s_config + CONFIG_BITS, bits))
        Process::Write32(s_config + CONFIG_BITS, (bits & ~CONFIG_STYLE_SET) | (s_configWas & CONFIG_STYLE_SET));
    s_config = 0;
}

bool FriendBattleStarted() { return s_started; }

} // namespace game
