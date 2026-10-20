// Extras are made once their original ran, from its spawn data and a hash of its key (never the game
// RNG), so every client gets the same ones.

#include "enemy/EnemyCount.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"

#include "m_Do/m_Do_ext.h"

#include "JSystem/JKernel/JKRExpHeap.h"
#include "SSystem/SComponent/c_malloc.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_obj_enemy_create.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_bg_s_lin_chk.h"
#include "d/d_bg_s_roof_chk.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace twili::enemy_count {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxExtrasPerEnemy = 4;
constexpr int kMaxExtrasPerRoom = 24;
constexpr u32 kMinFreeHeap = 4u * 1024 * 1024;
constexpr size_t kMaxPlaced = 512;
// The original runs a few updates first: its room's collision is up by then.
constexpr int kSettleUpdates = 3;
// A placed request this old no longer holds its room's decision back.
constexpr auto kPendingLimit = std::chrono::seconds(20);
// An extra that fell or strayed this far can hold a room clear forever.
constexpr float kMaxFall = 1000.0f;
constexpr float kMaxStray = 3000.0f;
constexpr auto kStrayTime = std::chrono::seconds(10);
// E_Make keeps its count in four bits.
constexpr int kMaxBrood = 15;

enum class Place : u8 { None, Ground, Air, Roof, GroundOrWater };

struct DupSpec {
    s16 procName;
    // Where the "defeated" switch lives: forced to 0xFF in extras.
    u32 switchMask;
    bool switchInAngleZ;
    Place place;
    // The variant's place instead (Place::None: not duplicated).
    Place (*placeFor)(u32 params);
};

// Type 2 is the bridge lookout, whose switch is elsewhere.
Place bokoblinPlace(u32 params) {
    const u8 type = params & 0xFF;
    return type == 0 || type == 1 || type == 0xFF ? Place::Ground : Place::None;
}

// Type 1 hangs from the ceiling until Link rolls into a wall.
Place gohmaPlace(u32 params) {
    return (params & 0xFF) == 1 ? Place::Roof : Place::Ground;
}

// 2 hangs from the ceiling; 1 grows back after every death (Ook's) and never dies.
Place dekuBabaPlace(u32 params) {
    const u8 type = params & 0xFF;
    return type == 1 ? Place::None : type == 2 ? Place::Roof : Place::Ground;
}

constexpr DupSpec kDupSpecs[] = {
    {fpcNm_E_OC_e, 0x00FF0000, false, Place::Ground, bokoblinPlace},       // Bokoblin
    {fpcNm_E_BS_e, 0x00FF0000, false, Place::Ground, nullptr},             // Stalkin
    {fpcNm_E_DN_e, 0xFF000000, false, Place::Ground, nullptr},             // Lizalfos
    {fpcNm_E_MF_e, 0xFF000000, false, Place::Ground, nullptr},             // Dynalfos
    {fpcNm_E_DD_e, 0xFF000000, false, Place::Ground, nullptr},             // Dodongo
    {fpcNm_E_MM_e, 0x0000FF00, false, Place::Ground, nullptr},             // Helmasaur (makes its own shell)
    {fpcNm_E_SF_e, 0, true, Place::Ground, nullptr},                       // Stalfos
    {fpcNm_E_BA_e, 0xFF000000, false, Place::Air, nullptr},                // Keese
    {fpcNm_E_KG_e, 0xFF000000, false, Place::Ground, gohmaPlace},          // Young Gohma
    {fpcNm_E_TT_e, 0x000000FF, false, Place::GroundOrWater, nullptr},      // Tektite
    {fpcNm_E_DB_e, 0xFF000000, false, Place::Ground, dekuBabaPlace},       // Deku Baba (makes its own leaf)
};

struct Job {
    enemy_sync::SpawnKey key;
    // The append is freed right after the original's create.
    fopAcM_prm_class prm;
    const DupSpec* spec;
    Place place;
    int seenExecuting = 0;
    // Deleted before its room decided: keeps its place in the room's order, spawns nothing.
    bool dead = false;
};

struct Extra {
    enemy_sync::SpawnKey key;
    fpc_ProcID originalId;
    const DupSpec* spec;
    fopAc_ac_c* actor = nullptr;  // set once created, valid until onActorDeleted
    cXyz spot;
    u32 params;
    s16 angleZ;
    Clock::time_point strayAt{};
    bool removed = false;
};

struct Pending {
    int8_t roomNo;
    Clock::time_point at;
};

// A spawner whose count we raised.
struct Brood {
    int8_t roomNo;
    int extra;
};

struct BroodSlot {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    int round = 0;
};

std::unordered_set<fpc_ProcID> s_placed;
std::deque<fpc_ProcID> s_placedOrder;
// Placed requests of enemies we duplicate, not created yet: their room waits for them.
std::unordered_map<fpc_ProcID, Pending> s_pending;
std::unordered_map<fpc_ProcID, Job> s_jobs;
std::unordered_map<fpc_ProcID, Extra> s_extras;
std::unordered_map<fpc_ProcID, Brood> s_broods;
std::unordered_map<fpc_ProcID, std::vector<BroodSlot>> s_broodSlots;

const DupSpec* findSpec(s16 procName, u32 params, Place& place) {
    for (const DupSpec& s : kDupSpecs) {
        if (s.procName != procName) {
            continue;
        }
        place = s.placeFor != nullptr ? s.placeFor(params) : s.place;
        return place != Place::None ? &s : nullptr;
    }
    return nullptr;
}

uint32_t fnv(uint32_t h, const void* data, size_t n) {
    const auto* b = static_cast<const uint8_t*>(data);
    while (n-- != 0) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

uint32_t keyHash(const enemy_sync::SpawnKey& k) {
    const int32_t v[] = {k.procName, k.roomNo, static_cast<int32_t>(k.params), k.setId,
        static_cast<int32_t>(std::lround(k.home[0])), static_cast<int32_t>(std::lround(k.home[1])),
        static_cast<int32_t>(std::lround(k.home[2]))};
    return fnv(2166136261u, v, sizeof(v));
}

// The order every client decides a room's extras in.
bool keyLess(const enemy_sync::SpawnKey& a, const enemy_sync::SpawnKey& b) {
    const auto tie = [](const enemy_sync::SpawnKey& k) {
        return std::make_tuple(k.procName, k.params, k.setId, std::lround(k.home[0]),
            std::lround(k.home[1]), std::lround(k.home[2]), k.dup);
    };
    return tie(a) < tie(b);
}

uint32_t nextRand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Extras for one enemy at `pct`; `hash` picks who gets the fraction.
int extrasFor(uint32_t hash, int pct) {
    const int n = pct / 100 - 1 + (static_cast<int>(hash % 100) < pct % 100 ? 1 : 0);
    return std::clamp(n, 0, kMaxExtrasPerEnemy);
}

bool heapLow() {
    return mDoExt_getGameHeap()->getFreeSize() < kMinFreeHeap;
}

// The roof above `p`, or G_CM3D_F_INF.
f32 roofAbove(const cXyz& p) {
    dBgS_ObjRoofChk roof;
    roof.SetPos(cXyz(p.x, p.y - 50.0f, p.z));
    return dComIfG_Bgsp().RoofChk(&roof);
}

// Around the original's placement: no wall in between, and the ground (or water, or ceiling) the
// original stands on at about the same height.
bool findSpawnPos(const cXyz& home, uint32_t seed, int k, Place place, cXyz& out) {
    uint32_t rng = seed != 0 ? seed : 0x9E3779B9u;
    const s16 base = static_cast<s16>((nextRand(rng) & 0xFFFF) + k * 0x5555);
    const f32 lift = place == Place::Air ? 0.0f : place == Place::Roof ? -60.0f : 60.0f;
    // Without a ceiling above it the original hangs in the air, and so does its extra.
    const f32 homeRoof = place == Place::Roof ? roofAbove(home) : G_CM3D_F_INF;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const s16 angle = static_cast<s16>(base + attempt * 0x2000);
        const f32 r = 90.0f + static_cast<f32>(nextRand(rng) % 61);
        cXyz cand(home.x + cM_ssin(angle) * r, home.y, home.z + cM_scos(angle) * r);
        cXyz from(home.x, home.y + lift, home.z);
        cXyz to(cand.x, cand.y + lift, cand.z);
        dBgS_LinChk lin;
        lin.Set(&from, &to, nullptr);
        if (dComIfG_Bgsp().LineCross(&lin)) {
            continue;
        }
        if (place == Place::Roof && homeRoof != G_CM3D_F_INF) {
            const f32 candRoof = roofAbove(cand);
            if (candRoof == G_CM3D_F_INF || std::fabs(candRoof - homeRoof) > 60.0f) {
                continue;
            }
            cand.y = home.y + (candRoof - homeRoof);
        } else if (place == Place::Ground || place == Place::GroundOrWater) {
            cXyz probe(cand.x, cand.y + 100.0f, cand.z);
            dBgS_ObjGndChk gnd;
            gnd.SetPos(&probe);
            const f32 groundY = dComIfG_Bgsp().GroundCross(&gnd);
            // Bokoblin's own water and lava test.
            dBgS_ObjGndChk_Spl spl;
            spl.SetPos(&probe);
            const f32 liquidY = dComIfG_Bgsp().GroundCross(&spl);
            if (liquidY != -G_CM3D_F_INF && (groundY == -G_CM3D_F_INF || liquidY >= groundY)) {
                // Tektites stand on water (attribute 7) as on ground.
                if (place != Place::GroundOrWater || dComIfG_Bgsp().GetPolyAtt0(spl) != 7 ||
                    std::fabs(liquidY - home.y) > 80.0f)
                {
                    continue;
                }
                cand.y = liquidY;
            } else {
                if (groundY == -G_CM3D_F_INF || std::fabs(groundY - home.y) > 80.0f) {
                    continue;
                }
                cand.y = groundY;
            }
        }
        out = cand;
        return true;
    }
    return false;
}

int roomExtras(int8_t roomNo) {
    return static_cast<int>(std::count_if(s_extras.begin(), s_extras.end(),
        [&](const auto& e) { return e.second.key.roomNo == roomNo; }));
}

void spawn(const Job& job, fpc_ProcID originalId, fopAc_ac_c* original, int n) {
    const uint32_t hash = keyHash(job.key);
    const cXyz home = job.prm.base.position;
    // The room's layer: counted by room clears, deleted with the room.
    layer_class* layer = original->layer_tag.layer;
    // Numbered by attempt: a spot missing on one client never renumbers the next extra.
    for (int k = 1; k <= n; ++k) {
        cXyz spot;
        if (!findSpawnPos(home, hash ^ (static_cast<uint32_t>(k) * 0x9E3779B9u), k, job.place,
                spot))
        {
            TwiliLog.info("[enemy] no spot for extra {} of {}", k, enemy_sync::keyText(job.key));
            continue;
        }
        fopAcM_prm_class* prm = fopAcM_CreateAppend();
        if (prm == nullptr) {
            break;
        }
        *prm = job.prm;
        prm->base.parameters = static_cast<u32>(job.prm.base.parameters) | job.spec->switchMask;
        csXyz angle = job.prm.base.angle;
        if (job.spec->switchInAngleZ) {
            angle.z = static_cast<s16>((angle.z & 0xFF00) | 0xFF);
        }
        prm->base.angle = angle;
        prm->base.position = spot;
        prm->base.setID = 0xFFFF;
        prm->parent_id = fpcM_ERROR_PROCESS_ID_e;
        layer_class* saved = fpcLy_CurrentLayer();
        fpcLy_SetCurrentLayer(layer);
        const fpc_ProcID id = fopAcM_Create(job.key.procName, NULL, prm);
        fpcLy_SetCurrentLayer(saved);
        if (id == fpcM_ERROR_PROCESS_ID_e) {
            cMl::free(prm);
            continue;
        }
        Extra e{};
        e.key = job.key;
        e.key.dup = static_cast<uint8_t>(k);
        e.originalId = originalId;
        e.spec = job.spec;
        e.spot = spot;
        e.params = static_cast<u32>(job.prm.base.parameters) | job.spec->switchMask;
        e.angleZ = angle.z;
        s_extras[id] = e;
    }
}

// The whole room at once, in key order: the room cap picks the same extras on every client.
void decideRoom(int8_t roomNo) {
    std::vector<fpc_ProcID> ids;
    for (const auto& [id, job] : s_jobs) {
        if (job.key.roomNo == roomNo) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end(),
        [](fpc_ProcID a, fpc_ProcID b) { return keyLess(s_jobs[a].key, s_jobs[b].key); });
    const int pct = countPercent();
    const bool low = heapLow();
    int budget = kMaxExtrasPerRoom - roomExtras(roomNo);
    int capped = 0;
    for (const fpc_ProcID id : ids) {
        const Job job = s_jobs[id];
        s_jobs.erase(id);
        const int want = extrasFor(keyHash(job.key), pct);
        const int n = std::min(want, std::max(budget, 0));
        capped += want - n;
        budget -= n;
        if (n <= 0 || job.dead) {
            continue;
        }
        if (low) {
            TwiliLog.warn("[enemy] game heap low: no extras for {}", enemy_sync::keyText(job.key));
            continue;
        }
        if (fopAc_ac_c* original = fopAcM_SearchByID(id)) {
            spawn(job, id, original, n);
        }
    }
    if (capped > 0) {
        TwiliLog.info("[enemy] room {}: {} extras over the cap of {} left out", roomNo, capped,
            kMaxExtrasPerRoom);
    }
}

// Removed like a defeat, so teammates lose theirs too.
void removeStuck(Extra& e, const char* why) {
    fopAc_ac_c* ac = e.actor;
    e.removed = true;
    TwiliLog.info("[enemy] removed extra {} of {}: {}", e.key.dup, enemy_sync::keyText(e.key), why);
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(ac->layer_tag.layer);
    fopAcM_createDisappear(ac, &ac->current.pos, 10, 0, 0xFF);
    fpcLy_SetCurrentLayer(saved);
    enemy_sync::markDefeated(ac);
    fopAcM_delete(ac);
}

void tickStuck() {
    const auto now = Clock::now();
    for (auto& [id, e] : s_extras) {
        if (e.actor == nullptr || e.removed || !fopAcM_IsExecuting(id) || enemy_sync::isDying(id)) {
            continue;
        }
        const cXyz& pos = e.actor->current.pos;
        if (pos.y < e.spot.y - kMaxFall) {
            removeStuck(e, "fell out of the room");
            continue;
        }
        const f32 dx = pos.x - e.spot.x, dy = pos.y - e.spot.y, dz = pos.z - e.spot.z;
        if (dx * dx + dy * dy + dz * dz <= kMaxStray * kMaxStray) {
            e.strayAt = {};
        } else if (e.strayAt == Clock::time_point{}) {
            e.strayAt = now;
        } else if (now - e.strayAt > kStrayTime) {
            removeStuck(e, "strayed from its spot");
        }
    }
}

// E_Make types whose children kill-sync; a Shadow Beast count is also its group number. Other
// spawners (wolf packs, Skullfish, Leevers) place their children by camera, game RNG or timing.
bool broodRaisable(u32 type) {
    switch (type) {
    case ENEMY_KEESE:
    case ENEMY_BUBBLE:
    case ENEMY_BOKOBLIN:
    case ENEMY_LIZALFOS:
    case ENEMY_BOKOBLIN2:
        return true;
    default:
        return false;
    }
}

int broodExtras(int8_t roomNo) {
    int n = 0;
    for (const auto& [id, b] : s_broods) {
        if (b.roomNo == roomNo) {
            n += b.extra;
        }
    }
    return n;
}

// The spawner reads its count (params bits 4-7) when it is created; its list follows it.
void raiseBrood(fpc_ProcID id, fopAcM_prm_class* prm) {
    const u32 params = static_cast<u32>(prm->base.parameters);
    const int num = (params >> 4) & 0xF;
    const int pct = countPercent();
    if (pct <= 100 || num == 0 || !broodRaisable(params & 0xF)) {
        return;
    }
    if (heapLow()) {
        TwiliLog.warn("[enemy] game heap low: no extra spawns in room {}", prm->room_no);
        return;
    }
    const cXyz& home = prm->base.position;
    const enemy_sync::SpawnKey key{fpcNm_Obj_E_CREATE_e, prm->room_no, params,
        static_cast<uint16_t>(prm->base.setID), {home.x, home.y, home.z}};
    uint32_t h = keyHash(key);
    int extra = 0;
    for (int i = 0; i < num; ++i) {
        extra += extrasFor(nextRand(h), pct);
    }
    // Requests come in the stage's placement order, the same on every client.
    extra = std::min({extra, kMaxBrood - num, kMaxExtrasPerRoom - broodExtras(prm->room_no)});
    if (extra <= 0) {
        return;
    }
    prm->base.parameters = (params & ~0xF0u) | (static_cast<u32>(num + extra) << 4);
    s_broods[id] = {prm->room_no, extra};
    TwiliLog.info("[enemy] spawner in room {} makes {} instead of {}", prm->room_no, num + extra,
        num);
}

void pruneRequests(Clock::time_point now) {
    for (auto it = s_pending.begin(); it != s_pending.end();) {
        // Created ones left in onActorCreated; these failed or never finish.
        if (!fpcM_IsCreating(it->first) || now - it->second.at > kPendingLimit) {
            it = s_pending.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s_broods.begin(); it != s_broods.end();) {
        if (!fpcM_IsCreating(it->first) && !fopAcM_IsExecuting(it->first)) {
            it = s_broods.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

void onPlacedRequested(fpc_ProcID id, int16_t procName, fopAcM_prm_class* prm) {
    if (id == fpcM_ERROR_PROCESS_ID_e || prm == nullptr || !s_placed.insert(id).second) {
        return;
    }
    // A request cancelled before its create never reaches onActorCreated; the oldest go first.
    s_placedOrder.push_back(id);
    if (s_placedOrder.size() > kMaxPlaced) {
        s_placed.erase(s_placedOrder.front());
        s_placedOrder.pop_front();
    }
    Place place;
    if (findSpec(procName, static_cast<u32>(prm->base.parameters), place) != nullptr) {
        s_pending[id] = {prm->room_no, Clock::now()};
    }
    if (procName == fpcNm_Obj_E_CREATE_e) {
        raiseBrood(id, prm);
    }
}

void onActorCreated(fopAc_ac_c* ac) {
    const fpc_ProcID id = fopAcM_GetID(ac);
    if (const auto it = s_extras.find(id); it != s_extras.end()) {
        it->second.actor = ac;
        return;
    }
    s_pending.erase(id);
    if (s_placed.erase(id) == 0) {
        return;
    }
    const fopAcM_prm_class* prm = fopAcM_GetAppend(ac);
    if (prm == nullptr) {
        return;
    }
    Job job{};
    job.spec = findSpec(fopAcM_GetName(ac), static_cast<u32>(prm->base.parameters), job.place);
    if (job.spec == nullptr || !enemy_sync::appendKey(ac, job.key)) {
        return;
    }
    job.prm = *prm;
    // The same placement from a room loaded again.
    std::erase_if(s_jobs,
        [&](const auto& e) { return e.second.dead && enemy_sync::sameKey(e.second.key, job.key); });
    s_jobs[id] = job;
}

void onActorDeleted(fopAc_ac_c* ac) {
    const fpc_ProcID id = fopAcM_GetID(ac);
    if (const auto it = s_jobs.find(id); it != s_jobs.end()) {
        it->second.dead = true;
    }
    s_extras.erase(id);
    s_placed.erase(id);
    s_pending.erase(id);
    s_broods.erase(id);
    s_broodSlots.erase(id);
}

bool extraKey(fpc_ProcID id, enemy_sync::SpawnKey& out) {
    const auto it = s_extras.find(id);
    if (it == s_extras.end()) {
        return false;
    }
    out = it->second.key;
    return true;
}

bool isExtra(fpc_ProcID id) {
    return s_extras.count(id) != 0;
}

fpc_ProcID extraOf(fpc_ProcID originalId, uint8_t dup) {
    for (const auto& [id, e] : s_extras) {
        if (e.originalId == originalId && e.key.dup == dup && fopAcM_IsExecuting(id)) {
            return id;
        }
    }
    return fpcM_ERROR_PROCESS_ID_e;
}

bool extraSpawn(fpc_ProcID id, ExtraSpawn& out) {
    const auto it = s_extras.find(id);
    if (it == s_extras.end()) {
        return false;
    }
    const Extra& e = it->second;
    out = {e.originalId, e.params, 0xFFFF, e.angleZ, e.spec->switchMask, e.spec->switchInAngleZ};
    return true;
}

int extrasInRoom(int roomNo) {
    return roomExtras(static_cast<int8_t>(roomNo));
}

bool broodKey(fopAc_ac_c* ac, enemy_sync::SpawnKey& out) {
    const fopAcM_prm_class* prm = fopAcM_GetAppend(ac);
    if (prm == nullptr || prm->parent_id == fpcM_ERROR_PROCESS_ID_e) {
        return false;
    }
    fopAc_ac_c* parent = fopAcM_SearchByID(prm->parent_id);
    if (parent == nullptr || fopAcM_GetName(parent) != fpcNm_Obj_E_CREATE_e) {
        return false;
    }
    const auto* spawner = static_cast<const daObjE_CREATE_c*>(parent);
    const fpc_ProcID id = fopAcM_GetID(ac);
    const int num = spawner->mEnemyNum;
    int index = -1;
    for (int i = 0; i < num && spawner->mActorList != nullptr; ++i) {
        if (spawner->mActorList[i] == id) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return false;
    }
    // A looping spawner refills its list: numbered on, round after round.
    std::vector<BroodSlot>& slots = s_broodSlots[prm->parent_id];
    slots.resize(num);
    BroodSlot& slot = slots[index];
    if (slot.id != id) {
        if (slot.id != fpcM_ERROR_PROCESS_ID_e) {
            ++slot.round;
        }
        slot.id = id;
    }
    const int ordinal = slot.round * num + index;
    if (ordinal >= 0x3F00) {
        return false;
    }
    const cXyz& home = parent->home.pos;
    out = {fopAcM_GetName(ac), fopAcM_GetHomeRoomNo(parent), static_cast<uint32_t>(prm->base.parameters),
        static_cast<uint16_t>(kBroodSetId + ordinal), {home.x, home.y, home.z}};
    return true;
}

fpc_ProcID broodChild(fpc_ProcID spawnerId, int index) {
    fopAc_ac_c* ac = fopAcM_SearchByID(spawnerId);
    if (ac == nullptr || fopAcM_GetName(ac) != fpcNm_Obj_E_CREATE_e) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    const auto* spawner = static_cast<const daObjE_CREATE_c*>(ac);
    if (spawner->mActorList == nullptr || index < 0 || index >= spawner->mEnemyNum) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    return spawner->mActorList[index];
}

int broodSize(fpc_ProcID spawnerId) {
    fopAc_ac_c* ac = fopAcM_SearchByID(spawnerId);
    return ac != nullptr && fopAcM_GetName(ac) == fpcNm_Obj_E_CREATE_e ?
               static_cast<const daObjE_CREATE_c*>(ac)->mEnemyNum :
               -1;
}

void tick() {
    tickStuck();
    const auto now = Clock::now();
    pruneRequests(now);
    if (s_jobs.empty()) {
        return;
    }
    // Offline the room's percent is unknown: undecided, so a later join matches teammates.
    const bool ready = Session::active() && Session::instance().joined() &&
                       dComIfGp_getPlayer(0) != nullptr && !dComIfGp_event_runCheck() &&
                       !dComIfGp_isEnableNextStage();
    struct Room {
        bool alive = false;
        bool settled = true;
    };
    std::unordered_map<int8_t, Room> rooms;
    for (auto& [id, job] : s_jobs) {
        Room& room = rooms[job.key.roomNo];
        if (job.dead) {
            continue;
        }
        room.alive = true;
        if (fopAcM_IsExecuting(id) && job.seenExecuting < kSettleUpdates) {
            ++job.seenExecuting;
        }
        if (job.seenExecuting < kSettleUpdates) {
            room.settled = false;
        }
    }
    for (const auto& [roomNo, room] : rooms) {
        if (!room.alive) {
            // Unloaded, or every original died before its room decided.
            const int8_t r = roomNo;
            std::erase_if(s_jobs, [&](const auto& e) { return e.second.key.roomNo == r; });
            continue;
        }
        const bool waiting = std::any_of(s_pending.begin(), s_pending.end(),
            [&](const auto& p) { return p.second.roomNo == roomNo; });
        // Once per area load: a slider change applies when the area loads again.
        if (ready && room.settled && !waiting) {
            decideRoom(roomNo);
        }
    }
}

int countPercent() {
    if (!Session::active() || !Session::instance().joined()) {
        return 100;
    }
    return std::clamp(Session::instance().roomState().enemyCountMultiplier, 100, 300);
}

void shutdown() {
    // Live extras stay: they die with their room.
    s_placed.clear();
    s_placedOrder.clear();
    s_pending.clear();
    s_jobs.clear();
    s_extras.clear();
    s_broods.clear();
    s_broodSlots.clear();
}

}  // namespace twili::enemy_count
