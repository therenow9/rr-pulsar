#include <kamek.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Real controller holders 4-7 for local players 5-8 (D26). They follow Input::Manager's own 0x415C
// bytes, so holder id is mgr + 4 + id * 0xEC plus kExtGap for ids 4+: index 4 of the game's array
// is the first AIControllerHolder. docs/plans/m2-engine-widening.md, Phase C, has the site table.

namespace SplitScreen8 {

const u32 kManagerSize = 0x415c;
const u32 kHolderSize = 0xec;
const u32 kExtGap = kManagerSize - 4 - kGameLocal * kHolderSize;

typedef Input::RealControllerHolder *(*HolderCtorFn)(Input::RealControllerHolder *);
typedef void (*HolderFn)(Input::RealControllerHolder *);
static const HolderCtorFn holderCtor = reinterpret_cast<HolderCtorFn>(0x805220bc);
static const HolderFn startGhostReading = reinterpret_cast<HolderFn>(0x805215d4);

Input::RealControllerHolder &Holder(Input::Manager &input, u32 id) {
    u8 *base = reinterpret_cast<u8 *>(&input) + 4 + id * kHolderSize;
    return *reinterpret_cast<Input::RealControllerHolder *>(id < kGameLocal ? base : base + kExtGap);
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
    if (input == nullptr) return input;
    for (u32 id = kGameLocal; id < kMaxLocal; ++id) {
        Input::RealControllerHolder &holder = Holder(*input, id);
        holderCtor(&holder);
        reinterpret_cast<u8 *>(&holder)[0x1c] = id;
        reinterpret_cast<u8 *>(holder.ghostWriter)[0x18] = id;
        holder.SetController(&input->dummyController, nullptr);
    }
    return input;
}

// CreateInstance+0x30 replaces "lis r4, -0x7f64", reached with r3 = the manager, or 0 if new failed.
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
        lis r4, -0x7f64;
        blr;)
}
kmCall(0x8052316c, ConstructExtHoldersStub);

// Per frame, after Update's loop over holders 0-3 (bound at 0x80523954), with the same pause flag.
static void UpdateExtHolders(Input::Manager *input) {
    for (u32 id = kGameLocal; id < raceLocalCount; ++id) Holder(*input, id).Update(input->isPaused);
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
    for (u32 id = 0; id < RaceHolderCount(); ++id) startGhostReading(&Holder(*input, id));
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

// Replaces "mulli r0, r0, 0xec" where r0 is a controller id and the result is added to the manager:
// ids 4-7 also step over the AI holders (kExtGap, 0x3da8). Branchless, so CR0 is untouched; r12 is
// dead at every site (InitControllers' VS path, RaceinfoPlayer::__ct's kart holder pointer,
// ComputeGPRank x2), and each function saved LR in its prologue.
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

}  // namespace SplitScreen8
