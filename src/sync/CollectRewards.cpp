// COLLECT_REWARD: Fused Shadows and Mirror Shards. The boss warp's Midna scene sets them directly (no
// item get), so they reach teammates as they are set, outside randomizer games (there the reward is a
// check and travels as GIVE_ITEM).

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "ui/ItemToasts.hpp"

namespace twili::sync::detail {
namespace {

// The toast's item: the randomizer's Fused Shadow ids, the vanilla Mirror Shard ones.
constexpr uint8_t kFusedShadowItem = 0xD8;
constexpr uint8_t kMirrorShardItem = dItemNo_MIRROR_PIECE_2_e;

struct Baseline {
    bool valid = false;
    uint8_t crystal = 0;
    uint8_t mirror = 0;
};
Baseline s_base;

dSv_player_collect_c& collect() {
    return dComIfGs_getSaveInfo()->getPlayer().getCollect();
}

uint8_t shadowItem(int bit) {
    return static_cast<uint8_t>(kFusedShadowItem + (bit < 3 ? bit : 2));
}

uint8_t shardItem(int bit) {
    return static_cast<uint8_t>(kMirrorShardItem + (bit > 1 ? bit - 1 : 0));
}

}  // namespace

void tickCollectRewards() {
    if (!isSaveLoaded()) {
        s_base.valid = false;
        return;
    }
    const dSv_player_collect_c& c = collect();
    if (!s_base.valid) {
        s_base = {true, c.mCrystal, c.mMirror};
        return;
    }
    const uint8_t crystal = c.mCrystal & ~s_base.crystal;
    const uint8_t mirror = c.mMirror & ~s_base.mirror;
    s_base.crystal = c.mCrystal;
    s_base.mirror = c.mMirror;
    if ((crystal == 0 && mirror == 0) || !enabled() || randoGame()) {
        return;
    }
    nlohmann::json packet = {
        {"type", "COLLECT_REWARD"},
        {"crystal", crystal},
        {"mirror", mirror},
    };
    stampWorldPacket(packet, true);
    send(std::move(packet));
    TwiliLog.info("[sync] collected reward bits: Fused Shadows 0x{:02X}, Mirror Shards 0x{:02X}", crystal,
        mirror);
}

void shiftCollectBaseline(uint8_t crystalBefore, uint8_t mirrorBefore) {
    if (!s_base.valid) {
        return;
    }
    const dSv_player_collect_c& c = collect();
    s_base.crystal |= static_cast<uint8_t>(c.mCrystal & ~crystalBefore);
    s_base.mirror |= static_cast<uint8_t>(c.mMirror & ~mirrorBefore);
}

void invalidateCollectBaseline() {
    s_base.valid = false;
}

void handleCollectReward(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "COLLECT_REWARD")) {
        return;
    }
    const uint8_t crystal = static_cast<uint8_t>(packet.value("crystal", 0u) & 0xFF);
    const uint8_t mirror = static_cast<uint8_t>(packet.value("mirror", 0u) & 0xFF);
    dSv_player_collect_c& c = collect();
    const uint8_t crystalBefore = c.mCrystal;
    const uint8_t mirrorBefore = c.mMirror;
    {
        RemoteApplyGuard guard;
        c.mCrystal |= crystal;
        c.mMirror |= mirror;
    }
    shiftCollectBaseline(crystalBefore, mirrorBefore);
    const uint8_t newCrystal = crystal & ~crystalBefore;
    const uint8_t newMirror = mirror & ~mirrorBefore;
    TwiliLog.info("[sync] reward bits from client {}: Fused Shadows 0x{:02X}, Mirror Shards 0x{:02X}",
        packet.value("clientId", 0u), newCrystal, newMirror);
    const auto source =
        packet.value("fromQueue", false) ? item_toasts::Source::Replay : item_toasts::Source::Live;
    const item_toasts::Sender who = item_toasts::senderOf(Session::instance(), packet);
    for (int bit = 0; bit < 8; bit++) {
        if ((crystal >> bit) & 1) {
            item_toasts::noteItem(who, shadowItem(bit), 1, -1, source, ((newCrystal >> bit) & 1) != 0);
        }
        if ((mirror >> bit) & 1) {
            item_toasts::noteItem(who, shardItem(bit), 1, -1, source, ((newMirror >> bit) & 1) != 0);
        }
    }
}

}  // namespace twili::sync::detail
