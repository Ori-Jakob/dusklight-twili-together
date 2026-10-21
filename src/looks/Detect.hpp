#pragma once

// The look mods the player has installed, through ModsService: per look type, the allowlisted
// archive entries whose bytes differ from the disc's, with their SHA-256. The listing runs on the
// game thread when the mods change; reading and hashing run on a worker thread.

#include "looks/Slots.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace twili::looks {

struct LookFile {
    LookArc arc = LookArc::Kmdl;
    std::string name;  // lowercase, as allowlisted
    std::string sha;   // 64 hex digits
    uint32_t size = 0;
    std::string modId;
};

struct LookMod {
    std::string id, name, version, author;
    // False when its mod.json says "multiplayer": {"share": false}.
    bool shareable = true;
    uint8_t types = 0;  // bit per LookType it changes
    uint64_t bytes = 0;
};

struct LocalLooks {
    // A scan of the current mods finished.
    bool known = false;
    bool scanning = false;
    std::array<std::vector<LookFile>, kLookTypeCount> files;
    // A changed entry of that type could not be read or hashed.
    std::array<bool, kLookTypeCount> unreadable{};
    std::vector<LookMod> mods;
    // Bit per LookArc: one of our mods replaces the archive or an entry of it; replaces it whole.
    uint32_t dirtyArcs = 0;
    uint32_t wholeArcs = 0;
    // Bumps whenever a scan result is taken.
    uint32_t revision = 0;
};

// False when the host has no ModsService.
bool detectAvailable();
// Starts a scan when the mods changed and takes a finished one; cheap otherwise.
void refreshLocalLooks();
const LocalLooks& localLooks();
// Whether a puppet mounting the archive's default path would get something of ours; true when we
// cannot tell.
bool archiveDirty(LookArc arc);
void shutdownDetect();

}  // namespace twili::looks
