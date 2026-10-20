// ENEMY_DAMAGE: one update's hits in one stage and layer; relayed to teammates there, uncached.

#include "enemy/EnemyDamage.hpp"

#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include <nlohmann/json.hpp>

namespace twili::enemy_damage {
namespace {

bool parseHit(const nlohmann::json& j, Hit& out) {
    int64_t dmg, pct, hpAfter, hits = 0;
    if (!enemy_sync::detail::parseKey(j, out.key) ||
        !enemy_sync::detail::intField(j, "dmg", 0, 30000, dmg) ||
        !enemy_sync::detail::intField(j, "pct", 100, 500, pct) ||
        !enemy_sync::detail::intField(j, "hpAfter", -1, 30000, hpAfter) ||
        (j.contains("hits") && !enemy_sync::detail::intField(j, "hits", 0, 4, hits)) ||
        (dmg == 0 && hits == 0))
    {
        return false;
    }
    out.dmg = static_cast<uint16_t>(dmg);
    out.pct = static_cast<uint16_t>(pct);
    out.hpAfter = static_cast<int16_t>(hpAfter);
    out.hits = static_cast<uint8_t>(hits);
    return true;
}

}  // namespace

bool detail::sendHits(const char* stage, int layer, const Hit* hits, size_t count) {
    if (count == 0 || !enemy_sync::detail::canSendHere(stage, layer)) {
        return false;
    }
    nlohmann::json list = nlohmann::json::array();
    for (size_t i = 0; i < count; ++i) {
        nlohmann::json j = enemy_sync::detail::keyJson(hits[i].key);
        j["dmg"] = hits[i].dmg;
        j["pct"] = hits[i].pct;
        j["hpAfter"] = hits[i].hpAfter;
        if (hits[i].hits > 0) {
            j["hits"] = hits[i].hits;
        }
        list.push_back(std::move(j));
    }
    nlohmann::json packet = {
        {"type", "ENEMY_DAMAGE"},
        {"v", kVersion},
        // A fight sends several a second: kept out of the relay's log.
        {"quiet", true},
        {"stageName", std::string(stage)},
        {"layerNo", layer},
        {"hits", std::move(list)},
    };
    sync::stampPacket(packet, false);
    Session::instance().send(packet);
    return true;
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type != "ENEMY_DAMAGE") {
        return false;
    }
    const Session& session = Session::instance();
    const RoomState& room = session.roomState();
    if (!session.isConnected() || !room.syncNPCs || !room.syncEnemyDamage || !isSaveLoaded() ||
        !enemy_sync::detail::acceptFromTeammate(packet, kVersion))
    {
        return true;
    }
    const auto hits = packet.find("hits");
    if (hits == packet.end() || !hits->is_array() || hits->size() > kMaxHitsPerPacket) {
        return true;
    }
    for (const auto& j : *hits) {
        Hit hit;
        if (parseHit(j, hit)) {
            detail::queueRemote(hit);
        }
    }
    tick();
    return true;
}

}  // namespace twili::enemy_damage
