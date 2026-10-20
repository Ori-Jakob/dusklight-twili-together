// Steps for the boss-fight scenarios (scenarios/boss.js); reference in the runner README.

#include "autotest/AutoTest.hpp"
#include "autotest/AutoTestSteps.hpp"

#include "autotest/State.hpp"

#include "boss/BossFight.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "story/StoryState.hpp"
#include "sync/WorldSync.hpp"
#include "ui/StoryPrompt.hpp"

#include "d/d_kankyo.h"
#include "m_Do/m_Do_ext.h"

#include "d/actor/d_a_b_bq.h"
#include "d/actor/d_a_e_fm.h"
#include "d/actor/d_a_e_gob.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_save.h"
#include "dolphin/pad.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <cstring>
#include <map>
#include <set>
#include <string>

namespace twili::autotest {
namespace {

namespace sd = story::detail;

// The actions each boss's own code ends its fight with (the enums of d_a_b_bq, d_a_e_fm, d_a_e_gob).
constexpr s16 kBqActionEnd = 4;
constexpr s16 kFmActionEnd = 12;
constexpr s16 kGobActionEnd = 10;
constexpr s16 kGobActionMessage = 11;
// fopAcM_createItemForBoss gives boss heart containers this item bit.
constexpr int kBossHeartBit = 0x9F;

bool sKeepLife = false;
// Accepted events by name since the script started (the tracker keeps only the last few).
std::map<std::string, int> sEventCounts;
uint32_t sLastSeq = 0;
u16 sMarkedMaxLife = 0;

int clearEvents() {
    int n = 0;
    for (const auto& [name, count] : sEventCounts) {
        n += name.rfind("BOSSCLEAR", 0) == 0 ? count : 0;
    }
    return n;
}

const char* currentStage() {
    const char* s = dComIfGp_getStartStageName();
    return s != nullptr ? s : "";
}

void tickBoss() {
    if (sKeepLife && dComIfGs_getLife() < dComIfGs_getMaxLifeGauge()) {
        dComIfGs_setLife(dComIfGs_getMaxLifeGauge());
    }
    if (const story::Instance* in = story::tracker().lastInstance(); in != nullptr && in->seq != sLastSeq) {
        sLastSeq = in->seq;
        sEventCounts[in->name]++;
    }
}

void* findBoss(void* proc, void*) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (!boss::isBossProfile(fopAcM_GetName(actor)) || !fopAcM_IsExecuting(fopAcM_GetID(actor))) {
        return nullptr;
    }
    const int room = dComIfGp_roomControl_getStayNo();
    return fopAcM_GetRoomNo(actor) == room || fopAcM_GetRoomNo(actor) < 0 ? actor : nullptr;
}

fopAc_ac_c* liveBoss() {
    return static_cast<fopAc_ac_c*>(fopAcM_Search(&findBoss, nullptr));
}

// Its own code has started its death or turned it friendly.
bool bossDying(fopAc_ac_c* actor) {
    switch (fopAcM_GetName(actor)) {
    case fpcNm_B_BQ_e:
        return static_cast<b_bq_class*>(actor)->mAction == kBqActionEnd;
    case fpcNm_E_FM_e:
        return static_cast<e_fm_class*>(actor)->mAction == kFmActionEnd;
    case fpcNm_E_GOB_e: {
        const s16 action = static_cast<e_gob_class*>(actor)->mAction;
        return action == kGobActionEnd || action == kGobActionMessage;
    }
    default:
        return false;
    }
}

struct Count {
    int16_t profile;
    int n = 0;
};

void* countActors(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    auto* c = static_cast<Count*>(data);
    if (fopAcM_GetName(actor) != c->profile) {
        return nullptr;
    }
    if (c->profile == fpcNm_Obj_LifeContainer_e) {
        auto* life = static_cast<daObjLife_c*>(actor);
        if (life->getSaveBitNo() != kBossHeartBit && life->m_itemNo != dItemNo_UTAWA_HEART_e) {
            return nullptr;
        }
    }
    c->n++;
    return nullptr;
}

int countOf(int16_t profile) {
    Count c{profile};
    fopAcM_Search(&countActors, &c);
    return c.n;
}

// The live stage bits of the stage we stand in, or the save slot of `stage`.
dSv_memBit_c* memBit(const std::string& stage) {
    if (stage.empty() || std::strncmp(stage.c_str(), currentStage(), 8) == 0) {
        return &dComIfGs_getSaveInfo()->getMemory().getBit();
    }
    const int tbl = story::stageSaveTbl(stage.c_str());
    if (tbl < 0) {
        return nullptr;
    }
    const int live = Session::instance().currentSaveTblNo();
    return tbl == live ? &dComIfGs_getSaveInfo()->getMemory().getBit() :
                         &dComIfGs_getSaveInfo()->getSavedata().getSave(tbl).getBit();
}

u16 maxLife() {
    return dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusA().getMaxLife();
}

// What a teammate's kill did to our fight, from the step's start.
struct Watch {
    char stage[8] = {};
    int room = -1;
    fpc_ProcID bossId = fpcM_ERROR_PROCESS_ID_e;
    int16_t bossProfile = -1;
    int warps = 0;
    int hearts = 0;
    uint32_t seq = 0;
    dSv_memBit_c bits{};
    uint8_t events[MAX_EVENTS] = {};
    u16 maxLife = 0;
    bool bossBit = false;
    bool middleBit = false;
    std::set<std::string> seen;
};
Watch sWatch;

void beginWatch() {
    sWatch = Watch{};
    std::strncpy(sWatch.stage, currentStage(), sizeof(sWatch.stage) - 1);
    sWatch.room = dComIfGp_roomControl_getStayNo();
    if (fopAc_ac_c* b = liveBoss()) {
        sWatch.bossId = fopAcM_GetID(b);
        sWatch.bossProfile = fopAcM_GetName(b);
    }
    sWatch.warps = countOf(fpcNm_Obj_BossWarp_e);
    sWatch.hearts = countOf(fpcNm_Obj_LifeContainer_e);
    const story::Instance* in = story::tracker().lastInstance();
    sWatch.seq = in != nullptr ? in->seq : 0;
    sWatch.bits = dComIfGs_getSaveInfo()->getMemory().getBit();
    std::memcpy(sWatch.events, dComIfGs_getSaveInfo()->getEvent().mEvent, sizeof(sWatch.events));
    sWatch.maxLife = maxLife();
    sWatch.bossBit = dComIfGs_isStageBossEnemy() != FALSE;
    sWatch.middleBit = dComIfGs_isStageMiddleBoss() != FALSE;
}

// The first thing that changed our fight, or "".
std::string interruption() {
    if (std::strncmp(sWatch.stage, currentStage(), 8) != 0 ||
        dComIfGp_roomControl_getStayNo() != sWatch.room || dComIfGp_isEnableNextStage())
    {
        return fmt::format("moved: {} room {}", currentStage(), dComIfGp_roomControl_getStayNo());
    }
    fopAc_ac_c* b = fopAcM_SearchByID(sWatch.bossId);
    if (sWatch.bossId != fpcM_ERROR_PROCESS_ID_e && (b == nullptr || !fopAcM_IsExecuting(sWatch.bossId))) {
        return "boss-gone";
    }
    if (b != nullptr && bossDying(b)) {
        return "boss-dying";
    }
    const story::Instance* in = story::tracker().lastInstance();
    if (in != nullptr && in->seq != sWatch.seq && in->requester != sWatch.bossProfile) {
        return fmt::format("event: '{}' (requester {}{})", in->name, in->requester,
            in->copyOf != 0 ? ", a pull-in copy" : "");
    }
    if (countOf(fpcNm_Obj_BossWarp_e) > sWatch.warps) {
        return "warp-appeared";
    }
    if (countOf(fpcNm_Obj_LifeContainer_e) > sWatch.hearts) {
        return "heart-appeared";
    }
    const sd::State& st = sd::state();
    if (st.prompt.kind != story::PromptKind::None || ui::storyPromptShowing()) {
        return fmt::format("prompt: {} showing {}", story::promptKindName(st.prompt.kind),
            st.prompt.showing || ui::storyPromptShowing());
    }
    if (story::loadActive()) {
        return "story-load";
    }
    if (sWatch.bossProfile == fpcNm_E_GOB_e && !sWatch.middleBit && dComIfGs_isStageMiddleBoss()) {
        return "armor-gone (mini-boss bit set: Dangoro's armor stops drawing)";
    }
    if (!sWatch.bossBit && dComIfGs_isStageBossEnemy()) {
        return "boss-bit (a game over would restart outside the arena; a reload clears the room)";
    }
    if (!sWatch.middleBit && dComIfGs_isStageMiddleBoss()) {
        return "mini-boss-bit";
    }
    return {};
}

std::string watchSummary() {
    const dSv_memBit_c& now = dComIfGs_getSaveInfo()->getMemory().getBit();
    std::string switches;
    for (int no = 0; no < dSv_info_c::MEMORY_SWITCH; no++) {
        const bool was = (static_cast<u32>(sWatch.bits.mSwitch[no >> 5]) >> (no & 31)) & 1u;
        const bool is = (static_cast<u32>(now.mSwitch[no >> 5]) >> (no & 31)) & 1u;
        if (was != is) {
            switches += fmt::format(" {}{}", is ? "+" : "-", no);
        }
    }
    std::string events;
    const uint8_t* ev = dComIfGs_getSaveInfo()->getEvent().mEvent;
    for (int i = 0; i < MAX_EVENTS; i++) {
        const uint8_t gained = ev[i] & ~sWatch.events[i];
        for (int b = 0; gained != 0 && b < 8; b++) {
            if ((gained >> b) & 1u) {
                events += fmt::format(" {:04X}", i << 8 | 1 << b);
            }
        }
    }
    return fmt::format("switches [{}] items 0x{:08X}->0x{:08X} dungeon 0x{:02X}->0x{:02X} events [{}] "
                       "max life {}->{} warps {} hearts {} held {} (sent back {})",
        switches, static_cast<u32>(sWatch.bits.mItem[0]), static_cast<u32>(now.mItem[0]),
        sWatch.bits.mDungeonItem, now.mDungeonItem, events, sWatch.maxLife, maxLife(),
        countOf(fpcNm_Obj_BossWarp_e), countOf(fpcNm_Obj_LifeContainer_e), sync::bossFightHeld(),
        sync::stats().bossReleased);
}

std::optional<bool> bossWatch(StepContext& ctx) {
    const std::string tag = ctx.step.value("tag", std::string{});
    const bool expect = ctx.step.value("expect", true);
    if (!ctx.begun) {
        beginWatch();
        TwiliLog.info("[autotest] BOSSWATCH {} begin: boss {} in {} room {}", tag, sWatch.bossProfile,
            sWatch.stage, sWatch.room);
    }
    const std::string what = interruption();
    if (!what.empty()) {
        const std::string kind = what.substr(0, what.find(':'));
        if (sWatch.seen.insert(kind).second) {
            TwiliLog.info("[autotest] BOSSWATCH {} interrupt {} after {:.1f}s", tag, what, ctx.seconds);
        }
        if (expect) {
            ctx.fail(fmt::format("bossWatch {}: {} ({})", tag, what, watchSummary()));
            return false;
        }
    }
    // until/from: watch until that instance's signal (and holdSec more), else for sec.
    if (const std::string until = ctx.step.value("until", std::string{}); !until.empty()) {
        static double s_signalAt = -1.0;
        if (!ctx.begun) {
            s_signalAt = -1.0;
        }
        if (s_signalAt < 0.0 && detail::state().signals.count(ctx.step.value("from", std::string{}) + ":" + until)) {
            s_signalAt = ctx.seconds;
        }
        if (s_signalAt < 0.0 || ctx.seconds < s_signalAt + ctx.step.value("holdSec", 15.0)) {
            if (ctx.seconds > ctx.timeout(900.0)) {
                ctx.fail(fmt::format("bossWatch {}: no signal {}", tag, until));
            }
            return false;
        }
    } else if (ctx.seconds < ctx.step.value("sec", 20.0)) {
        return false;
    }
    TwiliLog.info("[autotest] BOSSWATCH {} end: {} interrupt(s) [{}] {}", tag, sWatch.seen.size(),
        fmt::join(sWatch.seen, ", "), watchSummary());
    return true;
}

// The game's own end of the fight (debug pad paths of the boss code), or its kill's flag writes.
std::optional<bool> killBoss(StepContext& ctx) {
    fopAc_ac_c* b = liveBoss();
    if (b == nullptr || dComIfGp_event_runCheck()) {
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("killBoss: no live boss here (found {}, event {})", b != nullptr,
                dComIfGp_event_runCheck() != FALSE));
        }
        return false;
    }
    const std::string how = ctx.step.value("how", std::string("real"));
    const int16_t profile = fopAcM_GetName(b);
    const int room = fopAcM_GetRoomNo(b);
    if (how == "real" && profile == fpcNm_B_BQ_e) {
        auto* bq = static_cast<b_bq_class*>(b);
        bq->mAction = kBqActionEnd;
        bq->mMode = 0;
        bq->mDemoMode = 50;
    } else if (how == "real" && profile == fpcNm_E_FM_e) {
        auto* fm = static_cast<e_fm_class*>(b);
        fm->health = 0;
        fm->mAction = kFmActionEnd;
        fm->mMode = 0;
    } else if (how == "flags" && profile == fpcNm_E_GOB_e) {
        // The end of its defeat scene.
        dComIfGs_onStageMiddleBoss();
        fopAcM_onSwitch(b, 14);
    } else if (how == "flags" && profile == fpcNm_B_BQ_e) {
        dComIfGs_onSwitch(fopAcM_GetParam(b) >> 24, room);
        dComIfGs_onStageBossEnemy();
        dComIfGs_offSwitch((fopAcM_GetParam(b) >> 16) & 0xFF, room);
    } else if (how == "flags" && profile == fpcNm_E_FM_e) {
        dComIfGs_onStageBossEnemy();
        dComIfGs_onEventBit(dSv_event_flag_c::saveBitLabels[64]);
    } else {
        ctx.fail(fmt::format("killBoss: no '{}' kill for {}", how, fopAcM_getProcNameString(b)));
        return false;
    }
    TwiliLog.info("[autotest] killBoss {} ({}) in {} room {}", fopAcM_getProcNameString(b), how,
        currentStage(), room);
    return true;
}

// After our kill: its scenes are over, the clear left `heart` heart containers and `warp` warps.
// A boss with a warp is done after its own BOSSCLEAR (a teammate's boss bit may be set already).
// soft: a mismatch is only logged.
std::optional<bool> expectBossClear(StepContext& ctx) {
    static int s_clearsBefore = 0;
    if (!ctx.begun) {
        s_clearsBefore = clearEvents();
    }
    const bool soft = ctx.step.value("soft", false);
    const bool beaten = ctx.step.value("warp", 1) > 0 ?
                            clearEvents() > s_clearsBefore :
                            dComIfGs_isStageBossEnemy() || dComIfGs_isStageMiddleBoss();
    const bool quiet = !dComIfGp_event_runCheck() && story::tracker().quietTicks() >= 90;
    if (dComIfGp_event_runCheck() && !padBusy() && ctx.ticks % 20 == 0) {
        pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
    }
    if (!beaten || !quiet) {
        if (ctx.seconds > ctx.timeout(240.0)) {
            const std::string why = fmt::format("expectBossClear: beaten {} quiet {} ({})", beaten, quiet, sd::debugText());
            if (soft) {
                TwiliLog.warn("[autotest] {}", why);
                return true;
            }
            ctx.fail(why);
        }
        return false;
    }
    const int hearts = countOf(fpcNm_Obj_LifeContainer_e);
    const int warps = countOf(fpcNm_Obj_BossWarp_e);
    const int wantHearts = ctx.step.value("heart", 1);
    const int wantWarps = ctx.step.value("warp", 1);
    TwiliLog.info("[autotest] boss clear: {} heart container(s), {} warp(s), BOSSCLEAR events {}",
        hearts, warps, clearEvents());
    if ((hearts != wantHearts || warps != wantWarps) && soft) {
        TwiliLog.warn("[autotest] boss clear differs: {} heart container(s) and {} warp(s), want {} and {}",
            hearts, warps, wantHearts, wantWarps);
    } else if (hearts != wantHearts || warps != wantWarps) {
        ctx.fail(fmt::format("expectBossClear: {} heart container(s) and {} warp(s), want {} and {}",
            hearts, warps, wantHearts, wantWarps));
        return false;
    }
    return true;
}

std::optional<bool> bossSteps(const std::string& op, StepContext& ctx) {
    const nlohmann::json& step = ctx.step;

    if (op == "bossGuard") {
        boss::setGuardForTest(step.value("on", true));
        return true;
    }

    if (op == "keepLife") {
        sKeepLife = step.value("on", true);
        return true;
    }

    // Our own fight runs (value) and, for true, its intro is over (pressA for its text).
    if (op == "waitBossFight") {
        const bool want = step.value("value", true);
        const bool quiet = !dComIfGp_event_runCheck() &&
                           story::tracker().quietTicks() >= static_cast<uint32_t>(step.value("quietTicks", 60));
        if (dComIfGp_event_runCheck() && step.value("pressA", false) && !padBusy() && ctx.ticks % 20 == 0) {
            pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
        }
        if (boss::fighting() == want && (!want || quiet)) {
            TwiliLog.info("[autotest] boss fight {} in {}", want ? "under way" : "none", currentStage());
            return true;
        }
        if (ctx.seconds > ctx.timeout(120.0)) {
            ctx.fail(fmt::format("waitBossFight: fighting {} (want {}), quiet {}", boss::fighting(), want, quiet));
        }
        return false;
    }

    if (op == "expectBossFight") {
        if (boss::fighting() == step.value("value", true)) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectBossFight: fighting {}", boss::fighting()));
        }
        return false;
    }

    if (op == "killBoss") {
        return killBoss(ctx);
    }

    if (op == "bossWatch") {
        return bossWatch(ctx);
    }

    if (op == "expectBossClear") {
        return expectBossClear(ctx);
    }

    // Packets held for our fight now (min/max), or released so far (released).
    if (op == "expectBossHeld") {
        const size_t held = sync::bossFightHeld();
        const uint32_t released = sync::stats().bossReleased;
        const bool ok = held >= static_cast<size_t>(step.value("min", 0)) &&
                        (!step.contains("max") || held <= static_cast<size_t>(step.value("max", 0))) &&
                        released >= static_cast<uint32_t>(step.value("released", 0));
        if (ok) {
            TwiliLog.info("[autotest] boss fight holds {} packet(s), released {}", held, released);
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectBossHeld: {} held, {} released", held, released));
        }
        return false;
    }

    // A dungeon bit (MAP 0 .. STAGE_BOSS_ENEMY 3, STAGE_LIFE 4, STAGE_BOSS_ENEMY_2 7) of `stage`.
    if (op == "expectDungeonBit") {
        dSv_memBit_c* bit = memBit(step.value("stage", std::string{}));
        const int no = step.value("bit", 3);
        const bool want = step.value("set", true);
        if (bit != nullptr && ((bit->mDungeonItem >> no) & 1) == (want ? 1 : 0)) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectDungeonBit {} of {}: 0x{:02X}", no, step.value("stage", std::string{}),
                bit != nullptr ? bit->mDungeonItem : 0));
        }
        return false;
    }

    // A memory switch of `stage` (its save slot when we stand elsewhere).
    if (op == "expectStageSwitch") {
        dSv_memBit_c* bit = memBit(step.value("stage", std::string{}));
        const int no = step.value("no", 0);
        const bool want = step.value("set", true);
        if (bit != nullptr && no >= 0 && no < dSv_info_c::MEMORY_SWITCH &&
            (((static_cast<u32>(bit->mSwitch[no >> 5]) >> (no & 31)) & 1u) != 0) == want)
        {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectStageSwitch {} of {} never became {}", no,
                step.value("stage", std::string{}), want));
        }
        return false;
    }

    if (op == "markMaxLife") {
        sMarkedMaxLife = maxLife();
        TwiliLog.info("[autotest] max life {}", sMarkedMaxLife);
        return true;
    }

    // Max life minus the marked one is `delta` (5 per heart container) and stays so for forSec.
    if (op == "expectMaxLife") {
        const int delta = maxLife() - sMarkedMaxLife;
        const int want = step.value("delta", 5);
        if (delta == want) {
            return ctx.seconds >= step.value("forSec", 0.0);
        }
        if (ctx.seconds > ctx.timeout(30.0) || (ctx.seconds >= step.value("forSec", 0.0) && delta > want)) {
            ctx.fail(fmt::format("expectMaxLife: +{} (want +{})", delta, want));
        }
        return false;
    }

    // Events of that name accepted here since the script began.
    if (op == "expectEventCount") {
        const std::string name = step.value("name", std::string{});
        const bool prefix = step.value("prefix", false);
        int have = 0;
        for (const auto& [n, count] : sEventCounts) {
            if (prefix ? n.rfind(name, 0) == 0 : n == name) {
                have += count;
            }
        }
        if (have == step.value("count", 1)) {
            return true;
        }
        ctx.fail(fmt::format("expectEventCount {}: {} (want {})", name, have, step.value("count", 1)));
        return false;
    }

    // No pull-in copy of a teammate's event of that name runs for sec seconds.
    if (op == "expectNoCutsceneJoin") {
        const std::string want = step.value("name", std::string{});
        const sd::JoinInfo& j = sd::state().join;
        if (j.eventName == want && (j.state == story::JoinState::Waiting ||
                                       j.state == story::JoinState::Ordered ||
                                       j.state == story::JoinState::Running))
        {
            ctx.fail(fmt::format("expectNoCutsceneJoin: joined '{}' ({})", want, story::joinStateName(j.state)));
            return false;
        }
        if (ctx.seconds >= step.value("sec", 20.0)) {
            TwiliLog.info("[autotest] CUTSCENE PASS {} no join of '{}' ({} {})", step.value("tag", std::string{}),
                want, story::joinStateName(j.state), j.reason);
            return true;
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&bossSteps);
const bool sTickerRegistered = registerTicker(&tickBoss);

}  // namespace
}  // namespace twili::autotest
