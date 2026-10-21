#pragma once

// The disc's own copies of the look archives at /res/Object/tt/base/, for puppets shown in the
// game's look: OverlayService's add_base_alias where the host has it, else the archive read
// from the disc ourselves and overlaid as a buffer (entry overlays do not apply to a raw read, so
// that gives the disc's archive unless one of ours replaces it whole). Registered once, when a
// puppet first needs one, and kept until the mod unloads.

#include "looks/Slots.hpp"

#include <cstdint>

namespace twili::looks {

enum class AliasState : uint8_t { None, Pending, Ready, Failed };

// Asks for the archive's alias on first use.
AliasState baseAlias(LookArc arc);
// What the stopgap read could not give (for the log and the Looks tab), or null.
const char* aliasProblem(LookArc arc);
// Whether the archive's base copy came from the stopgap's read.
bool aliasFromDisc(LookArc arc);
// For tests: base copies made from now on take the stopgap's read even where add_base_alias exists.
void forceStopgapAliases(bool on);
// Every tick: registers what was asked for (all in one frame) and finishes stopgap reads.
void tickAliases();
void shutdownAliases();

}  // namespace twili::looks
