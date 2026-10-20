#include "actors/DummyHoZelda.hpp"

#include "core/Log.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_math.h"
#include "SSystem/SComponent/c_phase.h"

#include <algorithm>
#include <cmath>

namespace twili {
namespace {

constexpr const char* kArc = "HoZelda";
constexpr u16 kZeldaBmd = 0x23;
constexpr u16 kBowBmd = 0x20;
constexpr u16 kBowIdleBck = 0xC;
constexpr u16 kJointNum = 47;
constexpr u16 kUpperEndJoint = 23;
constexpr u16 kBodyJoint = 1;
constexpr u16 kHeadJoint = 4;
// The eyes' materials (daHoZelda_c::create's mpMatAnm)
constexpr u16 kEyeMaterial = 4;
constexpr u32 kHeapSize = 0x10000;
// She goes back to the archive after this long without the sender's Zelda.
constexpr uint32_t kReleaseTicks = 150;

// daHoZelda_c::setMatrix and setRideOffset, behind a rider or alone in the saddle
const Vec kRidePos = {-5.894f, 52.61f, 4.079f};
const Vec kFrontRidePos = {-75.893997f, 57.61f, 4.079f};
const Vec kRideOffset = {0.1f, 236.7f, -63.554f};
const Vec kFrontRideOffset = {0.1f, 241.7f, 6.445999f};

// The angle between the rotations of `a` and `b`, columns normalised against joint scale.
f32 turnDeg(const Mtx a, const Mtx b) {
    f32 trace = 0.0f;
    for (int k = 0; k < 3; k++) {
        f32 dot = 0.0f, la = 0.0f, lb = 0.0f;
        for (int r = 0; r < 3; r++) {
            dot += a[r][k] * b[r][k];
            la += a[r][k] * a[r][k];
            lb += b[r][k] * b[r][k];
        }
        const f32 norm = std::sqrt(la * lb);
        trace += norm > 1e-6f ? dot / norm : 1.0f;
    }
    return std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f)) * 180.0f / 3.14159265f;
}

J3DAnmTransform* clip(uint16_t id) {
    if (id == 0xFFFF) {
        return nullptr;
    }
    auto* anm = static_cast<J3DAnmBase*>(dComIfG_getObjectRes(kArc, id));
    const s32 kind = anm != nullptr ? anm->getKind() : -1;
    return kind == 0 || kind == 8 || kind == 9 ? static_cast<J3DAnmTransform*>(anm) : nullptr;
}

}  // namespace

// daHoZelda_matAnm_c::calc
void DummyHoZeldaEyeAnm::calc(J3DMaterial* material) const {
    J3DMaterialAnm::calc(material);
    const uint8_t flags = mFlags != nullptr ? *mFlags : 0;
    const uint8_t morf = (flags >> kMidnaEyeMorfShift) & 7;
    for (u32 i = 0; i < 8; i++) {
        if (!getTexMtxAnm(i).getAnmFlag()) {
            continue;
        }
        J3DTexMtxInfo& info = material->getTexGenBlock()->getTexMtx(i)->getTexMtxInfo();
        if (morf != 0) {
            const f32 t = 1.0f / (morf + 1);
            info.mSRT.mTranslationX = mOldX * (1.0f - t) + info.mSRT.mTranslationX * t;
            info.mSRT.mTranslationY = mOldY * (1.0f - t) + info.mSRT.mTranslationY * t;
        } else if (flags & kMidnaEyeMove) {
            info.mSRT.mTranslationX = mNowX;
            info.mSRT.mTranslationY = mNowY;
        }
        mOldX = info.mSRT.mTranslationX;
        mOldY = info.mSRT.mTranslationY;
    }
}

// daHoZelda_c::modelCallBack's upper-body switch (joints 1-22 while the bow is up) and turns.
int dummyHoZeldaJointCallBack(J3DJoint* joint, int timing) {
    if (timing == 0) {
        auto* self = reinterpret_cast<DummyHoZelda*>(j3dSys.getModel()->getUserArea());
        const u16 jnt = joint->getJntNo();
        if (jnt == 0 && self->mUpper) {
            self->mPacks[2].setRatio(1.0f);
        } else if (jnt == kUpperEndJoint) {
            self->mPacks[2].setRatio(0.0f);
        } else if ((jnt == kBodyJoint || jnt == kHeadJoint) && self->mLook) {
            self->turnJoint(jnt);
        }
    }
    return 1;
}

// modelCallBack's searchBodyAngle (joint 1) and setNeckAngle (joint 4) turns.
void DummyHoZelda::turnJoint(u16 jnt) {
    Mtx before;
    cMtx_copy(mModel->getAnmMtx(jnt), before);
    mDoMtx_stack_c::copy(before);
    if (jnt == kBodyJoint) {
        mDoMtx_stack_c::XrotM(mBody[1]);
        mDoMtx_stack_c::ZrotM(static_cast<s16>(mShapeX - mBody[0]));
    } else {
        mDoMtx_stack_c::XrotM(mNeck[1]);
        mDoMtx_stack_c::ZrotM(static_cast<s16>(-mNeck[0]));
    }
    mModel->setAnmMtx(jnt, mDoMtx_stack_c::get());
    cMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    const f32 turned = turnDeg(before, mDoMtx_stack_c::get());
    if (jnt == kBodyJoint) {
        mLookDebug.bodyTurnDeg = turned;
    } else {
        mLookDebug.headTurnDeg = turned;
    }
}

bool DummyHoZelda::build() {
    auto* data = static_cast<J3DModelData*>(dComIfG_getObjectRes(kArc, kZeldaBmd));
    auto* bowData = static_cast<J3DModelData*>(dComIfG_getObjectRes(kArc, kBowBmd));
    J3DAnmTransform* bowIdle = clip(kBowIdleBck);
    if (data == nullptr || bowData == nullptr || bowIdle == nullptr ||
        data->getJointNum() != kJointNum || data->getMaterialNum() <= kEyeMaterial + 1)
    {
        return false;
    }
    mHeap = mDoExt_createSolidHeapFromGame(kHeapSize, 0x20);
    if (mHeap == nullptr) {
        return false;
    }
    JKRHeap* prev = mDoExt_setCurrentHeap(mHeap);
    mModel = mDoExt_J3DModel__create(data, 0, 0x11020284);
    mBow = mDoExt_J3DModel__create(bowData, 0x80000, 0x11000084);
    mCalc = JKR_NEW mDoExt_MtxCalcAnmBlendTbl(3, mPacks);
    const bool bck = mBowBck.init(bowIdle, 1, 0, 1.0f, 0, -1, false);
    for (DummyHoZeldaEyeAnm*& eye : mEyeAnm) {
        eye = JKR_NEW DummyHoZeldaEyeAnm();
        if (eye != nullptr) {
            eye->mFlags = &mEyeFlags;
        }
    }
    mDoExt_setCurrentHeap(prev);
    mDoExt_adjustSolidHeap(mHeap);
    if (mModel == nullptr || mBow == nullptr || mCalc == nullptr || !bck ||
        mEyeAnm[0] == nullptr || mEyeAnm[1] == nullptr)
    {
        release();
        return false;
    }
    mModel->setUserArea(reinterpret_cast<uintptr_t>(this));
    mBowAnm = kBowIdleBck;
    dKy_tevstr_init(&mTevStr, -1, 0xFF);
    return true;
}

void DummyHoZelda::release() {
    if (mHeap != nullptr) {
        mDoExt_destroySolidHeap(mHeap);
        mHeap = nullptr;
    }
    mModel = mBow = nullptr;
    mCalc = nullptr;
    mEyeAnm[0] = mEyeAnm[1] = nullptr;
    mBtp = nullptr;
    mBtk = nullptr;
    mBtpId = mBtkId = 0xFFFF;
    mLook = false;
    mLookDebug = DummyHoZeldaLook{};
    for (mDoExt_AnmRatioPack& pack : mPacks) {
        pack.setAnmTransform(nullptr);
        pack.setRatio(0.0f);
    }
    for (uint16_t& id : mBound) {
        id = 0xFFFF;
    }
    mBowAnm = 0xFFFF;
    mShown = false;
    if (mRequested) {
        dComIfG_resDelete(&mPhase, kArc);
        mRequested = false;
    }
}

void DummyHoZelda::update(const RemoteHorseZelda& zelda, bool wanted, MtxP root,
                          const csXyz& angle, bool ridden) {
    mShown = false;
    if (!wanted) {
        if (mRequested && ++mAbsentTicks > kReleaseTicks) {
            release();
        }
        return;
    }
    mAbsentTicks = 0;
    if (mModel == nullptr) {
        if (mFailed) {
            return;
        }
        // As daHoZelda_c::create: the archive is the game's, counted per request.
        mRequested = true;
        const int phase = dComIfG_resLoad(&mPhase, kArc);
        if (phase == cPhs_ERROR_e) {
            mFailed = true;
            mRequested = false;
            TwiliLog.warn("[horse] HoZelda.arc did not load; the remote Zelda is not shown");
            return;
        }
        if (phase != cPhs_COMPLEATE_e) {
            return;
        }
        if (!build()) {
            mFailed = true;
            TwiliLog.warn("[horse] the remote Zelda's models could not be built");
            return;
        }
    }
    pose(zelda, root, angle, ridden);
}

void DummyHoZelda::pose(const RemoteHorseZelda& zelda, MtxP root, const csXyz& angle,
                        bool ridden) {
    for (int i = 0; i < 3; i++) {
        J3DAnmTransform* anm = clip(zelda.anm[i]);
        if (anm != nullptr && anm->getFrameMax() > 0) {
            anm->setFrame(std::clamp(zelda.frame[i], 0.0f, static_cast<f32>(anm->getFrameMax())));
        }
        mPacks[i].setAnmTransform(anm);
        mBound[i] = anm != nullptr ? zelda.anm[i] : 0xFFFF;
    }
    if (mPacks[0].getAnmTransform() == nullptr) {
        return;
    }
    const bool second = mPacks[1].getAnmTransform() != nullptr;
    mPacks[0].setRatio(second ? 1.0f - zelda.ratio : 1.0f);
    mPacks[1].setRatio(second ? zelda.ratio : 0.0f);
    mPacks[2].setRatio(0.0f);
    mUpper = zelda.upper && mPacks[2].getAnmTransform() != nullptr;
    mLook = zelda.look;
    mShapeX = angle.x;
    for (int i = 0; i < 2; i++) {
        mNeck[i] = zelda.neck[i];
        mBody[i] = zelda.body[i];
    }
    bindEyes(zelda);
    mLookDebug.look = mLook;
    mLookDebug.headTurnDeg = mLookDebug.bodyTurnDeg = 0.0f;
    if (mLook) {
        for (int i = 0; i < 2; i++) {
            mLookDebug.neck[i] = mNeck[i];
            mLookDebug.body[i] = mBody[i];
        }
        mLookDebug.bodyZ = static_cast<s16>(mShapeX - mBody[0]);
        mLookDebug.eyeFlags = mEyeFlags;
    }

    mDoMtx_multVec(root, ridden ? &kRidePos : &kFrontRidePos, &mPos);
    const Vec& offset = ridden ? kRideOffset : kFrontRideOffset;
    mDoMtx_stack_c::transS(mPos);
    mDoMtx_stack_c::ZXYrotM(angle.x, angle.y, angle.z);
    mDoMtx_stack_c::transM(-offset.x, -offset.y, -offset.z);
    mModel->setBaseTRMtx(mDoMtx_stack_c::get());

    // The model data is the game's: another daHoZelda_c may have hooked its joints.
    J3DModelData* data = mModel->getModelData();
    J3DJoint* rootJoint = data->getJointNodePointer(0);
    static constexpr u16 kHooked[] = {0, 1, 4, kUpperEndJoint};
    J3DJointCallBack saved[4];
    for (int i = 0; i < 4; i++) {
        J3DJoint* j = data->getJointNodePointer(kHooked[i]);
        saved[i] = j->getCallBack();
        j->setCallBack(dummyHoZeldaJointCallBack);
    }
    J3DMtxCalc* savedCalc = rootJoint->getMtxCalc();
    rootJoint->setMtxCalc(mCalc);
    mModel->calc();
    rootJoint->setMtxCalc(savedCalc);
    for (int i = 0; i < 4; i++) {
        data->getJointNodePointer(kHooked[i])->setCallBack(saved[i]);
    }

    // setBowModel
    if (zelda.bowAnm != mBowAnm) {
        if (J3DAnmTransform* bck = clip(zelda.bowAnm)) {
            mBowBck.init(bck, 1, -1, 1.0f, 0, -1, true);
            mBowAnm = zelda.bowAnm;
        }
    }
    mBowFrame = zelda.bowFrame;
    mDoMtx_stack_c::copy(mModel->getAnmMtx(0x11));
    mDoMtx_stack_c::transM(10.0f, -2.0f, 0.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(95.0f), 0, cM_deg2s(10.0f));
    mBow->setBaseTRMtx(mDoMtx_stack_c::get());
    mBowBck.entry(mBow->getModelData(), mBowFrame);
    mBow->calc();
    mShown = true;
}

// setEyeBtp / setEyeBtk with the sender's clips, setEyeMove with its offsets.
void DummyHoZelda::bindEyes(const RemoteHorseZelda& zelda) {
    J3DModelData* data = mModel->getModelData();
    const uint16_t btp = zelda.look ? zelda.btp : 0xFFFF;
    const uint16_t btk = zelda.look ? zelda.btk : 0xFFFF;
    if (btp != mBtpId) {
        auto* anm =
            static_cast<J3DAnmBase*>(btp != 0xFFFF ? dComIfG_getObjectRes(kArc, btp) : nullptr);
        mBtp = anm != nullptr && anm->getKind() == 2 ? static_cast<J3DAnmTexPattern*>(anm) : nullptr;
        if (mBtp != nullptr) {
            mBtp->searchUpdateMaterialID(data);
        }
        mBtpId = btp;
    }
    if (btk != mBtkId) {
        auto* anm =
            static_cast<J3DAnmBase*>(btk != 0xFFFF ? dComIfG_getObjectRes(kArc, btk) : nullptr);
        mBtk = anm != nullptr && anm->getKind() == 4 ? static_cast<J3DAnmTextureSRTKey*>(anm)
                                                     : nullptr;
        if (mBtk != nullptr) {
            mBtk->searchUpdateMaterialID(data);
        }
        mBtkId = btk;
    }
    mBtpFrame = zelda.btpFrame;
    mBtkFrame = zelda.btkFrame;
    mEyeFlags = zelda.eyeFlags;
    for (int i = 0; i < 2; i++) {
        mEyeAnm[i]->mNowX = zelda.eyeOffset[i * 2];
        mEyeAnm[i]->mNowY = zelda.eyeOffset[i * 2 + 1];
    }
}

// daHoZelda_c::draw
void DummyHoZelda::draw(const dKy_tevstr_c& horseTev, u32 shadowId) {
    if (!mShown) {
        return;
    }
    mTevStr.room_no = horseTev.room_no;
    mTevStr.YukaCol = horseTev.YukaCol;
    g_env_light.settingTevStruct(0, &mPos, &mTevStr);
    g_env_light.setLightTevColorType_MAJI(mModel, &mTevStr);
    // The model data is the game's: our eye animators come off again after the entry.
    J3DModelData* data = mModel->getModelData();
    const bool eyes = mLook && mBtp != nullptr && mBtk != nullptr;
    J3DMaterialAnm* saved[2] = {};
    if (eyes) {
        for (int i = 0; i < 2; i++) {
            J3DMaterial* material = data->getMaterialNodePointer(kEyeMaterial + i);
            saved[i] = material->getMaterialAnm();
            material->setMaterialAnm(mEyeAnm[i]);
        }
        mBtp->setFrame(static_cast<s16>(mBtpFrame));
        data->entryTexNoAnimator(mBtp);
        mBtk->setFrame(mBtkFrame);
        data->entryTexMtxAnimator(mBtk);
    }
    mDoExt_modelEntryDL(mModel);
    if (eyes) {
        data->removeTexNoAnimator(mBtp);
        data->removeTexMtxAnimator(mBtk);
        for (int i = 0; i < 2; i++) {
            data->getMaterialNodePointer(kEyeMaterial + i)->setMaterialAnm(saved[i]);
        }
    }
    mLookDebug.btp = eyes ? mBtpId : 0xFFFF;
    mLookDebug.btk = eyes ? mBtkId : 0xFFFF;
    g_env_light.setLightTevColorType_MAJI(mBow, &mTevStr);
    mDoExt_modelEntryDL(mBow);
    if (shadowId != 0) {
        dComIfGd_addRealShadow(shadowId, mModel);
        dComIfGd_addRealShadow(shadowId, mBow);
    }
}

void DummyHoZelda::destroy() {
    release();
    mFailed = false;
}

}  // namespace twili
