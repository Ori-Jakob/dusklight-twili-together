#pragma once

// What a look is made of: the four look types, the archives the puppets load them from and the
// entries of those archives that belong to a look (the allowlist).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace twili::looks {

// Also the per-type override of Their Look.
enum class LookType : uint8_t { Link, Wolf, Midna, Epona };
inline constexpr size_t kLookTypeCount = 4;

const char* lookTypeId(LookType type);    // "link", as sent
const char* lookTypeName(LookType type);  // "Link", as shown
std::optional<LookType> parseLookType(std::string_view id);

// The order of DummyPlayer's private archives, then Epona's.
enum class LookArc : uint8_t {
    Kmdl, Mmdl, Zmdl, Bmdl, Alink, HyShd, CWShd, SWShd, Wmdl, Midna, Horse,
};
inline constexpr size_t kLookArcCount = 11;

const char* arcName(LookArc arc);         // "Kmdl"
const char* arcDefaultPath(LookArc arc);  // "/res/Object/Kmdl.arc"
// Where the puppets mount the disc's own copy (Game default).
const char* arcAliasPath(LookArc arc);
std::optional<LookArc> arcByName(std::string_view name);
std::optional<LookArc> arcByPath(std::string_view discPath);
// The look type whose choice picks the archive a puppet mounts (Wmdl: the wolf's, until Midna's
// entries in it can come from elsewhere).
LookType arcType(LookArc arc);

// The type an entry of `arc` belongs to (ASCII case-insensitive), nullopt when it is not part of a
// look.
std::optional<LookType> entryType(LookArc arc, std::string_view name);
struct AllowedEntry {
    const char* name;  // lowercase
    LookType type;
};
std::span<const AllowedEntry> allowedEntries(LookArc arc);

bool iequals(std::string_view a, std::string_view b);

}  // namespace twili::looks
