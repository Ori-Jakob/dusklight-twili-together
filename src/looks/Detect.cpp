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
#include <set>
#include <thread>
#include <tuple>

namespace twili::looks {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxArchiveBytes = 64u << 20;
constexpr size_t kMaxEntryBytes = 16u << 20;
// A scan waits for the mods to stop changing this long, but never longer than kMaxWait.
constexpr auto kSettle = std::chrono::milliseconds(500);
constexpr auto kMaxWait = std::chrono::seconds(3);

struct EntryJob {
    std::string modId;
    uint64_t id = 0;
    LookArc arc = LookArc::Kmdl;
    std::string name;  // lowercase base name
    std::string path;  // lowercase disc path
    uint64_t size = 0;
};
struct ArchiveJob {
    std::string modId;
    uint64_t id = 0;
    uint64_t size = 0;
    LookArc arc = LookArc::Kmdl;
    // The cosmetics mod's: where its entries sit counts, the entries do not.
    bool counts = true;
};
struct Job {
    uint64_t generation = 0;
    std::string key;
    std::vector<EntryJob> entries;
    std::vector<ArchiveJob> archives;
};
struct Result {
    uint64_t generation = 0;
    std::string key;
    std::vector<LookFile> files;
    std::array<bool, kLookTypeCount> unreadable{};
    std::string summary;
};

LocalLooks sLocal;
// What the listing of the scan in flight said about each mod.
std::vector<LookMod> sJobMods;
uint64_t sListed = ~0ull;
bool sListedOnce = false;
// What the last taken scan was made of: a listing with the same key needs no new one.
std::string sScannedKey;
uint64_t sSeen = ~0ull;
Clock::time_point sSeenAt{};
bool sPending = false;
Clock::time_point sPendingSince{};

std::thread sThread;
std::atomic<bool> sRunning{false};
std::mutex sMutex;
std::optional<Result> sResult;

// Worker only: per allowlisted name of an archive, the disc path an entry overlay of it must have
// (JKRArchive resolves them by the archive's own root and directory names), and its bytes.
struct ArcLayout {
    bool ok = false;
    std::map<std::string, std::string> paths;
    std::map<std::string, std::pair<std::string, uint32_t>> entries;
};
std::array<std::optional<ArcLayout>, kLookArcCount> sVanilla;
// Whole-archive overrides by (mod, override id, size): ids change with the bytes.
using WholeKey = std::tuple<std::string, uint64_t, uint64_t>;
std::map<WholeKey, ArcLayout> sWhole;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view baseName(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::optional<std::vector<uint8_t>> decoded(std::vector<uint8_t> raw) {
    if (!isYaz0(raw)) {
        return raw;
    }
    return yaz0Decode(raw, kMaxArchiveBytes);
}

ArcLayout layoutOf(LookArc arc, std::optional<std::vector<uint8_t>> raw) {
    ArcLayout out;
    const auto bytes = raw ? decoded(std::move(*raw)) : std::nullopt;
    const auto files = bytes ? rarcPaths(*bytes) : std::nullopt;
    if (!files) {
        return out;
    }
    std::string dir = lower(arcDefaultPath(arc));
    dir.resize(dir.size() - 4);  // ".arc"
    dir += '/';
    for (const AllowedEntry& entry : allowedEntries(arc)) {
        const auto file = std::ranges::find_if(
            *files, [&](const RarcPath& f) { return baseName(f.path) == entry.name; });
        if (file == files->end()) {
            continue;
        }
        out.paths[entry.name] = dir + file->path;
        if (const auto data = rarcReadFile(*bytes, file->file, kMaxEntryBytes)) {
            out.entries[entry.name] = {hex(sha256(*data)), static_cast<uint32_t>(data->size())};
        }
    }
    out.ok = true;
    return out;
}

const ArcLayout& vanillaOf(LookArc arc) {
    auto& slot = sVanilla[static_cast<size_t>(arc)];
    if (!slot) {
        slot = layoutOf(arc, readBaseDiscFile(arcDefaultPath(arc)));
    }
    return *slot;
}

const ArcLayout& wholeOf(const ArchiveJob& a) {
    const WholeKey key{a.modId, a.id, a.size};
    auto it = sWhole.find(key);
    if (it == sWhole.end()) {
        it = sWhole.emplace(key, layoutOf(a.arc, readOverride(a.modId, a.id))).first;
    }
    return it->second;
}

bool sameAsDisc(const ArcLayout& vanilla, const std::string& name, const std::string& sha) {
    const auto it = vanilla.entries.find(name);
    return it != vanilla.entries.end() && it->second.first == sha;
}

void runJob(const Job& job, Result& out) {
    out.generation = job.generation;
    out.key = job.key;
    size_t same = 0, misplaced = 0;
    std::set<WholeKey> used;
    for (size_t i = 0; i < kLookArcCount; i++) {
        const auto arc = static_cast<LookArc>(i);
        const auto archive =
            std::ranges::find_if(job.archives, [&](const ArchiveJob& a) { return a.arc == arc; });
        const bool hasArchive = archive != job.archives.end();
        if (!hasArchive &&
            std::ranges::none_of(job.entries, [&](const EntryJob& e) { return e.arc == arc; }))
        {
            continue;
        }
        const ArcLayout& vanilla = vanillaOf(arc);
        const ArcLayout* whole = nullptr;
        if (hasArchive) {
            used.insert({archive->modId, archive->id, archive->size});
            whole = &wholeOf(*archive);
        }
        // The archive a puppet mounts names where its entry overlays go.
        const ArcLayout& mounted = whole != nullptr ? *whole : vanilla;
        if (!vanilla.ok || !mounted.ok) {
            if (hasArchive && archive->counts) {
                out.unreadable[static_cast<size_t>(arcType(arc))] = true;
            }
            for (const EntryJob& e : job.entries) {
                if (e.arc == arc) {
                    out.unreadable[static_cast<size_t>(*entryType(arc, e.name))] = true;
                }
            }
            continue;
        }
        std::set<std::string> overlaid;
        for (const EntryJob& e : job.entries) {
            if (e.arc != arc) {
                continue;
            }
            const auto path = mounted.paths.find(e.name);
            if (path == mounted.paths.end() || path->second != e.path) {
                misplaced++;
                continue;
            }
            overlaid.insert(e.name);
            const auto sha = hashOverride(e.modId, e.id);
            if (!sha) {
                out.unreadable[static_cast<size_t>(*entryType(arc, e.name))] = true;
                continue;
            }
            const std::string text = hex(*sha);
            if (sameAsDisc(vanilla, e.name, text)) {
                same++;
                continue;
            }
            out.files.push_back({arc, e.name, text, static_cast<uint32_t>(e.size), e.modId});
        }
        if (whole == nullptr || !archive->counts) {
            continue;
        }
        for (const auto& [name, data] : whole->entries) {
            // An entry overlay at its path is what the puppet reads.
            if (overlaid.contains(name)) {
                continue;
            }
            if (sameAsDisc(vanilla, name, data.first)) {
                same++;
                continue;
            }
            out.files.push_back({arc, name, data.first, data.second, archive->modId});
        }
    }
    std::erase_if(sWhole, [&](const auto& item) { return !used.contains(item.first); });
    out.summary = fmt::format("{} look file(s) differ from the disc, {} match it, {} at a path no "
                              "archive reads",
        out.files.size(), same, misplaced);
}

void worker(Job job) {
    const auto start = Clock::now();
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
        result.summary += fmt::format(
            ", {:.0f} ms", std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    } catch (...) {
    }
    {
        std::lock_guard lock{sMutex};
        sResult = std::move(result);
    }
    sRunning.store(false, std::memory_order_release);
}

// Game thread: the listing, which archives are ours and what the worker reads. Texture overrides
// are left out, so the puppets' own texture registrations cost little here.
void startScan(uint64_t generation) {
    Job job;
    job.generation = generation;
    uint32_t dirty = 0;
    uint32_t whole = 0;
    std::vector<LookMod> mods;
    for (const ModEntry& mod : listMods()) {
        const bool cosmetics = mod.id == kCosmeticsModId;
        LookMod info{mod.id, mod.name, mod.version, mod.author, shareAllowed(mod.manifest)};
        job.key += fmt::format("m {} {} {} {} {}\n", mod.id, mod.name, mod.version, mod.author,
            info.shareable);
        for (const OverrideEntry& o : listOverrides(mod.id, false)) {
            if (!o.wins || o.kind == OverrideKind::Texture) {
                continue;
            }
            if (o.kind == OverrideKind::DiscFile) {
                if (const auto arc = arcByPath(o.discPath)) {
                    dirty |= 1u << static_cast<uint32_t>(*arc);
                    whole |= 1u << static_cast<uint32_t>(*arc);
                    job.archives.push_back({mod.id, o.id, o.size, *arc, !cosmetics});
                    job.key += fmt::format("a {} {} {}\n", o.id, o.size, o.discPath);
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
                job.entries.push_back({mod.id, o.id, *arc, name, lower(o.discPath), o.size});
                job.key += fmt::format("e {} {} {}\n", o.id, o.size, o.discPath);
            }
        }
        mods.push_back(std::move(info));
    }
    sLocal.dirtyArcs = dirty;
    sLocal.wholeArcs = whole;
    if (sLocal.known && job.key == sScannedKey) {
        return;
    }
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
    sScannedKey = std::move(result.key);
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
    const auto now = Clock::now();
    if (generation != sSeen) {
        sSeen = generation;
        sSeenAt = now;
    }
    if (sRunning.load(std::memory_order_acquire) || (sListedOnce && generation == sListed)) {
        return;
    }
    if (!sPending) {
        sPending = true;
        sPendingSince = now;
    }
    // Registrations come in bursts (a puppet's blockers, a mod loading): list once they settle.
    if (sListedOnce && now - sSeenAt < kSettle && now - sPendingSince < kMaxWait) {
        return;
    }
    sPending = false;
    sListedOnce = true;
    sListed = generation;
    startScan(generation);
#endif
}

const LocalLooks& localLooks() {
    return sLocal;
}

bool archiveDirty(LookArc arc) {
    if (!available()) {
        return true;
    }
    if (!sListedOnce) {
        refreshLocalLooks();
    }
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
    sScannedKey.clear();
    sSeen = ~0ull;
    sPending = false;
    sWhole.clear();
}

}  // namespace twili::looks
