// Steps for enemies, enemy scaling and enemy-death sync; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"
#include "autotest/State.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "enemy/EnemyCount.hpp"
#include "enemy/EnemyDamage.hpp"
#include "enemy/EnemyScaling.hpp"
#include "enemy/EnemySync.hpp"
#include "hooks/Hooks.hpp"

#include "m_Do/m_Do_ext.h"  // the enemy headers are not self-contained

#include "SSystem/SComponent/c_malloc.h"
#include "d/actor/d_a_e_hz.h"
#include "d/actor/d_a_e_mm.h"
#include "d/actor/d_a_e_oc.h"
#include "d/actor/d_a_e_s1.h"
#include "d/actor/d_a_e_sm.h"
#include "d/d_bg_s_roof_chk.h"
#include "d/d_com_inf_game.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_layer.h"

#include <fmt/format.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace twili::autotest {
namespace {

// Actors created by spawnEnemy, by tag.
std::map<std::string, fpc_ProcID> sTags;
// Tick of the running expectEnemiesGone step at which each tag was first seen gone.
std::map<std::string, int> sGoneAt;

fpc_ProcID taggedId(const nlohmann::json& step) {
    const auto it = sTags.find(step.value("tag", std::string{}));
    return it != sTags.end() ? it->second : fpcM_ERROR_PROCESS_ID_e;
}

fopAc_ac_c* runningActor(fpc_ProcID id) {
    return id != fpcM_ERROR_PROCESS_ID_e && fopAcM_IsExecuting(id) ? fopAcM_SearchByID(id) :
                                                                     nullptr;
}

std::string tagOf(const StepContext& ctx) {
    return ctx.step.value("tag", std::string{});
}

// The ceiling over `p` (from 50 below it), or G_CM3D_F_INF.
f32 roofAbove(const cXyz& p) {
    dBgS_ObjRoofChk roof;
    roof.SetPos(cXyz(p.x, p.y - 50.0f, p.z));
    return dComIfG_Bgsp().RoofChk(&roof);
}

// A Helmasaur's helmet while it wears it.
fopAc_ac_c* helmetOf(fopAc_ac_c* ac) {
    if (fopAcM_GetName(ac) != fpcNm_E_MM_e) {
        return nullptr;
    }
    const auto* mm = reinterpret_cast<const e_mm_class*>(ac);
    return mm->field_0x672 != 0 ? fopAcM_SearchByID(mm->armorChildId) : nullptr;
}

// One actor from a spawnEnemy-style spec `e`.
bool spawnOne(StepContext& ctx, const nlohmann::json& e) {
    const std::string name = e.value("name", std::string{});
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    dStage_objectNameInf* inf = dStage_searchName(name.c_str());
    const int roomNo = dStage_roomControl_c::getStayNo();
    scene_class* room =
        roomNo >= 0 ? fopScnM_SearchByID(dStage_roomControl_c::getStatusProcID(roomNo)) : nullptr;
    if (player == nullptr || inf == nullptr || room == nullptr) {
        ctx.fail(fmt::format("spawnEnemy {}: no player, unknown actor name or no room", name));
        return false;
    }
    fopAcM_prm_class* prm = fopAcM_CreateAppend();
    if (prm == nullptr) {
        ctx.fail("spawnEnemy: out of memory");
        return false;
    }
    prm->base.parameters = static_cast<u32>(e.value("param", 0xFFFFFFFFu));
    const cXyz& anchor = e.value("anchor", std::string("player")) == "playerHome" ?
                             player->home.pos :
                             player->current.pos;
    prm->base.position = cXyz(anchor.x + e.value("dx", 0.0f),
        anchor.y + e.value("dy", 0.0f), anchor.z + e.value("dz", 0.0f));
    if (e.value("onRoof", false)) {
        // `dy` below the ceiling over that spot.
        dBgS_ObjRoofChk roof;
        roof.SetPos(cXyz(prm->base.position.x, anchor.y + 50.0f, prm->base.position.z));
        const f32 roofY = dComIfG_Bgsp().RoofChk(&roof);
        if (roofY == G_CM3D_F_INF) {
            cMl::free(prm);
            ctx.fail("spawnEnemy " + name + ": no ceiling above that spot");
            return false;
        }
        prm->base.position.y = roofY + e.value("dy", 0.0f);
    }
    prm->base.angle.x = static_cast<s16>(e.value("angleX", 0));
    prm->base.angle.z = static_cast<s16>(e.value("angleZ", 0));
    prm->base.setID = static_cast<u16>(e.value("setId", 0xFFFF));
    prm->room_no = static_cast<s8>(roomNo);
    prm->argument = inf->argument;
    // In the room's layer like a stage-placed actor, so it is deleted with the room.
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(&room->base.layer);
    fpc_ProcID id;
    if (e.value("placed", false)) {
        // Seen by Enemy Count as dStage_actorCreate's request.
        hooks::ScopeGuard placed(hooks::ScopeKind::StagePlaced, prm);
        id = fopAcM_Create(inf->procname, NULL, prm);
    } else {
        id = fopAcM_Create(inf->procname, NULL, prm);
    }
    fpcLy_SetCurrentLayer(saved);
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        cMl::free(prm);
        ctx.fail("spawnEnemy " + name + ": create request failed");
        return false;
    }
    sTags[e.value("tag", name)] = id;
    return true;
}

std::optional<bool> spawnEnemy(StepContext& ctx) {
    return spawnOne(ctx, ctx.step);
}

// All in one update, in the given order; the step's own fields are every entry's defaults.
std::optional<bool> spawnEnemies(StepContext& ctx) {
    for (const nlohmann::json& entry : ctx.step.value("enemies", nlohmann::json::array())) {
        nlohmann::json e = ctx.step;
        e.erase("enemies");
        e.update(entry);
        if (!spawnOne(ctx, e)) {
            return false;
        }
    }
    return true;
}

std::optional<bool> expectEnemyHealth(StepContext& ctx) {
    const fpc_ProcID id = taggedId(ctx.step);
    fopAc_ac_c* ac = runningActor(id);
    enemy_scaling::TrackedHealth tracked{};
    const bool isTracked = ac != nullptr && enemy_scaling::trackedHealth(id, tracked);
    fopAc_ac_c* helmet = ac != nullptr ? helmetOf(ac) : nullptr;
    const auto want = [&](const char* key, int have) {
        return !ctx.step.contains(key) || ctx.step[key].get<int>() == have;
    };
    if (ac != nullptr && want("health", ac->health) && want("max", ac->field_0x560) &&
        want("shared", enemy_damage::sharedHealth(ac)) &&
        want("hitsLeft", enemy_damage::countedHitsLeft(ac)) &&
        want("helmetHealth", helmet != nullptr ? helmet->health : -1) &&
        (!ctx.step.contains("tracked") || isTracked == ctx.step["tracked"].get<bool>()))
    {
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(ac == nullptr ?
                     fmt::format("enemy '{}' is not running", tagOf(ctx)) :
                     fmt::format("enemy '{}' has health {}/{} (tracked {}, shared {}, hits left "
                                 "{}, helmet {}), step wants {}",
                         tagOf(ctx), ac->health, ac->field_0x560, isTracked,
                         enemy_damage::sharedHealth(ac), enemy_damage::countedHitsLeft(ac),
                         helmet != nullptr ? helmet->health : -1, ctx.step.dump()));
    }
    return false;
}

std::optional<bool> killEnemy(StepContext& ctx) {
    const std::string tag = tagOf(ctx);
    const std::string how = ctx.step.value("how", std::string("disappear"));
    fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
    if (ac == nullptr) {
        ctx.fail("killEnemy: enemy '" + tag + "' is not running");
        return false;
    }
    if (how == "disappear") {
        // DISAPPEAR is fast-created in the current layer, which between frames is none.
        layer_class* saved = fpcLy_CurrentLayer();
        fpcLy_SetCurrentLayer(ac->layer_tag.layer);
        fopAcM_createDisappear(ac, &ac->current.pos, static_cast<u8>(ctx.step.value("size", 10)),
            static_cast<u8>(ctx.step.value("type", 0)), 0xFF);
        fpcLy_SetCurrentLayer(saved);
        fopAcM_delete(ac);
        // In Deku Baba's order: the zone actor bit after the delete request.
        if (ctx.step.value("onActor", false)) {
            fopAcM_onActor(ac);
        }
        return true;
    }
    if (how == "hz" || how == "hzReal") {
        if (fopAcM_GetName(ac) != fpcNm_E_HZ_e) {
            ctx.fail("killEnemy " + how + ": '" + tag + "' is not a Tile Worm");
            return false;
        }
        auto* hz = static_cast<daE_HZ_c*>(ac);
        if (how == "hz") {
            // The end of every death path: its puff, then the death wait (mode 0 sets its switch).
            layer_class* saved = fpcLy_CurrentLayer();
            fpcLy_SetCurrentLayer(ac->layer_tag.layer);
            fopAcM_createDisappear(ac, &ac->current.pos, 10, 0, 5);
            fpcLy_SetCurrentLayer(saved);
            hz->setActionMode(11);
        } else {
            // ACTION_DEATH as damage_check starts it; the roll needs ground contact.
            ac->health = 0;
            hz->field_0x6cc = 0;
            hz->setActionMode(6);
        }
        return true;
    }
    if (fopAcM_GetName(ac) != fpcNm_E_OC_e) {
        ctx.fail("killEnemy " + how + ": '" + tag + "' is not a Bokoblin");
        return false;
    }
    auto* oc = static_cast<daE_OC_c*>(ac);
    if (how == "real") {
        // What damage_check does for a finishing blow (E_OC_ACTION_DEATH, state 1).
        ac->health = 0;
        oc->setActionMode(9, 1);
        return true;
    }
    if (how == "fall") {
        // E_OC_ACTION_FALL_DEAD, as checkFall.
        oc->setActionMode(13, 0);
        return true;
    }
    ctx.fail("killEnemy: unknown how '" + how + "'");
    return false;
}

std::optional<bool> expectEnemySync(StepContext& ctx) {
    const enemy_sync::Stats& st = enemy_sync::stats();
    const std::pair<const char*, uint32_t> counters[] = {
        {"sent", st.sent},
        {"received", st.received},
        {"applied", st.applied},
        {"echoes", st.echoes},
        {"expired", st.expired},
    };
    bool match = true;
    for (const auto& [name, value] : counters) {
        if (ctx.step.contains(name) && ctx.step[name].get<uint32_t>() != value) {
            match = false;
        }
    }
    if (match) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(fmt::format("enemy sync stats sent={} received={} applied={} echoes={} "
                             "expired={}, step wants {}",
            st.sent, st.received, st.applied, st.echoes, st.expired, ctx.step.dump()));
    }
    return false;
}

std::optional<bool> expectEnemyDamage(StepContext& ctx) {
    const enemy_damage::Stats& st = enemy_damage::stats();
    const std::pair<const char*, uint32_t> counters[] = {
        {"sent", st.sent},
        {"received", st.received},
        {"applied", st.applied},
        {"floored", st.floored},
        {"healed", st.healed},
        {"dropped", st.dropped},
    };
    bool match = true;
    for (const auto& [name, value] : counters) {
        if (ctx.step.contains(name) && ctx.step[name].get<uint32_t>() != value) {
            match = false;
        }
    }
    if (match) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(fmt::format("enemy damage stats sent={} received={} applied={} floored={} "
                             "healed={} dropped={}, step wants {}",
            st.sent, st.received, st.applied, st.floored, st.healed, st.dropped,
            ctx.step.dump()));
    }
    return false;
}

std::optional<bool> expectEnemiesGone(StepContext& ctx) {
    if (!ctx.begun) {
        sGoneAt.clear();
    }
    const auto tags = ctx.step.value("tags", std::vector<std::string>{});
    for (const std::string& tag : tags) {
        const auto it = sTags.find(tag);
        if (it == sTags.end()) {
            ctx.fail("expectEnemiesGone: no enemy tagged '" + tag + "'");
            return false;
        }
        if (!fopAcM_IsExecuting(it->second) && !sGoneAt.count(tag)) {
            sGoneAt[tag] = ctx.ticks;
        }
    }
    if (sGoneAt.size() < tags.size()) {
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(
                fmt::format("expectEnemiesGone: only {} of {} gone", sGoneAt.size(), tags.size()));
        }
        return false;
    }
    int first = INT_MAX, last = INT_MIN;
    for (const auto& [tag, tick] : sGoneAt) {
        first = std::min(first, tick);
        last = std::max(last, tick);
    }
    const int maxSpread = ctx.step.value("maxSpreadTicks", INT_MAX);
    if (last - first > maxSpread) {
        ctx.fail(fmt::format(
            "expectEnemiesGone: gone over {} ticks, step allows {}", last - first, maxSpread));
        return false;
    }
    TwiliLog.info("[autotest] {} enemies gone within {} ticks", tags.size(), last - first);
    return true;
}

std::optional<bool> expectZoneActor(StepContext& ctx) {
    const int setId = ctx.step.value("setId", 0);
    const int room = ctx.step.value("room", static_cast<int>(dStage_roomControl_c::getStayNo()));
    const bool want = ctx.step.value("set", true);
    // isActor asserts on these; the zone exists while the room is loaded.
    const int zoneNo = room >= 0 && room < 64 ? dComIfGp_roomControl_getZoneNo(room) : -1;
    if (setId < 0 || setId >= dSv_zoneActor_c::ACTOR_MAX || zoneNo < 0 ||
        zoneNo >= dSv_info_c::ZONE_MAX)
    {
        ctx.fail(
            fmt::format("expectZoneActor: setId {} / room {} has no zone actor bit", setId, room));
        return false;
    }
    if ((dComIfGs_isActor(setId, room) != FALSE) == want) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(fmt::format(
            "zone actor bit {} (room {}) never became {}", setId, room, want ? "set" : "clear"));
    }
    return false;
}

using EnemyVisit = void (*)(fopAc_ac_c* ac, void* data);

// Running ENEMY-group actors whose home is the current room.
void forRoomEnemies(EnemyVisit visit, void* data) {
    struct Ctx {
        int roomNo;
        EnemyVisit visit;
        void* data;
    } c{dStage_roomControl_c::getStayNo(), visit, data};
    fopAcIt_Executor(
        [](void* p, void* raw) -> int {
            auto* ac = static_cast<fopAc_ac_c*>(p);
            const auto* c = static_cast<const Ctx*>(raw);
            if (fopAcM_GetGroup(ac) == fopAc_ENEMY_e && fopAcM_GetHomeRoomNo(ac) == c->roomNo &&
                fopAcM_IsExecuting(fopAcM_GetID(ac)))
            {
                c->visit(ac, c->data);
            }
            return 1;
        },
        &c);
}

uint8_t dupOf(fopAc_ac_c* ac) {
    const enemy_sync::SpawnKey* key = enemy_sync::trackedKey(fopAcM_GetID(ac));
    return key != nullptr ? key->dup : 0;
}

// FNV of the room's enemies (name, params, home, dup): equal on clients that made the same extras.
// `flat`: maxDist across the ground only, whatever the height.
std::pair<uint32_t, int> enemyDigest(float maxDist, bool flat) {
    struct Digest {
        std::vector<std::tuple<int, uint32_t, long, long, long, int>> list;
        float maxDist;
        bool flat;
        cXyz from;
    } d{{}, maxDist, flat, {}};
    if (fopAc_ac_c* player = dComIfGp_getPlayer(0)) {
        d.from = player->home.pos;
    }
    forRoomEnemies(
        [](fopAc_ac_c* ac, void* data) {
            auto* d = static_cast<Digest*>(data);
            const f32 dist =
                d->flat ? ac->home.pos.absXZ(d->from) : (ac->home.pos - d->from).abs();
            if (d->maxDist <= 0.0f || dist <= d->maxDist) {
                d->list.emplace_back(fopAcM_GetName(ac), fopAcM_GetParam(ac),
                    std::lround(ac->home.pos.x), std::lround(ac->home.pos.y),
                    std::lround(ac->home.pos.z), dupOf(ac));
            }
        },
        &d);
    auto& list = d.list;
    std::sort(list.begin(), list.end());
    uint32_t h = 2166136261u;
    for (const auto& e : list) {
        const long v[] = {std::get<0>(e), static_cast<long>(std::get<1>(e)), std::get<2>(e),
            std::get<3>(e), std::get<4>(e), std::get<5>(e)};
        for (size_t i = 0; i < sizeof(v); ++i) {
            h ^= reinterpret_cast<const uint8_t*>(v)[i];
            h *= 16777619u;
        }
    }
    return {h, static_cast<int>(list.size())};
}

std::optional<bool> expectEnemyCount(StepContext& ctx) {
    struct Count {
        std::string name;
        float maxDist;
        cXyz from;
        int n;
    } c{ctx.step.value("name", std::string{}), ctx.step.value("maxDist", 0.0f), {}, 0};
    if (fopAc_ac_c* player = dComIfGp_getPlayer(0)) {
        c.from = player->home.pos;
    }
    forRoomEnemies(
        [](fopAc_ac_c* ac, void* data) {
            auto* c = static_cast<Count*>(data);
            const char* name = dStage_getName(fopAcM_GetName(ac), ac->argument);
            if (name != nullptr && c->name == name &&
                (c->maxDist <= 0.0f || (ac->home.pos - c->from).abs() <= c->maxDist))
            {
                ++c->n;
            }
        },
        &c);
    const int want = ctx.step.value("count", 0);
    if (c.n == want) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("{} {} running in this room, step wants {}", c.n, c.name, want));
    }
    return false;
}

std::optional<bool> expectEnemyExtras(StepContext& ctx) {
    const auto it = sTags.find(ctx.step.value("of", std::string{}));
    fopAc_ac_c* original = it != sTags.end() ? runningActor(it->second) : nullptr;
    const int want = ctx.step.value("count", 0);
    std::string why;
    int n = 0;
    if (original == nullptr) {
        why = "the original is not running";
    } else {
        for (int dup = 1; dup <= enemy_sync::kMaxDup; ++dup) {
            fopAc_ac_c* ac = runningActor(enemy_count::extraOf(fopAcM_GetID(original), dup));
            enemy_count::ExtraSpawn spawn{};
            if (ac == nullptr || !enemy_count::extraSpawn(fopAcM_GetID(ac), spawn)) {
                continue;
            }
            ++n;
            const u32 mask = spawn.switchMask;
            if (ac->setID != 0xFFFF) {
                why = fmt::format("extra {} has setID 0x{:04X}", dup, ac->setID);
            } else if ((fopAcM_GetParam(ac) & mask) != mask ||
                       (spawn.switchInAngleZ && (spawn.angleZ & 0xFF) != 0xFF))
            {
                why = fmt::format("extra {} keeps a defeated switch (0x{:08X})", dup,
                    fopAcM_GetParam(ac));
            } else if (ac->layer_tag.layer != original->layer_tag.layer) {
                why = fmt::format("extra {} is not in its original's layer", dup);
            } else if (ctx.step.contains("roofGap")) {
                // Hangs as far below its ceiling as the original below its own.
                const f32 gap = roofAbove(ac->home.pos) - ac->home.pos.y;
                const f32 originalGap = roofAbove(original->home.pos) - original->home.pos.y;
                if (std::fabs(gap - originalGap) > ctx.step["roofGap"].get<float>()) {
                    why = fmt::format("extra {} hangs {:.0f} below its ceiling, the original {:.0f}",
                        dup, gap, originalGap);
                }
            }
        }
        if (why.empty() && n != want) {
            why = fmt::format("{} extras, step wants {}", n, want);
        }
    }
    if (why.empty()) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectEnemyExtras: " + why);
    }
    return false;
}

void dumpEnemies() {
    int roomNo = dStage_roomControl_c::getStayNo();
    fopAcIt_Executor(
        [](void* p, void* data) -> int {
            auto* ac = static_cast<fopAc_ac_c*>(p);
            if (fopAcM_GetGroup(ac) != fopAc_ENEMY_e ||
                fopAcM_GetHomeRoomNo(ac) != *static_cast<int*>(data))
            {
                return 1;
            }
            const char* name = dStage_getName(fopAcM_GetName(ac), ac->argument);
            enemy_scaling::TrackedHealth tracked{};
            const bool isTracked = enemy_scaling::trackedHealth(fopAcM_GetID(ac), tracked);
            enemy_count::ExtraSpawn spawn{};
            const bool extra = enemy_count::extraSpawn(fopAcM_GetID(ac), spawn);
            TwiliLog.info("[autotest] enemy {} id={} param=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) "
                          "health={}/{} {} dup={}{}",
                name != nullptr ? name : "?", fopAcM_GetID(ac), fopAcM_GetParam(ac), ac->home.pos.x,
                ac->home.pos.y, ac->home.pos.z, ac->health, ac->field_0x560,
                isTracked ?
                    fmt::format("scaled base={} pct={}", tracked.baseMax, tracked.appliedPct) :
                    std::string("not scaled"),
                dupOf(ac), extra ? fmt::format(" extra of {}", spawn.originalId) : std::string{});
            return 1;
        },
        &roomNo);
}

std::optional<bool> enemySteps(const std::string& op, StepContext& ctx) {
    if (op == "spawnEnemy") {
        return spawnEnemy(ctx);
    }

    if (op == "spawnEnemies") {
        return spawnEnemies(ctx);
    }

    // Live Enemy Count extras in the current room, all of them.
    if (op == "expectRoomExtras") {
        const int have = enemy_count::extrasInRoom(dStage_roomControl_c::getStayNo());
        if (have == ctx.step.value("count", 0)) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("{} extras in this room, step wants {}", have,
                ctx.step.value("count", 0)));
        }
        return false;
    }

    if (op == "expectEnemyHealth") {
        return expectEnemyHealth(ctx);
    }

    // Once we own the room: the owner's value is the room's.
    if (op == "setEnemyHealthPercent") {
        if (Session::instance().isRoomOwner()) {
            config::setInt(config::Var::EnemyHealthMultiplier, ctx.step.value("value", 100));
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail("setEnemyHealthPercent: never became the room owner");
        }
        return false;
    }

    if (op == "setEnemyCountPercent") {
        if (Session::instance().isRoomOwner()) {
            config::setInt(config::Var::EnemyCountMultiplier, ctx.step.value("value", 100));
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail("setEnemyCountPercent: never became the room owner");
        }
        return false;
    }

    if (op == "expectEnemyCountPercent") {
        const int want = ctx.step.value("value", 100);
        if (enemy_count::countPercent() == want) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format(
                "enemy count percent is {}, not {}", enemy_count::countPercent(), want));
        }
        return false;
    }

    if (op == "expectEnemyCount") {
        return expectEnemyCount(ctx);
    }

    if (op == "expectEnemyExtras") {
        return expectEnemyExtras(ctx);
    }

    if (op == "tagExtra") {
        const auto it = sTags.find(ctx.step.value("of", std::string{}));
        const fpc_ProcID id = it == sTags.end() ?
                                  fpcM_ERROR_PROCESS_ID_e :
                                  enemy_count::extraOf(it->second,
                                      static_cast<uint8_t>(ctx.step.value("dup", 1)));
        if (runningActor(id) != nullptr) {
            sTags[tagOf(ctx)] = id;
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("tagExtra: no running extra {} of '{}'", ctx.step.value("dup", 1),
                ctx.step.value("of", std::string{})));
        }
        return false;
    }

    if (op == "enemyDigest") {
        const auto [hash, count] =
            enemyDigest(ctx.step.value("maxDist", 0.0f), ctx.step.value("flat", false));
        TwiliLog.info("[autotest] enemy digest {:08x} ({} enemies)", hash, count);
        Session::instance().sendAutotestSignal(
            detail::state().instance, fmt::format("enemies:{:08x}", hash));
        return true;
    }

    // Until the peer's enemyDigest signal matches ours.
    if (op == "expectPeerEnemyDigest") {
        const auto [hash, count] =
            enemyDigest(ctx.step.value("maxDist", 0.0f), ctx.step.value("flat", false));
        const std::string name = fmt::format(
            "{}:enemies:{:08x}", ctx.step.value("from", std::string{}), hash);
        if (detail::state().signals.count(name) != 0) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("no peer enemy digest matches ours ({:08x}, {} enemies)", hash,
                count));
        }
        return false;
    }

    if (op == "expectEnemyHealthPercent") {
        const int want = ctx.step.value("value", 100);
        if (enemy_scaling::healthPercent() == want) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format(
                "enemy health percent is {}, not {}", enemy_scaling::healthPercent(), want));
        }
        return false;
    }

    // Writes health directly, as a wolf bite or a get-up does.
    if (op == "setEnemyHealth") {
        fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
        if (ac == nullptr) {
            ctx.fail("setEnemyHealth: enemy '" + tagOf(ctx) + "' is not running");
            return false;
        }
        ac->health = static_cast<s16>(ctx.step.value("health", 0));
        return true;
    }

    // An attributed hit (a cc_at_check that lowered health), unlike setEnemyHealth.
    if (op == "damageEnemy") {
        fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
        if (ac == nullptr) {
            ctx.fail("damageEnemy: enemy '" + tagOf(ctx) + "' is not running");
            return false;
        }
        // On its helmet, the hit is the helmet's (d_a_e_mm_mt.cpp cc_at_check).
        fopAc_ac_c* target = ac;
        if (ctx.step.value("helmet", false) && (target = helmetOf(ac)) == nullptr) {
            ctx.fail("damageEnemy: '" + tagOf(ctx) + "' wears no helmet");
            return false;
        }
        enemy_damage::noteLocalHitForTest(target);
        target->health = static_cast<s16>(target->health - ctx.step.value("amount", 1));
        // A Chu Worm's counted hits (C_DamageCheck).
        if (const int hits = ctx.step.value("hits", 0); hits > 0) {
            if (fopAcM_GetName(ac) != fpcNm_E_SM_e) {
                ctx.fail("damageEnemy: '" + tagOf(ctx) + "' is not a Chu Worm");
                return false;
            }
            static_cast<daE_SM_c*>(ac)->field_0x984 -= hits;
        }
        return true;
    }

    // The helmet off, as the clawshot pull leaves it (d_a_e_mm.cpp damage_checkMetOn).
    if (op == "removeHelmet") {
        fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
        if (ac == nullptr || helmetOf(ac) == nullptr) {
            ctx.fail("removeHelmet: '" + tagOf(ctx) + "' is not a running Helmasaur with a helmet");
            return false;
        }
        reinterpret_cast<e_mm_class*>(ac)->field_0x672 = 0;
        return true;
    }

    if (op == "expectBroodSize") {
        const auto it = sTags.find(ctx.step.value("of", std::string{}));
        const int have = it == sTags.end() ? -1 : enemy_count::broodSize(it->second);
        if (have == ctx.step.value("count", 0)) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("spawner '{}' makes {}, step wants {}",
                ctx.step.value("of", std::string{}), have, ctx.step.value("count", 0)));
        }
        return false;
    }

    // Tags the child at `index` of the tagged spawner's list, once it runs.
    if (op == "tagBrood") {
        const auto it = sTags.find(ctx.step.value("of", std::string{}));
        const fpc_ProcID id = it == sTags.end() ?
                                  fpcM_ERROR_PROCESS_ID_e :
                                  enemy_count::broodChild(it->second, ctx.step.value("index", 0));
        if (runningActor(id) != nullptr) {
            sTags[tagOf(ctx)] = id;
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("tagBrood: no running child {} of '{}'",
                ctx.step.value("index", 0), ctx.step.value("of", std::string{})));
        }
        return false;
    }

    if (op == "sendEnemyDamageForTest") {
        fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
        if (ac == nullptr ||
            !enemy_damage::sendForTest(ac, ctx.step.value("dmg", 1), ctx.step.value("pct", 100),
                ctx.step.value("hpAfter", -1), ctx.step.value("hits", 0)))
        {
            ctx.fail("sendEnemyDamageForTest: '" + tagOf(ctx) +
                     "' is not a running shared enemy, or no teammate is here");
            return false;
        }
        return true;
    }

    if (op == "expectEnemyDamage") {
        return expectEnemyDamage(ctx);
    }

    if (op == "deleteEnemy") {
        fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
        if (ac == nullptr || !fopAcM_delete(ac)) {
            ctx.fail(
                "deleteEnemy: enemy '" + tagOf(ctx) + "' is not running or refused the delete");
            return false;
        }
        return true;
    }

    if (op == "expectEnemyGone") {
        const fpc_ProcID id = taggedId(ctx.step);
        if (id == fpcM_ERROR_PROCESS_ID_e) {
            ctx.fail("expectEnemyGone: no enemy tagged '" + tagOf(ctx) + "'");
            return false;
        }
        enemy_scaling::TrackedHealth tracked{};
        if (!fopAcM_IsExecuting(id) && !enemy_scaling::trackedHealth(id, tracked)) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail("enemy '" + tagOf(ctx) + "' is still running or still tracked");
        }
        return false;
    }

    if (op == "dumpEnemies") {
        dumpEnemies();
        return true;
    }

    if (op == "killEnemy") {
        return killEnemy(ctx);
    }

    if (op == "expectZoneActor") {
        return expectZoneActor(ctx);
    }

    if (op == "expectEnemySync") {
        return expectEnemySync(ctx);
    }

    if (op == "expectEnemyDeadInPlace") {
        fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
        const bool dead = ac != nullptr && fopAcM_GetName(ac) == fpcNm_E_HZ_e &&
                          static_cast<daE_HZ_c*>(ac)->field_0x6e8 != 0 &&
                          fopAcM_GetGroup(ac) != fopAc_ENEMY_e;
        if (dead) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(ac == nullptr ? "enemy '" + tagOf(ctx) + "' is not running" :
                                     "enemy '" + tagOf(ctx) + "' is not defeated in place");
        }
        return false;
    }

    // Tagged Shadow Beasts into their downed wait: the group is condemned without the demo.
    if (op == "forceGroupFail") {
        for (const std::string& tag : ctx.step.value("tags", std::vector<std::string>{})) {
            const auto it = sTags.find(tag);
            fopAc_ac_c* ac = it != sTags.end() ? runningActor(it->second) : nullptr;
            if (ac == nullptr || fopAcM_GetName(ac) != fpcNm_E_S1_e) {
                ctx.fail("forceGroupFail: '" + tag + "' is not a running Shadow Beast");
                return false;
            }
            auto* s1 = static_cast<e_s1_class*>(ac);
            s1->mAction = 10;  // ACT_FAIL
            s1->mMode = 10;
            s1->mTimers[1] = 0;
        }
        return true;
    }

    if (op == "expectEnemiesGone") {
        return expectEnemiesGone(ctx);
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&enemySteps);

}  // namespace
}  // namespace twili::autotest
