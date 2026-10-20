// Chain switches: the puller streams its chain, our copy follows the puppet's hand, and only the
// puller's game sets or clears the switch (it reaches us as a flag).

#include "push/PushState.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "d/actor/d_a_obj_swchain.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

#include <algorithm>
#include <cmath>

namespace twili::push {
namespace detail {
namespace {

using Chain = daObjSwChain_c;

// Without a word from the puller this long, it let go.
constexpr auto kStreamTtl = std::chrono::milliseconds(1500);
// Ticks between packets while held: ten a second.
constexpr int kSendEvery = 3;
// The puppet stands this close to where the puller reported the chain end, or the report wins.
constexpr float kPuppetSlack = 300.0f;
// d_a_obj_swchain.cpp's link length.
constexpr float kLinkLength = 35.0f;

Chain* chainOf(const Record& r) {
    return static_cast<Chain*>(r.actor);
}

bool streamHeld(const Record& r) {
    return r.hasStream && r.stream.held && Clock::now() - r.stream.at < kStreamTtl;
}

bool localHeld(const Chain* c, const Record& r) {
    return c->mCarry != 0 || (c->field_0xa61 != 0 && !r.hookBitOurs);
}

// Rooms 4 and 6 (Arbiter's Grounds) let the chain out step by step, up to field_0xa67.
bool steppedRoom(const Chain* c) {
    const int room = fopAcM_GetRoomNo(c);
    return room == 4 || room == 6;
}

nlohmann::json xyzJson(const cXyz& p) {
    return {{"x", std::round(p.x * 10.0f) / 10.0f}, {"y", std::round(p.y * 10.0f) / 10.0f},
        {"z", std::round(p.z * 10.0f) / 10.0f}};
}

// Where the chain end goes: the puppet's hand, else where the puller said it was.
cXyz endTarget(const Record& r, const Chain* c) {
    const ChainStream& s = r.stream;
    cXyz target = s.end;
    if (!s.hook && s.hasRel) {
        if (fopAc_ac_c* puppet = Session::instance().dummyActorForClient(s.client)) {
            const float co = cM_scos(puppet->shape_angle.y);
            const float si = cM_ssin(puppet->shape_angle.y);
            const cXyz hand(puppet->current.pos.x + s.rel.x * co + s.rel.z * si,
                puppet->current.pos.y + s.rel.y, puppet->current.pos.z - s.rel.x * si + s.rel.z * co);
            if (hand.abs(s.end) < kPuppetSlack) {
                target = hand;
            }
        }
    }
    const float reach = kLinkLength * std::max(1, c->mChainNum - 1);
    const cXyz d = target - c->home.pos;
    const float len = d.abs();
    if (len > reach && len > 0.0f) {
        target = c->home.pos + d * (reach / len);
    }
    return target;
}

void sendChain(Record& r, Chain* c, bool held) {
    const bool hook = held && c->mCarry == 0;
    const cXyz end = c->mChains[c->mChainNum].field_0x34;
    nlohmann::json packet = {
        {"type", "CHAIN_PULL"},
        {"held", held},
        {"hook", hook},
        {"out", c->mOutNum},
        {"cap", c->field_0xa67},
        {"end", xyzJson(end)},
    };
    if (held && !hook) {
        if (const daPy_py_c* player = daPy_getPlayerActorClass()) {
            const cXyz d = end - player->current.pos;
            const float co = cM_scos(player->shape_angle.y);
            const float si = cM_ssin(player->shape_angle.y);
            packet["rel"] = xyzJson(cXyz(d.x * co - d.z * si, d.y, d.x * si + d.z * co));
        }
    }
    const bool sent = sendPacket(packet, r);
    if (sent) {
        ++r.sent;
        if (held != r.sentHeld) {
            TwiliLog.info("[push] swchain ({}): our {} sent, {} out", keyText(r.key),
                held ? "pull" : "release", c->mOutNum);
        }
    }
    // Nobody to tell about a release: done all the same.
    r.sentHeld = held && sent;
    r.sentOut = c->mOutNum;
    r.sendTicks = 0;
}

// execute does not recurse into another chain's.
fopAc_ac_c* s_guarded = nullptr;
int s_switchNo = -1;
bool s_switchWas = false;

}  // namespace

void applyStream(Record& r, const ChainStream& s) {
    const Chain* c = chainOf(r);
    // Our own Link holds it: ours wins here.
    if (s.held && localHeld(c, r)) {
        return;
    }
    // Two teammates on one chain: the lower client id drives our copy.
    if (s.held && streamHeld(r) && r.stream.client != s.client && s.client > r.stream.client) {
        return;
    }
    if (!s.held && r.hasStream && r.stream.client != s.client) {
        return;
    }
    r.stream = s;
    r.hasStream = true;
}

void tickChain(Record& r) {
    if (!optionOn()) {
        r.hasStream = false;
        r.sentHeld = false;
        return;
    }
    if (r.hasStream && !r.stream.held && !r.driving) {
        r.hasStream = false;
    }
}

int chainOut(const Record& r) {
    return chainOf(r)->mOutNum;
}

bool chainHeldLocally(const Record& r) {
    return localHeld(chainOf(r), r);
}

cXyz chainEnd(const Record& r) {
    const Chain* c = chainOf(r);
    return c->mChains[c->mChainNum].field_0x34;
}

}  // namespace detail

using namespace detail;

void chainExecutePre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    if (r == nullptr) {
        return;
    }
    Chain* c = chainOf(*r);
    const bool local = localHeld(c, *r);
    if (!local && optionOn() && streamHeld(*r)) {
        // The clawshot's path: execute leaves the end where current.pos puts it.
        fopAcM_setHookCarryNow(c);
        r->hookBitOurs = true;
        c->current.pos = endTarget(*r, c);
        const int chainNum = c->mChainNum;
        if (r->stream.out > c->mOutNum + 2) {
            c->mOutNum = static_cast<u8>(std::min<int>(r->stream.out - 1, chainNum));
        }
        if (steppedRoom(c) && r->stream.cap > c->field_0xa67) {
            c->field_0xa67 = static_cast<u8>(std::min<int>(r->stream.cap, chainNum));
        }
        if (!r->driving) {
            TwiliLog.info("[push] swchain ({}): follows client {}'s pull", keyText(r->key),
                r->stream.client);
        }
        r->driving = true;
        r->touched = true;
        ++r->drivenTicks;
    } else {
        if (r->hookBitOurs) {
            fopAcM_cancelHookCarryNow(c);
            r->hookBitOurs = false;
        }
        if (r->driving) {
            TwiliLog.info("[push] swchain ({}): let go by client {}", keyText(r->key),
                r->stream.client);
        }
        r->driving = false;
    }
    // A chain a teammate moved never sets or clears its switch here on its own.
    if (r->touched && !local && c->getSwbit() != 0xFF) {
        s_guarded = actor;
        s_switchNo = c->getSwbit();
        s_switchWas = fopAcM_isSwitch(c, s_switchNo) != FALSE;
        sync::RemoteApplyGuard::enter();
    }
}

void chainExecutePost(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    if (s_guarded == actor) {
        s_guarded = nullptr;
        const bool now = fopAcM_isSwitch(actor, s_switchNo) != FALSE;
        if (now != s_switchWas) {
            if (s_switchWas) {
                fopAcM_onSwitch(actor, s_switchNo);
            } else {
                fopAcM_offSwitch(actor, s_switchNo);
            }
            if (r != nullptr) {
                ++r->blockedSwitchWrites;
            }
            TwiliLog.info("[push] swchain: kept switch {} {}: the puller's game decides it",
                s_switchNo, s_switchWas ? "on" : "off");
        }
        sync::RemoteApplyGuard::leave();
    }
    if (r == nullptr) {
        return;
    }
    Chain* c = chainOf(*r);
    if (localHeld(c, *r)) {
        r->touched = false;
        r->hasStream = false;
        if (!r->sentHeld || c->mOutNum != r->sentOut || ++r->sendTicks >= kSendEvery) {
            sendChain(*r, c, true);
        }
    } else if (r->sentHeld) {
        sendChain(*r, c, false);
    }
}

}  // namespace twili::push
