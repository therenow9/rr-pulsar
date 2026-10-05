#include <kamek.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Screens past the game's 4 (Screens.cpp) where per-screen storage is 4 wide: writes are dropped
// and reads answer "draw". Screen indices of 4+ exist only in a widened race. Culling for those
// screens is not modelled yet (docs/plans/m2-engine-widening.md, phase B).

namespace SplitScreen8 {

// ModelDirector's per-screen visibility bit is 0x10000 << screen in bitfield +4, so screens 4-7
// would land on bits 20 (G3D initialized), 21 (in ScnGroups), 22 and 23 (every ctor sets it). In
// a widened race they use bits 28-31, which nothing in the game reads or writes (D20).
// Otherwise this is slw's result exactly: a 6-bit shift amount, 32-63 giving 0 (hud slot -1).
static u32 ScreenBit(u32 screen) {
    if (raceScreenCount == 0 || screen < kGameLocal) {
        screen &= 0x3f;
        return screen < 32 ? 0x10000u << screen : 0;
    }
    return screen < kMaxLocal ? 0x10000000u << (screen - kGameLocal) : 0;
}

// The five screen-bit leaves (5-7 instructions, no stack frame), replaced whole.
static void EnableScreen(ModelDirector &director, u32 screen) {
    director.bitfield |= ScreenBit(screen);
}
kmBranch(0x8055cce0, EnableScreen);
kmBranch(0x8055cd30, EnableScreen);  // EnableScreen2, the same body

static void DisableScreen(ModelDirector &director, u32 screen) {
    director.bitfield &= ~ScreenBit(screen);
}
kmBranch(0x8055ccf8, DisableScreen);

static bool IsVisibleOnScreen(const ModelDirector &director, u32 screen) {
    return (director.bitfield & ScreenBit(screen)) != 0;
}
kmBranch(0x8055cd10, IsVisibleOnScreen);
kmBranch(0x8055cd48, IsVisibleOnScreen);  // IsVisibleOnScreen2, the same body

// ModelDirector::UpdateVisibility+0x40 replaces "slw r0, r0, r4" (r0 = 0x10000 from the lis before,
// r4 = screen); the next "and r3, r3, r0" tests bitfield +4. r12 is reloaded before it is read; the
// prologue saved LR. 4 is kGameLocal, 8 kMaxLocal; ScreenBit has the rule.
asmFunc UpdateVisibilityScreenBit() {
    ASM(
        nofralloc;
        cmplwi r4, 4;
        blt game;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq game;
        li r0, 0;
        cmplwi r4, 8;
        bgelr;
        lis r0, 0x1000;
        addi r12, r4, -4;
        slw r0, r0, r12;
        blr;
        game :;
        slw r0, r0, r4;
        blr;)
}
kmCall(0x8055cc4c, UpdateVisibilityScreenBit);

// The leaf 0x8055d2f4 (per-screen visibility of general-list models) tests the screen bit inline at
// +0x30 and +0x48 as "lis r0, 1; slw r0, r0, r4; and. r0, r5, r0" (r5 = bitfield +4). No stack
// frame, so branch out and back past the and.; r7 is never read in the leaf. One blr each, for
// kmPatchExitPoint.
asmFunc LeafScreenBitHide() {
    ASM(
        nofralloc;
        lis r0, 1;
        cmplwi r4, 4;
        blt game;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq game;
        li r0, 0;
        cmplwi r4, 8;
        bge test;
        lis r0, 0x1000;
        addi r7, r4, -4;
        slw r0, r0, r7;
        b test;
        game :;
        slw r0, r0, r4;
        test :;
        and.r0, r5, r0;
        blr;)
}
kmBranch(0x8055d324, LeafScreenBitHide);
kmPatchExitPoint(LeafScreenBitHide, 0x8055d330);

asmFunc LeafScreenBitShow() {
    ASM(
        nofralloc;
        lis r0, 1;
        cmplwi r4, 4;
        blt game;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq game;
        li r0, 0;
        cmplwi r4, 8;
        bge test;
        lis r0, 0x1000;
        addi r7, r4, -4;
        slw r0, r0, r7;
        b test;
        game :;
        slw r0, r0, r4;
        test :;
        and.r0, r5, r0;
        blr;)
}
kmBranch(0x8055d33c, LeafScreenBitShow);
kmPatchExitPoint(LeafScreenBitShow, 0x8055d348);

// ClipInfoMgr::Update+0x1C replaces "lwz r25, 0x4bf0(r4)": r25 bounds both its ClipScreenInfo loop
// (4 allocated) and the per-screen bytes at ClipInfo+0x20..0x23, which the next ClipInfo follows.
// r4 is rewritten before it is read again; LR is saved by the prologue. 4 is kGameLocal.
asmFunc ClipScreensToGame() {
    ASM(
        nofralloc;
        lwz r25, 0x4bf0(r4);
        cmpwi r25, 4;
        blelr;
        li r25, 4;
        blr;)
}
kmCall(0x80787790, ClipScreensToGame);

// ModelDirector::SetDisableDrawScnOptionsFromClipInfo+0x54 replaces "lbz r0, 0x20(r5)" (r5 =
// ClipInfo + screen r4): bit 0 set hides the model on that screen. ClipInfoMgr no longer writes
// screens 4+, so they read 0. The next instruction sets CR0 from r0.
asmFunc ClipByteForGameScreens() {
    ASM(
        nofralloc;
        li r0, 0;
        cmplwi r4, 4;
        bgelr;
        lbz r0, 0x20(r5);
        blr;)
}
kmCall(0x8055d5a8, ClipByteForGameScreens);

// Effects::Mgr keeps one Sub9d8 (per-player culling for a screen) per racesScenario.screenCount,
// which stays 4, next to inline per-screen blocks that end where the Sub9d8 pointers begin, so the
// count cannot widen. Screens 4+ read this stand-in: every player near (0) and on screen (1).
struct Sub9d8Visible {
    const u8 *distance;  // 0x0
    void *unknown_0x4;
    void *unknown_0x8;
    const s32 *onScreen;  // 0xC
};
static const u8 visibleDistance[12] = {};
static const s32 visibleOnScreen[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static const Sub9d8Visible sub9d8Visible = {visibleDistance, nullptr, nullptr, visibleOnScreen};

// Effects::Mgr::Draw+0x6B8 replaces "lwzx r5, r18, r3" (r3 = the Sub9d8 array, r18 = screen * 4),
// the per-player loop's only read of it. r3 is rewritten next; the prologue saved LR. 16 is
// kGameLocal * 4.
asmFunc Sub9d8ForGameScreens() {
    ASM(
        nofralloc;
        cmplwi r18, 16;
        bge wide;
        lwzx r5, r18, r3;
        blr;
        wide :;
        lis r5, sub9d8Visible @ha;
        addi r5, r5, sub9d8Visible @l;
        blr;)
}
kmCall(0x8067d62c, Sub9d8ForGameScreens);

}  // namespace SplitScreen8
