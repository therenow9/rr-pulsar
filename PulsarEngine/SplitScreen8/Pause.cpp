#include <kamek.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/UI/Page/RaceHUD/RaceHUD.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Pause by players 5-8. Every page's input manager has 5 slots (0-3 and the dummy 4), so P5-8 pause
// through slot 4: their START press calls RaceHUD::OnPause with slot 4, as the page's own START
// handler calls it with 0-3, and while that pause is up SectionPad::GetControllerHolder answers slot 4
// with the pausing player's holder. The pause menu takes its mask (1 << slot, 0x80859B9C) and its
// initial selection from that slot. docs/plans/m4-menu-flow.md, phase A, has the sites.

namespace SplitScreen8 {

// The holder of the player 5-8 whose pause is up, or null.
static Input::RealControllerHolder *pauseProxy;

// OnPause's own test for a pause already up (0x808569F0).
static bool PauseUp(const Pages::RaceHUD &page) {
    const Page *pause = reinterpret_cast<const Page *>(page.pausePage);
    return pause != nullptr && pause->currentState >= STATE_ACTIVATING;
}

static bool StartPressed(const Input::RealControllerHolder &holder) {
    return (holder.uiinputStates[0].buttonActions & 4) != 0 && (holder.uiinputStates[1].buttonActions & 4) == 0;
}

// After the RaceHUD's own START check, each frame its input manager is checked: the proxy is dropped
// once no pause is up, and a new START from a player 5-8 opens one.
static void CheckExtPause(PageManipulatorManager *manager) {
    Pages::RaceHUD *page = Pages::RaceHUD::sInstance;
    if (page == nullptr || manager != &page->manipulatorManager || PauseUp(*page)) return;
    pauseProxy = nullptr;
    if (raceScreenCount == 0 || raceLocalCount <= kGameLocal || manager->inaccessible) return;
    Input::Manager *input = Input::Manager::sInstance;
    for (u32 id = kGameLocal; id < raceLocalCount; ++id) {
        Input::RealControllerHolder &holder = Holder(*input, id);
        if (!StartPressed(holder)) continue;
        pauseProxy = &holder;
        page->OnPause(kGameLocal);
        if (!PauseUp(*page)) pauseProxy = nullptr;
        return;
    }
}

// PageManipulatorManager::CheckActions+0x158 replaces "lmw r24, 0x10(r1)", where its early exit and its
// loop end meet (r26 = the manager). The lmw runs after this frame is popped; the prologue saved LR.
asmFunc CheckExtPauseStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r26;
        bl CheckExtPause;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lmw r24, 0x10(r1);
        blr;)
}
kmCall(0x805ef594, CheckExtPauseStub);

// SectionPad::GetControllerHolder (r3 = the pad, r4 = the slot), a leaf: slot 4 answers the proxy while
// it is set, returning through ReturnProxy (kmPatchExitPoint takes a function with one return);
// anything else replays the replaced "slwi r0, r4, 4" and goes back. r12 is volatile at every call.
asmFunc ReturnProxy() {
    ASM(
        nofralloc;
        mr r3, r12;
        blr;)
}

asmFunc ControllerHolderProxy() {
    ASM(
        nofralloc;
        cmpwi r4, 4;
        bne game;
        lis r12, pauseProxy @ha;
        lwz r12, pauseProxy @l(r12);
        cmpwi r12, 0;
        beq game;
        b ReturnProxy;
        game :;
        slwi r0, r4, 4;
        blr;)
}
kmBranch(0x8061b398, ControllerHolderProxy);
kmPatchExitPoint(ControllerHolderProxy, 0x8061b39c);

// ManipulatorManager::Update and ControlsManipulatorManager::Update ask every slot 0-4 with no mask
// (0x805EEDC0, 0x805F1F7C, 0x805F1FE8), so a proxy left by a Quit would reach the menus: it goes at
// every race load (a Restart) and every section load (a Quit).
static void ClearPauseProxy() { pauseProxy = nullptr; }
static RaceLoadHook clearPauseProxyOnRace(ClearPauseProxy);
static SectionLoadHook clearPauseProxyOnSection(ClearPauseProxy);

}  // namespace SplitScreen8
