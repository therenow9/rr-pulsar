#include <kamek.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <MarioKartWii/3D/Scn/ScnMgr.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

// RaceScene::CreateAndInitInstances+0x310: every item object's models load per screen on the ScnMgr's heap, 2.6 MB of MEM1
// at 8 screens under RR's Royale limits, which leaves Lakitu's none; a 5-8 player race loads them on MEM2 while 2.5 MB is free.
static Item::Manager *CreateItemManager() {
    EGG::ExpHeap *mem2 = RaceMem2(0x280000);
    if (mem2 == nullptr)
        return Item::Manager::CreateInstance();
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    EGG::Heap *oldHeap = scnMgr->curHeap;
    EGG::Allocator *oldAllocator = scnMgr->curAllocator;
    scnMgr->curHeap = mem2;
    scnMgr->curAllocator = scnMgr->mem2Allocator;
    EGG::Heap *oldCurrentHeap = mem2->BecomeCurrentHeap();
    Item::Manager *mgr = Item::Manager::CreateInstance();
    oldCurrentHeap->BecomeCurrentHeap();
    scnMgr->curHeap = oldHeap;
    scnMgr->curAllocator = oldAllocator;
    return mgr;
}
kmCall(0x80554518, CreateItemManager);

// GessoMgr::CreateInstance+0x8 replaces "li r3, 0x54", new's size: gessoScreens[4] at +0x44, indexed by
// every camera kart's screen, becomes 8 wide. r0 holds the caller's LR for the stw after, so stays untouched.
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

// PowMgr::CreateInstance+0x8 replaces "li r3, 0x28": powScreens[4] at +0x18 becomes 8 wide; r0 holds LR as above.
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

// GessoMgr+0x3C and PowMgr+0x4C replace "lbz r0, 0x25(r4)", the stored count bounding every screen loop.
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

// GessoScreen::__ct+0xAC replaces "cmpwi r30, 4", ending the loop that shows the model on its own screen
// only; widened, it runs over every screen, so a screen 4-7 model keeps its own bit. The blt reads CR0; r12 is free.
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

// ObjThunder::ApplyToPlayers+0xFC replaces "lbz r27, 0x25(r3)", the bound of its per-screen white-flash loop.
// r12 is free; the function saved LR.
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
