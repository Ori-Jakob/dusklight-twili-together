#pragma once

#include "presence/RemotePose.hpp"

#include "d/d_kankyo_tev_str.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "SSystem/SComponent/c_phase.h"
#include "m_Do/m_Do_ext.h"

#include <cstdint>

class J3DAnmTexPattern;
class J3DAnmTextureSRTKey;
class J3DJoint;
class J3DModel;
class JKRSolidHeap;

namespace twili {

// daHoZelda_matAnm_c with the sender's eye state instead of its statics.
class DummyHoZeldaEyeAnm : public J3DMaterialAnm {
public:
    void calc(J3DMaterial* material) const override;
    const uint8_t* mFlags = nullptr;  // RemoteHorseZelda::eyeFlags shown
    float mNowX = 0.0f, mNowY = 0.0f;
    mutable float mOldX = 0.0f, mOldY = 0.0f;
};

// What the autotest reads of her look (DummyHorseDebugInfo).
struct DummyHoZeldaLook {
    bool look = false;       // the sender's look is applied
    int16_t neck[2] = {}, body[2] = {};
    int16_t bodyZ = 0;       // the body joint's Z turn (her pitch less mBodyAngle.x)
    uint16_t btp = 0xFFFF, btk = 0xFFFF;  // eye clips entered at the last draw
    uint8_t eyeFlags = 0;
    float headTurnDeg = 0.0f;  // what the joint callbacks turned joints 4 and 1 by
    float bodyTurnDeg = 0.0f;
};

// daHoZelda_c's look behind a remote rider, from the game's HoZelda.arc and the sender's clips.
class DummyHoZelda {
public:
    // Each tick of the horse puppet; `root` is the horse's root joint.
    void update(const RemoteHorseZelda& zelda, bool wanted, MtxP root, const csXyz& angle,
                bool ridden);
    void draw(const dKy_tevstr_c& horseTev, u32 shadowId);
    void destroy();
    bool shown() const { return mShown; }
    const DummyHoZeldaLook& look() const { return mLookDebug; }
    const cXyz& pos() const { return mPos; }
    uint16_t anm() const { return mShown ? mBound[0] : 0xFFFF; }

private:
    bool build();
    void release();
    void pose(const RemoteHorseZelda& zelda, MtxP root, const csXyz& angle, bool ridden);
    void bindEyes(const RemoteHorseZelda& zelda);
    void turnJoint(u16 jnt);

    request_of_phase_process_class mPhase{};
    bool mRequested = false;
    bool mFailed = false;
    JKRSolidHeap* mHeap = nullptr;
    J3DModel* mModel = nullptr;
    J3DModel* mBow = nullptr;
    mDoExt_AnmRatioPack mPacks[3];
    mDoExt_MtxCalcAnmBlendTbl* mCalc = nullptr;
    mDoExt_bckAnm mBowBck;
    uint16_t mBound[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t mBowAnm = 0xFFFF;
    float mBowFrame = 0.0f;
    bool mUpper = false;
    bool mShown = false;
    uint32_t mAbsentTicks = 0;
    cXyz mPos = cXyz::Zero;
    dKy_tevstr_c mTevStr;
    // The sender's look: setNeckAngle and searchBodyAngle's turns, its eye clips and offsets
    bool mLook = false;
    int16_t mNeck[2] = {}, mBody[2] = {};
    int16_t mShapeX = 0;
    uint8_t mEyeFlags = 0;
    DummyHoZeldaEyeAnm* mEyeAnm[2] = {};
    J3DAnmTexPattern* mBtp = nullptr;
    J3DAnmTextureSRTKey* mBtk = nullptr;
    uint16_t mBtpId = 0xFFFF, mBtkId = 0xFFFF;
    float mBtpFrame = 0.0f, mBtkFrame = 0.0f;
    DummyHoZeldaLook mLookDebug;

    friend int dummyHoZeldaJointCallBack(J3DJoint* joint, int timing);
};

}  // namespace twili
