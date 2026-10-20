// Steps for block and chain sync (syncPushPull); reference in the runner README.

#include "autotest/AutoTest.hpp"
#include "autotest/AutoTestSteps.hpp"
#include "autotest/State.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "push/PushState.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_obj_swchain.h"
#include "d/actor/d_a_player.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace twili::autotest {
namespace {

using nlohmann::json;
using push::detail::Kind;
using push::detail::Record;
using push::detail::XZ;

constexpr uint16_t kPadR = 0x20;
// Ticks the R press gets to pick the chain up before the stick pulls.
constexpr int kGrabTicks = 25;

bool readXZ(const json& step, const char* key, XZ& out) {
    const auto it = step.find(key);
    if (it == step.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() ||
        !(*it)[1].is_number())
    {
        return false;
    }
    out = {(*it)[0].get<float>(), (*it)[1].get<float>()};
    return true;
}

float distXZ(const XZ& a, const XZ& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

// The object of `kind` placed nearest `near` ([x, z]), else nearest our Link.
Record* findRecord(Kind kind, const json& step) {
    XZ near{};
    if (!readXZ(step, "near", near)) {
        const daPy_py_c* player = daPy_getPlayerActorClass();
        if (player == nullptr) {
            return nullptr;
        }
        near = {player->current.pos.x, player->current.pos.z};
    }
    Record* best = nullptr;
    float bestDist = 0.0f;
    for (auto& [id, r] : push::detail::records()) {
        if (r.kind != kind || r.dup) {
            continue;
        }
        const float d = distXZ(near, {r.key.home[0], r.key.home[2]});
        if (best == nullptr || d < bestDist) {
            best = &r;
            bestDist = d;
        }
    }
    return best;
}

Kind stepKind(const json& step, Kind fallback) {
    const Kind kind = push::detail::kindFromName(step.value("kind", std::string{}));
    return kind == Kind::None ? fallback : kind;
}

std::string recordText(const Record& r) {
    const XZ pos = r.kind == Kind::SwChain ? XZ{r.actor->current.pos.x, r.actor->current.pos.z}
                                           : push::detail::blockPos(r);
    return fmt::format("{} home {:.0f} {:.0f} at {:.0f} {:.0f} (sent {}, applied {}, snapped {}, "
                       "rejected {}, kept switch {}, driven {})",
        push::detail::kindName(r.kind), r.key.home[0], r.key.home[2], pos.x, pos.z, r.sent,
        r.applied, r.snapped, r.rejected, r.blockedSwitchWrites, r.drivenTicks);
}

// --- pushBlock

struct PushState {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    int dir = 0;
    bool any = true;
    XZ from;
};
PushState sPush;

bool pushBlock(StepContext& ctx) {
    const json& step = ctx.step;
    Record* r = nullptr;
    if (ctx.begun) {
        const auto it = push::detail::records().find(sPush.id);
        r = it != push::detail::records().end() ? &it->second : nullptr;
    } else {
        r = findRecord(stepKind(step, Kind::Movebox), step);
        if (r != nullptr) {
            const int dir = step.value("dir", -1);
            const int first = step.value("firstDir", 0);
            sPush = {fopAcM_GetID(r->actor), dir >= 0 && dir <= 3 ? dir : (first & 3),
                dir < 0 || dir > 3, push::detail::blockPos(*r)};
        }
    }
    if (r == nullptr) {
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail("pushBlock: no such block here");
        }
        return false;
    }
    if (r->localWalk) {
        TwiliLog.info("[autotest] pushed the {} dir {} from {:.0f} {:.0f}",
            push::detail::kindName(r->kind), sPush.dir, sPush.from.x, sPush.from.z);
        return true;
    }
    // A blocked direction never starts: try the next one.
    if (sPush.any && ctx.ticks > 0 && ctx.ticks % 10 == 0) {
        sPush.dir = (sPush.dir + 1) % 4;
    }
    push::detail::pushForTest(*r, sPush.dir, step.value("pull", false));
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("pushBlock: the block never started to move: " + recordText(*r));
    }
    return false;
}

// --- expectBlock

struct StillState {
    XZ last;
    int ticks = 0;
};
StillState sStill;

bool expectBlock(StepContext& ctx) {
    const json& step = ctx.step;
    Record* r = findRecord(stepKind(step, Kind::Movebox), step);
    if (!ctx.begun) {
        sStill = {};
    }
    std::string why;
    if (r == nullptr) {
        why = "no such block here";
    } else {
        const XZ pos = push::detail::blockPos(*r);
        // A turn object's spot is its step (placed on step 0).
        const bool turn = push::detail::isTurn(r->kind);
        const XZ home = turn ? push::detail::turnSpot(0) : XZ{r->key.home[0], r->key.home[2]};
        const float tol = step.value("tol", 5.0f);
        const bool walking = r->localWalk || r->remoteWalk;
        XZ at;
        if (readXZ(step, "at", at) && distXZ(pos, at) > tol) {
            why = fmt::format("at {:.0f} {:.0f}, want {:.0f} {:.0f}", pos.x, pos.z, at.x, at.z);
        } else if (turn && step.contains("step") &&
                   push::detail::turnStep(*r) != step.value("step", 0))
        {
            why = fmt::format("on step {}", push::detail::turnStep(*r));
        } else if (step.contains("moved") &&
                   (distXZ(pos, home) > tol) != step.value("moved", true))
        {
            why = step.value("moved", true) ? "still on its placement" : "moved off its placement";
        } else if (step.contains("moving") && walking != step.value("moving", true)) {
            why = walking ? "moving" : "not moving";
        } else if (r->applied < step.value("minApplied", 0u) ||
                   (step.contains("maxApplied") && r->applied > step.value("maxApplied", 0u)))
        {
            why = fmt::format("{} teammate move(s) replayed", r->applied);
        } else if (r->snapped < step.value("minSnapped", 0u)) {
            why = fmt::format("{} snap(s)", r->snapped);
        } else if (step.value("matchesRemote", false) &&
                   (!r->hasRemoteTarget || distXZ(pos, r->remoteTarget) > tol))
        {
            why = r->hasRemoteTarget ? fmt::format("teammate left it at {:.0f} {:.0f}",
                                           r->remoteTarget.x, r->remoteTarget.z)
                                     : "no teammate move seen";
        } else if (const int still = step.value("stillFrames", 0); still > 0) {
            if (walking || distXZ(pos, sStill.last) > 0.5f) {
                sStill.ticks = 0;
            } else {
                ++sStill.ticks;
            }
            sStill.last = pos;
            if (sStill.ticks < still) {
                why = fmt::format("moved within the last {} ticks", still);
            }
        }
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] block ok: {}", recordText(*r));
        return true;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail("expectBlock: " + why + (r != nullptr ? ": " + recordText(*r) : std::string{}));
    }
    return false;
}

// Signal names carry our spot, so the other instance compares it with its own.
bool signalBlockPos(StepContext& ctx) {
    Record* r = findRecord(stepKind(ctx.step, Kind::Movebox), ctx.step);
    if (r == nullptr) {
        ctx.fail("signalBlockPos: no such block here");
        return false;
    }
    const XZ pos = push::detail::blockPos(*r);
    const std::string name = fmt::format("{}:{}:{}", ctx.step.value("name", std::string("blockpos")),
        static_cast<int>(std::lround(pos.x)), static_cast<int>(std::lround(pos.z)));
    Session::instance().sendAutotestSignal(detail::state().instance, name);
    TwiliLog.info("[autotest] block at {:.0f} {:.0f} signalled", pos.x, pos.z);
    return true;
}

bool expectBlockPosSignal(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string prefix = fmt::format("{}:{}:", step.value("from", std::string{}),
        step.value("name", std::string("blockpos")));
    Record* r = findRecord(stepKind(step, Kind::Movebox), step);
    for (const std::string& signal : detail::state().signals) {
        if (r == nullptr || signal.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        int x = 0, z = 0;
        if (std::sscanf(signal.c_str() + prefix.size(), "%d:%d", &x, &z) != 2) {
            continue;
        }
        const XZ pos = push::detail::blockPos(*r);
        if (distXZ(pos, {static_cast<float>(x), static_cast<float>(z)}) <= step.value("tol", 5.0f)) {
            TwiliLog.info("[autotest] block at {:.0f} {:.0f} matches the peer's {} {}", pos.x,
                pos.z, x, z);
            return true;
        }
        ctx.fail(fmt::format("expectBlockPosSignal: ours at {:.0f} {:.0f}, the peer's at {} {}",
            pos.x, pos.z, x, z));
        return false;
    }
    if (ctx.seconds > ctx.timeout(60.0)) {
        ctx.fail(r == nullptr ? "expectBlockPosSignal: no such block here"
                              : "expectBlockPosSignal: no signal " + prefix);
    }
    return false;
}

// --- chains

struct ChainHold {
    bool active = false;
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    // Ticks since the grab; the stick pulls after kGrabTicks while `pulling`.
    int ticks = 0;
    bool pulling = false;
    // The chain's tension while every link is out, until its force pull starts: the stick alone
    // walks Link at about 6, under the 10 its switch takes without iron boots.
    float pullPower = 0.0f;
};
ChainHold sHold;

Record* chainRecord(fpc_ProcID id) {
    const auto it = push::detail::records().find(id);
    return it != push::detail::records().end() ? &it->second : nullptr;
}

// Holds R every tick (the chain drops when it is let go), the stick pulling away from the base.
void holdChainTicker() {
    if (!sHold.active) {
        return;
    }
    Record* r = chainRecord(sHold.id);
    if (r == nullptr) {
        sHold = {};
        return;
    }
    ++sHold.ticks;
    float stickX = 0.0f;
    float stickY = 0.0f;
    camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
    if (sHold.pulling && sHold.ticks > kGrabTicks && camera != nullptr) {
        const cXyz base = r->actor->home.pos;
        const daPy_py_c* player = daPy_getPlayerActorClass();
        if (player != nullptr) {
            const s16 want = cLib_targetAngleY(&base, &player->current.pos);
            const s16 camY = dCam_getControledAngleY(camera);
            // Stick input is camera-relative.
            const float theta = static_cast<s16>(want - camY + 0x8000) * (3.14159265f / 32768.0f);
            stickX = std::sin(theta);
            stickY = -std::cos(theta);
            // Read by the chain's next count, before its own tension pass.
            auto* chain = static_cast<daObjSwChain_c*>(r->actor);
            if (sHold.pullPower > 0.0f && chain->mOutNum == chain->mChainNum &&
                chain->field_0xa6d == 0 && chain->pow < sHold.pullPower)
            {
                chain->pow = sHold.pullPower;
            }
        }
    }
    pulsePad(stickX, stickY, kPadR, 2);
}

const bool sTickerRegistered = registerTicker(&holdChainTicker);

// Puts our Link beside the chain's handle, on its far side from the base.
bool approachChain(StepContext& ctx) {
    Record* r = findRecord(Kind::SwChain, ctx.step);
    daPy_py_c* player = daPy_getPlayerActorClass();
    if (r == nullptr || player == nullptr) {
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail("approachChain: no chain switch here");
        }
        return false;
    }
    const cXyz base = r->actor->home.pos;
    const cXyz end = push::detail::chainEnd(*r);
    s16 yaw = cLib_targetAngleY(&base, &end);
    if (base.absXZ(end) < 1.0f) {
        yaw = r->actor->shape_angle.y;
    }
    const float dist = ctx.step.value("dist", 40.0f);
    cXyz pos(end.x + cM_ssin(yaw) * dist, end.y, end.z + cM_scos(yaw) * dist);
    // Facing the handle: R picks it up.
    player->setPlayerPosAndAngle(&pos, static_cast<s16>(yaw + 0x8000), TRUE);
    TwiliLog.info("[autotest] beside the chain's handle at {:.0f} {:.0f} {:.0f} ({} out)", pos.x,
        pos.y, pos.z, push::detail::chainOut(*r));
    return true;
}

bool pullChain(StepContext& ctx) {
    const json& step = ctx.step;
    if (!ctx.begun) {
        Record* r = findRecord(Kind::SwChain, step);
        if (r == nullptr) {
            ctx.fail("pullChain: no chain switch here");
            return false;
        }
        sHold = {true, fopAcM_GetID(r->actor), 0, true, step.value("pullPower", 0.0f)};
        return false;
    }
    Record* r = chainRecord(sHold.id);
    if (r == nullptr || !sHold.active) {
        ctx.fail("pullChain: the chain is gone");
        return false;
    }
    auto* chain = static_cast<daObjSwChain_c*>(r->actor);
    const int frames = step.value("frames", 0);
    const bool untilSwitch = step.value("untilSwitch", false);
    const int untilOut = step.value("untilOut", 0);
    const bool switchOn = chain->getSwbit() != 0xFF && fopAcM_isSwitch(chain, chain->getSwbit());
    const bool done = (untilSwitch && switchOn) ||
                      (untilOut > 0 && push::detail::chainOut(*r) >= untilOut) ||
                      (frames > 0 && ctx.ticks >= frames);
    if (done) {
        // Keeps holding, still: releaseChain lets go.
        sHold.pulling = false;
        TwiliLog.info("[autotest] pulled the chain: {} out, switch {}, held {}",
            push::detail::chainOut(*r), switchOn ? "on" : "off",
            push::detail::chainHeldLocally(*r));
        return true;
    }
    if (step.value("trace", false) && ctx.ticks % 10 == 0) {
        const daPy_py_c* player = daPy_getPlayerActorClass();
        const cXyz end = push::detail::chainEnd(*r);
        TwiliLog.info("[autotest] chain t{}: {} out, len {:.0f}, pow {:.1f}, Link speed {:.1f} "
                      "proc {} at {:.0f} {:.0f}, a68 {} a6d {}",
            ctx.ticks, push::detail::chainOut(*r), end.abs(chain->home.pos), chain->pow,
            player != nullptr ? player->speedF : 0.0f,
            player != nullptr ? static_cast<const daAlink_c*>(player)->mProcID : -1,
            player != nullptr ? player->current.pos.x : 0.0f,
            player != nullptr ? player->current.pos.z : 0.0f, chain->field_0xa68,
            chain->field_0xa6d);
    }
    if (ctx.ticks == kGrabTicks + 5 && !push::detail::chainHeldLocally(*r)) {
        ctx.fail("pullChain: R did not pick the chain up");
        return false;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(fmt::format("pullChain: {} out, switch {}", push::detail::chainOut(*r),
            switchOn ? "on" : "off"));
    }
    return false;
}

bool releaseChain(StepContext& ctx) {
    sHold = {};
    pulsePad(0.0f, 0.0f, 0, 2);
    if (ctx.ticks < 10) {
        return false;
    }
    TwiliLog.info("[autotest] let go of the chain");
    return true;
}

bool expectChain(StepContext& ctx) {
    const json& step = ctx.step;
    Record* r = findRecord(Kind::SwChain, step);
    std::string why;
    if (r == nullptr) {
        why = "no chain switch here";
    } else if (step.contains("driven") && r->driving != step.value("driven", true)) {
        why = r->driving ? "follows a teammate" : "follows no teammate";
    } else if (push::detail::chainOut(*r) < step.value("minOut", 0)) {
        why = fmt::format("{} out", push::detail::chainOut(*r));
    } else if (step.contains("maxOut") && push::detail::chainOut(*r) > step.value("maxOut", 0)) {
        why = fmt::format("{} out", push::detail::chainOut(*r));
    } else if (step.contains("held") &&
               push::detail::chainHeldLocally(*r) != step.value("held", true))
    {
        why = push::detail::chainHeldLocally(*r) ? "our Link holds it" : "our Link lets it be";
    } else if (r->drivenTicks < step.value("minDrivenTicks", 0u)) {
        why = fmt::format("followed a teammate for {} ticks", r->drivenTicks);
    } else if (step.contains("endNear")) {
        XZ near{};
        const cXyz end = push::detail::chainEnd(*r);
        if (readXZ(step, "endNear", near) &&
            distXZ({end.x, end.z}, near) > step.value("tol", 150.0f))
        {
            why = fmt::format("its end at {:.0f} {:.0f}", end.x, end.z);
        }
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] chain ok: {} out, {}", push::detail::chainOut(*r), recordText(*r));
        return true;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail("expectChain: " + why + (r != nullptr ? ": " + recordText(*r) : std::string{}));
    }
    return false;
}

// The chain end follows the first peer's puppet (its hand) within `maxDist`.
bool expectChainAtPuppet(StepContext& ctx) {
    Record* r = findRecord(Kind::SwChain, ctx.step);
    if (r != nullptr && r->hasStream) {
        if (fopAc_ac_c* puppet = Session::instance().dummyActorForClient(r->stream.client)) {
            const cXyz end = push::detail::chainEnd(*r);
            const float d = end.absXZ(puppet->current.pos);
            if (d <= ctx.step.value("maxDist", 120.0f)) {
                TwiliLog.info("[autotest] chain end {:.0f} from the puller's puppet", d);
                return true;
            }
        }
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("expectChainAtPuppet: the chain end is not at the puller's puppet" +
                 (r != nullptr ? ": " + recordText(*r) : std::string{}));
    }
    return false;
}

std::optional<bool> pushSteps(const std::string& op, StepContext& ctx) {
    if (op == "pushBlock") return pushBlock(ctx);
    if (op == "expectBlock") return expectBlock(ctx);
    if (op == "signalBlockPos") return signalBlockPos(ctx);
    if (op == "expectBlockPosSignal") return expectBlockPosSignal(ctx);
    if (op == "approachChain") return approachChain(ctx);
    if (op == "pullChain") return pullChain(ctx);
    if (op == "releaseChain") return releaseChain(ctx);
    if (op == "expectChain") return expectChain(ctx);
    if (op == "expectChainAtPuppet") return expectChainAtPuppet(ctx);
    if (op == "dumpPush") {
        for (const auto& [id, r] : push::detail::records()) {
            TwiliLog.info("[autotest] push record: {}{}", recordText(r), r.dup ? " (dup)" : "");
        }
        return true;
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&pushSteps);

}  // namespace
}  // namespace twili::autotest
