#include "looks/Looks.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "looks/Alias.hpp"
#include "looks/Detect.hpp"
#include "looks/Inventory.hpp"

#include <mods/svc/overlay.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <map>

namespace twili::looks {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kRespawnGap = std::chrono::seconds(10);
constexpr auto kModListInterval = std::chrono::seconds(1);
constexpr size_t kMaxText = 64;
constexpr size_t kMaxSlotMods = 16;
constexpr size_t kMaxModList = 64;
// The relay drops a LOOK_MANIFEST over 16 KiB: this leaves room for the cosmetics and the rest.
constexpr size_t kMaxManifestLooksBytes = 14 * 1024;
constexpr const char* kStateIds[] = {"vanilla", "shared", "unshared"};

std::map<uint32_t, PeerLooks> sPeers;
// Bit per type the last manifest left unshared to fit.
uint8_t sTrimmed = 0;

struct Respawn {
    uint32_t own = 0;
    uint32_t blocks = 0;
    Clock::time_point at{};
};
std::map<uint64_t, Respawn> sRespawns;
// Per player: how often we blocked or unblocked their look.
std::map<uint32_t, uint32_t> sBlockActions;

bool sModListSent = false;
nlohmann::json sSentModList = nlohmann::json::array();
Clock::time_point sModListAt{};

std::string clip(std::string text) {
    return clipUtf8(std::move(text), kMaxText);
}

std::string textOf(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? clip(it->get<std::string>()) : std::string{};
}

bool validSha(const std::string& sha) {
    return sha.size() == 64 && std::ranges::all_of(sha, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::optional<SlotState> parseState(const nlohmann::json& j) {
    const std::string id = j.is_string() ? j.get<std::string>() : std::string{};
    for (size_t i = 0; i < std::size(kStateIds); i++) {
        if (id == kStateIds[i]) {
            return static_cast<SlotState>(i);
        }
    }
    return std::nullopt;
}

std::string megabytes(uint64_t bytes) {
    if (bytes < 1024 * 1024) {
        return fmt::format("{} KB", std::max<uint64_t>(1, (bytes + 512) / 1024));
    }
    return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

const LookMod* localMod(const std::string& id) {
    const auto& mods = localLooks().mods;
    const auto it = std::ranges::find_if(mods, [&](const LookMod& m) { return m.id == id; });
    return it == mods.end() ? nullptr : &*it;
}

// Every type at once: they share one budget, taken in type order.
std::array<OwnSlot, kLookTypeCount> ownSlots(ShareScope scope) {
    std::array<OwnSlot, kLookTypeCount> out{};
    const LocalLooks& local = localLooks();
    uint64_t budget = kMaxLookPlayerBytes;
    size_t files = kMaxLookFiles;
    for (size_t t = 0; t < kLookTypeCount; t++) {
        OwnSlot& slot = out[t];
        const auto& list = local.files[t];
        for (const LookFile& f : list) {
            slot.bytes += f.size;
        }
        slot.files = list.size();
        if (list.empty() && !local.unreadable[t]) {
            continue;
        }
        slot.state = SlotState::Unshared;
        if (scope == ShareScope::Off) {
            slot.why = "Share My Looks is Off";
            continue;
        }
        if (local.unreadable[t]) {
            slot.why = "a file of it could not be read";
            continue;
        }
        const auto forbidding = std::ranges::find_if(list, [](const LookFile& f) {
            const LookMod* mod = localMod(f.modId);
            return mod != nullptr && !mod->shareable;
        });
        if (forbidding != list.end()) {
            const LookMod* mod = localMod(forbidding->modId);
            slot.why = fmt::format("{} does not allow sharing", mod->name.empty() ? mod->id : mod->name);
            continue;
        }
        if (std::ranges::any_of(list, [](const LookFile& f) { return f.size > kMaxLookFileBytes; })) {
            slot.why = "a file of it is over 8 MB";
            continue;
        }
        if (slot.bytes > budget || slot.files > files) {
            slot.why = "over 24 MB or 64 files with your other looks";
            continue;
        }
        budget -= slot.bytes;
        files -= slot.files;
        slot.state = SlotState::Shared;
    }
    return out;
}

const Client* clientOf(uint32_t clientId) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(clientId);
    return it == clients.end() ? nullptr : &it->second;
}

const PeerSlot* peerSlot(uint32_t clientId, LookType type) {
    const PeerLooks* peer = peerLooks(clientId);
    if (peer == nullptr) {
        return nullptr;
    }
    const auto& slot = peer->slots[static_cast<size_t>(type)];
    return slot ? &*slot : nullptr;
}

bool slotBlocked(const PeerSlot& slot) {
    return std::ranges::any_of(slot.files, [](const PeerFile& f) { return shaBlocked(f.sha); });
}

PuppetSource fallbackSource() {
    return config::getInt(config::Var::LooksFallback) == 1 ? PuppetSource::GameDefault :
                                                             PuppetSource::MyOwn;
}

// What we chose for a player's puppets: their choices, our fallback and Show Other Players' Looks.
uint32_t ownSettings(uint32_t clientId) {
    uint32_t bits = 0;
    for (size_t t = 0; t < kLookTypeCount; t++) {
        bits |= static_cast<uint32_t>(effectiveChoice(clientId, static_cast<LookType>(t))) << (t * 2);
    }
    bits |= (fallbackSource() == PuppetSource::GameDefault ? 1u : 0u) << 8;
    bits |= (config::getBool(config::Var::ShowLooks) ? 1u : 0u) << 9;
    return bits;
}

nlohmann::json wantedModList() {
    nlohmann::json list = nlohmann::json::array();
    if (!config::getBool(config::Var::ShowModList)) {
        return list;
    }
    for (const ModEntry& mod : listMods()) {
        if (list.size() >= kMaxModList) {
            break;
        }
        list.push_back({{"id", clip(mod.id)}, {"name", clip(mod.name)}, {"version", clip(mod.version)}});
    }
    return list;
}

// Show My Mod List goes out as client state, so the relay keeps it for whoever joins later.
void tickModList() {
    Session& session = Session::instance();
    if (!session.joined()) {
        return;
    }
    const auto now = Clock::now();
    if (sModListSent && now - sModListAt < kModListInterval) {
        return;
    }
    nlohmann::json list = wantedModList();
    if (sModListSent && list == sSentModList) {
        return;
    }
    // An empty list sent once is the same as none: only send it to take a shown one back.
    if (!sModListSent && list.empty()) {
        sModListSent = true;
        sSentModList = std::move(list);
        return;
    }
    session.send({{"type", "UPDATE_CLIENT_STATE"}, {"modList", list}});
    TwiliLog.info("[looks] mod list to the room: {} mod(s)", list.size());
    sModListSent = true;
    sSentModList = std::move(list);
    sModListAt = now;
}

}  // namespace

const char* slotStateId(SlotState state) {
    return kStateIds[static_cast<size_t>(state)];
}

bool canSendLooks() {
    return detectAvailable();
}

bool canReceiveLooks() {
    return detectAvailable() && svc_overlay != nullptr;
}

const PeerLooks* peerLooks(uint32_t clientId) {
    const auto it = sPeers.find(clientId);
    return it == sPeers.end() ? nullptr : &it->second;
}

void takePeerLooks(uint32_t clientId, const nlohmann::json& looks) {
    const auto drop = [&](const char* why) {
        if (sPeers.erase(clientId) != 0 || why != nullptr) {
            TwiliLog.info("[looks] client {} lists no looks{}{}", clientId, why ? ": " : "", why ? why : "");
        }
    };
    if (!looks.is_array() || looks.empty() || !canReceiveLooks()) {
        drop(nullptr);
        return;
    }
    PeerLooks out;
    uint64_t total = 0;
    size_t count = 0;
    for (const auto& e : looks) {
        if (!e.is_object()) {
            continue;
        }
        const auto type = parseLookType(textOf(e, "slot"));
        const auto stateIt = e.find("state");
        const auto state = stateIt != e.end() ? parseState(*stateIt) : std::nullopt;
        if (!type || !state || out.slots[static_cast<size_t>(*type)]) {
            continue;
        }
        PeerSlot slot;
        slot.state = *state;
        if (slot.state == SlotState::Shared) {
            const auto mods = e.find("mods");
            if (mods != e.end() && mods->is_array()) {
                for (const auto& m : *mods) {
                    if (m.is_object() && slot.mods.size() < kMaxSlotMods) {
                        slot.mods.push_back(
                            {textOf(m, "id"), textOf(m, "name"), textOf(m, "version"), textOf(m, "author")});
                    }
                }
            }
            const auto files = e.find("files");
            if (files != e.end() && files->is_array()) {
                for (const auto& f : *files) {
                    if (!f.is_object()) {
                        continue;
                    }
                    const auto shaIt = f.find("sha");
                    const std::string sha =
                        shaIt != f.end() && shaIt->is_string() ? shaIt->get<std::string>() : std::string{};
                    if (!validSha(sha)) {
                        drop("a file id is not a SHA-256");
                        return;
                    }
                    const auto arc = arcByName(textOf(f, "arc"));
                    std::string name = textOf(f, "name");
                    std::ranges::transform(name, name.begin(), [](char c) {
                        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                    });
                    const auto size = f.find("size");
                    if (!arc || entryType(*arc, name) != type || size == f.end() ||
                        !size->is_number_unsigned() || size->get<uint64_t>() == 0 ||
                        size->get<uint64_t>() > kMaxLookFileBytes ||
                        std::ranges::any_of(slot.files,
                            [&](const PeerFile& p) { return p.arc == *arc && p.name == name; }))
                    {
                        continue;
                    }
                    slot.files.push_back({*arc, std::move(name), sha, size->get<uint32_t>()});
                    slot.bytes += slot.files.back().size;
                }
            }
            if (slot.files.empty()) {
                slot.state = SlotState::Unshared;
                slot.mods.clear();
            }
            total += slot.bytes;
            count += slot.files.size();
        }
        out.slots[static_cast<size_t>(*type)] = std::move(slot);
    }
    if (count > kMaxLookFiles || total > kMaxLookPlayerBytes) {
        drop("over 64 files or 24 MB");
        return;
    }
    std::string summary;
    for (size_t t = 0; t < kLookTypeCount; t++) {
        if (const auto& slot = out.slots[t]) {
            summary += fmt::format(" {} {}", lookTypeId(static_cast<LookType>(t)), slotStateId(slot->state));
            if (slot->state == SlotState::Shared) {
                summary += fmt::format(" ({} files, {} B)", slot->files.size(), slot->bytes);
            }
        }
    }
    TwiliLog.info("[looks] client {} lists:{}", clientId, summary.empty() ? " nothing usable" : summary);
    sPeers[clientId] = std::move(out);
}

OwnSlot ownSlot(LookType type, ShareScope scope) {
    OwnSlot slot = ownSlots(scope)[static_cast<size_t>(type)];
    if (slot.state == SlotState::Shared && ((sTrimmed >> static_cast<uint32_t>(type)) & 1) != 0) {
        slot.state = SlotState::Unshared;
        slot.why = "too many mods and files for the relay to pass on";
    }
    return slot;
}

nlohmann::json manifestLooks(ShareScope scope) {
    nlohmann::json out = nlohmann::json::array();
    const LocalLooks& local = localLooks();
    sTrimmed = 0;
    if (scope == ShareScope::Off || !canSendLooks() || !local.known) {
        return out;
    }
    const auto slots = ownSlots(scope);
    for (size_t t = 0; t < kLookTypeCount; t++) {
        nlohmann::json entry = {{"slot", lookTypeId(static_cast<LookType>(t))},
            {"state", slotStateId(slots[t].state)}};
        if (slots[t].state == SlotState::Shared) {
            std::vector<LookFile> files = local.files[t];
            std::ranges::sort(files, [](const LookFile& a, const LookFile& b) {
                return a.arc != b.arc ? a.arc < b.arc : a.name < b.name;
            });
            nlohmann::json mods = nlohmann::json::array();
            nlohmann::json list = nlohmann::json::array();
            std::vector<std::string> seen;
            for (const LookFile& f : files) {
                list.push_back({{"arc", arcName(f.arc)}, {"name", f.name}, {"sha", f.sha}, {"size", f.size}});
                if (std::ranges::find(seen, f.modId) != seen.end() || seen.size() >= kMaxSlotMods) {
                    continue;
                }
                seen.push_back(f.modId);
                const LookMod* mod = localMod(f.modId);
                mods.push_back({{"id", clip(f.modId)}, {"name", clip(mod ? mod->name : "")},
                    {"version", clip(mod ? mod->version : "")}, {"author", clip(mod ? mod->author : "")}});
            }
            entry["mods"] = std::move(mods);
            entry["files"] = std::move(list);
        }
        out.push_back(std::move(entry));
    }
    if (out.dump().size() > kMaxManifestLooksBytes) {
        for (auto& entry : out) {
            if (const auto mods = entry.find("mods"); mods != entry.end()) {
                for (auto& mod : *mods) {
                    mod.erase("version");
                    mod.erase("author");
                }
            }
        }
    }
    for (size_t t = kLookTypeCount; t-- > 0 && out.dump().size() > kMaxManifestLooksBytes;) {
        if (slots[t].state == SlotState::Shared) {
            out[t] = {{"slot", lookTypeId(static_cast<LookType>(t))},
                {"state", slotStateId(SlotState::Unshared)}};
            sTrimmed |= static_cast<uint8_t>(1u << t);
        }
    }
    return out;
}

std::string manifestLooksSummary(const nlohmann::json& looks) {
    if (!looks.is_array() || looks.empty()) {
        return "no looks";
    }
    std::string out = "looks:";
    for (const auto& e : looks) {
        out += fmt::format(" {} {}", e.value("slot", std::string{}), e.value("state", std::string{}));
        if (const auto files = e.find("files"); files != e.end() && files->is_array()) {
            out += fmt::format(" ({} files)", files->size());
        }
    }
    return out;
}

ResolvedLook resolveLook(uint32_t clientId, LookType type) {
    ResolvedLook out;
    out.choice = effectiveChoice(clientId, type);
    if (out.choice != LookChoice::Theirs) {
        out.source = out.choice == LookChoice::GameDefault ? PuppetSource::GameDefault : PuppetSource::MyOwn;
        return out;
    }
    const PeerSlot* slot = peerSlot(clientId, type);
    if (slot != nullptr && slot->state == SlotState::Vanilla) {
        out.source = PuppetSource::GameDefault;
        return out;
    }
    out.source = fallbackSource();
    if (slot == nullptr) {
        out.fallback = "not listed";
    } else if (slot->state == SlotState::Unshared) {
        out.fallback = "unshared";
    } else if (slotBlocked(*slot)) {
        out.fallback = "blocked";
    } else if (!config::getBool(config::Var::ShowLooks)) {
        out.fallback = "hidden";
    } else {
        // Fetching peers' files is not built yet.
        out.fallback = "not downloaded";
    }
    return out;
}

bool puppetArcPath(uint32_t clientId, LookArc arc, const char*& out) {
    out = arcDefaultPath(arc);
    if (resolveLook(clientId, arcType(arc)).source == PuppetSource::MyOwn || !archiveDirty(arc)) {
        return true;
    }
    switch (baseAlias(arc)) {
    case AliasState::Ready:
        out = arcAliasPath(arc);
        return true;
    case AliasState::Failed:
        return true;
    default:
        return false;
    }
}

uint32_t puppetSignature(uint32_t clientId, std::span<const LookArc> arcs) {
    uint32_t bits = 0;
    for (const LookArc arc : arcs) {
        if (resolveLook(clientId, arcType(arc)).source == PuppetSource::GameDefault && archiveDirty(arc) &&
            baseAlias(arc) != AliasState::Failed)
        {
            bits |= 1u << static_cast<uint32_t>(arc);
        }
    }
    return bits;
}

bool mayRespawn(uint32_t clientId, bool horse) {
    const auto now = Clock::now();
    Respawn& r = sRespawns[static_cast<uint64_t>(clientId) << 1 | (horse ? 1 : 0)];
    const uint32_t own = ownSettings(clientId);
    const auto blocks = sBlockActions.find(clientId);
    const uint32_t blockCount = blocks != sBlockActions.end() ? blocks->second : 0;
    const bool ours = own != r.own || blockCount != r.blocks;
    if (!ours && r.at != Clock::time_point{} && now - r.at < kRespawnGap) {
        return false;
    }
    r.own = own;
    r.blocks = blockCount;
    r.at = now;
    return true;
}

bool lookBlockable(uint32_t clientId) {
    const PeerLooks* peer = peerLooks(clientId);
    return peer != nullptr && std::ranges::any_of(peer->slots, [](const auto& slot) {
        return slot && slot->state == SlotState::Shared;
    });
}

bool lookBlocked(uint32_t clientId) {
    const PeerLooks* peer = peerLooks(clientId);
    return peer != nullptr && std::ranges::any_of(peer->slots, [](const auto& slot) {
        return slot && slot->state == SlotState::Shared && slotBlocked(*slot);
    });
}

void setLookBlocked(uint32_t clientId, bool blocked) {
    const PeerLooks* peer = peerLooks(clientId);
    if (peer == nullptr) {
        return;
    }
    std::vector<std::string> shas;
    for (const auto& slot : peer->slots) {
        if (slot && slot->state == SlotState::Shared) {
            for (const PeerFile& f : slot->files) {
                shas.push_back(f.sha);
            }
        }
    }
    setShasBlocked(shas, blocked);
    sBlockActions[clientId]++;
}

void updateLooks() {
    refreshLocalLooks();
    tickAliases();
    tickChoices();
    if (!Session::active()) {
        return;
    }
    const auto& clients = Session::instance().clients();
    for (auto it = sPeers.begin(); it != sPeers.end();) {
        it = clients.contains(it->first) ? std::next(it) : sPeers.erase(it);
    }
    tickModList();
}

void resetLooksSession() {
    sPeers.clear();
    sRespawns.clear();
    sBlockActions.clear();
    sModListSent = false;
    sSentModList = nlohmann::json::array();
}

void shutdownLooks() {
    resetLooksSession();
    flushChoices();
    shutdownDetect();
    shutdownAliases();
}

std::string looksPlayerTag(uint32_t clientId) {
    std::vector<std::string> parts;
    const LookChoice choice = lookChoice(clientId);
    if (choice != LookChoice::Theirs) {
        parts.push_back(choice == LookChoice::GameDefault ? "game default look" : "my own look");
    }
    for (size_t t = 0; t < kLookTypeCount; t++) {
        const auto type = static_cast<LookType>(t);
        if (const auto own = typeChoice(clientId, type); own && *own != choice) {
            parts.push_back(fmt::format("{}: {}", lookTypeName(type), lookChoiceName(*own)));
        }
    }
    // Their own look, where we would show it.
    const PeerLooks* peer = peerLooks(clientId);
    std::string shared, unshared;
    uint64_t bytes = 0;
    const char* why = nullptr;
    for (size_t t = 0; peer != nullptr && t < kLookTypeCount; t++) {
        const auto type = static_cast<LookType>(t);
        const auto& slot = peer->slots[t];
        if (!slot || effectiveChoice(clientId, type) != LookChoice::Theirs) {
            continue;
        }
        if (slot->state == SlotState::Shared) {
            bytes += slot->bytes;
            if (shared.empty()) {
                shared = !slot->mods.empty() && !slot->mods.front().name.empty() ? slot->mods.front().name :
                                                                                   lookTypeName(type);
            }
            why = resolveLook(clientId, type).fallback;
        } else if (slot->state == SlotState::Unshared) {
            unshared += fmt::format("{}{}", unshared.empty() ? "" : "/", lookTypeName(type));
        }
    }
    if (!shared.empty()) {
        parts.push_back(why != nullptr && std::string_view(why) == "blocked" ?
                            fmt::format("look blocked: {}", shared) :
                            fmt::format("look: {} {}{}", shared, megabytes(bytes),
                                why != nullptr ? fmt::format(" ({})", why) : std::string{}));
    }
    if (!unshared.empty()) {
        parts.push_back(fmt::format("{} look unshared", unshared));
    }
    std::string out;
    for (const std::string& p : parts) {
        out += (out.empty() ? "" : " · ") + p;
    }
    return out;
}

std::string modListText(uint32_t clientId) {
    const Client* c = clientOf(clientId);
    if (c == nullptr || c->modList.empty()) {
        return {};
    }
    std::string out;
    for (const ListedMod& m : c->modList) {
        out += fmt::format("{}{}{}{}", out.empty() ? "" : ", ", m.name.empty() ? m.id : m.name,
            m.version.empty() ? "" : " ", m.version);
    }
    return out;
}

std::vector<std::string> localLooksLines() {
    std::vector<std::string> lines;
    refreshLocalLooks();
    if (!canSendLooks()) {
        lines.push_back("This Dusklight cannot list your look mods (host too old): nobody sees them.");
        return lines;
    }
    const LocalLooks& local = localLooks();
    if (!local.known) {
        lines.push_back("Looking for look mods...");
        return lines;
    }
    if (local.mods.empty()) {
        lines.push_back("No look mods found: you look as the game does.");
    }
    for (const LookMod& mod : local.mods) {
        std::string types;
        for (size_t t = 0; t < kLookTypeCount; t++) {
            if ((mod.types >> t) & 1) {
                types += fmt::format("{}{}", types.empty() ? "" : ", ", lookTypeName(static_cast<LookType>(t)));
            }
        }
        lines.push_back(fmt::format("{} {}{}: {}, {}{}", mod.name.empty() ? mod.id : mod.name, mod.version,
            mod.author.empty() ? "" : " by " + mod.author, types, megabytes(mod.bytes),
            mod.shareable ? "" : " (does not allow sharing)"));
    }
    const ShareScope scope = shareScope();
    for (size_t t = 0; t < kLookTypeCount; t++) {
        const OwnSlot slot = ownSlot(static_cast<LookType>(t), scope);
        const char* name = lookTypeName(static_cast<LookType>(t));
        if (slot.state == SlotState::Vanilla) {
            lines.push_back(fmt::format("{}: the game's look.", name));
        } else if (slot.state == SlotState::Shared) {
            lines.push_back(fmt::format("{}: shared with {} ({} files, {}).", name,
                scope == ShareScope::Team ? "your team" : "everyone in the room", slot.files,
                megabytes(slot.bytes)));
        } else {
            lines.push_back(fmt::format("{}: not shared, {}.", name, slot.why));
        }
    }
    if (Session::instance().joined() && !relayPassesLooks()) {
        lines.push_back("The relay is too old to pass look mods on.");
    }
    return lines;
}

}  // namespace twili::looks
