// Activity notices: watches our own game for things friends would like to hear about and queues
// them as events (T_EVENT), which friends show as notices ("Sami caught a Wingull!").
//   Caught:  a Pokemon that wasn't in the party or the PC before a battle is there after it (a
//            catch is the only way one appears during a battle; trades and eggs happen on the field).
//   Evolved: a party Pokemon (same encryption constant) has a different species than before.
//   Badge:   a new bit in the badge byte.
// Read-only; game thread.
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstring>
#include "Session.hpp"
#include "Game.hpp"

using namespace CTRPluginFramework;

namespace coop {

namespace {

constexpr u64 POLL_MS = 500;

struct Mon { u32 ec = 0; u16 species = 0; };

struct Watch {
    bool ready = false;          // first look done (nothing is reported for what we start with)
    u64 last = 0;
    u8 badges = 0;
    bool haveBadges = false;
    Mon party[6];                // the party at the last look on the field
    bool inBattle = false;       // a battle started since the last look on the field
    u32 before[6 + game::BOX_SLOTS]; // party + box ECs when it started
};
Watch g_watch;

// Reads the party, and publishes it (eggs left out) for friends who want to battle it.
void ReadParty(Mon* out) {
    int count = game::PartyCount();
    u8 pub[6][game::PK_SIZE];
    int pubCount = 0;
    for (int i = 0; i < 6; i++) {
        out[i] = Mon();
        u8 pk[game::PK_SIZE];
        bool egg = false;
        if (i < count && game::ReadPartySlot(i, pk) && game::SpeciesOf(pk, out[i].species, &egg)) {
            std::memcpy(&out[i].ec, pk, 4);
            if (!egg) std::memcpy(pub[pubCount++], pk, game::PK_SIZE);
        }
    }
    g_session.lock.Lock();
    std::memcpy(g_session.localParty, pub, sizeof pub);
    g_session.localPartyCount = pubCount;
    g_session.lock.Unlock();
}

bool WasThere(u32 ec) {
    for (u32 b : g_watch.before) if (b == ec) return true;
    return false;
}

void Queue(u8 kind, u16 species, u16 from = 0, u8 value = 0) {
    g_session.lock.Lock();
    if (g_session.eventCount < MAX_EVENTS) {
        Event& e = g_session.events[g_session.eventCount++];
        e.kind = kind;
        e.value = value;
        e.species = species;
        e.from = from;
    }
    g_session.lock.Unlock();
}

void SnapshotBeforeBattle() {
    for (int i = 0; i < 6; i++) g_watch.before[i] = g_watch.party[i].ec;
    for (int s = 0; s < game::BOX_SLOTS; s++) g_watch.before[6 + s] = game::BoxSlotEC(s);
}

// Back on the field after a battle: anything new in the party or the PC was caught in it.
void ReportCatches(const Mon* party) {
    for (int i = 0; i < 6; i++) {
        if (party[i].ec && !WasThere(party[i].ec)) Queue(EVENT_CAUGHT, party[i].species);
    }
    for (int s = 0; s < game::BOX_SLOTS; s++) {
        u32 ec = game::BoxSlotEC(s);
        if (!ec || ec == g_watch.before[6 + s] || WasThere(ec)) continue;
        u8 pk[game::PK_DATA];
        u16 species = 0;
        if (game::ReadBoxSlot(s, pk) && game::SpeciesOf(pk, species)) Queue(EVENT_CAUGHT, species);
    }
}

void ReportEvolutions(const Mon* party) {
    for (int i = 0; i < 6; i++) {
        const Mon& now = party[i];
        if (!now.ec) continue;
        for (const Mon& was : g_watch.party) {
            if (was.ec == now.ec && was.species && was.species != now.species)
                Queue(EVENT_EVOLVED, now.species, was.species);
        }
    }
}

void ReportBadges() {
    u8 badges = 0;
    if (!game::ReadBadges(badges)) return;
    if (g_watch.haveBadges) {
        for (int b = 0; b < 8; b++) {
            if ((badges & (1 << b)) && !(g_watch.badges & (1 << b))) Queue(EVENT_BADGE, 0, 0, (u8)(b + 1));
        }
    }
    g_watch.badges = badges;
    g_watch.haveBadges = true;
}

} // namespace

void UpdateActivity(const game::PlayerState& me, bool quiet) {
    u64 now = osGetTime();
    if (now - g_watch.last < POLL_MS) return;
    g_watch.last = now;

    if (game::BattleModuleLoaded()) {
        if (g_watch.ready && !g_watch.inBattle) {
            SnapshotBeforeBattle();
            g_watch.inBattle = true;
        }
        return;
    }
    // Look only on the plain field: the evolution scene, the party screen and the bag run in
    // the field module's place, and the intro/title has the save loaded but nothing to report.
    if (!me.valid || !me.inField || !game::FieldModuleLoaded()) return;

    Mon party[6];
    ReadParty(party);
    if (g_watch.ready && !quiet) {
        if (g_watch.inBattle) ReportCatches(party);
        ReportEvolutions(party);
    }
    ReportBadges();
    std::memcpy(g_watch.party, party, sizeof party);
    g_watch.inBattle = false;
    g_watch.ready = true;
}

} // namespace coop
