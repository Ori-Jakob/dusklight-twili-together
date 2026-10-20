// Block and chain sync: identity, the gate, the packets and who wins a concurrent move.

#include "push/PushState.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace twili::push {
namespace detail {
namespace {

using json = nlohmann::json;

constexpr float kHomeTolerance = 1.0f;
// Two moves from the same start this close together were made at once.
constexpr auto kConcurrentWindow = std::chrono::seconds(5);
// Wire positions are this far off at most; a block on its grid is far closer.
constexpr float kFromTolerance = 8.0f;

std::map<fpc_ProcID, Record> s_records;

Kind kindOf(s16 procName) {
    switch (procName) {
    case fpcNm_Obj_Movebox_e: return Kind::Movebox;
    case fpcNm_Obj_IceBlock_e: return Kind::IceBlock;
    case fpcNm_Obj_SwChain_e: return Kind::SwChain;
    default: return Kind::None;
    }
}

bool intField(const json& j, const char* key, int64_t lo, int64_t hi, int64_t& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) {
        return false;
    }
    out = it->is_number_unsigned() && it->get<uint64_t>() > uint64_t(INT64_MAX) ?
              INT64_MAX :
              it->get<int64_t>();
    return out >= lo && out <= hi;
}

bool floatField(const json& j, const char* key, float& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return false;
    }
    const double v = it->get<double>();
    if (!std::isfinite(v) || std::fabs(v) >= 1e6) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

bool boolField(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

bool parseXZ(const json& j, const char* key, XZ& out) {
    const auto it = j.find(key);
    return it != j.end() && it->is_object() && floatField(*it, "x", out.x) &&
           floatField(*it, "z", out.z);
}

bool parseXYZ(const json& j, const char* key, cXyz& out) {
    const auto it = j.find(key);
    return it != j.end() && it->is_object() && floatField(*it, "x", out.x) &&
           floatField(*it, "y", out.y) && floatField(*it, "z", out.z);
}

bool parseKey(const json& packet, Key& out) {
    const auto k = packet.find("key");
    if (k == packet.end() || !k->is_object()) {
        return false;
    }
    int64_t roomNo, procName, params, setId;
    if (!intField(*k, "roomNo", -1, 63, roomNo) || !intField(*k, "procName", 0, 0x7FFF, procName) ||
        !intField(*k, "params", 0, 0xFFFFFFFFll, params) || !intField(*k, "setId", 0, 0xFFFF, setId))
    {
        return false;
    }
    const auto home = k->find("home");
    if (home == k->end() || !home->is_object() || !floatField(*home, "x", out.home[0]) ||
        !floatField(*home, "y", out.home[1]) || !floatField(*home, "z", out.home[2]))
    {
        return false;
    }
    const auto angle = k->find("angle");
    if (angle == k->end() || !angle->is_array() || angle->size() != 3) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!(*angle)[i].is_number_integer()) {
            return false;
        }
        const int64_t a = (*angle)[i].get<int64_t>();
        if (a < SHRT_MIN || a > SHRT_MAX) {
            return false;
        }
        out.angle[i] = static_cast<int16_t>(a);
    }
    out.roomNo = static_cast<int8_t>(roomNo);
    out.procName = static_cast<int16_t>(procName);
    out.params = static_cast<uint32_t>(params);
    out.setId = static_cast<uint16_t>(setId);
    return true;
}

// Sender, team, protocol, packet version, member gate, and our current stage and layer.
bool acceptFromTeammate(const json& packet) {
    const Session& session = Session::instance();
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == session.selfClientId() ||
        packet.value("senderSessionKey", std::string{}) == session.sessionKey() ||
        packet.value("teamId", std::string{}) != session.selfTeamId() ||
        packet.value("protocolVersion", -1) != Session::kProtocolVersion ||
        packet.value("v", 0) != kVersion || !session.memberMaySync(id))
    {
        return false;
    }
    const char* stage = dComIfGp_getStartStageName();
    const std::string pStage = packet.value("stageName", std::string{});
    return stage != nullptr && pStage.size() <= 7 && std::strncmp(stage, pStage.c_str(), 8) == 0 &&
           packet.value("layerNo", -128) == dComIfG_play_c::getLayerNo(0);
}

Record* findByKey(const Key& key) {
    for (auto& [id, r] : s_records) {
        if (!r.dup && sameKey(r.key, key)) {
            return &r;
        }
    }
    return nullptr;
}

void handleMove(Record& r, uint32_t client, const json& packet) {
    if (r.kind != Kind::Movebox && r.kind != Kind::IceBlock) {
        return;
    }
    Move m;
    int64_t dir, walk = 1;
    if (!parseXZ(packet, "from", m.from) || !intField(packet, "dir", 0, 3, dir) ||
        (packet.contains("walk") && !intField(packet, "walk", 1, 2, walk)))
    {
        return;
    }
    m.hasTo = parseXZ(packet, "to", m.to);
    m.client = client;
    m.dir = static_cast<uint8_t>(dir);
    m.pull = boolField(packet, "pull", false);
    m.walk = static_cast<uint8_t>(walk);
    m.at = Clock::now();
    if (takeAuthority(r, client, m.from)) {
        queueMove(r, m);
    }
}

void handleRest(Record& r, uint32_t client, const json& packet) {
    if (r.kind != Kind::Movebox && r.kind != Kind::IceBlock) {
        return;
    }
    XZ from, pos;
    if (!parseXZ(packet, "from", from) || !parseXZ(packet, "pos", pos)) {
        return;
    }
    if (takeAuthority(r, client, from)) {
        queueRest(r, client, from, pos);
    }
}

void handleChain(Record& r, uint32_t client, const json& packet) {
    if (r.kind != Kind::SwChain) {
        return;
    }
    ChainStream s;
    int64_t out = 0, cap = 0;
    if (!parseXYZ(packet, "end", s.end) || !intField(packet, "out", 0, 255, out) ||
        (packet.contains("cap") && !intField(packet, "cap", 0, 255, cap)))
    {
        return;
    }
    s.client = client;
    s.held = boolField(packet, "held", false);
    s.hook = boolField(packet, "hook", false);
    s.out = static_cast<uint8_t>(out);
    s.cap = static_cast<uint8_t>(cap);
    s.hasRel = parseXYZ(packet, "rel", s.rel);
    s.at = Clock::now();
    applyStream(r, s);
}

}  // namespace

const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Movebox: return "movebox";
    case Kind::IceBlock: return "iceblock";
    case Kind::SwChain: return "swchain";
    default: return "none";
    }
}

Kind kindFromName(const std::string& name) {
    if (name == "movebox") return Kind::Movebox;
    if (name == "iceblock") return Kind::IceBlock;
    if (name == "swchain") return Kind::SwChain;
    return Kind::None;
}

bool sameKey(const Key& a, const Key& b) {
    return a.procName == b.procName && a.roomNo == b.roomNo && a.params == b.params &&
           a.setId == b.setId && a.angle[0] == b.angle[0] && a.angle[1] == b.angle[1] &&
           a.angle[2] == b.angle[2] && std::fabs(a.home[0] - b.home[0]) <= kHomeTolerance &&
           std::fabs(a.home[1] - b.home[1]) <= kHomeTolerance &&
           std::fabs(a.home[2] - b.home[2]) <= kHomeTolerance;
}

std::string keyText(const Key& k) {
    return fmt::format("proc 0x{:X}, room {}, params 0x{:08X}, home {:.0f} {:.0f} {:.0f}",
        k.procName, k.roomNo, k.params, k.home[0], k.home[1], k.home[2]);
}

bool nearXZ(const XZ& a, const XZ& b, float tolerance) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

std::map<fpc_ProcID, Record>& records() {
    return s_records;
}

Record* recordOf(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return nullptr;
    }
    const auto it = s_records.find(fopAcM_GetID(actor));
    return it == s_records.end() || it->second.dup ? nullptr : &it->second;
}

bool optionOn() {
    if (!Session::active()) {
        return false;
    }
    const Session& session = Session::instance();
    return session.joined() && session.roomState().syncPushPull && sync::enabled();
}

uint32_t selfId() {
    return Session::instance().selfClientId();
}

bool takeAuthority(Record& r, uint32_t client, const XZ& from) {
    const auto now = Clock::now();
    if (r.authValid && client != r.authClient && now - r.authAt < kConcurrentWindow &&
        nearXZ(from, r.authFrom, kFromTolerance) && client > r.authClient)
    {
        ++r.rejected;
        TwiliLog.info("[push] {} ({}): client {}'s move loses to client {}'s from the same spot",
            kindName(r.kind), keyText(r.key), client, r.authClient);
        return false;
    }
    r.authValid = true;
    r.authClient = client;
    r.authFrom = from;
    r.authAt = now;
    return true;
}

void setOwnAuthority(Record& r, const XZ& from) {
    // A teammate's concurrent move from here already won if its id is lower.
    if (r.authValid && r.authClient < selfId() && Clock::now() - r.authAt < kConcurrentWindow &&
        nearXZ(from, r.authFrom, kFromTolerance))
    {
        return;
    }
    r.authValid = true;
    r.authClient = selfId();
    r.authFrom = from;
    r.authAt = Clock::now();
}

json keyJson(const Key& k) {
    return {
        {"roomNo", static_cast<int>(k.roomNo)},
        {"procName", k.procName},
        {"params", k.params},
        {"setId", k.setId},
        {"home", {{"x", k.home[0]}, {"y", k.home[1]}, {"z", k.home[2]}}},
        {"angle", {k.angle[0], k.angle[1], k.angle[2]}},
    };
}

bool sendPacket(json packet, const Record& r) {
    Session& session = Session::instance();
    if (!optionOn() || !isSaveLoaded() || r.dup) {
        return false;
    }
    const char* stage = dComIfGp_getStartStageName();
    const int layer = dComIfG_play_c::getLayerNo(0);
    // The server routes by the stage it last heard from us.
    if (stage == nullptr || std::strncmp(stage, session.reportedStageName(), 8) != 0 ||
        layer != session.reportedLayerNo())
    {
        return false;
    }
    const auto& clients = session.clients();
    if (!std::any_of(clients.begin(), clients.end(), [&](const auto& e) {
            return !e.second.self && session.clientIsInCurrentLayer(e.second) &&
                   session.isTeammate(e.second);
        }))
    {
        return false;
    }
    packet["v"] = kVersion;
    packet["stageName"] = std::string(stage);
    packet["layerNo"] = layer;
    packet["key"] = keyJson(r.key);
    sync::stampPacket(packet, false);
    return session.send(packet);
}

}  // namespace detail

using namespace detail;

void onActorCreated(fopAc_ac_c* actor) {
    const Kind kind = kindOf(fopAcM_GetName(actor));
    if (kind == Kind::None) {
        return;
    }
    const fopAcM_prm_class* prm = fopAcM_GetAppend(actor);
    // Spawned by another actor: no placement to match it by.
    if (prm == nullptr || prm->parent_id != fpcM_ERROR_PROCESS_ID_e) {
        return;
    }
    Record r;
    r.actor = actor;
    r.kind = kind;
    const cXyz home = prm->base.position;
    const csXyz angle = prm->base.angle;
    r.key.procName = fopAcM_GetName(actor);
    r.key.roomNo = prm->room_no;
    r.key.params = static_cast<uint32_t>(prm->base.parameters);
    r.key.setId = static_cast<uint16_t>(prm->base.setID);
    r.key.home[0] = home.x;
    r.key.home[1] = home.y;
    r.key.home[2] = home.z;
    r.key.angle[0] = angle.x;
    r.key.angle[1] = angle.y;
    r.key.angle[2] = angle.z;
    for (auto& [id, other] : s_records) {
        if (sameKey(other.key, r.key)) {
            other.dup = true;
            r.dup = true;
        }
    }
    if (r.dup) {
        TwiliLog.info("[push] two {} with the same placement ({}): neither syncs", kindName(kind),
            keyText(r.key));
    }
    s_records[fopAcM_GetID(actor)] = std::move(r);
}

void onActorDeleted(fopAc_ac_c* actor) {
    s_records.erase(fopAcM_GetID(actor));
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type != "PUSH_MOVE" && type != "PUSH_REST" && type != "CHAIN_PULL") {
        return false;
    }
    if (!optionOn() || !isSaveLoaded() || !acceptFromTeammate(packet)) {
        return true;
    }
    Key key;
    if (!parseKey(packet, key)) {
        return true;
    }
    Record* r = findByKey(key);
    if (r == nullptr) {
        // Live only: an object whose room is not loaded here keeps its saved state.
        return true;
    }
    const uint32_t client = packet.value("clientId", 0u);
    if (type == "PUSH_MOVE") {
        handleMove(*r, client, packet);
    } else if (type == "PUSH_REST") {
        handleRest(*r, client, packet);
    } else {
        handleChain(*r, client, packet);
    }
    return true;
}

void tick() {
    for (auto& [id, r] : s_records) {
        if (r.dup) {
            continue;
        }
        if (r.kind == Kind::SwChain) {
            tickChain(r);
        } else {
            tickBlock(r);
        }
    }
}

void resetSession() {
    // Records stay: they mirror live actors, connected or not.
    for (auto& [id, r] : s_records) {
        r.queue.clear();
        r.hasRest = false;
        r.hasStream = false;
        r.authValid = false;
        r.sentHeld = false;
    }
}

void shutdown() {
    s_records.clear();
}

}  // namespace twili::push
