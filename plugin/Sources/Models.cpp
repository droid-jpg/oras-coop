// Friends' own trainer models. Only models in the map's resident list get their 3D data loaded,
// and the list is built while a map loads. We wrap the list loader (FUN_003F7728) and, the moment
// it starts a new map's list, append descriptors for the models our friends use — the game then
// loads them like any other map NPC.
// Verified on AS 1.4: a May trainer added this way is fully visible and animated.
//
// Brendan/May have known descriptors. Any other model (another outfit, a game whose player model
// was swapped) gets its descriptor from the game's own descriptor archive, read ahead of time on
// the plain field (PrepareWantedModels), never inside the loader while its list read is running.
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstdio>
#include <cstring>
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

// Descriptors read from the game (models other than Brendan/May). Game thread only.
struct Cached {
    u16 model = 0;
    bool ok = false;           // read; false + model set = the game has none (don't retry)
    u32 d[RES_SIZE / 4];
};
constexpr int CACHE = 8;
Cached s_cache[CACHE];
int s_cacheNext = 0;

// What the loader hook appends, in priority order, with each descriptor (game thread only).
struct Wanted {
    int count = 0;
    u16 model[MAX_WANTED_MODELS];
    u32 d[MAX_WANTED_MODELS][RES_SIZE / 4];
};
Wanted s_wanted;
Hook s_hook;

const Cached* FindCached(u16 model) {
    for (const Cached& c : s_cache) if (c.model == model) return &c;
    return nullptr;
}

// Descriptors read before are kept on the SD card (0x18 bytes each, next to the plugin), so a
// friend's outfit is loaded from the first map after Continue: that map loads before we could
// read anything from the game.
const char* const CACHE_PATH = "oras-mmo-models.bin";

void LoadCacheFile() {
    File f(CACHE_PATH, File::READ);
    if (!f.IsOpen()) return;
    u32 d[RES_SIZE / 4];
    u32 entries = (u32)f.GetSize() / RES_SIZE; // File::Read doesn't fail at the end of the file
    for (u32 e = 0; e < entries && s_cacheNext < CACHE && f.Read(d, RES_SIZE) == 0; e++) {
        u16 model = (u16)(d[0] & 0xFFFF);
        if (!model || model >= MODEL_LIMIT || IsPlayerModel(model) || FindCached(model)) continue;
        Cached& c = s_cache[s_cacheNext++];
        c.model = model;
        c.ok = true;
        std::memcpy(c.d, d, RES_SIZE);
    }
    s_cacheNext %= CACHE;
}

void SaveCacheFile() {
    File f(CACHE_PATH, File::RWC | File::TRUNCATE);
    if (!f.IsOpen()) return;
    for (const Cached& c : s_cache) if (c.ok) f.Write(c.d, RES_SIZE);
}

bool DescriptorFor(u16 model, u32* d) {
    if (IsPlayerModel(model)) { MakeDescriptor(model, d); return true; }
    const Cached* c = FindCached(model);
    if (!c || !c->ok) return false;
    std::memcpy(d, c->d, RES_SIZE);
    return true;
}

void AppendWanted(u32 manager) {
    volatile u32* count = reinterpret_cast<volatile u32*>(manager + RES_COUNT);
    volatile u32* count2 = reinterpret_cast<volatile u32*>(manager + RES_COUNT2);
    u32 n = *count;
    if (n == 0 || n > RES_MAX) return;
    for (int w = 0; w < s_wanted.count && n < RES_MAX; w++) {
        u16 model = s_wanted.model[w];
        bool present = false;
        for (u32 i = 0; i < n && !present; i++)
            present = *reinterpret_cast<volatile u16*>(manager + RES_TABLE + i * RES_SIZE) == model;
        if (present) continue;
        // Slot n is just past the list file's entries (the async read fills slots 1..n-1).
        volatile u32* dst = reinterpret_cast<volatile u32*>(manager + RES_TABLE + n * RES_SIZE);
        for (u32 i = 0; i < RES_SIZE / 4; i++) dst[i] = s_wanted.d[w][i];
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

u16 s_asked[MAX_WANTED_MODELS];
int s_askedCount = 0;

} // namespace

void SetWantedModels(const u16* models, int count) {
    if (count > MAX_WANTED_MODELS) count = MAX_WANTED_MODELS;
    s_askedCount = count;
    for (int i = 0; i < count; i++) s_asked[i] = models[i];
    // Only models with a descriptor go in; the rest follow once PrepareWantedModels has read theirs.
    Wanted w;
    for (int i = 0; i < count; i++) {
        if (!models[i] || models[i] >= MODEL_LIMIT) continue;
        bool dup = false;
        for (int k = 0; k < w.count && !dup; k++) dup = w.model[k] == models[i];
        if (dup || !DescriptorFor(models[i], w.d[w.count])) continue;
        w.model[w.count++] = models[i];
    }
    s_wanted = w;
}

void PrepareWantedModels() {
    for (int i = 0; i < s_askedCount; i++) {
        u16 model = s_asked[i];
        if (!model || model >= MODEL_LIMIT || IsPlayerModel(model) || FindCached(model)) continue;
        Cached& c = s_cache[s_cacheNext];
        s_cacheNext = (s_cacheNext + 1) % CACHE;
        c = Cached();
        c.model = model;
        c.ok = ReadModelDescriptor(model, c.d);
        if (c.ok) SaveCacheFile();
        return; // one read per frame
    }
}

int DescribeWantedModels(char* out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < s_askedCount && n < len; i++) {
        u16 m = s_asked[i];
        const Cached* c = FindCached(m);
        const char* st = IsPlayerModel(m) ? "" : !c ? " (not read)" : c->ok ? " (read)" : " (no descriptor)";
        n += snprintf(out + n, len - n, "%s%X%s%s", i ? ", " : "", (unsigned)m, st, IsModelResident(m) ? " loaded" : "");
    }
    return n;
}

bool ModelUsable(u16 model) {
    if (IsPlayerModel(model)) return true;
    const Cached* c = FindCached(model);
    return c && c->ok;
}

bool InstallModelHook() {
    LoadCacheFile();
    s_hook.InitializeForMitm(FN_LOAD_LIST, (u32)LoadListHook);
    return s_hook.Enable() == HookResult::Success;
}

} // namespace game
