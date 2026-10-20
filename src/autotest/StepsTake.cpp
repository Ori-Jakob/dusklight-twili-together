// Steps for items the game takes away (TAKE_ITEM); reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "sync/WorldSync.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_info.h"

#include <fmt/format.h>

#include <optional>
#include <string>

namespace twili::autotest {
namespace {

using nlohmann::json;

struct StatField {
    const char* name;
    uint32_t sync::Stats::* field;
};

constexpr StatField kTakeStats[] = {
    {"sent", &sync::Stats::takesSent},
    {"received", &sync::Stats::takesReceived},
    {"applied", &sync::Stats::takesApplied},
    {"skipped", &sync::Stats::takesSkipped},
    {"mergeTakes", &sync::Stats::mergeTakes},
    {"deferred", &sync::Stats::takesDeferred},
    {"regrants", &sync::Stats::takeRegrants},
    {"returns", &sync::Stats::returnsSent},
};

int takeSlotOf(int item) {
    switch (item) {
    case dItemNo_KANTERA_e:
        return SLOT_1;
    case dItemNo_RAFRELS_MEMO_e:
    case dItemNo_ASHS_SCRIBBLING_e:
        return SLOT_19;
    case dItemNo_LETTER_e:
    case dItemNo_BILL_e:
    case dItemNo_WOOD_STATUE_e:
    case dItemNo_IRIAS_PENDANT_e:
    case dItemNo_HORSE_FLUTE_e:
        return SLOT_21;
    case dItemNo_ANCIENT_DOCUMENT_e:
    case dItemNo_AIR_LETTER_e:
    case dItemNo_ANCIENT_DOCUMENT2_e:
        return SLOT_22;
    default:
        return -1;
    }
}

// The call the game makes for this take (monkey, Shad, Ilia, the "Item Remove" flow event).
bool takeItem(const json& step, std::string& why) {
    const int item = step.value("item", -1);
    if (item == dItemNo_TOMATO_PUREE_e || item == dItemNo_TASTE_e) {
        dComIfGs_offItemFirstBit(static_cast<u8>(item));
        return true;
    }
    const int slot = step.value("slot", takeSlotOf(item));
    if (slot < 0 || slot >= MAX_ITEM_SLOTS) {
        why = fmt::format("takeItem: no slot for item 0x{:02X}", item);
        return false;
    }
    if (dComIfGs_getItem(slot, false) != item) {
        why = fmt::format("takeItem: slot {} holds 0x{:02X}, not 0x{:02X}", slot,
            dComIfGs_getItem(slot, false), item);
        return false;
    }
    if (slot == SLOT_21) {
        dComIfGs_setWarashibeItem(dItemNo_NONE_e);
    } else {
        if (item == dItemNo_KANTERA_e) {
            dMeter2Info_setOilGaugeBackUp(dComIfGs_getOil());
        }
        dComIfGs_setItem(slot, dItemNo_NONE_e);
    }
    return true;
}

std::optional<bool> takeSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "takeItem") {
        std::string why;
        if (!takeItem(step, why)) {
            ctx.fail(why);
            return false;
        }
        TwiliLog.info("[autotest] took item 0x{:02X}", step.value("item", -1));
        return true;
    }

    // The "Trade Item Set" flow event: a trade item swapped in without an item get.
    if (op == "setTradeItem") {
        dComIfGs_setWarashibeItem(static_cast<u8>(step.value("item", 255)));
        return true;
    }

    // As the collection screen does; burnShield then removes this one.
    if (op == "equipShield") {
        dMeter2Info_setShield(static_cast<u8>(step.value("item", 255)), false);
        return true;
    }

    // What a game over during the monkey's theft does (d_gameover.cpp).
    if (op == "restoreLantern") {
        if (!dComIfGs_isItemFirstBit(dItemNo_KANTERA_e) ||
            dComIfGs_getItem(SLOT_1, true) != dItemNo_NONE_e)
        {
            ctx.fail("restoreLantern: the lantern is not missing");
            return false;
        }
        dComIfGs_setItem(SLOT_1, dItemNo_KANTERA_e);
        dComIfGs_setOil(dMeter2Info_getOilGaugeBackUp());
        return true;
    }

    // item 255 = empty; forSec: it must stay so.
    if (op == "expectSlot" || op == "expectFirstBit") {
        static bool sReached = false;
        if (!ctx.begun) {
            sReached = false;
        }
        const int item = step.value("item", 255);
        bool ok = false;
        std::string have;
        if (op == "expectSlot") {
            const int slot = step.value("slot", takeSlotOf(item));
            if (slot < 0 || slot >= MAX_ITEM_SLOTS) {
                ctx.fail("expectSlot: no slot");
                return false;
            }
            const int held = dComIfGs_getItem(slot, false);
            ok = held == item;
            have = fmt::format("slot {} holds 0x{:02X} (want 0x{:02X})", slot, held, item);
        } else {
            const bool want = step.value("set", true);
            const bool set = dComIfGs_isItemFirstBit(static_cast<u8>(item)) != 0;
            ok = set == want;
            have = fmt::format("first-get bit 0x{:02X} is {} (want {})", item, set, want);
        }
        if (!ok && sReached) {
            ctx.fail(have + " changed back");
            return false;
        }
        if (ok) {
            sReached = true;
            return ctx.seconds >= step.value("forSec", 0.0);
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(have);
        }
        return false;
    }

    // Waits until every `min` count is reached; fails at once above a `max` count.
    if (op == "expectTakeStats") {
        const sync::Stats& stats = sync::stats();
        const json max = step.value("max", json::object());
        const json min = step.value("min", json::object());
        std::string missing;
        for (const StatField& f : kTakeStats) {
            const uint32_t value = stats.*(f.field);
            if (max.contains(f.name) && value > max[f.name].get<uint32_t>()) {
                ctx.fail(fmt::format("take stat {} is {}, want at most {}", f.name, value,
                    max[f.name].get<uint32_t>()));
                return false;
            }
            if (min.contains(f.name) && value < min[f.name].get<uint32_t>()) {
                missing += fmt::format(" {}={}<{}", f.name, value, min[f.name].get<uint32_t>());
            }
        }
        if (missing.empty()) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail("take stats:" + missing);
        }
        return false;
    }

    if (op == "holdTakes") {
        sync::holdTakesForTest(step.value("hold", true));
        return true;
    }

    // What an autosave does: our world state to the team (and the relay's cache).
    if (op == "publishWorldState") {
        if (sync::enabled()) {
            sync::onSaveWritten();
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail("publishWorldState: world sync never enabled");
        }
        return false;
    }

    if (op == "expectLantern") {
        daAlink_c* link = local::liveLink();
        if (link == nullptr) {
            if (ctx.seconds > ctx.timeout(20.0)) {
                ctx.fail("expectLantern: no player");
            }
            return false;
        }
        const bool inHand = link->mEquipItem == dItemNo_KANTERA_e;
        const bool lit = link->checkUseKandelaar(0);
        const bool ok = (!step.contains("inHand") || inHand == step.value("inHand", true)) &&
                        (!step.contains("lit") || lit == step.value("lit", true));
        if (ok) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectLantern: in hand {}, lit {}, proc 0x{:X}", inHand, lit,
                link->mProcID));
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&takeSteps);

}  // namespace
}  // namespace twili::autotest
