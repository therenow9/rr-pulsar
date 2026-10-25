#include <kamek.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Holders 4-7 follow Input::Manager's own 0x415C bytes: holder id is mgr + 4 + id * 0xEC, plus kExtGap for ids 4+,
// because index 4 of the game's array is the first AIControllerHolder.

namespace SplitScreen8 {

const u32 kManagerSize = 0x415c;
const u32 kHolderSize = 0xec;
const u32 kExtGap = kManagerSize - 4 - kGameLocal * kHolderSize;

extern "C" Input::RealControllerHolder *holderCtor(Input::RealControllerHolder *);

Input::RealControllerHolder &Holder(Input::Manager &input, u32 id) {
    u8 *base = reinterpret_cast<u8 *>(&input) + 4 + id * kHolderSize;
    return *reinterpret_cast<Input::RealControllerHolder *>(id < kGameLocal ? base : base + kExtGap);
}

// P1-4's holders are SectionPad's (null for a slot with no pad); P5-8's are holders 4-7.
Input::RealControllerHolder *PlayerHolder(u32 player) {
    if (player < kGameLocal)
        return SectionMgr::sInstance->pad.padInfos[player].controllerHolder;
    return &Holder(*Input::Manager::sInstance, player);
}

// uiinputStates[0] is this frame's, [1] the last frame's; buttonActions has one bit per Action.
bool UIPressed(const Input::RealControllerHolder &holder, u32 action) {
    const u32 bit = 1 << action;
    return (holder.uiinputStates[0].buttonActions & bit) != 0 && (holder.uiinputStates[1].buttonActions & bit) == 0;
}

// Holders a race uses: 0-3 as the game's loops, and 4-7 only for a race of more than 4 locals.
static u32 RaceHolderCount() {
    return raceLocalCount > kGameLocal ? raceLocalCount : kGameLocal;
}

// CreateInstance allocates the manager with room for the extra holders: li r3, 0x415c + 4 * 0xec.
kmWrite32(0x80523158, 0x3860450c);

// Built as Manager::__ct's loop builds holders 0-3 (0x80523438): the holder's +0x1C and its ghost
// writer's +0x18 take the id, and the holder starts on the dummy controller.
static Input::Manager *ConstructExtHolders(Input::Manager *input) {
    if (input == nullptr)
        return input;
    for (u32 id = kGameLocal; id < kMaxLocal; ++id) {
        Input::RealControllerHolder &holder = Holder(*input, id);
        holderCtor(&holder);
        reinterpret_cast<u8 *>(&holder)[0x1c] = id;
        reinterpret_cast<u8 *>(holder.ghostWriter)[0x18] = id;
        holder.SetController(&input->dummyController, nullptr);
    }
    return input;
}

// CreateInstance+0x30 replaces "lis r4" of sInstance, reached with r3 = the manager, or 0 if new failed.
extern "C" void ptr_inputBase(void *);
asmFunc ConstructExtHoldersStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        bl ConstructExtHolders;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lis r4, ptr_inputBase @ha;
        blr;)
}
kmCall(0x8052316c, ConstructExtHoldersStub);

// Per frame, after Update's loop over holders 0-3 (bound at 0x80523954), with the same pause flag:
// those a race of more than 4 locals uses, and those a 5-8 player game's menus join (Join.cpp).
static void UpdateExtHolders(Input::Manager *input) {
    const u32 count = raceLocalCount > menuLocalCount ? raceLocalCount : menuLocalCount;
    for (u32 id = kGameLocal; id < count; ++id) Holder(*input, id).Update(input->isPaused);
}

// Update+0x6C replaces "lwz r12, 0x15b4(r29)" (r29 = the manager); r3, r4 and r12 are set after it.
asmFunc UpdateExtHoldersStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r29;
        bl UpdateExtHolders;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lwz r12, 0x15b4(r29);
        blr;)
}
kmCall(0x8052395c, UpdateExtHoldersStub);

// Every scene's Prepare initialises holders 0-3 (loop bound 0x805236DC); the extra holders a race uses too.
static void PrepareExtHolders(Input::Manager *input) {
    for (u32 id = kGameLocal; id < RaceHolderCount(); ++id) Holder(*input, id).Init();
}

// Prepare+0x54 replaces "mr r3, r30" (r30 = the manager).
asmFunc PrepareExtHoldersStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r30;
        bl PrepareExtHolders;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        mr r3, r30;
        blr;)
}
kmCall(0x805236e4, PrepareExtHoldersStub);

// Raceinfo::Init blocks every holder's input until StartGhostReading releases it; both must cover
// the extra holders, or players 5-8 never move. Whole functions, unrolled or looped over 4 in the game.
static void BlockAllInputs(Input::Manager *input) {
    for (u32 id = 0; id < RaceHolderCount(); ++id) Holder(*input, id).blockInputs = true;
}
kmBranch(0x80524568, BlockAllInputs);

static void StartAllGhostReading(Input::Manager *input) {
    for (u32 id = 0; id < RaceHolderCount(); ++id) Holder(*input, id).StartGhostReading();
}
kmBranch(0x80524580, StartAllGhostReading);

static void EndAllGhostWriting(Input::Manager *input) {
    for (u32 id = 0; id < RaceHolderCount(); ++id) Holder(*input, id).EndGhostWriting();
}
kmBranch(0x805245dc, EndAllGhostWriting);

static void ResetAllIdleFrameCounters(Input::Manager *input) {
    for (u32 id = 0; id < RaceHolderCount(); ++id) {
        Input::RealControllerHolder &holder = Holder(*input, id);
        holder.idleFrameCounter = 0;
        holder.unknown_0xc4 = 0;
    }
}
kmBranch(0x805235ac, ResetAllIdleFrameCounters);

// Raceinfo::EndPlayerRace+0x4C calls it with the finishing player's controller id.
static void EndGhostWriting(Input::Manager *input, u8 id) {
    Holder(*input, id).EndGhostWriting();
}
kmBranch(0x805245cc, EndGhostWriting);

// Replaces "mulli r0, r0, 0xec" (r0 = a controller id), adding kExtGap (0x3da8) for ids 4-7. Branchless so CR0 survives;
// r12 is dead and LR saved at every site: InitControllers, RaceinfoPlayer::__ct, ComputeGPRank x2, TrySetController x2.
asmFunc HolderOffset() {
    ASM(
        nofralloc;
        srwi r12, r0, 2;
        mulli r0, r0, 0xec;
        mulli r12, r12, 0x3da8;
        add r0, r0, r12;
        blr;)
}
kmCall(0x8052ede8, HolderOffset);
kmCall(0x8052ee28, HolderOffset);
kmCall(0x8052ee68, HolderOffset);
kmCall(0x80534140, HolderOffset);
kmCall(0x80536b8c, HolderOffset);
kmCall(0x80536bd8, HolderOffset);

// TrySetController's "mulli r30, r4, 0xec" (r4 = the holder id) and its Wii and GC scans cover all eight holders.
// With 1-4 players holders 4-7 hold the dummy controller, which no scan matches, so the answer is vanilla's.
asmFunc TrySetHolderOffset() {
    ASM(
        nofralloc;
        srwi r12, r4, 2;
        mulli r30, r4, 0xec;
        mulli r12, r12, 0x3da8;
        add r30, r30, r12;
        blr;)
}
kmCall(0x80523ecc, TrySetHolderOffset);
kmCall(0x80523f3c, HolderOffset);
kmCall(0x80524034, HolderOffset);
kmWrite32(0x80523f5c, 0x28000008);  // the scans' bound: cmplwi r0, 8 for vanilla's 4
kmWrite32(0x80524054, 0x28000008);

// SectionPad::SetDriftType's mgr + 4 + hud * 0xEC is an AI holder for hud 4-7, and the race reads drift type from the
// holder (0x805944F4). Replaced whole at its entry: the leaf's tail call keeps no frame to patch into.
static void SetDriftTypeWide(const void *, u32 hud, u16 isDriftAuto) {
    Holder(*Input::Manager::sInstance, hud & 0xff).SetDriftType(isDriftAuto);
}
kmBranch(0x8061b420, SetDriftTypeWide);

}  // namespace SplitScreen8
