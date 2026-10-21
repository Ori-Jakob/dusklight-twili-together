#include "looks/Choices.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"

#include <mods/svc/host.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace twili::looks {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxPlayers = 256;
constexpr size_t kMaxBlocked = 1024;
constexpr auto kSaveDelay = std::chrono::milliseconds(500);
constexpr const char* kChoiceIds[] = {"theirs", "default", "mine"};
constexpr const char* kChoiceNames[] = {"Theirs", "Game Default", "My Own"};

struct Saved {
    std::string name;
    LookChoice look = LookChoice::Theirs;
    std::array<std::optional<LookChoice>, kLookTypeCount> types{};
    int64_t seen = 0;

    bool isDefault() const {
        return look == LookChoice::Theirs &&
               std::ranges::none_of(types, [](const auto& t) { return t.has_value(); });
    }
};

bool sLoaded = false;
std::string sInstallId;
// "id:<install id>", or "name:<name>" for players the relay gave no id.
std::map<std::string, Saved> sPlayers;
std::set<std::string> sBlocked;
fs::path sPath;
bool sDirty = false;
Clock::time_point sSaveAt{};
std::thread sWriter;
uint32_t sRevision = 0;

int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string newInstallId() {
    std::random_device rd;
    return fmt::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());
}

bool validSha(std::string_view sha) {
    return sha.size() == 64 && std::ranges::all_of(sha, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

fs::path filePath() {
    const char* dir = nullptr;
    if (!SERVICE_HAS(svc_host, HostService, data_dir) ||
        svc_host->data_dir(mod_ctx, &dir) != MOD_OK || dir == nullptr)
    {
        return {};
    }
    return fs::path(reinterpret_cast<const char8_t*>(dir)) / "looks.json";
}

const Client* clientOf(uint32_t clientId) {
    if (!Session::active()) {
        return nullptr;
    }
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(clientId);
    return it == clients.end() ? nullptr : &it->second;
}

std::string idKey(const Client& c) {
    return "id:" + c.installId;
}

std::string nameKey(const Client& c) {
    return "name:" + c.name;
}

const Saved* find(uint32_t clientId) {
    const Client* c = clientOf(clientId);
    if (c == nullptr || c->self) {
        return nullptr;
    }
    if (!c->installId.empty()) {
        if (const auto it = sPlayers.find(idKey(*c)); it != sPlayers.end()) {
            return &it->second;
        }
    }
    const auto it = sPlayers.find(nameKey(*c));
    return it == sPlayers.end() ? nullptr : &it->second;
}

nlohmann::json savedJson(const Saved& s) {
    nlohmann::json types = nlohmann::json::object();
    for (size_t t = 0; t < kLookTypeCount; t++) {
        if (s.types[t]) {
            types[lookTypeId(static_cast<LookType>(t))] = lookChoiceId(*s.types[t]);
        }
    }
    return {{"name", s.name}, {"look", lookChoiceId(s.look)}, {"types", std::move(types)},
        {"seen", s.seen}};
}

void trimPlayers() {
    while (sPlayers.size() > kMaxPlayers) {
        auto oldest = sPlayers.begin();
        for (auto it = sPlayers.begin(); it != sPlayers.end(); ++it) {
            if (it->second.seen < oldest->second.seen) {
                oldest = it;
            }
        }
        sPlayers.erase(oldest);
    }
}

void writeFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = fs::path(path).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return;
        }
    }
    fs::rename(tmp, path, ec);
}

void saveNow() {
    sDirty = false;
    if (sPath.empty()) {
        return;
    }
    nlohmann::json players = nlohmann::json::object();
    for (const auto& [key, s] : sPlayers) {
        players[key] = savedJson(s);
    }
    nlohmann::json blocked = nlohmann::json::array();
    for (const std::string& sha : sBlocked) {
        blocked.push_back(sha);
    }
    const nlohmann::json j = {{"version", 1}, {"installId", sInstallId},
        {"players", std::move(players)}, {"blocked", std::move(blocked)}};
    if (sWriter.joinable()) {
        sWriter.join();
    }
    sWriter = std::thread(writeFile, sPath, j.dump(1));
}

void changed() {
    ++sRevision;
    if (!sDirty) {
        sDirty = true;
    }
    sSaveAt = Clock::now() + kSaveDelay;
}

std::optional<LookChoice> choiceOf(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return std::nullopt;
    }
    return parseLookChoice(it->get<std::string>());
}

void parseFile(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    if (const auto id = j.find("installId"); id != j.end() && id->is_string() &&
                                             validInstallId(id->get<std::string>()))
    {
        sInstallId = id->get<std::string>();
    }
    if (const auto players = j.find("players"); players != j.end() && players->is_object()) {
        for (const auto& [key, value] : players->items()) {
            if (!value.is_object() || (!key.starts_with("id:") && !key.starts_with("name:"))) {
                continue;
            }
            Saved s;
            if (const auto name = value.find("name"); name != value.end() && name->is_string()) {
                s.name = name->get<std::string>();
            }
            s.look = choiceOf(value, "look").value_or(LookChoice::Theirs);
            if (const auto types = value.find("types"); types != value.end() && types->is_object()) {
                for (size_t t = 0; t < kLookTypeCount; t++) {
                    s.types[t] = choiceOf(*types, lookTypeId(static_cast<LookType>(t)));
                }
            }
            if (const auto seen = value.find("seen"); seen != value.end() && seen->is_number_integer()) {
                s.seen = seen->get<int64_t>();
            }
            if (!s.isDefault()) {
                sPlayers[key] = std::move(s);
            }
        }
    }
    if (const auto blocked = j.find("blocked"); blocked != j.end() && blocked->is_array()) {
        for (const auto& sha : *blocked) {
            if (sha.is_string() && validSha(sha.get<std::string>()) && sBlocked.size() < kMaxBlocked) {
                sBlocked.insert(sha.get<std::string>());
            }
        }
    }
}

Saved& entryFor(const Client& c) {
    if (!c.installId.empty()) {
        // A choice made while the player had no id moves under the id.
        if (const auto byName = sPlayers.find(nameKey(c)); byName != sPlayers.end()) {
            Saved moved = std::move(byName->second);
            sPlayers.erase(byName);
            sPlayers.try_emplace(idKey(c), std::move(moved));
        }
        return sPlayers[idKey(c)];
    }
    return sPlayers[nameKey(c)];
}

void store(const Client& c, Saved& s) {
    s.name = c.name;
    s.seen = unixMs();
    if (s.isDefault()) {
        sPlayers.erase(c.installId.empty() ? nameKey(c) : idKey(c));
    }
    trimPlayers();
    changed();
}

}  // namespace

const char* lookChoiceId(LookChoice choice) {
    return kChoiceIds[static_cast<size_t>(choice)];
}

const char* lookChoiceName(LookChoice choice) {
    return kChoiceNames[static_cast<size_t>(choice)];
}

std::optional<LookChoice> parseLookChoice(std::string_view id) {
    for (size_t i = 0; i < std::size(kChoiceIds); i++) {
        if (id == kChoiceIds[i]) {
            return static_cast<LookChoice>(i);
        }
    }
    return std::nullopt;
}

bool validInstallId(std::string_view id) {
    return id.size() == 32 && std::ranges::all_of(id, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void loadChoices() {
    if (sLoaded) {
        return;
    }
    sLoaded = true;
    sPath = filePath();
    if (!sPath.empty()) {
        // A few kilobytes, once at start.
        std::ifstream in(sPath, std::ios::binary);
        if (in) {
            std::ostringstream text;
            text << in.rdbuf();
            parseFile(text.str());
        }
    }
    if (sInstallId.empty()) {
        sInstallId = newInstallId();
        TwiliLog.info("[looks] new install id {}", sInstallId);
        if (!sPath.empty()) {
            saveNow();
        }
    }
    TwiliLog.info("[looks] {} saved look choice(s), {} blocked look file(s){}", sPlayers.size(),
        sBlocked.size(), sPath.empty() ? " (no data directory: not kept)" : "");
}

void flushChoices() {
    if (sDirty) {
        saveNow();
    }
    if (sWriter.joinable()) {
        sWriter.join();
    }
}

void tickChoices() {
    if (sDirty && Clock::now() >= sSaveAt) {
        saveNow();
    }
}

const std::string& installId() {
    loadChoices();
    return sInstallId;
}

LookChoice lookChoice(uint32_t clientId) {
    const Saved* s = find(clientId);
    return s == nullptr ? LookChoice::Theirs : s->look;
}

std::optional<LookChoice> typeChoice(uint32_t clientId, LookType type) {
    const Saved* s = find(clientId);
    return s == nullptr ? std::nullopt : s->types[static_cast<size_t>(type)];
}

LookChoice effectiveChoice(uint32_t clientId, LookType type) {
    const Saved* s = find(clientId);
    if (s == nullptr) {
        return LookChoice::Theirs;
    }
    return s->types[static_cast<size_t>(type)].value_or(s->look);
}

void setLookChoice(uint32_t clientId, LookChoice choice) {
    const Client* c = clientOf(clientId);
    if (c == nullptr || c->self) {
        return;
    }
    Saved& s = entryFor(*c);
    s.look = choice;
    store(*c, s);
    TwiliLog.info("[looks] {}'s look: {}", c->name, lookChoiceId(choice));
}

void setTypeChoice(uint32_t clientId, LookType type, std::optional<LookChoice> choice) {
    const Client* c = clientOf(clientId);
    if (c == nullptr || c->self) {
        return;
    }
    Saved& s = entryFor(*c);
    s.types[static_cast<size_t>(type)] = choice;
    store(*c, s);
    TwiliLog.info("[looks] {}'s {} look: {}", c->name, lookTypeId(type),
        choice ? lookChoiceId(*choice) : "as their look");
}

bool shaBlocked(const std::string& sha) {
    return sBlocked.contains(sha);
}

void setShasBlocked(const std::vector<std::string>& shas, bool blocked) {
    size_t count = 0;
    for (const std::string& sha : shas) {
        if (!validSha(sha)) {
            continue;
        }
        if (blocked && sBlocked.size() < kMaxBlocked) {
            count += sBlocked.insert(sha).second ? 1 : 0;
        } else if (!blocked) {
            count += sBlocked.erase(sha);
        }
    }
    if (count != 0) {
        changed();
        TwiliLog.info("[looks] {} {} look file(s)", blocked ? "blocked" : "unblocked", count);
    }
}

uint32_t choicesRevision() {
    return sRevision;
}

std::string choicesPath() {
    return sPath.empty() ? std::string{} : sPath.string();
}

}  // namespace twili::looks
