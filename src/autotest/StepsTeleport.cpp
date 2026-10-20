// Steps for teleporting to a player; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "teleport/Teleport.hpp"

#include "d/actor/d_a_player.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace twili::autotest {
namespace {

using nlohmann::json;

// teleport::status().resultSeq when the running teleportTo step sent its request.
uint32_t sTeleportSeq = 0;

const Client* findPeer(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.online && c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return c.online && c.isSaveLoaded && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

bool resultMatches(const json& step, const std::string& got, const std::string& reason) {
    const json expect = step.value("expect", json("arrived"));
    const auto one = [&](const json& e) {
        if (!e.is_string()) {
            return false;
        }
        const std::string want = e.get<std::string>();
        return want == got || (want == "arrived" && (got == "local" || got == "stage"));
    };
    bool ok = false;
    if (expect.is_array()) {
        for (const auto& e : expect) {
            ok = ok || one(e);
        }
    } else {
        ok = one(expect);
    }
    const std::string wantReason = step.value("reason", std::string{});
    const std::string wantPlacement = step.value("placement", std::string{});
    return ok && (wantReason.empty() || wantReason == reason) &&
           (wantPlacement.empty() || wantPlacement == teleport::status().placement);
}

int sHoldSince = -1;

bool held(bool ok, const StepContext& ctx, int holdTicks) {
    if (!ctx.begun) {
        sHoldSince = -1;
    }
    if (!ok) {
        sHoldSince = -1;
        return false;
    }
    if (sHoldSince < 0) {
        sHoldSince = ctx.ticks;
    }
    return ctx.ticks - sHoldSince >= holdTicks;
}

void* findRoomEventTag(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    return fopAcM_GetName(actor) == fpcNm_TAG_EVENT_e &&
                   fopAcM_GetRoomNo(actor) == dComIfGp_roomControl_getStayNo() &&
                   static_cast<daTag_Event_c*>(actor)->getEventNo() ==
                       *static_cast<const uint8_t*>(data) ?
               actor :
               nullptr;
}

// Solid ground inside the tag's area `margin` from its edge, nearest its centre, or with
// `outside` within 1000 past that margin outside it, nearest us.
std::optional<bool> standInEventTag(StepContext& ctx) {
    uint8_t eventNo = static_cast<uint8_t>(ctx.step.value("mapToolId", 0));
    const float margin = ctx.step.value("margin", 300.0f);
    const bool outside = ctx.step.value("outside", false);
    auto* tag = static_cast<daTag_Event_c*>(fopAcM_Search(&findRoomEventTag, &eventNo));
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    if (tag == nullptr || player == nullptr) {
        ctx.fail(fmt::format("standInEventTag: no event tag {} in our room", eventNo));
        return false;
    }
    const cXyz& c = tag->current.pos;
    const bool box = tag->getAreaType() == 0x8000;
    const float hx = box ? tag->scale.x * 0.5f : tag->scale.x;
    const float hz = box ? tag->scale.z * 0.5f : tag->scale.x;
    const float minY = (box ? c.y : c.y - tag->scale.y) - (outside ? 500.0f : 0.0f);
    const float maxY = c.y + tag->scale.y - 150.0f;
    // How far past the edge, negative inside: per axis for a box, radial for a cylinder.
    const auto past = [&](float dx, float dz) {
        return box ? std::max(std::fabs(dx) - hx, std::fabs(dz) - hz) :
                     std::sqrt(dx * dx + dz * dz) - hx;
    };
    const float reach = std::max(hx, hz) + margin + 1000.0f;
    const cXyz from = outside ? player->current.pos : c;
    struct Spot {
        float dist2, x, z;
    };
    std::vector<Spot> spots;
    for (float dx = -reach; dx <= reach; dx += 100.0f) {
        for (float dz = -reach; dz <= reach; dz += 100.0f) {
            const float d = past(dx, dz);
            if (outside ? d >= margin && d <= margin + 1000.0f : d <= -margin) {
                const float fx = c.x + dx - from.x;
                const float fz = c.z + dz - from.z;
                spots.push_back({fx * fx + fz * fz, c.x + dx, c.z + dz});
            }
        }
    }
    std::sort(spots.begin(), spots.end(),
        [](const Spot& a, const Spot& b) { return a.dist2 < b.dist2; });
    dBgS& bg = dComIfG_Bgsp();
    // Ground Link can stand on at x, z, in the tag's room and height range.
    const auto groundAt = [&](float x, float z) -> std::optional<float> {
        cXyz probe(x, maxY, z);
        dBgS_LinkGndChk gnd;
        gnd.SetPos(&probe);
        const f32 floorY = bg.GroundCross(&gnd);
        if (floorY < minY || floorY > maxY || bg.GetRoomId(gnd) != fopAcM_GetRoomNo(tag) ||
            bg.GetExitId(gnd) != 0x3F)
        {
            return std::nullopt;
        }
        const int code = bg.GetGroundCode(gnd);
        if (code == 4 || code == 5 || code == 9 || code == 10) {
            return std::nullopt;
        }
        return floorY;
    };
    for (const Spot& spot : spots) {
        const std::optional<float> floorY = groundAt(spot.x, spot.z);
        if (!floorY) {
            continue;
        }
        // Level for 150 around, so whoever lands beside us stays there.
        bool level = true;
        for (const auto& [ox, oz] : {std::pair{150.0f, 0.0f}, {-150.0f, 0.0f}, {0.0f, 150.0f},
                 {0.0f, -150.0f}})
        {
            const std::optional<float> y = groundAt(spot.x + ox, spot.z + oz);
            level = level && y && std::fabs(*y - *floorY) <= 30.0f;
        }
        if (!level) {
            continue;
        }
        cXyz pos(spot.x, *floorY, spot.z);
        player->setPlayerPosAndAngle(&pos, player->shape_angle.y, TRUE);
        TwiliLog.info("[autotest] standing {} event tag {} at ({:.0f} {:.0f} {:.0f})",
            outside ? "outside" : "in", eventNo, pos.x, pos.y, pos.z);
        return true;
    }
    ctx.fail(fmt::format("standInEventTag: no ground {} event tag {}", outside ? "outside" : "in",
        eventNo));
    return false;
}

std::optional<bool> teleportSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "standInEventTag") {
        return standInEventTag(ctx);
    }

    if (op == "teleportTo") {
        const std::string target = step.value("target", std::string{});
        if (!ctx.begun) {
            const Client* c = findPeer(target);
            if (c == nullptr) {
                if (resultMatches(step, "refused", "offline")) {
                    return true;
                }
                ctx.fail("teleportTo: no online peer named '" + target + "'");
                return false;
            }
            sTeleportSeq = teleport::status().resultSeq;
            const json offset = step.value("offset", json::array({0, 0, 0}));
            teleport::setPlacementOffsetForTest(
                offset.at(0).get<float>(), offset.at(1).get<float>(), offset.at(2).get<float>());
            // A local refusal is reported as a result too.
            teleport::request(c->clientId);
        }
        const teleport::Status& st = teleport::status();
        if (st.resultSeq == sTeleportSeq) {
            if (ctx.seconds > ctx.timeout(90.0)) {
                teleport::setPlacementOffsetForTest(0.0f, 0.0f, 0.0f);
                ctx.fail(fmt::format("teleportTo {} timed out after {:.0f}s ({})", target,
                    ctx.timeout(90.0), st.message));
            }
            return false;
        }
        teleport::setPlacementOffsetForTest(0.0f, 0.0f, 0.0f);
        const std::string got = teleport::resultName(st.lastResult);
        TwiliLog.info("[autotest] teleport result={} reason='{}' placement='{}' ({})", got,
            st.reason, st.placement, st.message);
        if (!resultMatches(step, got, st.reason)) {
            ctx.fail(fmt::format("teleportTo {}: got {} '{}' placement '{}' ({})", target, got,
                st.reason, st.placement, st.message));
            return false;
        }
        return true;
    }

    // Beside the peer (not inside its dummy) and facing its way, as a teleport leaves us.
    if (op == "expectBeside") {
        const std::string target = step.value("target", std::string{});
        const float minDist = step.value("minDist", 40.0f);
        const float maxDist = step.value("maxDist", 150.0f);
        const float maxAngle = step.value("maxAngleDeg", 20.0f);
        const Client* c = findPeer(target);
        fopAc_ac_c* player = dComIfGp_getPlayer(0);
        std::string why = "peer not visible in our layer";
        if (c != nullptr && player != nullptr && !dComIfGp_isEnableNextStage() &&
            peerInMyLayer(*c) && c->hasPlayerUpdate)
        {
            const float dx = player->current.pos.x - c->posX;
            const float dz = player->current.pos.z - c->posZ;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const int16_t turn = static_cast<int16_t>(player->shape_angle.y - c->shapeAngleY);
            const float angle = std::fabs(static_cast<float>(turn)) * (180.0f / 32768.0f);
            if (dist >= minDist && dist <= maxDist && angle <= maxAngle) {
                TwiliLog.info("[autotest] beside {}: {:.0f} units, facing {:.0f} deg off", target,
                    dist, angle);
                return true;
            }
            why = fmt::format("{:.0f} units away, facing {:.0f} deg off", dist, angle);
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectBeside {}: {} (want {:.0f}-{:.0f} units, {:.0f} deg)",
                target, why, minDist, maxDist, maxAngle));
        }
        return false;
    }

    // Whether an event runs in our own game, held for holdTicks.
    if (op == "expectOwnEvent") {
        const bool want = step.value("running", false);
        const bool running = dComIfGp_event_runCheck() != FALSE;
        if (held(running == want, ctx, step.value("holdTicks", 60))) {
            TwiliLog.info("[autotest] own event running={}", running);
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectOwnEvent: running={}, want {}", running, want));
        }
        return false;
    }

    if (op == "expectNear") {
        const std::string target = step.value("target", std::string{});
        const float maxDist = step.value("maxDist", 250.0f);
        const Client* c = findPeer(target);
        fopAc_ac_c* player = dComIfGp_getPlayer(0);
        float dist = -1.0f;
        if (c != nullptr && player != nullptr && !dComIfGp_isEnableNextStage() &&
            peerInMyLayer(*c) && c->hasPlayerUpdate)
        {
            const float dx = player->current.pos.x - c->posX;
            const float dy = player->current.pos.y - c->posY;
            const float dz = player->current.pos.z - c->posZ;
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist <= maxDist) {
                TwiliLog.info("[autotest] {:.0f} units from {}", dist, target);
                return true;
            }
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("expectNear {}: {} (max {:.0f})", target,
                dist < 0 ? std::string("peer not visible in our layer") :
                           fmt::format("{:.0f} units away", dist),
                maxDist));
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&teleportSteps);

}  // namespace
}  // namespace twili::autotest
