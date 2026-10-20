#pragma once

#include "fx/CosmeticsRecolor.hpp"
#include "presence/RemotePose.hpp"

#include "d/actor/d_a_player.h"
#include "d/d_kankyo_tev_str.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphBase/J3DMatBlock.h"
#include "m_Do/m_Do_ext.h"
#include "Z2AudioLib/Z2Creature.h"

class J3DAnmTevRegKey;
class J3DAnmTexPattern;
class J3DAnmTextureSRTKey;
class J3DAnmTransform;
class J3DModel;
class J3DModelData;

namespace twili {

// Hair scale like daMidna_McaMorfCB1_c, which asks the local Midna whether to scale.
class DummyMidnaHairCB : public mDoExt_McaMorfCallBack1_c {
public:
    int execute(u16 jnt, J3DTransformInfo* info) override;
    const cXyz* mScale = nullptr;  // nullptr: FLG0_NO_HAIR_SCALE on the sender
};

// daMidna_matAnm_c with the sender's eye state instead of its statics.
class DummyMidnaEyeAnm : public J3DMaterialAnm {
public:
    void calc(J3DMaterial* material) const override;
    const uint8_t* mFlags = nullptr;  // RemoteMidnaPose::eyeFlags shown
    float mNowX = 0.0f, mNowY = 0.0f;
    mutable float mOldX = 0.0f, mOldY = 0.0f;
};

class DummyMidna {
public:
    // Wmdl models as changeWolf builds them for the local wolf.
    struct Models {
        J3DModelData* body;
        J3DModel* mask;
        J3DModel* hands;
        J3DModel* hairHand;
        J3DAnmTevRegKey* tiredBrk[4];
        // daMidna_c's shadow from Midna.arc, null where it failed to load
        J3DModel* shadowBody;  // s_md.bmd, posed from md.bmd's joints
        J3DModel* shadowMask;
        J3DModel* shadowHands;
        J3DModel* shadowHairHand;
        J3DModel* gokou;
        J3DAnmTextureSRTKey* moyaBtk[4];  // body, mask, hands, hair hand
        J3DAnmTevRegKey* appearBrk[4];    // body, mask, hair hand, gokou
    };

    // Forgets every pointer, without freeing
    void clearPointers();
    // Inside daDummyPlayer_c::createHeapImpl.
    bool createHeap(const Models& models);
    // initializeShell
    void initHeaps();
    void destroyHeaps();
    void deactivate();
    // No morph from the pose shown before (a snap or a form change).
    void resetContinuity() { mFresh = true; }
    // The next frames show her latest pose instead of blending across a jump.
    void snap();
    // Once per tick after the wolf body's calc; `wolf` may be null while she is apart.
    void update(const RemoteMidnaPose& pose, J3DModel* wolf, float frameAlpha, bool shown);
    void draw(const dKy_tevstr_c& owner, bool ownerTinted);
    void addRealShadow(u32 shadowId);
    // One of her sounds from the wire, at her place or at `fallback` while she is not posed.
    void playSfx(uint32_t id, bool voice, uint32_t mapInfo, const cXyz& fallback);
    uint32_t sfxPlayed() const { return mSfxPlayed; }
    uint32_t apartTicks() const { return mApartTicks; }
    uint32_t shownTicks() const { return mShownTicks; }
    bool shadowReady() const { return mShadowReady; }
    // Drew through the shadow branch (gokou and invisible models) on one of the last updates.
    bool drawsShadow() const {
        return mActive && mShadowDraws != 0 && mUpdates - mShadowDrawnAt <= 2;
    }
    uint32_t shadowDraws() const { return mShadowDraws; }
    bool eyeMoving() const { return mActive && (mEyeFlags & kMidnaEyeMove) != 0; }
    // daMidna_c::baseModelCallBack for md.bmd's joints (calc timing 0).
    void jointCallBack(u16 jnt);

    J3DModel* bodyModel() const;
    bool ready() const { return mReady; }
    uint8_t mode() const { return mActive ? mMode : kMidnaNone; }
    uint16_t bodyId() const { return mBodyId; }
    uint16_t upperId() const { return mShownUpperId; }
    uint8_t hairHand() const { return mHairHand; }
    uint8_t leftHand() const { return mLeftHand; }
    uint8_t rightHand() const { return mRightHand; }
    bool tired() const { return mTired; }
    uint32_t refused() const { return mRefusedCount; }
    // From her root to the wolf's joint WL_JNT_MD, 0 before the first update.
    float backDist() const { return mBackDist; }
    // The hair hand turned toward the remote's lock target on the last update, by this angle.
    bool hairAimApplied() const { return mActive && mHairAimApplied; }
    int16_t hairAimAngle() const { return mHairAimAngle; }
    // The owner's cosmetics hair palette indices, 0 for the game's colours.
    void setHairPalette(uint32_t base, uint32_t tips);
    uint8_t hairBase() const { return mHairBase; }
    uint8_t hairTips() const { return mHairTips; }

private:
    // Whether `id` may be read into a slot
    bool accept(uint16_t id, bool allowed, uint16_t& refused);
    void refuse(uint16_t id, uint16_t& refused);
    // True when the body clip changed.
    bool bindBody(uint16_t id, bool fresh);
    J3DAnmTransform* bindLayer(daPy_anmHeap_c& heap, uint16_t id, bool face,
                               uint16_t& boundId, J3DAnmTransform*& bound, uint16_t& refused);
    void bindTexture(bool btp, uint16_t id);
    void updateFaceTextures(float animFrame);
    void swapUpper();
    void swapFace();
    void updateParts(const RemoteMidnaPose& pose);
    void setHandShapes(uint8_t left, uint8_t right);
    void setTired(bool tired);
    void initHairColours();
    void chaseHairColours(bool big);
    void bindSound();
    void updateSound(const RemoteMidnaPose& pose, bool shown);
    bool createShadow(const Models& models);
    void updateShadow();
    void drawShadow(const dKy_tevstr_c& owner);

    mDoExt_McaMorfSO* mpMorf = nullptr;
    DummyMidnaHairCB mHairCB;
    J3DModel* mpMask = nullptr;
    J3DModel* mpHands = nullptr;
    J3DModel* mpHairHand = nullptr;
    J3DAnmTevRegKey* mTiredBrk[4] = {};
    daPy_anmHeap_c mBodyHeap;
    daPy_anmHeap_c mUpperHeap;
    daPy_anmHeap_c mFaceHeap;
    daPy_anmHeap_c mBtpHeap;
    daPy_anmHeap_c mBtkHeap;
    J3DAnmTransform* mBodyBck = nullptr;
    J3DAnmTransform* mUpperBck = nullptr;
    J3DAnmTransform* mFaceBck = nullptr;
    J3DAnmTexPattern* mpBtp = nullptr;
    J3DAnmTextureSRTKey* mpBtk = nullptr;
    // What each heap holds
    uint16_t mBodyId = 0, mUpperId = 0, mFaceId = 0, mBtpId = 0, mBtkId = 0;
    uint16_t mShownUpperId = 0, mShownFaceId = 0;
    // The last id each slot refused, so it is neither read nor counted again.
    uint16_t mRefusedBody = 0, mRefusedUpper = 0, mRefusedFace = 0, mRefusedBtp = 0,
             mRefusedBtk = 0;
    uint32_t mRefusedCount = 0;
    // The clips changeUpperBck / changeFaceBck swap with the morf's during calc.
    J3DAnmTransform* mUpperHeld = nullptr;
    J3DAnmTransform* mFaceHeld = nullptr;
    uint16_t mFaceJoint = 4;  // MD_JNT_HEAD_e
    s16 mNeckX = 0, mNeckY = 0, mBackboneZ = 0;
    int mBlinkFrame = 0;  // daMidna_c::mFrameCounter
    J3DGXColorS10 mHairColor;
    J3DGXColor mHairK1;
    J3DGXColor mHairK2;
    MidnaHairColours mHair = midnaHairColours(0, 0);
    uint8_t mHairBase = 0, mHairTips = 0;
    dKy_tevstr_c mTevStr;
    cXyz mPos = cXyz::Zero;
    float mBackDist = 0.0f;
    uint8_t mMode = kMidnaNone;
    bool mNoShadow = false;  // kMidnaNoShadow
    uint8_t mHairHand = 0xFF;
    uint8_t mLeftHand = 0xFE, mRightHand = 0xFE;
    bool mTired = false;
    bool mHairAimApplied = false;
    int16_t mHairAimAngle = 0;
    bool mReady = false;
    bool mActive = false;
    bool mFresh = true;
    DummyMidnaEyeAnm* mEyeAnm[2] = {};
    uint8_t mEyeFlags = 0;
    // daMidna_c::mSound: her voices from the wire, her clips' own sounds replayed here
    Z2Creature mSound;
    cXyz mEyePos = cXyz::Zero;
    bool mSoundReady = false;
    bool mBasBound = false;
    float mSoundFrame = 0.0f;
    uint32_t mSfxPlayed = 0;
    uint32_t mApartTicks = 0;
    uint32_t mShownTicks = 0;
    bool mSilhouette = false;
    // daMidna_c::draw's shadow branch
    J3DModel* mpShadowBody = nullptr;
    J3DModel* mpShadowMask = nullptr;
    J3DModel* mpShadowHands = nullptr;
    J3DModel* mpShadowHairHand = nullptr;
    J3DModel* mpGokou = nullptr;
    J3DAnmTextureSRTKey* mMoyaBtk[4] = {};
    J3DAnmTevRegKey* mAppearBrk[4] = {};
    mDoExt_invisibleModel mInvBody;
    mDoExt_invisibleModel mInvMask;
    mDoExt_invisibleModel mInvHands;
    mDoExt_invisibleModel mInvHairHand;
    bool mShadowReady = false;
    bool mShadowPosed = false;  // its matrices are this pose's, not an old one's
    uint32_t mUpdates = 0;
    uint32_t mShadowDrawnAt = 0;
    uint32_t mShadowDraws = 0;
};

}  // namespace twili

