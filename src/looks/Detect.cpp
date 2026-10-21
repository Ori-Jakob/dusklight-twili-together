#include "looks/Detect.hpp"

#include "core/Log.hpp"
#include "looks/Archive.hpp"
#include "looks/Inventory.hpp"
#include "looks/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace twili::looks {
namespace {

constexpr size_t kMaxArchiveBytes = 64u << 20;
constexpr size_t kMaxEntryBytes = 16u << 20;

struct EntryJob {
    std::string modId;
    uint64_t id = 0;
    LookArc arc = LookArc::Kmdl;
    std::string name;
    uint64_t size = 0;
};
struct ArchiveJob {
    std::string modId;
    uint64_t id = 0;
    LookArc arc = LookArc::Kmdl;
};
struct Job {
    uint64_t generation = 0;
    std::vector<EntryJob> entries;
    std::vector<ArchiveJob> archives;
};
struct Result {
    uint64_t generation = 0;
    std::vector<LookFile> files;
    std::array<bool, kLookTypeCount> unreadable{};
    std::string summary;
};

LocalLooks sLocal;
// What the listing of the scan in flight said about each mod.
std::vector<LookMod> sJobMods;
uint64_t sListed = ~0ull;
bool sListedOnce = false;

std::thread sThread;
std::atomic<bool> sRunning{false};
std::mutex sMutex;
std::optional<Result> sResult;

// Worker only: the disc's own entries, by lowercase name.
std::array<std::optional<std::map<std::string, std::string>>, kLookArcCount> sVanilla;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::optional<std::vector<uint8_t>> decoded(std::vector<uint8_t> raw) {
    if (!isYaz0(raw)) {
        return raw;
    }
    return yaz0Decode(raw, kMaxArchiveBytes);
}

const std::map<std::string, std::string>& vanillaOf(LookArc arc) {
    auto& slot = sVanilla[static_cast<size_t>(arc)];
    if (slot) {
        return *slot;
    }
    slot.emplace();
    auto raw = readBaseDiscFile(arcDefaultPath(arc));
    auto arcBytes = raw ? decoded(std::move(*raw)) : std::nullopt;
    if (!arcBytes) {
        return *slot;
    }
    for (const AllowedEntry& entry : allowedEntries(arc)) {
        if (auto bytes = rarcRead(*arcBytes, entry.name, kMaxEntryBytes)) {
            (*slot)[entry.name] = hex(sha256(*bytes));
        }
    }
    return *slot;
}

bool sameAsDisc(LookArc arc, const std::string& name, const std::string& sha) {
    const auto& vanilla = vanillaOf(arc);
    const auto it = vanilla.find(name);
    return it != vanilla.end() && it->second == sha;
}

void runJob(const Job& job, Result& out) {
    out.generation = job.generation;
    size_t same = 0;
    for (const EntryJob& e : job.entries) {
        const LookType type = *entryType(e.arc, e.name);
        const auto sha = hashOverride(e.modId, e.id);
        if (!sha) {
            out.unreadable[static_cast<size_t>(type)] = true;
            continue;
        }
        const std::string text = hex(*sha);
        if (sameAsDisc(e.arc, e.name, text)) {
            same++;
            continue;
        }
        out.files.push_back({e.arc, e.name, text, static_cast<uint32_t>(e.size), e.modId});
    }
    for (const ArchiveJob& a : job.archives) {
        auto raw = readOverride(a.modId, a.id);
        auto bytes = raw ? decoded(std::move(*raw)) : std::nullopt;
        if (!bytes || !rarcFiles(*bytes)) {
            out.unreadable[static_cast<size_t>(arcType(a.arc))] = true;
            continue;
        }
        for (const AllowedEntry& entry : allowedEntries(a.arc)) {
            // An entry overlay of the same name is what the puppet reads.
            if (std::ranges::any_of(job.entries,
                    [&](const EntryJob& e) { return e.arc == a.arc && e.name == entry.name; }))
            {
                continue;
            }
            const auto data = rarcRead(*bytes, entry.name, kMaxEntryBytes);
            if (!data) {
                continue;
            }
            const std::string text = hex(sha256(*data));
            if (sameAsDisc(a.arc, entry.name, text)) {
                same++;
                continue;
            }
            out.files.push_back(
                {a.arc, entry.name, text, static_cast<uint32_t>(data->size()), a.modId});
        }
    }
    out.summary = fmt::format("{} look file(s) differ from the disc, {} match it", out.files.size(), same);
}

void worker(Job job) {
    const auto start = std::chrono::steady_clock::now();
    Result result;
    try {
        runJob(job, result);
    } catch (const std::exception& e) {
        result = Result{};
        result.generation = job.generation;
        result.unreadable.fill(true);
        try {
            result.summary = std::string("scan failed: ") + e.what();
        } catch (...) {
        }
    } catch (...) {
        result = Result{};
        result.generation = job.generation;
        result.unreadable.fill(true);
    }
    try {
        result.summary += fmt::format(", {:.0f} ms",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    } catch (...) {
    }
    {
        std::lock_guard lock{sMutex};
        sResult = std::move(result);
    }
    sRunning.store(false, std::memory_order_release);
}

bool shareAllowed(const std::string& manifest) {
    const auto json = nlohmann::json::parse(manifest, nullptr, false);
    if (!json.is_object()) {
        return true;
    }
    const auto mp = json.find("multiplayer");
    if (mp == json.end() || !mp->is_object()) {
        return true;
    }
    const auto share = mp->find("share");
    return share == mp->end() || !share->is_boolean() || share->get<bool>();
}

// Game thread: the listing, which archives are ours and what the worker reads.
void startScan(uint64_t generation) {
    Job job;
    job.generation = generation;
    uint32_t dirty = 0;
    uint32_t whole = 0;
    std::vector<LookMod> mods;
    for (const ModEntry& mod : listMods()) {
        const auto overrides = listOverrides(mod.id);
        const bool cosmetics = mod.id == kCosmeticsModId;
        LookMod info{mod.id, mod.name, mod.version, mod.author, shareAllowed(mod.manifest)};
        for (const OverrideEntry& o : overrides) {
            if (!o.wins || o.kind == OverrideKind::Texture) {
                continue;
            }
            if (o.kind == OverrideKind::DiscFile) {
                if (const auto arc = arcByPath(o.discPath)) {
                    dirty |= 1u << static_cast<uint32_t>(*arc);
                    whole |= 1u << static_cast<uint32_t>(*arc);
                    if (!cosmetics) {
                        job.archives.push_back({mod.id, o.id, *arc});
                    }
                }
                continue;
            }
            const auto arc = arcByPath(o.archivePath);
            if (!arc) {
                continue;
            }
            dirty |= 1u << static_cast<uint32_t>(*arc);
            const std::string name = lower(baseName(o.discPath));
            if (!cosmetics && entryType(*arc, name)) {
                job.entries.push_back({mod.id, o.id, *arc, name, o.size});
            }
        }
        mods.push_back(std::move(info));
    }
    sLocal.dirtyArcs = dirty;
    sLocal.wholeArcs = whole;
    sLocal.scanning = true;
    sJobMods = std::move(mods);
    if (sThread.joinable()) {
        sThread.join();
    }
    sRunning.store(true, std::memory_order_release);
    sThread = std::thread(worker, std::move(job));
}

void takeResult(Result result) {
    sLocal.scanning = false;
    sLocal.known = true;
    sLocal.revision++;
    for (auto& list : sLocal.files) {
        list.clear();
    }
    sLocal.unreadable = result.unreadable;
    sLocal.mods.clear();
    for (LookFile& file : result.files) {
        const LookType type = *entryType(file.arc, file.name);
        auto mod = std::ranges::find_if(sJobMods, [&](const LookMod& m) { return m.id == file.modId; });
        if (mod != sJobMods.end()) {
            auto shown = std::ranges::find_if(sLocal.mods, [&](const LookMod& m) { return m.id == mod->id; });
            if (shown == sLocal.mods.end()) {
                sLocal.mods.push_back(*mod);
                shown = std::prev(sLocal.mods.end());
            }
            shown->types |= 1u << static_cast<uint32_t>(type);
            shown->bytes += file.size;
        }
        sLocal.files[static_cast<size_t>(type)].push_back(std::move(file));
    }
    TwiliLog.info("[looks] scan of generation {}: {}; {} look mod(s)", result.generation,
        result.summary, sLocal.mods.size());
    for (const LookMod& mod : sLocal.mods) {
        std::string types;
        for (size_t t = 0; t < kLookTypeCount; t++) {
            if ((mod.types >> t) & 1) {
                types += fmt::format("{}{}", types.empty() ? "" : "/", lookTypeId(static_cast<LookType>(t)));
            }
        }
        TwiliLog.info("[looks]   {} '{}' {}: {} B for {}{}", mod.id, mod.name, mod.version, mod.bytes,
            types, mod.shareable ? "" : " (not shareable)");
    }
}

}  // namespace

bool detectAvailable() {
    return available();
}

void refreshLocalLooks() {
    if (!available()) {
        return;
    }
    if (!sRunning.load(std::memory_order_acquire)) {
        std::optional<Result> result;
        {
            std::lock_guard lock{sMutex};
            result.swap(sResult);
        }
        if (result) {
            if (sThread.joinable()) {
                sThread.join();
            }
            // A result for older mods is dropped: a scan of the new ones follows.
            if (result->generation == sListed) {
                takeResult(std::move(*result));
            }
        }
    }
#if TWILI_HAS_MODS_SERVICE
    const uint64_t generation = svc_mods->generation(mod_ctx);
    if ((!sListedOnce || generation != sListed) && !sRunning.load(std::memory_order_acquire)) {
        sListedOnce = true;
        sListed = generation;
        startScan(generation);
    }
#endif
}

const LocalLooks& localLooks() {
    return sLocal;
}

bool archiveDirty(LookArc arc) {
    if (!available()) {
        return true;
    }
    refreshLocalLooks();
    return ((sLocal.dirtyArcs >> static_cast<uint32_t>(arc)) & 1) != 0;
}

void shutdownDetect() {
    if (sThread.joinable()) {
        sThread.join();
    }
    sRunning.store(false);
    sResult.reset();
    sLocal = {};
    sJobMods.clear();
    sListed = ~0ull;
    sListedOnce = false;
}

}  // namespace twili::looks
