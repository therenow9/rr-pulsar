#include <kamek.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Per-screen item effects on screens 4-7 (D38, D39). The Blooper and POW managers keep one screen
// object per racesScenario.screenCount (4, D10) in their last field, and ApplyBlooper indexes it by
// the screen of every kart with a camera, so a widened race allocates both 8 wide with the wide count.
// The lightning flash walks the wide count too.

namespace SplitScreen8 {

// Item::GessoMgr::CreateInstance+0x8 replaces "li r3, 0x54", the size passed to new: gessoScreens[4]
// at +0x44 becomes 8 wide. r0 holds the caller's LR for the stw after this, so r0 stays untouched; CR0
// is set again after new.
asmFunc GessoMgrSize() {
    ASM(
        nofralloc;
        lis r3, raceScreenCount @ha;
        lbz r3, raceScreenCount @l(r3);
        cmpwi r3, 0;
        li r3, 0x54;
        beqlr;
        li r3, 0x64;
        blr;)
}
kmCall(0x807a8f0c, GessoMgrSize);

// Item::PowMgr::CreateInstance+0x8 replaces "li r3, 0x28": powScreens[4] at +0x18 becomes 8 wide. As
// above, r0 holds the caller's LR.
asmFunc PowMgrSize() {
    ASM(
        nofralloc;
        lis r3, raceScreenCount @ha;
        lbz r3, raceScreenCount @l(r3);
        cmpwi r3, 0;
        li r3, 0x28;
        beqlr;
        li r3, 0x38;
        blr;)
}
kmCall(0x807b1bc8, PowMgrSize);

// GessoMgr::CreateInstance+0x3C and PowMgr::CreateInstance+0x4C replace "lbz r0, 0x25(r4)", the
// screen count stored as the manager's count (+0 and +4), which bounds every loop over its screens.
// r12 is free; both functions saved LR.
asmFunc ItemScreenCount() {
    ASM(
        nofralloc;
        lbz r0, 0x25(r4);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r0, r12;
        blr;)
}
kmCall(0x807a8f40, ItemScreenCount);
kmCall(0x807b1c0c, ItemScreenCount);

// GessoScreen::__ct+0xAC replaces "cmpwi r30, 4", the end of its loop that shows the model on its own
// screen and hides it on the others. A widened race walks every screen, so a screen 4-7 model shows
// on its own tile (D20's bits); otherwise ScreenBit's vanilla path would clear bits 20-21 for 4-7.
// The blt after it reads CR0; r12 is free and the ctor saved LR.
asmFunc GessoScreenLoopEnd() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        bne wide;
        li r12, 4;
        wide :;
        cmpw r30, r12;
        blr;)
}
kmCall(0x807a850c, GessoScreenLoopEnd);

// PowScreen::__ct+0x9C, the same loop with its index in r21.
asmFunc PowScreenLoopEnd() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        bne wide;
        li r12, 4;
        wide :;
        cmpw r21, r12;
        blr;)
}
kmCall(0x807b25dc, PowScreenLoopEnd);

// ObjThunder::ApplyToPlayers+0xFC replaces "lbz r27, 0x25(r3)", the bound of its loop requesting the
// white flash on each screen; the course filters are already one per screen of the static count. r12
// is free; the function saved LR.
asmFunc ThunderFlashScreens() {
    ASM(
        nofralloc;
        lbz r27, 0x25(r3);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r27, r12;
        blr;)
}
kmCall(0x807b7d1c, ThunderFlashScreens);

}  // namespace SplitScreen8
