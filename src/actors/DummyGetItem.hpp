#pragma once

#include "presence/RemotePose.hpp"

#include "d/d_kankyo.h"
#include "d/d_kankyo_tev_str.h"
#include "SSystem/SComponent/c_phase.h"
#include "SSystem/SComponent/c_xyz.h"
#include "Z2AudioLib/Z2SoundObject.h"

#include <cstdint>

class J3DModel;
class JKRSolidHeap;
class fopAc_ac_c;
class mDoExt_bckAnm;
class mDoExt_brkAnm;
class mDoExt_btkAnm;
class mDoExt_btpAnm;

namespace twili {

// What the autotest reads (DummyPlayerDebugInfo::getItem).
struct DummyGetItemDebug {
    uint8_t wanted = 0xFF;     // display item the pose holds up, 0xFF for none
    uint8_t item = 0xFF;       // display item whose model is built, 0xFF for none
    bool held = false;         // the pose has it drawn there
    bool loading = false;      // its archive is on its way
    bool placed = false;       // drawn this tick
    uint32_t placedTicks = 0;  // ticks drawn
    uint32_t builds = 0;       // models built
    uint32_t failures = 0;     // items that could not be shown
    uint32_t heapUsed = 0;     // of the latest model's heap
    float scale = 0.0f;        // this tick's
    float offset[3] = {};      // where it is drawn: from the dummy, turned by -shape_angle.y
    uint8_t emitters = 0;      // effect emitters alive this tick
    // daDitem_c's light, the heart sparkles' fade-out and the Poe soul's sound
    bool light = false;            // the item's light is set this tick
    float lightPow = 0.0f;
    uint32_t lightTicks = 0;       // ticks lit while the item was drawn
    uint32_t lightFades = 0;       // lights that faded to 0 after their item went
    uint32_t lightFadeTicks = 0;   // ticks the latest of those took
    uint8_t sparkleAlpha = 0;      // the heart sparkles' alpha while they fade out
    uint32_t sparkleFades = 0;     // sparkle fade-outs finished
    uint32_t sparkleFadeTicks = 0; // ticks the latest took
    bool sound = false;            // the Poe soul's sound started this tick
    uint32_t soundTicks = 0;
};

// daDitem_c's model held up by a remote player, built from the game's item archive.
class DummyGetItem {
public:
    // Each dummy tick after its matrices; `pos` and `yaw` are the dummy's current.pos and
    // shape_angle.y, `shown` false while the dummy is hidden.
    void update(const RemoteGetItem& gi, const cXyz& pos, s16 yaw, bool shown);
    // For a tick update does not run (the remote is elsewhere): frees everything.
    void stop();
    void draw(fopAc_ac_c& dummy);
    // True once no archive load is under way; call until then before destroy.
    bool settled();
    void destroy();

    const DummyGetItemDebug& debug() const { return mDebug; }

private:
    enum class Load : uint8_t { Idle, Loading, Loaded, Failed };

    bool request();
    bool build();
    void release();
    void fail(const char* why);
    void updateModel(const RemoteGetItem& gi, const cXyz& pos, s16 yaw, bool shown);
    void place(const RemoteGetItem& gi, const cXyz& pos, s16 yaw);
    void setEffects(const RemoteGetItem& gi);
    cXyz afterglowPos(const cXyz& pos, s16 yaw) const;
    void updateLight(const RemoteGetItem& gi, const cXyz& pos, s16 yaw, bool shown);
    void cutLight();
    void updateSparkles(const cXyz& pos, s16 yaw, bool shown);
    void stopSparkles();
    void updateSound(const RemoteGetItem& gi);

    request_of_phase_process_class mPhase{};
    Load mLoad = Load::Idle;
    const char* mArc = nullptr;
    uint8_t mItem = 0xFF;  // display item requested
    bool mBuildTried = false;
    bool mEffectMtx = false;
    s8 mTevFrm = -1;
    s8 mBtpFrm = -1;
    JKRSolidHeap* mHeap = nullptr;
    J3DModel* mModel = nullptr;
    mDoExt_btkAnm* mBtk = nullptr;
    mDoExt_bckAnm* mBck = nullptr;
    mDoExt_brkAnm* mBrk = nullptr;
    mDoExt_btpAnm* mBtp = nullptr;
    bool mPlaced = false;
    cXyz mPos = cXyz(0.0f, 0.0f, 0.0f);
    cXyz mEyePos = cXyz(0.0f, 0.0f, 0.0f);
    dKy_tevstr_c mTevStr;
    bool mTevReady = false;
    uint32_t mEmitters[2] = {};
    // Where the item was last drawn, from the dummy: the light and sparkles stay there as they fade
    float mLastOffset[3] = {};
    float mLastLift = 0.0f;
    uint8_t mLastReal = 0xFF;
    bool mLastNoLight = false;
    LIGHT_INFLUENCE mLight{};
    bool mLightOn = false;
    uint32_t mLightFadeTicks = 0;
    uint32_t mFadeEmitters[2] = {};
    uint8_t mSparkleAlpha = 0;
    uint32_t mSparkleFadeTicks = 0;
    Z2SoundObjSimple mSound;
    cXyz mSoundPos = cXyz(0.0f, 0.0f, 0.0f);
    bool mSoundReady = false;
    DummyGetItemDebug mDebug;
};

}  // namespace twili
