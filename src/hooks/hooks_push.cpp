#include "hooks/Hooks.hpp"

#include "core/Session.hpp"
#include "push/PushSync.hpp"

#include "JSystem/JParticle/JPAEmitter.h"  // d_a_obj_iceblock.h is not self-contained
#include "SSystem/SComponent/c_phase.h"
#include "d/actor/d_a_obj_iceblock.h"
#include "d/actor/d_a_obj_movebox.h"
#include "d/actor/d_a_obj_swchain.h"

namespace twili::hooks {

DEFINE_HOOK_SYMBOL("src/f_op/f_op_actor.cpp#fopAc_Create", int(void*), PushActorCreate);
DEFINE_HOOK_SYMBOL("src/f_op/f_op_actor.cpp#fopAc_Delete", int(void*), PushActorDelete);
// Called through their mode tables, so never inlined.
DEFINE_HOOK(&daObjMovebox::Act_c::mode_wait, MoveboxModeWait);
DEFINE_HOOK(&daObjMovebox::Act_c::mode_walk, MoveboxModeWalk);
DEFINE_HOOK(&daObjIceBlk_c::mode_proc_wait, IceBlockModeWait);
DEFINE_HOOK(&daObjIceBlk_c::mode_proc_walk, IceBlockModeWalk);
// The profile's execute: execute itself may be inlined into it.
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_obj_swchain.cpp#daObjSwChain_Execute",
    int(daObjSwChain_c*), SwChainExecute);

namespace {

fopAc_ac_c* actorArg(void* args) {
    return static_cast<fopAc_ac_c*>(mods::arg<void*>(args, 0));
}

// A group that failed part way keeps the callbacks it added: they do nothing.
bool on() {
    return active(Group::Push);
}

void onCreatePost(ModContext*, void* args, void* retval, void*) {
    if (on() && Session::active() && *static_cast<int*>(retval) == cPhs_COMPLEATE_e) {
        push::onActorCreated(actorArg(args));
    }
}

void onDeletePost(ModContext*, void* args, void* retval, void*) {
    if (on() && Session::active() && *static_cast<int*>(retval) == 1) {
        push::onActorDeleted(actorArg(args));
    }
}

HookAction onBoxWaitPre(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::moveboxWaitPre(mods::arg<daObjMovebox::Act_c*>(args, 0));
    }
    return HOOK_CONTINUE;
}

void onBoxWaitPost(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::moveboxWaitPost(mods::arg<daObjMovebox::Act_c*>(args, 0));
    }
}

HookAction onBoxWalkPre(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::moveboxWalkPre(mods::arg<daObjMovebox::Act_c*>(args, 0));
    }
    return HOOK_CONTINUE;
}

void onBoxWalkPost(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::moveboxWalkPost(mods::arg<daObjMovebox::Act_c*>(args, 0));
    }
}

HookAction onIceWaitPre(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::iceWaitPre(mods::arg<daObjIceBlk_c*>(args, 0));
    }
    return HOOK_CONTINUE;
}

void onIceWaitPost(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::iceWaitPost(mods::arg<daObjIceBlk_c*>(args, 0));
    }
}

HookAction onIceWalkPre(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::iceWalkPre(mods::arg<daObjIceBlk_c*>(args, 0));
    }
    return HOOK_CONTINUE;
}

void onIceWalkPost(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::iceWalkPost(mods::arg<daObjIceBlk_c*>(args, 0));
    }
}

HookAction onChainExecutePre(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::chainExecutePre(mods::arg<daObjSwChain_c*>(args, 0));
    }
    return HOOK_CONTINUE;
}

void onChainExecutePost(ModContext*, void* args, void*, void*) {
    if (on()) {
        push::chainExecutePost(mods::arg<daObjSwChain_c*>(args, 0));
    }
}

}  // namespace

ModResult installPush(std::string& error) {
    const ModResult results[] = {
        addPost<PushActorCreate>(onCreatePost, kDefault, "fopAc_Create", error),
        addPost<PushActorDelete>(onDeletePost, kDefault, "fopAc_Delete", error),
        addPre<MoveboxModeWait>(onBoxWaitPre, kObserve, "daObjMovebox::Act_c::mode_wait", error),
        addPost<MoveboxModeWait>(onBoxWaitPost, kDefault, "daObjMovebox::Act_c::mode_wait", error),
        addPre<MoveboxModeWalk>(onBoxWalkPre, kObserve, "daObjMovebox::Act_c::mode_walk", error),
        addPost<MoveboxModeWalk>(onBoxWalkPost, kDefault, "daObjMovebox::Act_c::mode_walk", error),
        addPre<IceBlockModeWait>(onIceWaitPre, kObserve, "daObjIceBlk_c::mode_proc_wait", error),
        addPost<IceBlockModeWait>(onIceWaitPost, kDefault, "daObjIceBlk_c::mode_proc_wait", error),
        addPre<IceBlockModeWalk>(onIceWalkPre, kObserve, "daObjIceBlk_c::mode_proc_walk", error),
        addPost<IceBlockModeWalk>(onIceWalkPost, kDefault, "daObjIceBlk_c::mode_proc_walk", error),
        addPre<SwChainExecute>(onChainExecutePre, kObserve, "daObjSwChain_Execute", error),
        addPost<SwChainExecute>(onChainExecutePost, kDefault, "daObjSwChain_Execute", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
