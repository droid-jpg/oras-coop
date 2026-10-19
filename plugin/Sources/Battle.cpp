// Battles (read-only) and the party (read, and written by a finished trade).
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstring>
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace game {

namespace {

bool HeapPtr(u32 a, u32& v) {
    return Process::Read32(a, v) && v >= 0x08000000 && v < 0x10000000 && (v & 3) == 0;
}

} // namespace

// ---- Battle ----
// DllBattle keeps a global { battle object, side table } whose address is in the literal pool of
// its code at 0x0074580C (DllBattle is loaded at 0x006F3000 like DllField; the global itself is
// on the heap and moves). Side table +4: per side 6 pointers to battle Pokemon (slot 0 = the one
// out, reordered on switches) and a u32 count, 0x1C bytes per side (0 = us, 1 = the opponent).
// Battle Pokemon: +0xC u16 species, +0xE max HP, +0x10 HP, +0x18 u8 level (verified in a wild
// battle on Route 109: our three, and the Wingull after switching).
constexpr u32 BTL_GLOBAL_LITERAL = 0x0074580C;
constexpr u32 BTL_SIDES = 0x04, BTL_SIDE_SIZE = 0x1C, BTL_SIDE_COUNT = 0x18;
constexpr u32 BP_SPECIES = 0x0C, BP_MAX_HP = 0x0E, BP_HP = 0x10, BP_LEVEL = 0x18;
constexpr u16 MAX_SPECIES = 721;

bool ReadBattle(BattleState& out) {
    out = BattleState();
    if (!BattleModuleLoaded()) return false;
    u32 global = 0, table = 0;
    if (!HeapPtr(BTL_GLOBAL_LITERAL, global) || !HeapPtr(global + 4, table)) return false;
    for (int side = 0; side < 2; side++) {
        u32 base = table + BTL_SIDES + side * BTL_SIDE_SIZE;
        u32 count = 0;
        if (!Process::Read32(base + BTL_SIDE_COUNT, count) || count > BATTLE_PARTY) return false;
        u8 n = 0;
        for (u32 i = 0; i < count; i++) {
            u32 bp = 0;
            if (!HeapPtr(base + i * 4, bp)) continue;
            BattleMon m;
            Process::Read16(bp + BP_SPECIES, m.species);
            Process::Read16(bp + BP_MAX_HP, m.maxHp);
            Process::Read16(bp + BP_HP, m.hp);
            Process::Read8(bp + BP_LEVEL, m.level);
            if (!m.species || m.species > MAX_SPECIES || !m.maxHp || m.hp > m.maxHp || !m.level || m.level > 100)
                return false; // not the layout we know (yet): show nothing rather than garbage
            out.mon[side][n++] = m;
        }
        out.count[side] = n;
    }
    out.active = out.count[0] > 0 && out.count[1] > 0;
    return out.active;
}

// ---- Party ----
// The party is 6 pointers at 0x08CFB1E0 followed by a u32 count (allocated at boot, same address
// in every run seen). Each slot object (vtable 0x005DE89C): +4 -> the encrypted Pokemon data
// (0xE8 bytes, the usual PK6 layout), +8 -> its encrypted party stats (0x1C bytes: status, level,
// HP...). Both are encrypted with keys derived from the data's own first word, so copying both
// blocks from another game gives a consistent Pokemon.
constexpr u32 PARTY_ARRAY = 0x08CFB1E0, PARTY_COUNT = PARTY_ARRAY + 6 * 4;
constexpr u32 SLOT_VTABLE = 0x005DE89C, SLOT_DATA = 4, SLOT_STATS = 8;

namespace {

bool SlotBlocks(int slot, u32& data, u32& stats) {
    u32 count = 0, obj = 0, vt = 0;
    if (slot < 0 || slot >= 6 || !Process::Read32(PARTY_COUNT, count) || count == 0 || count > 6 ||
        (u32)slot >= count || !HeapPtr(PARTY_ARRAY + slot * 4, obj) || !Process::Read32(obj, vt) ||
        vt != SLOT_VTABLE)
        return false;
    return HeapPtr(obj + SLOT_DATA, data) && HeapPtr(obj + SLOT_STATS, stats);
}

void Crypt(u8* b, u32 seed, int start, int end) {
    for (int i = start; i + 1 < end; i += 2) {
        seed = seed * 0x41C64E6D + 0x6073;
        u16 w = (u16)(b[i] | b[i + 1] << 8) ^ (u16)(seed >> 16);
        b[i] = (u8)w;
        b[i + 1] = (u8)(w >> 8);
    }
}

} // namespace

int PartyCount() {
    u32 count = 0;
    return Process::Read32(PARTY_COUNT, count) && count <= 6 ? (int)count : 0;
}

bool ReadPartySlot(int slot, u8* out) {
    u32 data = 0, stats = 0;
    if (!SlotBlocks(slot, data, stats)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(data), PK_DATA);
    std::memcpy(out + PK_DATA, reinterpret_cast<const void*>(stats), PK_STATS);
    return true;
}

bool WritePartySlot(int slot, const u8* in) {
    u32 data = 0, stats = 0;
    if (!SlotBlocks(slot, data, stats)) return false;
    std::memcpy(reinterpret_cast<void*>(data), in, PK_DATA);
    std::memcpy(reinterpret_cast<void*>(stats), in + PK_DATA, PK_STATS);
    return true;
}

bool DescribePokemon(const u8* pk, u16& species, u8& level) {
    static const char ORDER[24][5] = {
        "ABCD", "ABDC", "ACBD", "ACDB", "ADBC", "ADCB", "BACD", "BADC", "BCAD", "BCDA", "BDAC", "BDCA",
        "CABD", "CADB", "CBAD", "CBDA", "CDAB", "CDBA", "DABC", "DACB", "DBAC", "DBCA", "DCAB", "DCBA",
    };
    u8 d[PK_SIZE];
    std::memcpy(d, pk, PK_SIZE);
    u32 ec = d[0] | d[1] << 8 | d[2] << 16 | (u32)d[3] << 24;
    Crypt(d, ec, 8, PK_DATA);
    Crypt(d + PK_DATA, ec, 0, PK_STATS);
    // Block A (species first) sits wherever the shuffle put it.
    int sv = ((ec >> 13) & 31) % 24;
    int posA = (int)(std::strchr(ORDER[sv], 'A') - ORDER[sv]);
    const u8* a = d + 8 + posA * 56;
    species = (u16)(a[0] | a[1] << 8);
    level = d[PK_DATA + 4];
    return species > 0 && species <= MAX_SPECIES && level >= 1 && level <= 100;
}

} // namespace game
