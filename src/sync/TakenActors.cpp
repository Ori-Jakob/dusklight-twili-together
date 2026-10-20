// Poes whose soul a teammate took fade here too; a golden wolf whose skill a teammate learned goes.

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "m_Do/m_Do_ext.h"  // the enemy headers are not self-contained

#include "d/actor/d_a_e_hp.h"
#include "d/actor/d_a_e_po.h"
#include "d/actor/d_a_npc_gwolf.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_name.h"

#include <vector>

namespace twili::sync::detail {
namespace {

// daE_HP_c::action's executeDead.
constexpr int kImpPoeDead = 6;
// l_delFlag of d_a_npc_gwolf.cpp: each field wolf's skill, by its mode.
constexpr int kWolfSkillLabels[] = {0x153, 0x1EC, 0x1ED, 0x1EE, 0x1EF, 0x1F0, 0x1F1};
// Ticks a taken actor waits out an event of ours before it stays.
constexpr int kPendingTicks = 60 * 30;

struct Pending {
    fpc_ProcID id;
    int ticksLeft;
};
std::vector<Pending> s_pending;

int poeSwitch(fopAc_ac_c* actor) {
    switch (fopAcM_GetName(actor)) {
    case fpcNm_E_HP_e: return static_cast<daE_HP_c*>(actor)->bitSw;
    case fpcNm_E_PO_e: return reinterpret_cast<e_po_class*>(actor)->BitSW;
    default: return -1;
    }
}

// We pull its soul out, or the wolf holds it (a delete would cut Link's grab).
bool poeDying(fopAc_ac_c* actor) {
    if (fopAcM_GetName(actor) == fpcNm_E_HP_e) {
        return static_cast<daE_HP_c*>(actor)->mAction == kImpPoeDead;
    }
    const auto* po = reinterpret_cast<e_po_class*>(actor);
    return po->mActionID == ACT_WOLF_BITE || po->mActionID == ACT_DEAD;
}

bool fieldWolf(fopAc_ac_c* actor) {
    if (fopAcM_GetName(actor) != fpcNm_NPC_GWOLF_e) {
        return false;
    }
    const auto* wolf = static_cast<daNpc_GWolf_c*>(actor);
    return wolf->mType == 0 && wolf->mParamMode >= 0;
}

bool wolfSkillBit(uint16_t no) {
    for (const int label : kWolfSkillLabels) {
        if (dSv_event_flag_c::saveBitLabels[label] == no) {
            return true;
        }
    }
    return false;
}

// createDisappear's puff without its item roll, queued in the Poe's layer (between frames the
// current layer is none).
void puff(fopAc_ac_c* actor) {
    layer_class* layer = actor->layer_tag.layer;
    if (layer == nullptr) {
        return;
    }
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(layer);
    fopAcM_create(fpcNm_DISAPPEAR_e, (0xFFu << 16) | (8u << 8) | 3u, &actor->current.pos,
        fopAcM_GetRoomNo(actor), &actor->current.angle, nullptr, -1);
    fpcLy_SetCurrentLayer(saved);
}

void addPending(fpc_ProcID id) {
    for (const Pending& p : s_pending) {
        if (p.id == id) {
            return;
        }
    }
    s_pending.push_back({id, kPendingTicks});
}

// Not in the middle of an event of ours; true once it is gone or stays for good.
bool tryRemove(fopAc_ac_c* actor) {
    if (dComIfGp_event_runCheck() || actor->eventInfo.checkCommandDemoAccrpt()) {
        return false;
    }
    if (fieldWolf(actor)) {
        // isDelete reads the skill's event bit and turns the wolf's map marker off, as on load.
        RemoteApplyGuard guard;
        if (!static_cast<daNpc_GWolf_c*>(actor)->isDelete()) {
            return true;
        }
        TwiliLog.info("[sync] golden wolf (mode {}) taught a teammate its skill: removed",
            static_cast<daNpc_GWolf_c*>(actor)->mParamMode);
        fopAcM_delete(actor);
    } else {
        if (poeDying(actor)) {
            return true;
        }
        TwiliLog.info("[sync] Poe (switch {}) taken by a teammate: removed", poeSwitch(actor));
        puff(actor);
        fopAcM_delete(actor);
    }
    state().stats.actorsRemoved++;
    return true;
}

struct PoeMatch {
    int bitSw;
    int room;  // -1: any room
};

void* removePoe(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const auto* match = static_cast<const PoeMatch*>(data);
    if (poeSwitch(actor) != match->bitSw || !fopAcM_IsExecuting(fopAcM_GetID(actor)) ||
        (match->room >= 0 && fopAcM_GetHomeRoomNo(actor) != match->room))
    {
        return nullptr;
    }
    if (!tryRemove(actor)) {
        addPending(fopAcM_GetID(actor));
    }
    return nullptr;
}

void* removeWolf(void* proc, void*) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (fieldWolf(actor) && fopAcM_IsExecuting(fopAcM_GetID(actor)) && !tryRemove(actor)) {
        addPending(fopAcM_GetID(actor));
    }
    return nullptr;
}

struct Dying {
    int bitSw;
    bool found = false;
};

void* findDying(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    auto* dying = static_cast<Dying*>(data);
    if (poeSwitch(actor) == dying->bitSw && fopAcM_IsExecuting(fopAcM_GetID(actor)) &&
        poeDying(actor))
    {
        dying->found = true;
        return actor;
    }
    return nullptr;
}

}  // namespace

bool poeDyingForUs(int bitSw) {
    Dying dying{bitSw};
    fopAcM_Search(&findDying, &dying);
    return dying.found;
}

void removeTakenPoes(int bitSw, int roomNo) {
    if (bitSw < 0 || bitSw >= 0xFF) {
        return;
    }
    PoeMatch match{bitSw, roomNo};
    fopAcM_Search(&removePoe, &match);
}

void removeTakenGoldenWolves(uint16_t eventBit) {
    if (wolfSkillBit(eventBit)) {
        fopAcM_Search(&removeWolf, nullptr);
    }
}

void tickTakenActors() {
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        fopAc_ac_c* actor = fopAcM_SearchByID(p.id);
        if (actor != nullptr && fopAcM_IsExecuting(p.id) && !tryRemove(actor) && --p.ticksLeft > 0) {
            i++;
            continue;
        }
        s_pending[i] = s_pending.back();
        s_pending.pop_back();
    }
}

void clearTakenActors() {
    s_pending.clear();
}

}  // namespace twili::sync::detail
