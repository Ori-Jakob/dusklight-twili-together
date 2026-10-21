#pragma once

// Per player, how their puppets look to us: Their Look (one choice for all their puppets) with an
// optional choice per look type, and the looks we blocked. Saved in data_dir/looks.json under
// the player's install id (their name while the relay passes none), next to our own install id.
// The handshake sends an id derived from ours and the relay's host, so it stays the same on one
// relay and links nothing across relays. Unauthenticated: a convenience, not an identity.

#include "looks/Slots.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace twili::looks {

enum class LookChoice : uint8_t { Theirs, GameDefault, MyOwn };

const char* lookChoiceId(LookChoice choice);    // "theirs", "default", "mine"
const char* lookChoiceName(LookChoice choice);  // "Theirs", "Game Default", "My Own"
std::optional<LookChoice> parseLookChoice(std::string_view id);

// Reads looks.json (a new install id when there is none). At start.
void loadChoices();
// Writes a pending change and waits for the write. At shutdown.
void flushChoices();
// Every tick: writes changes a moment after the last one, off the game thread.
void tickChoices();
const std::string& installId();
// What the handshake sends to the relay at `relayUrl`: 32 hex digits of SHA-256 of our install id
// and the relay's host.
std::string relayInstallId(std::string_view relayUrl);
bool validInstallId(std::string_view id);

LookChoice lookChoice(uint32_t clientId);
std::optional<LookChoice> typeChoice(uint32_t clientId, LookType type);
// The type's own choice, else Their Look.
LookChoice effectiveChoice(uint32_t clientId, LookType type);
void setLookChoice(uint32_t clientId, LookChoice choice);
void setTypeChoice(uint32_t clientId, LookType type, std::optional<LookChoice> choice);

bool shaBlocked(const std::string& sha);
void setShasBlocked(const std::vector<std::string>& shas, bool blocked);

// Where the file is (empty without a data directory).
std::string choicesPath();

}  // namespace twili::looks
