#pragma once

// Look mods between players: what of ours LOOK_MANIFEST lists, what the others list, and which
// archives each player's puppets mount. Shared files are not fetched yet, so a puppet shows its
// player's look only while that look is the game's own; otherwise it shows the game's look or our
// own installed one, as the player's choice and the fallback setting say.

#include "looks/Choices.hpp"
#include "looks/Cosmetics.hpp"
#include "looks/Slots.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace twili::looks {

enum class SlotState : uint8_t { Vanilla, Shared, Unshared };
const char* slotStateId(SlotState state);

inline constexpr uint32_t kMaxLookFileBytes = 8u << 20;
inline constexpr uint64_t kMaxLookPlayerBytes = 24u << 20;
inline constexpr size_t kMaxLookFiles = 64;

struct PeerMod {
    std::string id, name, version, author;
};
struct PeerFile {
    LookArc arc = LookArc::Kmdl;
    std::string name, sha;
    uint32_t size = 0;
};
struct PeerSlot {
    SlotState state = SlotState::Vanilla;
    std::vector<PeerMod> mods;
    std::vector<PeerFile> files;
    uint64_t bytes = 0;
};
struct PeerLooks {
    std::array<std::optional<PeerSlot>, kLookTypeCount> slots;
};

// ModsService: we can tell what we have installed. Receiving needs OverlayService as well.
bool canSendLooks();
bool canReceiveLooks();

// What a player lists of their looks, null when nothing (or nothing received yet).
const PeerLooks* peerLooks(uint32_t clientId);
// The `looks` of their LOOK_MANIFEST; a bad entry drops all of them.
void takePeerLooks(uint32_t clientId, const nlohmann::json& looks);

// One look type of ours as the manifest lists it at `scope`, and why it is not shared (a type
// the last manifest could not fit under the relay's size limit included).
struct OwnSlot {
    SlotState state = SlotState::Vanilla;
    std::string why;
    uint64_t bytes = 0;
    size_t files = 0;
};
OwnSlot ownSlot(LookType type, ShareScope scope);
// Ours for LOOK_MANIFEST: an entry per type, none while sharing is Off or the scan is not done.
// Too big for the relay, the mods lose their version and author, then the last shared types go
// out unshared.
nlohmann::json manifestLooks(ShareScope scope);
std::string manifestLooksSummary(const nlohmann::json& looks);

// What a player's puppets of one type show right now.
enum class PuppetSource : uint8_t { MyOwn, GameDefault };
struct ResolvedLook {
    PuppetSource source = PuppetSource::MyOwn;
    LookChoice choice = LookChoice::Theirs;
    // Theirs: they share a look of their own and we show the fallback instead, because...
    const char* fallback = nullptr;
};
ResolvedLook resolveLook(uint32_t clientId, LookType type);

// The archive a puppet of the player mounts; false while its base copy is still being made.
bool puppetArcPath(uint32_t clientId, LookArc arc, const char*& out);
// Bit per LookArc of `arcs`: mounted from its base copy. A puppet whose bits change respawns.
uint32_t puppetSignature(uint32_t clientId, std::span<const LookArc> arcs);
// A puppet (or its Epona's) asks before it respawns for a new look: our own choices for that
// player, a block or our fallback setting go at once, a player's new manifest at most every 10 s.
bool mayRespawn(uint32_t clientId, bool horse);

// Their shared look on their row: blocked by us or not.
bool lookBlockable(uint32_t clientId);
bool lookBlocked(uint32_t clientId);
void setLookBlocked(uint32_t clientId, bool blocked);

// Every tick, connected or not.
void updateLooks();
void resetLooksSession();
void shutdownLooks();

// The Players tab's row tag ("" for none), a player's listed mods, the Looks tab's lines.
std::string looksPlayerTag(uint32_t clientId);
std::string modListText(uint32_t clientId);
std::vector<std::string> localLooksLines();

}  // namespace twili::looks
