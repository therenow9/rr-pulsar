#include <kamek.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Screens past the game's 4 (Screens.cpp) where per-screen storage is 4 wide: writes are dropped
// and reads answer "draw". Screen indices of 4+ exist only in a widened race. Culling and model
// visibility for those screens are not modelled yet (docs/plans/m2-engine-widening.md, phase B).

namespace SplitScreen8 {

// ModelDirector::EnableScreen2: screen bits are 0x10000 << screen, so screens 4-7 would set bit 20
// (G3D initialized), 21 (inserted into ScnGroups), 22 and 23. Kart::Part's model setup calls it for
// every screen of the static count.
static void EnableScreen2(ModelDirector &director, u32 screen) {
    if (screen < kGameLocal) director.bitfield |= 0x10000 << screen;
}
kmBranch(0x8055cd30, EnableScreen2);

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

}  // namespace SplitScreen8
