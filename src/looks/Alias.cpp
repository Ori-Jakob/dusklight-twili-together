#include "looks/Alias.hpp"

#include "core/Log.hpp"
#include "looks/Detect.hpp"

#include <mods/svc/overlay.h>

#include <dolphin/dvd.h>

#include <array>
#include <atomic>
#include <memory>

namespace twili::looks {
namespace {

// The file table applies a registration at the end of its frame.
constexpr uint32_t kReadyTicks = 2;
constexpr uint32_t kMaxArchiveBytes = 64u << 20;

enum class Phase : uint8_t { None, Wanted, Reading, Registered, Ready, Failed };

struct Alias {
    Phase phase = Phase::None;
    OverlayHandle handle = 0;
    uint32_t registeredAt = 0;
    const char* problem = nullptr;
    bool fromDisc = false;
    // The stopgap's read of the default path.
    DVDFileInfo info{};
    bool open = false;
    std::unique_ptr<uint8_t[]> storage;
    uint8_t* buffer = nullptr;
    uint32_t length = 0;
    std::atomic<bool> done{false};
    std::atomic<int32_t> result{0};
};

std::array<Alias, kLookArcCount> sAliases;
uint32_t sTick = 0;
bool sLogged = false;
bool sForceStopgap = false;

void onRead(s32 result, DVDFileInfo* info) {
    for (Alias& a : sAliases) {
        if (&a.info == info) {
            a.result.store(result, std::memory_order_relaxed);
            a.done.store(true, std::memory_order_release);
            return;
        }
    }
}

void closeRead(Alias& a) {
    if (a.open) {
        DVDClose(&a.info);
        a.open = false;
    }
    a.storage.reset();
    a.buffer = nullptr;
    a.length = 0;
}

void fail(LookArc arc, Alias& a, const char* problem) {
    closeRead(a);
    a.phase = Phase::Failed;
    a.problem = problem;
    TwiliLog.warn("[looks] no base copy of {}: {}", arcName(arc), problem);
}

bool hasBaseAlias() {
#if OVERLAY_SERVICE_MINOR >= 1
    return !sForceStopgap && svc_overlay != nullptr && SERVICE_HAS(svc_overlay, OverlayService, add_base_alias);
#else
    return false;
#endif
}

void registered(LookArc arc, Alias& a, OverlayHandle handle, const char* how) {
    a.handle = handle;
    a.phase = Phase::Registered;
    a.registeredAt = sTick;
    TwiliLog.info("[looks] base copy of {} at {} ({})", arcName(arc), arcAliasPath(arc), how);
}

// Without add_base_alias: the default path read raw, unless one of ours replaces it whole.
void startRead(LookArc arc, Alias& a) {
    if (detectAvailable() && ((localLooks().wholeArcs >> static_cast<uint32_t>(arc)) & 1) != 0) {
        fail(arc, a, "one of your mods replaces the whole archive and this Dusklight cannot read under it");
        return;
    }
    if (!DVDOpen(arcDefaultPath(arc), &a.info)) {
        fail(arc, a, "the archive could not be opened");
        return;
    }
    a.open = true;
    a.length = a.info.length;
    if (a.length == 0 || a.length > kMaxArchiveBytes) {
        fail(arc, a, "the archive is empty or too large");
        return;
    }
    const uint32_t readSize = (a.length + 31) & ~31u;
    a.storage = std::make_unique<uint8_t[]>(readSize + 32);
    a.buffer = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(a.storage.get()) + 31) & ~uintptr_t{31});
    a.done.store(false);
    a.phase = Phase::Reading;
    DVDReadAsyncPrio(&a.info, a.buffer, static_cast<s32>(readSize), 0, onRead, 2);
}

void finishRead(LookArc arc, Alias& a) {
    if (!a.done.load(std::memory_order_acquire)) {
        return;
    }
    if (a.result.load(std::memory_order_relaxed) < static_cast<int32_t>(a.length)) {
        fail(arc, a, "reading the archive failed");
        return;
    }
    OverlayHandle handle = 0;
    if (svc_overlay->add_buffer(mod_ctx, arcAliasPath(arc), a.buffer, a.length, &handle) != MOD_OK) {
        fail(arc, a, "the overlay was refused");
        return;
    }
    closeRead(a);
    a.fromDisc = true;
    registered(arc, a, handle, "read from the disc");
}

}  // namespace

AliasState baseAlias(LookArc arc) {
    Alias& a = sAliases[static_cast<size_t>(arc)];
    switch (a.phase) {
    case Phase::None:
        a.phase = Phase::Wanted;
        return AliasState::Pending;
    case Phase::Ready:
        return AliasState::Ready;
    case Phase::Failed:
        return AliasState::Failed;
    default:
        return AliasState::Pending;
    }
}

const char* aliasProblem(LookArc arc) {
    return sAliases[static_cast<size_t>(arc)].problem;
}

bool aliasFromDisc(LookArc arc) {
    return sAliases[static_cast<size_t>(arc)].fromDisc;
}

void forceStopgapAliases(bool on) {
    sForceStopgap = on;
}

void tickAliases() {
    ++sTick;
    for (size_t i = 0; i < kLookArcCount; i++) {
        const auto arc = static_cast<LookArc>(i);
        Alias& a = sAliases[i];
        switch (a.phase) {
        case Phase::Wanted:
            if (svc_overlay == nullptr) {
                fail(arc, a, "this Dusklight has no OverlayService");
            } else if (hasBaseAlias()) {
#if OVERLAY_SERVICE_MINOR >= 1
                OverlayHandle handle = 0;
                if (svc_overlay->add_base_alias(mod_ctx, arcAliasPath(arc), arcDefaultPath(arc), &handle) ==
                    MOD_OK)
                {
                    registered(arc, a, handle, "base alias");
                } else {
                    fail(arc, a, "add_base_alias refused it");
                }
#endif
            } else {
                if (!sLogged) {
                    sLogged = true;
                    TwiliLog.info("[looks] the host has no add_base_alias: base copies are read from the disc");
                }
                startRead(arc, a);
            }
            break;
        case Phase::Reading:
            finishRead(arc, a);
            break;
        case Phase::Registered:
            if (sTick - a.registeredAt >= kReadyTicks) {
                a.phase = Phase::Ready;
            }
            break;
        default:
            break;
        }
    }
}

void shutdownAliases() {
    for (Alias& a : sAliases) {
        // A read still in flight writes into the buffer: DVDClose waits for it.
        closeRead(a);
        // The host removes our overlays with the mod.
        a.phase = Phase::None;
        a.handle = 0;
        a.problem = nullptr;
        a.fromDisc = false;
    }
    sTick = 0;
    sLogged = false;
    sForceStopgap = false;
}

}  // namespace twili::looks
