#include <kamek.hpp>
#include <core/nw4r/snd/BasicSound.hpp>
#include <core/nw4r/snd/SoundHandle.hpp>
#include <MarioKartWii/3D/Camera/CameraMgr.hpp>
#include <MarioKartWii/Audio/RaceMgr.hpp>
#include <MarioKartWii/Audio/RSARPlayer.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/Kart/KartLink.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/UI/Section/SectionPad.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// A 3D sound reaches at most 4 listeners (Voice::Acquire clamps to 4), so each camera past the 4th folds into the listener of
// the screen above it. Hud 4-7's per-hud state lives in side copies; echo, ambience and track triggers stay with P1-4.

namespace SplitScreen8 {

static const u32 kExtra = kMaxLocal - kGameLocal;

// The vanilla pan code's own slot for a hud slot of a widened race: 0 pans left, 1 right, -1 centre.
static s8 panSlot[kMaxLocal];
// RSARPlayer+0x15 holds a roulette flag per hud 0-3; the object is 0x1C, so 4-7 live here.
static u8 extRoulette[kExtra];
// Each camera 4-7's listener matrix, and its target position for the actor culling's distances.
// FoldCull reads a target's x, y, z and valid flag at +0, +4, +8 and +0xC.
struct ExtTarget {
    float x, y, z;
    u32 valid;
};
static float extMtx[kExtra][3][4];
static ExtTarget extTarget[kExtra];
// RaceMgr::kartActors holds 4 local KartActors; hud 4-7's live here, by hud.
static Audio::KartActor *extKartActors[kExtra];
// SoundTriggerMgr+0x12 holds each hud's last track trigger, 4 wide (the object is 0x18); hud 4-7's here.
static u8 extVariant[kExtra];

typedef void (*RouletteSoundFn)(Audio::RSARPlayer *);
typedef void (*GetViewMtxFn)(const RaceCamera *, float (*)[4], ExtTarget *, float);
typedef void (*MultVecFn)(const float (*)[4], const float *, float *);
static const RouletteSoundFn rouletteSound = reinterpret_cast<RouletteSoundFn>(0x807156f4);
static const GetViewMtxFn getViewMtx = reinterpret_cast<GetViewMtxFn>(0x805a6c58);
static const MultVecFn multVec = reinterpret_cast<MultVecFn>(0x8019a91c);
// The handle RaceRSARPlayer::HoldSound plays the roulette spin through.
static nw4r::snd::SoundHandle &rouletteHandle = *reinterpret_cast<nw4r::snd::SoundHandle *>(0x809c283c);

// Audio::RaceMgr::__ct+0x1EC replaces "stw r26, 0x848(r27)"; it runs once per race, after InitScreens set raceScreenCount.
// Nothing after it reads a volatile register and __ct saved LR, so the C++ returns straight to the site.
static void ResetAudioSide() {
    const u32 cols = raceScreenCount / 2;
    for (u32 hud = 0; hud < kMaxLocal; ++hud) {
        // A column left of the middle pans left, right of it right, the middle one of 3 none.
        const u32 twice = cols == 0 ? 0 : 2 * (hud % cols) + 1;
        panSlot[hud] = cols == 0 || twice == cols ? -1 : twice < cols ? 0 : 1;
    }
    for (u32 i = 0; i < kExtra; ++i) {
        extRoulette[i] = 0;
        extTarget[i].valid = 0;
        extKartActors[i] = nullptr;
    }
}
asmFunc RaceMgrCtorEnd() {
    ASM(
        nofralloc;
        stw r26, 0x848(r27);
        b ResetAudioSide;)
}
kmCall(0x80710874, RaceMgrCtorEnd);

// Item::Player::Update's hud gates on the roulette flag (+0x6A0) and item-get chimes (+0x6F4, +0x818), "cmplwi rN, 4" before a
// bge: hud 4+ passes only for a local of a widened race. r12 is reloaded before it is read and Update saved LR.
asmFunc ItemSoundGateR0() {
    ASM(
        nofralloc;
        cmplwi r0, 4;
        blt end;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq vanilla;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmplw r0, r12;
        blr;
        vanilla :;
        cmplwi r0, 4;
        end :;
        blr;)
}
kmCall(0x80797fc8, ItemSoundGateR0);

asmFunc ItemSoundGateR5() {
    ASM(
        nofralloc;
        cmplwi r5, 4;
        blt end;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq vanilla;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmplw r5, r12;
        blr;
        vanilla :;
        cmplwi r5, 4;
        end :;
        blr;)
}
kmCall(0x8079801c, ItemSoundGateR5);
kmCall(0x80798140, ItemSoundGateR5);

// The roulette flag setter 0x80715648+0x2C, "lbz r0, 0x4c(r5)", bounds hud (r6) by the listener count, 4 when widened, so
// hud 4+ get the local count. The cmpw after it sets CR0 again and the function saved LR.
asmFunc RouletteFlagBound() {
    ASM(
        nofralloc;
        lbz r0, 0x4c(r5);
        cmpwi r6, 4;
        blt end;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq end;
        lis r12, raceLocalCount @ha;
        lbz r0, raceLocalCount @l(r12);
        end :;
        blr;)
}
kmCall(0x80715674, RouletteFlagBound);

// Its store "stb r4, 0x15(r3)", r3 = player + hud (r0): hud 4+ goes to extRoulette, past the object.
asmFunc RouletteFlagStore() {
    ASM(
        nofralloc;
        cmpwi r0, 4;
        blt store;
        cmpwi r0, 8;
        bge skip;
        lis r12, extRoulette @ha;
        addi r12, r12, extRoulette @l;
        add r12, r12, r0;
        stb r4, -4(r12);
        skip :;
        blr;
        store :;
        stb r4, 0x15(r3);
        blr;)
}
kmCall(0x807156d8, RouletteFlagStore);

// RaceRSARPlayer::Calc+0x184 calls the roulette player, which plays the spin once, panned by even (left) and odd (right) flagged
// huds. Widened, all eight flags count, panned by column: right only +1, left only -1, else (right - left) / 2.
static void RouletteSoundWide(Audio::RSARPlayer *player) {
    if (raceScreenCount == 0) {
        rouletteSound(player);
        return;
    }
    u8 *flags = reinterpret_cast<u8 *>(player) + 0x15;
    s32 left = 0;
    s32 right = 0;
    for (u32 hud = 0; hud < kMaxLocal; ++hud) {
        const u8 flag = hud < kGameLocal ? flags[hud] : hud < raceLocalCount ? extRoulette[hud - kGameLocal] : 0;
        if (flag == 0)
            continue;
        if (panSlot[hud] != 1)
            ++left;
        if (panSlot[hud] != 0)
            ++right;
    }
    if (left == 0 && right == 0)
        return;
    const float pan = left == 0 ? 1.0f : right == 0 ? -1.0f : (right - left) * 0.5f;
    player->HoldSound(0xe2);
    if (rouletteHandle.basicSound != nullptr)
        rouletteHandle.basicSound->SetPan(pan);
    for (u32 i = 0; i < kGameLocal; ++i) flags[i] = 0;
    for (u32 i = 0; i < kExtra; ++i) extRoulette[i] = 0;
}
kmCall(0x807165f0, RouletteSoundWide);

// RSARPlayer::PlaySound+0xBA0, "cmpwi r28, 0": item-get chimes (0xE3-0xE6), the lap jingle (0xDA) and 0x74 pan hud 0/2 left,
// 1/3 right. Widened, r28 (the hud, dead after this block) becomes its column's slot.
asmFunc ChimePanSlot() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq end;
        cmplwi r28, 8;
        bge end;
        lis r12, panSlot @ha;
        addi r12, r12, panSlot @l;
        lbzx r28, r12, r28;
        extsb r28, r28;
        end :;
        cmpwi r28, 0;
        blr;)
}
kmCall(0x8071551c, ChimePanSlot);

// Audio::RaceMgr::Calc+0x144 calls UpdatePlayerMatrix, which sets the 4 listeners from cameras 0-3. Cameras 4+ get the same
// view matrix and target here, under the same gates and distance factor by game type.
static void UpdatePlayerMatrixWide(Audio::RaceMgr *mgr) {
    mgr->UpdatePlayerMatrix();
    if (raceScreenCount == 0)
        return;
    if (Audio::RSARPlayer::IsDemo()) {
        for (u32 i = 0; i < kExtra; ++i) extTarget[i].valid = 0;
        return;
    }
    const u8 *gate = *reinterpret_cast<u8 **>(0x809c1998);
    if (gate != nullptr && *reinterpret_cast<const s32 *>(gate + 0x28) >= 0)
        return;
    float factor;
    if (static_cast<u32>(mgr->gameType) - 7 <= 5)
        factor = *reinterpret_cast<const float *>(0x808a1e90);
    else if (mgr->gameType == 0)
        factor = *reinterpret_cast<const float *>(*reinterpret_cast<u8 **>(0x809c2898) + 0x8bc);
    else
        factor = *reinterpret_cast<const float *>(0x808a1f00);
    const RaceCameraMgr *cameras = RaceCameraMgr::sInstance;
    for (u32 k = kGameLocal; k < raceLocalCount && k < kMaxLocal; ++k) {
        if (cameras == nullptr || k >= cameras->cameraCount)
            break;
        ExtTarget &target = extTarget[k - kGameLocal];
        getViewMtx(cameras->sortedCameras[k], extMtx[k - kGameLocal], &target, factor);
        target.valid = 1;
    }
}
kmCall(0x80710de4, UpdatePlayerMatrixWide);

// Audio::Engine3D::UpdateAmbientParam+0x314, PSMTXMultVec(listener i, actor, out): out gives distance and pan. Listener i (r18)
// also carries camera i + columns, the screen below, when that is a local past the 4th; the nearer gives the vector.
static void FoldMultVec(const float (*listener)[4], const float *pos, float *out, s32 i) {
    multVec(listener, pos, out);
    if (raceScreenCount == 0 || i < 0)
        return;
    const u32 k = i + raceScreenCount / 2;
    if (k < kGameLocal || k >= raceLocalCount || k >= kMaxLocal)
        return;
    if (extTarget[k - kGameLocal].valid == 0)
        return;
    float other[3];
    multVec(extMtx[k - kGameLocal], pos, other);
    const float otherSq = other[0] * other[0] + other[1] * other[1] + other[2] * other[2];
    if (otherSq < out[0] * out[0] + out[1] * out[1] + out[2] * out[2]) {
        out[0] = other[0];
        out[1] = other[1];
        out[2] = other[2];
    }
}
asmFunc FoldListener() {
    ASM(
        nofralloc;
        mr r6, r18;
        b FoldMultVec;)
}
kmCall(0x806f7108, FoldListener);

// Its +0x5C4, "extsb. r0, r18": listener i's pan picks 0/2 left, 1/3 right; widened, it takes the column's slot (the folded
// screen below shares it). Float registers are live here, so only r0 and r12 are used.
asmFunc ListenerPanSlot() {
    ASM(
        nofralloc;
        extsb r0, r18;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq end;
        cmplwi r0, 8;
        bge end;
        lis r12, panSlot @ha;
        addi r12, r12, panSlot @l;
        lbzx r0, r12, r0;
        extsb r0, r0;
        end :;
        cmpwi r0, 0;
        blr;)
}
kmCall(0x806f73b8, ListenerPanSlot);

// Audio::RaceMgr::SortAndToggleActors' five loops open with "lbz r0, 0x4c(r24)" (r23 = actor position) and keep an actor by taxicab
// distance (f31) to the nearest listener target; this lowers f31 for cameras 4+. f0-f3, CR0, CTR are rewritten; LR saved.
asmFunc FoldCull() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq end;
        lis r12, extTarget @ha;
        addi r12, r12, extTarget @l;
        li r0, 4;
        mtctr r0;
        loop :;
        lwz r0, 0xc(r12);
        cmpwi r0, 0;
        beq next;
        lfs f3, 0(r12);
        lfs f2, 0(r23);
        fsubs f3, f3, f2;
        fabs f3, f3;
        lfs f1, 4(r12);
        lfs f2, 4(r23);
        fsubs f1, f1, f2;
        fabs f1, f1;
        lfs f0, 8(r12);
        lfs f2, 8(r23);
        fsubs f0, f0, f2;
        fabs f0, f0;
        fadds f3, f3, f1;
        fadds f3, f3, f0;
        fcmpo cr0, f3, f31;
        bge next;
        fmr f31, f3;
        next :;
        addi r12, r12, 0x10;
        bdnz loop;
        end :;
        lbz r0, 0x4c(r24);
        blr;)
}
kmCall(0x80713078, FoldCull);
kmCall(0x80713100, FoldCull);
kmCall(0x80713360, FoldCull);
kmCall(0x807133e8, FoldCull);
kmCall(0x8071348c, FoldCull);

// It then keeps the nearest 5 karts and 6 voice actors (0x808A1ED4/DC[listeners]), sparing every local in 4P; past 4 locals each
// limit grows by the extras. Replaces "lbzx r0, r25, r0" before "cmpw r22, r0"; r12 and CR0 are free, LR saved.
asmFunc CullLimitWide() {
    ASM(
        nofralloc;
        lbzx r0, r25, r0;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmplwi r12, 4;
        blelr;
        addi r12, r12, -4;
        add r0, r0, r12;
        blr;)
}
kmCall(0x80713610, CullLimitWide);
kmCall(0x8071365c, CullLimitWide);

// A KartActor's hud slot (+0xB3), 0-7 for a local of a widened race since KartSoundHud keeps it.
static s32 ActorHud(const Audio::KartActor *actor) {
    return reinterpret_cast<const s8 *>(actor)[0xb3];
}

typedef bool (*AnyKartFn)(const u8 *raceMgr);
typedef void (*ApplyTriggerFn)(u8 *triggers, s32 variant, s32 hud, void *link);
typedef void (*EchoVolumeFn)(void *echo, u32 hud, u32 frames, float volume);
typedef void (*AmbienceVolumeFn)(void *ambience, u32 hud, u32 frames);
static const AnyKartFn isAPlayerInMega = reinterpret_cast<AnyKartFn>(0x807117a0);
static const AnyKartFn isAPlayerInStar = reinterpret_cast<AnyKartFn>(0x8071172c);
static const AnyKartFn isAPlayerSquishedOrSmall = reinterpret_cast<AnyKartFn>(0x80711668);
static const ApplyTriggerFn applyTrigger = reinterpret_cast<ApplyTriggerFn>(0x80719044);
static const EchoVolumeFn echoSetVolume = reinterpret_cast<EchoVolumeFn>(0x807182b8);
static const AmbienceVolumeFn setAllAmbiencesVolume = reinterpret_cast<AmbienceVolumeFn>(0x806fcfa0);

// KartActor::Link+0xBC calls RaceMgr::SetKartSound, which keeps the first 4 locals in player order, so hud 4-7 could displace
// P1-4. They go to extKartActors; the counts (+0x28; +0x29 scales engine volume by locals racing) stay P1-4's.
static void SetKartSoundWide(Audio::RaceMgr *mgr, Audio::KartActor *actor) {
    const s32 hud = ActorHud(actor);
    if (IsWideHud(hud)) {
        if (hud < kMaxLocal)
            extKartActors[hud - kGameLocal] = actor;
        return;
    }
    mgr->SetKartActor(actor);
}
kmCall(0x807075f0, SetKartSoundWide);

// ItemAlterationMgr::UpdateStatus+0x28/+0x34/+0x40 ask if any local is Mega, starred or squished, for the music. Each reads only
// +0x40 (race state), +0x28 (count) and +0x18 (actors), so hud 4-7's are asked through a RaceMgr copy holding them.
static bool AnyKartWide(const u8 *mgr, AnyKartFn any) {
    if (any(mgr))
        return true;
    if (raceScreenCount == 0)
        return false;
    u8 copy[0x44];
    *reinterpret_cast<u32 *>(copy + 0x40) = *reinterpret_cast<const u32 *>(mgr + 0x40);
    u32 n = 0;
    for (u32 i = 0; i < kExtra; ++i) {
        if (extKartActors[i] != nullptr)
            reinterpret_cast<Audio::KartActor **>(copy + 0x18)[n++] = extKartActors[i];
    }
    copy[0x28] = n;
    return n != 0 && any(copy);
}
static bool IsAPlayerInMegaWide(const u8 *mgr) {
    return AnyKartWide(mgr, isAPlayerInMega);
}
static bool IsAPlayerInStarWide(const u8 *mgr) {
    return AnyKartWide(mgr, isAPlayerInStar);
}
static bool IsAPlayerSquishedOrSmallWide(const u8 *mgr) {
    return AnyKartWide(mgr, isAPlayerSquishedOrSmall);
}
kmCall(0x8070fef0, IsAPlayerInMegaWide);
kmCall(0x8070fefc, IsAPlayerInStarWide);
kmCall(0x8070ff08, IsAPlayerSquishedOrSmallWide);

// KartActor::ApplyKCLSoundTrigger's tail call to ApplyTrigger, which sets the hud's listener's ambience, echo and music, then
// curVariant[hud]. Hud 4-7 share P1-4's listeners, so only the store is done, under its two early returns (+0x38 bit 1, race state).
static void ApplyTriggerWide(u8 *triggers, s32 variant, s32 hud, void *link) {
    if (!IsWideHud(hud)) {
        applyTrigger(triggers, variant, hud, link);
        return;
    }
    if (hud >= kMaxLocal)
        return;
    if (link != nullptr) {
        const u8 *player = reinterpret_cast<const u8 *>(Raceinfo::sInstance->players[static_cast<const Kart::Link *>(link)->GetPlayerIdx()]);
        if (*reinterpret_cast<const u32 *>(player + 0x38) & 2)
            return;
    }
    const u32 state = Audio::RaceMgr::sInstance->raceState;
    if (state != 1 && (state < 3 || state > 6))
        return;
    extVariant[hud - kGameLocal] = variant;
}
kmBranch(0x80708ba4, ApplyTriggerWide);

// SoundTriggerMgr::Init+0x894, "lwz r0, 0x204(r1)", in the epilogue after huds below the listener count got their starting
// trigger; hud 4-7 start from hud 0's. r11 and r12 are not read by the epilogue.
asmFunc InitVariantsWide() {
    ASM(
        nofralloc;
        lbz r12, 0x12(r31);
        lis r11, extVariant @ha;
        addi r11, r11, extVariant @l;
        stb r12, 0(r11);
        stb r12, 1(r11);
        stb r12, 2(r11);
        stb r12, 3(r11);
        lwz r0, 0x204(r1);
        blr;)
}
kmCall(0x80718edc, InitVariantsWide);

// KartActor::StartSoundLimited+0x298/+0x380, "lbz r0, 0x12(r3)" with r3 = triggers + hud (r0): a
// local-only sound plays only on trigger 4. Hud 4-7 read extVariant. The cmpwi after sets CR0 again.
asmFunc LimitedVariantWide() {
    ASM(
        nofralloc;
        cmpwi r0, 4;
        blt vanilla;
        lis r12, extVariant @ha;
        addi r12, r12, extVariant @l;
        add r12, r12, r0;
        lbz r0, -4(r12);
        blr;
        vanilla :;
        lbz r0, 0x12(r3);
        blr;)
}
kmCall(0x80708570, LimitedVariantWide);
kmCall(0x80708658, LimitedVariantWide);

// KartActor::UpdateLapSounds, when a local finishes: EchoMgr::SetVolume and SetAllAmbiencesVolume write 4-wide per-listener
// arrays by hud, which hud 4+ would overrun; P1-4's listeners keep theirs.
static void EchoVolumeLocal(void *echo, u32 hud, u32 frames, float volume) {
    if (IsWideHud(hud))
        return;
    echoSetVolume(echo, hud, frames, volume);
}
static void AmbienceVolumeLocal(void *ambience, u32 hud, u32 frames) {
    if (IsWideHud(hud))
        return;
    setAllAmbiencesVolume(ambience, hud, frames);
}
kmCall(0x8070b3c8, EchoVolumeLocal);
kmCall(0x8070b3dc, AmbienceVolumeLocal);

// Its +0x19C, "addi r0, r3, -1": locals racing (+0x29) loses the finisher; hud 4-7 are not counted, so hud >> 2 (1 only for them,
// as a finisher's hud is 0-7) cancels it. No CR field is touched. The addi reads r12: with rA = r0 it is li.
asmFunc LocalsRacingWide() {
    ASM(
        nofralloc;
        lbz r12, 0xb3(r29);
        srwi r12, r12, 2;
        add r12, r3, r12;
        addi r0, r12, -1;
        blr;)
}
kmCall(0x8070b3ec, LocalsRacingWide);

// RSARPlayer::PlaySound+0xAE4 picks Wii Remote speakers via SectionPad::GetNewID, which reads padInfos[4]; hud 4-7 compute a local's
// ID from its holder (CalculateID), anything else 0. Join.cpp's chime passes P5-8's slot with raceScreenCount 0 or stale.
static u32 NewIDWide(const void *pads, s32 hud) {
    if (hud < kGameLocal || (raceScreenCount == 0 && menuLocalCount == 0))
        return static_cast<const SectionPad *>(pads)->GetNewID(hud);
    const u32 count = raceLocalCount > menuLocalCount ? raceLocalCount : menuLocalCount;
    if (hud >= count || hud >= kMaxLocal)
        return 0;
    return calculateID(&Holder(*Input::Manager::sInstance, hud));
}
kmCall(0x80715460, NewIDWide);

// The 4 CharacterActor voice-pan sites, "cmplwi r0, 4; bgt" on the hud (+0x6ED): hud 5+ skip the pan
// and 4 gets the centre. A local's hud 4-7 of a widened race passes (CR0 eq); the bgt reads CR0 only.
asmFunc VoicePanGate() {
    ASM(
        nofralloc;
        cmplwi r0, 4;
        blelr;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq gt;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmplw r0, r12;
        bge gt;
        cmplw r0, r0;
        blr;
        gt :;
        cmplwi r0, 4;
        blr;)
}
kmCall(0x808645c8, VoicePanGate);
kmCall(0x80864b0c, VoicePanGate);
kmCall(0x80864ff8, VoicePanGate);
kmCall(0x808658d8, VoicePanGate);

// Their pan leaf 0x806F6BEC ("cmplwi r5, 0xff" first) pans hud 0/2 left, 1/3 right; widened, a local's hud (r4) takes its column's
// slot, else centre (4), with r5 = 2 for the per-hud path. No LR saved, so it returns via CTR, which the leaf never reads.
asmFunc VoicePanSlot() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq end;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmplw r4, r12;
        bge centre;
        lis r12, panSlot @ha;
        addi r12, r12, panSlot @l;
        lbzx r4, r12, r4;
        extsb.r4, r4;
        bge slot;
        centre :;
        li r4, 4;
        slot :;
        li r5, 2;
        end :;
        cmplwi r5, 0xff;
        lis r12, 0x806f;
        ori r12, r12, 0x6bf0;
        mtctr r12;
        bctr;)
}
kmBranch(0x806f6bec, VoicePanSlot);

}  // namespace SplitScreen8
