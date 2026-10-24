// The save data the game keeps in memory (read-only): badges and the PC boxes.
// The save is one boot-time allocation, at the same address in every run seen (like the player
// object and the party). Its blocks are packed differently from the save file; found by matching
// the save file's blocks in a RAM dump (re/NOTES.md, "Save data in memory").
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstring>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

// Save file block 0x4200 (money u32 @+8, badges u8 @+0xC, one bit per gym in order) is at
// 0x08C71DB8. Money is capped at 9,999,999 by the game, which makes a cheap sanity check.
constexpr u32 SITUATION = 0x08C71DB8, MONEY = SITUATION + 8, BADGES = SITUATION + 0xC;
constexpr u32 MONEY_MAX = 9999999;

// Save file 0x33000: 31 boxes x 30 slots of encrypted PK6 data (0xE8 bytes, no party stats).
constexpr u32 BOXES = 0x08C9E134;

void Crypt(u8* b, u32 seed, int start, int end) {
    for (int i = start; i + 1 < end; i += 2) {
        seed = seed * 0x41C64E6D + 0x6073;
        u16 w = (u16)(b[i] | b[i + 1] << 8) ^ (u16)(seed >> 16);
        b[i] = (u8)w;
        b[i + 1] = (u8)(w >> 8);
    }
}

} // namespace

bool ReadBadges(u8& out) {
    u32 money = 0;
    if (!Process::Read32(MONEY, money) || money > MONEY_MAX) return false;
    return Process::Read8(BADGES, out);
}

bool ReadMoney(u32& out) {
    return Process::Read32(MONEY, out) && out <= MONEY_MAX;
}

bool WriteMoney(u32 money) {
    u32 now = 0;
    return money <= MONEY_MAX && ReadMoney(now) && Process::Write32(MONEY, money);
}

u32 BoxSlotEC(int slot) {
    u32 ec = 0;
    if (slot < 0 || slot >= BOX_SLOTS || !Process::Read32(BOXES + slot * PK_DATA, ec)) return 0;
    return ec;
}

bool ReadBoxSlot(int slot, u8* out) {
    if (slot < 0 || slot >= BOX_SLOTS) return false;
    std::memcpy(out, reinterpret_cast<const void*>(BOXES + slot * PK_DATA), PK_DATA);
    return true;
}

bool SpeciesOf(const u8* pk, u16& species, bool* egg) {
    u8 d[PK_DATA];
    std::memcpy(d, pk, PK_DATA);
    u32 ec = d[0] | d[1] << 8 | d[2] << 16 | (u32)d[3] << 24;
    if (!ec) return false;
    Crypt(d, ec, 8, PK_DATA);
    // The stored checksum (u16 @6) is the sum of the decrypted words: rejects empty or torn slots.
    u16 sum = 0;
    for (int i = 8; i < PK_DATA; i += 2) sum += (u16)(d[i] | d[i + 1] << 8);
    if (sum != (u16)(d[6] | d[7] << 8)) return false;
    // Species is the first word of block A, the egg flag bit 30 of the IVs (block B +0x34),
    // wherever the shuffle put them.
    static const char ORDER[24][5] = {
        "ABCD", "ABDC", "ACBD", "ACDB", "ADBC", "ADCB", "BACD", "BADC", "BCAD", "BCDA", "BDAC", "BDCA",
        "CABD", "CADB", "CBAD", "CBDA", "CDAB", "CDBA", "DABC", "DACB", "DBAC", "DBCA", "DCAB", "DCBA",
    };
    const char* order = ORDER[((ec >> 13) & 31) % 24];
    const u8* a = d + 8 + (std::strchr(order, 'A') - order) * 56;
    const u8* b = d + 8 + (std::strchr(order, 'B') - order) * 56;
    species = (u16)(a[0] | a[1] << 8);
    if (egg) *egg = (b[0x37] & 0x40) != 0;
    return species >= 1 && species <= 721;
}

bool IsEgg(const u8* pk) {
    u16 species = 0;
    bool egg = false;
    return SpeciesOf(pk, species, &egg) && egg;
}

// Affection: the current trainer's at block C +0x2B (0xA3) and the original trainer's at block D
// +0x1B (0xCB), as FUN_004D2D84 reads them for the battle.
void ClearAffection(u8* pk) {
    static const char ORDER[24][5] = {
        "ABCD", "ABDC", "ACBD", "ACDB", "ADBC", "ADCB", "BACD", "BADC", "BCAD", "BCDA", "BDAC", "BDCA",
        "CABD", "CADB", "CBAD", "CBDA", "CDAB", "CDBA", "DABC", "DACB", "DBAC", "DBCA", "DCAB", "DCBA",
    };
    u32 ec = pk[0] | pk[1] << 8 | pk[2] << 16 | (u32)pk[3] << 24;
    if (!ec) return;
    Crypt(pk, ec, 8, PK_DATA);
    const char* order = ORDER[((ec >> 13) & 31) % 24];
    pk[8 + (std::strchr(order, 'C') - order) * 56 + 0x2B] = 0;
    pk[8 + (std::strchr(order, 'D') - order) * 56 + 0x1B] = 0;
    u16 sum = 0;
    for (int i = 8; i < PK_DATA; i += 2) sum += (u16)(pk[i] | pk[i + 1] << 8);
    pk[6] = (u8)sum;
    pk[7] = (u8)(sum >> 8);
    Crypt(pk, ec, 8, PK_DATA);
}

void HealPokemon(u8* pk) {
    u32 ec = pk[0] | pk[1] << 8 | pk[2] << 16 | (u32)pk[3] << 24;
    u8* st = pk + PK_DATA; // party stats: status u32 @0, level @4, HP @8, max HP @0xA
    Crypt(st, ec, 0, PK_STATS);
    std::memset(st, 0, 4);
    st[8] = st[0xA];
    st[9] = st[0xB];
    Crypt(st, ec, 0, PK_STATS);
}

} // namespace game
