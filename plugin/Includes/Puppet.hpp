// "Real models" mode — spawns a trainer per remote player and drives it. Writes game memory.
#pragma once
#include "Session.hpp"
#include "Game.hpp"

namespace coop {

// Call once per frame with the smoothed remote positions. talkingTo = the remote we're talking
// to (its trainer turns to face us), 0 for none.
void UpdatePuppets(bool enabled, const game::PlayerState& me, const Remote* remotes, u8 myId, int talkingTo = 0);

// True if remote player `id` is currently shown by a spawned trainer.
bool IsPuppeted(int id);

} // namespace coop
