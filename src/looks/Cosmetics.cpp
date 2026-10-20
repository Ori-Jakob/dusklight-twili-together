#include "looks/Cosmetics.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/CosmeticsRecolor.hpp"
#include "looks/Inventory.hpp"

#include <mods/svc/host.h>

#include <fmt/format.h>

#include <bit>
#include <chrono>
#include <filesystem>
#include <map>

namespace twili::looks {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr auto kReadInterval = std::chrono::seconds(1);
constexpr auto kSendInterval = std::chrono::seconds(2);
constexpr size_t kMaxVersionBytes = 32;
// The cosmetics mod's settings our recolour was written for.
constexpr const char* kKnownMajor = "1";

std::optional<CosmeticsValues> sLocal;
bool sLocalRead = false;
Clock::time_point sReadAt{};
uint64_t sGeneration = ~0ull;
std::string sVersion;

// A mod's content-keyed texture (per generation), and texture packs (checked when turned on).
bool sModTextures = false;
bool sPacksChecked = false;
bool sPackFiles = false;

bool sRelayCosmetics = false;
bool sSent = false;
std::optional<CosmeticsValues> sSentValues;
ShareScope sSentScope = ShareScope::Off;
Clock::time_point sSentAt{};
uint32_t sRev = 0;

std::map<uint32_t, CosmeticsValues> sShared;
// Their Look, by name so a reconnect keeps it.
std::map<std::string, LookChoice> sChoices;
bool sShow = true;

std::optional<uint32_t> parseColor(const std::string& text) {
    if (text.size() != 6) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (const char c : text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' :
                          c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) {
            return std::nullopt;
        }
        value = value << 4 | static_cast<uint32_t>(digit);
    }
    return value;
}

bool validIndex(int64_t value) {
    return value >= 1 && value <= 9;
}

bool canRead() {
#if TWILI_HAS_MODS_SERVICE
    return svc_mods != nullptr;
#else
    return false;
#endif
}

// Our recolour goes only on textures checked against the disc, which needs ModsService.
bool canShow() {
    return svc_texture != nullptr && available();
}

#if TWILI_HAS_MODS_SERVICE
bool hasContentTexture(const std::string& modId) {
    bool found = false;
    svc_mods->list_overrides(
        mod_ctx, modId.c_str(),
        [](void* user, const ModOverride* o) {
            if (o->kind == MOD_OVR_TEXTURE && o->texture_key.kind == TEXTURE_KEY_SOURCE) {
                *static_cast<bool*>(user) = true;
                return false;
            }
            return true;
        },
        &found);
    return found;
}
#endif

// The host loads packs from <config>/texture_replacements; data_dir is <config>/mod_data/<id>.
bool texturePackFiles() {
    const char* dir = nullptr;
    if (!SERVICE_HAS(svc_host, HostService, data_dir) ||
        svc_host->data_dir(mod_ctx, &dir) != MOD_OK || dir == nullptr)
    {
        return true;
    }
    fs::path ours(reinterpret_cast<const char8_t*>(dir));
    if (!ours.has_filename()) {
        ours = ours.parent_path();
    }
    std::error_code ec;
    const fs::path packs = ours.parent_path().parent_path() / "texture_replacements";
    fs::directory_iterator it(packs, ec);
    if (ec) {
        return fs::exists(packs, ec);
    }
    return it != fs::directory_iterator();
}

bool texturePacksActive() {
    if (!config::hostBool("game.enableTextureReplacements", true)) {
        sPacksChecked = false;
        return false;
    }
    if (!sPacksChecked) {
        sPacksChecked = true;
        sPackFiles = texturePackFiles();
    }
    return sPackFiles;
}

// Whether a content-keyed texture of ours (cosmetics, a mod's texture, a pack) could land on a
// puppet's textures. Without ModsService we cannot tell, so we assume so.
bool ownTexturesActive() {
    if (!canRead() || (sLocal && sLocal->mask != 0) || sModTextures) {
        return true;
    }
    return texturePacksActive();
}

// nullopt when the cosmetics mod is not active (ModsService gives none of its vars).
std::optional<CosmeticsValues> readLocal() {
#if TWILI_HAS_MODS_SERVICE
    if (svc_mods == nullptr) {
        return std::nullopt;
    }
    if (const uint64_t generation = svc_mods->generation(mod_ctx); generation != sGeneration) {
        sGeneration = generation;
        sVersion.clear();
        sModTextures = false;
        for (const ModEntry& mod : listMods()) {
            if (mod.id == kCosmeticsModId) {
                sVersion = mod.version.substr(0, kMaxVersionBytes);
            }
            sModTextures = sModTextures || hasContentTexture(mod.id);
        }
    }
    CosmeticsValues values;
    values.version = sVersion;
    bool active = false;
    for (size_t i = 0; i < kCosmeticsVars.size(); i++) {
        const CosmeticsVar& var = kCosmeticsVars[i];
        if (var.isInt) {
            int64_t value = 0;
            if (svc_mods->get_mod_var_int(mod_ctx, kCosmeticsModId, var.name, &value) == MOD_OK) {
                active = true;
                if (validIndex(value)) {
                    values.mask |= 1u << i;
                    values.value[i] = static_cast<uint32_t>(value);
                }
            }
            continue;
        }
        char text[16] = {};
        const ModResult result = svc_mods->get_mod_var_string(
            mod_ctx, kCosmeticsModId, var.name, text, sizeof(text), nullptr);
        // Too long for a colour is still the mod speaking: that one value is invalid.
        active |= result == MOD_OK || result == MOD_INVALID_ARGUMENT;
        if (result == MOD_OK) {
            if (const auto color = parseColor(text)) {
                values.mask |= 1u << i;
                values.value[i] = *color;
            }
        }
    }
    if (!active) {
        return std::nullopt;
    }
    return values;
#else
    return std::nullopt;
#endif
}

nlohmann::json cosmeticsJson(const std::optional<CosmeticsValues>& values) {
    if (!values || values->mask == 0) {
        return nullptr;
    }
    nlohmann::json vars = nlohmann::json::object();
    for (size_t i = 0; i < kCosmeticsVars.size(); i++) {
        if (!values->has(static_cast<uint8_t>(i))) {
            continue;
        }
        if (kCosmeticsVars[i].isInt) {
            vars[kCosmeticsVars[i].name] = values->value[i];
        } else {
            vars[kCosmeticsVars[i].name] = fmt::format("{:06x}", values->value[i]);
        }
    }
    return {{"ver", values->version}, {"vars", std::move(vars)}};
}

std::optional<CosmeticsValues> parseCosmetics(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    CosmeticsValues values;
    if (const auto ver = json.find("ver"); ver != json.end() && ver->is_string()) {
        values.version = ver->get<std::string>().substr(0, kMaxVersionBytes);
    }
    const auto vars = json.find("vars");
    if (vars == json.end() || !vars->is_object()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < kCosmeticsVars.size(); i++) {
        const auto it = vars->find(kCosmeticsVars[i].name);
        if (it == vars->end()) {
            continue;
        }
        if (kCosmeticsVars[i].isInt) {
            if (it->is_number_integer() && validIndex(it->get<int64_t>())) {
                values.mask |= 1u << i;
                values.value[i] = it->get<uint32_t>();
            }
        } else if (it->is_string()) {
            if (const auto color = parseColor(it->get<std::string>())) {
                values.mask |= 1u << i;
                values.value[i] = *color;
            }
        }
    }
    if (values.mask == 0) {
        return std::nullopt;
    }
    return values;
}

// Once a second: the cosmetics mod may be turned on or off, its colours changed.
void refreshLocal() {
    const auto now = Clock::now();
    if (!sLocalRead || now - sReadAt >= kReadInterval) {
        sLocalRead = true;
        sReadAt = now;
        sLocal = readLocal();
    }
}

const Client* clientOf(uint32_t clientId) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(clientId);
    return it == clients.end() ? nullptr : &it->second;
}

std::optional<CosmeticsValues> wanted(ShareScope scope) {
    if (scope == ShareScope::Off || !sLocal || sLocal->mask == 0) {
        return std::nullopt;
    }
    return sLocal;
}

void sendManifest(const std::optional<CosmeticsValues>& values, ShareScope scope) {
    const char* scopeName = scope == ShareScope::Team ? "team" : "room";
    Session::instance().send({
        {"type", "LOOK_MANIFEST"},
        {"quiet", true},
        {"rev", ++sRev},
        {"gameId", gameId()},
        {"scope", scopeName},
        {"looks", nlohmann::json::array()},
        {"cosmetics", cosmeticsJson(values)},
    });
    TwiliLog.info("[cosmetics] sent manifest rev {} ({}): {}", sRev, scopeName,
        values ? fmt::format("{} values (cosmetics {})", std::popcount(values->mask), values->version) :
                 std::string("none"));
}

}  // namespace

ShareScope shareScope() {
    const int64_t value = config::getInt(config::Var::ShareLooks);
    return value == 1 ? ShareScope::Team : value == 2 ? ShareScope::Room : ShareScope::Off;
}

nlohmann::json handshakeFeatures() {
    nlohmann::json features = nlohmann::json::array();
    if (canRead()) {
        features.push_back("cos.send");
    }
    if (canShow()) {
        features.push_back("cos.recv");
    }
    return features;
}

std::string gameId() {
#if TWILI_HAS_MODS_SERVICE
    if (svc_mods != nullptr && svc_mods->game_id != nullptr) {
        return svc_mods->game_id;
    }
#endif
    return {};
}

void onRoster(const nlohmann::json& allClientState) {
    sRelayCosmetics = false;
    if (const auto features = allClientState.find("relayFeatures");
        features != allClientState.end() && features->is_array())
    {
        for (const auto& f : *features) {
            sRelayCosmetics |= f.is_string() && f.get<std::string>() == "cos.send";
        }
    }
    if (!sRelayCosmetics) {
        TwiliLog.info("[cosmetics] the relay does not pass cosmetics on");
    }
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type != "LOOK_MANIFEST") {
        return false;
    }
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == Session::instance().selfClientId()) {
        return true;
    }
    const auto cosmetics = packet.find("cosmetics");
    auto values = cosmetics != packet.end() ? parseCosmetics(*cosmetics) : std::nullopt;
    const auto it = sShared.find(id);
    if (!values) {
        if (it != sShared.end()) {
            sShared.erase(it);
            TwiliLog.info("[cosmetics] client {} shares none", id);
        }
        return true;
    }
    if (it == sShared.end() || !(it->second == *values)) {
        TwiliLog.info("[cosmetics] client {} shares {} values (cosmetics {})", id,
            std::popcount(values->mask), values->version);
        sShared[id] = std::move(*values);
    }
    return true;
}

void tick() {
    sShow = config::getBool(config::Var::ShowCosmetics);
    Session& session = Session::instance();
    if (!session.joined()) {
        return;
    }
    warmUpCosmeticsKeys();
    refreshLocal();
    const auto now = Clock::now();
    for (auto it = sShared.begin(); it != sShared.end();) {
        it = session.clients().contains(it->first) ? std::next(it) : sShared.erase(it);
    }
    if (!sRelayCosmetics || !canRead()) {
        return;
    }
    const ShareScope scope = shareScope();
    const std::optional<CosmeticsValues> values = wanted(scope);
    // Off goes out as a room-wide null, so everyone who had ours clears it.
    const ShareScope sendScope = values ? scope : ShareScope::Room;
    const bool same = values == sSentValues && sendScope == sSentScope;
    if (sSent && (same || now - sSentAt < kSendInterval)) {
        return;
    }
    sendManifest(values, sendScope);
    sSent = true;
    sSentValues = values;
    sSentScope = sendScope;
    sSentAt = now;
}

void resetSession() {
    sShared.clear();
    sRelayCosmetics = false;
    sSent = false;
    sSentValues.reset();
    sSentScope = ShareScope::Off;
}

void shutdown() {
    resetSession();
    shutdownCosmeticsKeys();
}

const CosmeticsValues* sharedCosmetics(uint32_t clientId) {
    const auto it = sShared.find(clientId);
    return it == sShared.end() ? nullptr : &it->second;
}

const CosmeticsValues* shownCosmetics(uint32_t clientId) {
    if (!sShow || !canShow() || lookChoice(clientId) != LookChoice::Theirs) {
        return nullptr;
    }
    return sharedCosmetics(clientId);
}

const CosmeticsValues* hairCosmetics(uint32_t clientId) {
    if (lookChoice(clientId) == LookChoice::MyOwn) {
        return sLocal ? &*sLocal : nullptr;
    }
    return shownCosmetics(clientId);
}

bool blockOwnLooks(uint32_t clientId) {
    return lookChoice(clientId) != LookChoice::MyOwn && ownTexturesActive();
}

LookChoice lookChoice(uint32_t clientId) {
    const Client* c = clientOf(clientId);
    if (c == nullptr) {
        return LookChoice::Theirs;
    }
    const auto it = sChoices.find(c->name);
    return it == sChoices.end() ? LookChoice::Theirs : it->second;
}

void setLookChoice(uint32_t clientId, LookChoice choice) {
    const Client* c = clientOf(clientId);
    if (c == nullptr || c->self) {
        return;
    }
    if (choice == LookChoice::Theirs) {
        sChoices.erase(c->name);
    } else {
        sChoices[c->name] = choice;
    }
    static constexpr const char* kNames[] = {"theirs", "game default", "my own"};
    TwiliLog.info("[cosmetics] {}'s look: {}", c->name, kNames[static_cast<size_t>(choice)]);
}

const std::optional<CosmeticsValues>& localCosmetics() {
    return sLocal;
}

bool cosmeticsSent() {
    return sSent;
}

const std::optional<CosmeticsValues>& sentCosmetics() {
    return sSentValues;
}

bool cosmeticsApproximate(const CosmeticsValues& values) {
    const std::string major = values.version.substr(0, values.version.find('.'));
    return !values.version.empty() && major != kKnownMajor;
}

std::string cosmeticsStatusText() {
    refreshLocal();
    std::string mine;
    const ShareScope scope = shareScope();
    if (!canRead()) {
        mine = "This Dusklight cannot read your cosmetics (host too old), so nobody sees them.";
    } else if (!sLocal) {
        mine = "The Cosmetics mod is off or missing: you share no cosmetics.";
    } else if (sLocal->mask == 0) {
        mine = "Your cosmetics are all at their defaults: nothing to share.";
    } else if (scope == ShareScope::Off) {
        mine = fmt::format("You keep your {} cosmetics colour(s) to yourself.", std::popcount(sLocal->mask));
    } else {
        mine = fmt::format("You share {} cosmetics colour(s) with {}.", std::popcount(sLocal->mask),
            scope == ShareScope::Team ? "your team" : "everyone in the room");
    }
    const Session& session = Session::instance();
    if (session.joined() && !sRelayCosmetics) {
        mine += " The relay is too old to pass cosmetics on.";
    }
    std::string theirs;
    if (!canShow()) {
        theirs = "This Dusklight cannot show other players' cosmetics (host too old).";
    } else {
        size_t shown = 0;
        for (const auto& [id, values] : sShared) {
            shown += shownCosmetics(id) != nullptr ? 1 : 0;
        }
        theirs = fmt::format("{} other player(s) share cosmetics, {} shown.", sShared.size(), shown);
    }
    return mine + "\n" + theirs;
}

std::string cosmeticsPlayerTag(uint32_t clientId) {
    if (const LookChoice choice = lookChoice(clientId); choice != LookChoice::Theirs) {
        return choice == LookChoice::GameDefault ? "game default look" : "my own look";
    }
    const CosmeticsValues* values = sharedCosmetics(clientId);
    if (values == nullptr) {
        return {};
    }
    if (shownCosmetics(clientId) == nullptr) {
        return "cosmetics hidden";
    }
    return cosmeticsApproximate(*values) ? "cosmetics (approximate)" : "cosmetics";
}

}  // namespace twili::looks
