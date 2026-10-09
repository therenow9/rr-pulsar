#include <kamek.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Page/Menu/MainMenu.hpp>
#include <MarioKartWii/UI/Page/Menu/MultiPlayer.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The way into a 5-8 player game (D63-D65): 5-8 Players buttons beside the main menu's 2-4 Players
// in a widened Multiplayer panel, the join page in place of RegisteredPads in section 0x54, and the
// multiplayer menu without Battle. The main menu changes at every player count (D63); everything
// after it acts only while menuLocalCount is set. docs/plans/m4-menu-flow.md, phase B, has the sites.

namespace SplitScreen8 {

u8 menuLocalCount;

const s32 kEntryButtonId = 8;  // 5-8 Players; the main menu's own ids are 0-7
const u32 kEntryButtons = kMaxLocal - kGameLocal;
// The 5-8 Players button whose game was set up last, for the focus when the main menu comes back.
static u32 lastEntryCount;
static PushButton *entryButtons[kEntryButtons];

typedef void (*PushButtonCtorFn)(PushButton *);
typedef void (*CropMovieFn)(LayoutUIControl *, const char *, float, float, float, float);
typedef void (*SoundIdFn)(PushButton *, u32);
typedef void (*ResetParamsFn)(Pages::MainMenu *, u8);
typedef void (*PositionFromBaseFn)(UIControl *);
typedef void (*SelectButtonFn)(Pages::MainMenu *, PushButton *);
static const PushButtonCtorFn pushButtonCtor = reinterpret_cast<PushButtonCtorFn>(0x805bd3a8);
static const CropMovieFn cropMovie = reinterpret_cast<CropMovieFn>(0x8063e5c4);
static const SoundIdFn setSelectionSoundId = reinterpret_cast<SoundIdFn>(0x805be430);
static const ResetParamsFn resetSectionParams = reinterpret_cast<ResetParamsFn>(0x808516bc);
static const PositionFromBaseFn positionFromBase = reinterpret_cast<PositionFromBaseFn>(0x8063d3cc);

// D63: the panel 1.50x wider about its centre; seven buttons share its row. Vanilla's buttons sit at
// x -54, 0, 54 (16:9, scale 1.05 x 0.93, TopMenuMulti.brctr), about 50.6 units wide with 3.4 between.
const float kPanelScale = 1.5f;
const float kButtonWidth = 50.6f;
const float kButtonGap = 3.4f;
const float kButtonScaleX = 1.05f;
const u32 kRowButtons = 3 + kEntryButtons;

// A CtrlMenuMovieButton built as MainMenu::CreateControl builds its own (0x80850750): handler part
// first, then the PushButton at +0x18, then both vtables.
static PushButton *NewMovieButton() {
    u8 *raw = static_cast<u8 *>(::operator new(0x26c));
    *reinterpret_cast<u32 *>(raw) = 0x808d3608;
    raw[0xc] = 0;
    *reinterpret_cast<u32 *>(raw + 0x10) = 0;
    pushButtonCtor(reinterpret_cast<PushButton *>(raw + 0x18));
    *reinterpret_cast<u32 *>(raw) = 0x808d3590;
    *reinterpret_cast<u32 *>(raw + 0x18) = 0x808d35a8;
    return reinterpret_cast<PushButton *>(raw + 0x18);
}

// The 2-4 Players label, "N\n{font scale|50}Players", as BMG_TEXT's string argument: the text is drawn
// from it every frame (the panes' buffers hold only a 0xFF13 marker), and the tag processor reads the
// font-scale escape (0x1A, size 8, type 0, 50%) whole, so its zero word does not end the string.
static wchar_t buttonLabels[kEntryButtons][14];

static void SetButtonDigit(PushButton &button, wchar_t digit) {
    wchar_t *label = buttonLabels[digit - L'5'];
    const wchar_t text[] = {digit, L'\n', 0x1a, 0x0800, 0x0000, 50, L'P', L'l', L'a', L'y', L'e', L'r', L's', 0};
    for (u32 i = 0; i < sizeof(text) / sizeof(text[0]); ++i) label[i] = text[i];
    Text::Info info;
    info.strings[0] = label;
    button.SetMessage(Pulsar::UI::BMG_TEXT, &info);
}

static void PlaceInRow(PushButton &button, u32 index) {
    const float row = (2 * 54.0f + kButtonWidth) * kPanelScale;
    const float width = (row - (kRowButtons - 1) * kButtonGap) / kRowButtons;
    PositionAndScale &base = button.positionAndscale[0];
    base.position.x = (static_cast<float>(index) - (kRowButtons - 1) / 2.0f) * (width + kButtonGap);
    base.scale.x = kButtonScaleX * width / kButtonWidth;
    positionFromBase(&button);
}

// TopMenuMultiWaku is all picture panes but its heading; widening the pictures keeps the text's size.
static void WidenPanel(LayoutUIControl &panel) {
    static const char *const panes[] = {"touch", "black", "borderline", "waku_sha", "waku_co",
                                        "Picture_00", "waku_light", "Picture_02", "Picture_01"};
    for (u32 i = 0; i < sizeof(panes) / sizeof(panes[0]); ++i) {
        nw4r::lyt::Pane *pane = panel.layout.GetPaneByName(panes[i]);
        if (pane != nullptr) pane->size.x *= kPanelScale;
    }
}

// After CreateControl has built the Multiplayer panel's three buttons (InitControlGroup now sizes it
// for seven): four more as the 4 Players button is built (0x80850A2C), then all seven placed.
static void AddEntryButtons(Pages::MainMenu *page, LayoutUIControl *panel) {
    for (u32 i = 0; i < kEntryButtons; ++i) {
        PushButton *button = NewMovieButton();
        panel->AddControl(3 + i, button);
        button->Load("button", "TopMenuMulti", "ButtonMulti4P", 1, 0, false);
        static_cast<Pages::MenuInteractable *>(page)->SetButtonHandlers(*button);
        button->buttonId = kEntryButtonId + i;
        cropMovie(button, "black_base", 0.0f, 0.5f, 0.8333333f, 1.0f);
        entryButtons[i] = button;
    }
    // MainMenu.hpp keeps them private: topMenuMulti2P, 3P and 4P at +0xC98, +0xC9C and +0xCA0.
    PushButton *const *multi = reinterpret_cast<PushButton *const *>(reinterpret_cast<u8 *>(page) + 0xc98);
    for (u32 i = 0; i < 3; ++i) PlaceInRow(*multi[i], i);
    for (u32 i = 0; i < kEntryButtons; ++i) PlaceInRow(*entryButtons[i], 3 + i);
    WidenPanel(*panel);
}

// CreateControl's Multiplayer case ends "b 0x80850D2C" after its third button (r28 = the page,
// r29 = the panel); the shared exit reads only r1 and the saved registers.
asmFunc AddEntryButtonsStub() {
    ASM(
        nofralloc;
        mr r3, r28;
        mr r4, r29;
        bl AddEntryButtons;
        lis r12, 0x8085;
        ori r12, r12, 0x0d2c;
        mtctr r12;
        bctr;)
}
kmWrite32(0x80850744, 0x38800007);  // the Multiplayer panel's InitControlGroup: li r4, 7 (was 3)
kmBranch(0x80850a28, AddEntryButtonsStub);

// As the 2-4 Players buttons click (0x808511C0), with the game's count kept at 4 (D60).
static void EntryClick(Pages::MainMenu *page, PushButton *button) {
    const u32 count = kGameLocal + 1 + (button->buttonId - kEntryButtonId);
    SectionMgr::sInstance->sectionParams->category = 3;  // the 4 Players button's
    lastEntryCount = count;
    resetSectionParams(page, kGameLocal);
    menuLocalCount = count;
    page->ChangeSectionById(SECTION_LOCAL_MULTIPLAYER, *button);
    setSelectionSoundId(button, 0xd0);
}

static void EntrySelect(Pages::MainMenu *page, PushButton *button) {
    // English only: Retro Rewind's text stops at "Race and battle with four players!".
    static const wchar_t *const lines[kEntryButtons] = {
        L"Race with five players!",
        L"Race with six players!",
        L"Race with seven players!",
        L"Race with eight players!",
    };
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(lines[button->buttonId - kEntryButtonId]);
    page->bottomText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
}

// OnButtonClick and OnExternalButtonSelect (r3 = the page, r4 = the button) handle ids 0-7; the new
// ones go to EntryClick and EntrySelect, which return to the caller, and the rest replay "stwu".
asmFunc EntryClickStub() {
    ASM(
        nofralloc;
        lwz r12, 0x240(r4);
        cmpwi r12, 8;
        blt game;
        cmpwi r12, 11;
        bgt game;
        b EntryClick;
        game :;
        stwu r1, -0x20(r1);
        lis r12, 0x8085;
        ori r12, r12, 0x1110;
        mtctr r12;
        bctr;)
}
kmBranch(0x8085110c, EntryClickStub);

asmFunc EntrySelectStub() {
    ASM(
        nofralloc;
        lwz r12, 0x240(r4);
        cmpwi r12, 8;
        blt game;
        cmpwi r12, 11;
        bgt game;
        b EntrySelect;
        game :;
        stwu r1, -0x10(r1);
        lis r12, 0x8085;
        ori r12, r12, 0x1520;
        mtctr r12;
        bctr;)
}
kmBranch(0x8085151c, EntrySelectStub);

// OnButtonDeselect plays the panel's deselect only for the panel's own ids: the new buttons are
// deselected as the 4 Players button (+0xCA0) is.
asmFunc EntryDeselectStub() {
    ASM(
        nofralloc;
        lwz r12, 0x240(r4);
        cmpwi r12, 8;
        blt game;
        cmpwi r12, 11;
        bgt game;
        lwz r4, 0xca0(r3);
        game :;
        stwu r1, -0x20(r1);
        lis r12, 0x8085;
        ori r12, r12, 0x15a4;
        mtctr r12;
        bctr;)
}
kmBranch(0x808515a0, EntryDeselectStub);

// Every game starts at the main menu, so a 5-8 player game's state ends there: the count, P5-8's pads,
// and holders 4-7 back on the dummy controller (TrySetController's scans read them, Input.cpp). The
// focus goes back to the button last clicked, as OnActivate does for ids 0-7 (vtable 0x78).
static void EntryActivate(Pages::MainMenu *page) {
    menuLocalCount = 0;
    ClearExtPads();
    for (u32 i = 0; i < kMaxLocal - kGameLocal; ++i) extPicks[i].picked = false;
    // The buttons' text is laid out after CreateControl, so the digits are set here.
    for (u32 i = 0; i < kEntryButtons; ++i) {
        if (entryButtons[i] != nullptr) SetButtonDigit(*entryButtons[i], L'5' + i);
    }
    if (lastEntryCount <= kGameLocal || SectionMgr::sInstance->sectionParams->category != 3) return;
    PushButton *button = entryButtons[lastEntryCount - kGameLocal - 1];
    lastEntryCount = 0;
    if (button == nullptr) return;
    const SelectButtonFn select = reinterpret_cast<SelectButtonFn>((*reinterpret_cast<void ***>(page))[0x78 / 4]);
    select(page, button);
}

// OnActivate's shared exit, "lwz r0, 0x24(r1)" (r30 = the page); the prologue saved LR.
asmFunc EntryActivateStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r30;
        bl EntryActivate;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lwz r0, 0x24(r1);
        blr;)
}
kmCall(0x808510f0, EntryActivateStub);

static_assert(Pulsar::UI::PULPAGE_SS8JOIN == 0x10f, "RegisteredPadsOrJoin loads the join page's id as 0x10f");

// RegisteredPads (0x61), or the join page in a 5-8 player game: section 0x54's first page
// (AddInitialLayers, 0x80630984) and the multiplayer menu's Back (button 0x8084D52C, B 0x8084DB8C).
// Each replaces "li r4, 0x61" and leaves r0, r3 and r5 alone (0x8084DB8C sits before "stw r0").
asmFunc RegisteredPadsOrJoin() {
    ASM(
        nofralloc;
        lis r12, menuLocalCount @ha;
        lbz r12, menuLocalCount @l(r12);
        li r4, 0x61;
        cmplwi r12, 4;
        blelr;
        li r4, 0x10f;
        blr;)
}
kmCall(0x80630984, RegisteredPadsOrJoin);
kmCall(0x8084d52c, RegisteredPadsOrJoin);
kmCall(0x8084db8c, RegisteredPadsOrJoin);

// D65: in a 5-8 player game the multiplayer menu hides Battle and titles itself with the real count;
// its OnInit took "Multiplayer (4P)" from the game's count (titleBmg = 0x7EC + count).
// Section 0x54 is built again for every game, so a page that hid Battle never serves a 1-4 player one.
static void MultiActivate(Pages::MultiPlayer *page) {
    if (menuLocalCount <= kGameLocal) return;
    for (int i = 0; i < page->externControlCount; ++i) {
        PushButton *button = page->externControls[i];
        if (button == nullptr || button->buttonId != 1) continue;  // Battle
        button->isHidden = true;
        button->manipulator.inaccessible = true;
    }
    if (page->titleText == nullptr) return;
    static const wchar_t *const titles[kEntryButtons] = {
        L"Multiplayer (5P)",
        L"Multiplayer (6P)",
        L"Multiplayer (7P)",
        L"Multiplayer (8P)",
    };
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(titles[menuLocalCount - kGameLocal - 1]);
    page->titleText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
}

// MultiPlayer::OnActivate after Menu::OnActivate set the title, "lis r5, -0x7f75" (r30 = the page).
asmFunc MultiActivateStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r30;
        bl MultiActivate;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lis r5, -0x7f75;
        blr;)
}
kmCall(0x8084d3e0, MultiActivateStub);

ExtPick extPicks[kMaxLocal - kGameLocal];

// The menus' pages set every player's type from the game's count, 4 (VSModeSelect::OnActivate,
// CharacterSelect::OnActivate, MultiPlayer's VS click), so players 5..N are made local where each race
// takes the menu scenario: InitRace, before RacedataScenario::Init counts its locals. The scenario's
// character and kart for 5..N are rewritten there too: character select's CPU fill (0x8083EC28) and
// kart select's (0x8084745C) give every slot from 4 a random one. With no pick, D62's default:
// character i on the Standard Kart of its weight class, as the debug boot picks it.
static void MenuLocalsLocal(Racedata *racedata) {
    if (menuLocalCount <= kGameLocal) return;
    for (u32 i = kGameLocal; i < menuLocalCount; ++i) {
        RacedataPlayer &player = racedata->menusScenario.players[i];
        if (player.playerType == PLAYER_REAL_LOCAL) continue;
        const ExtPick &pick = extPicks[i - kGameLocal];
        const CharacterId character = pick.picked ? pick.character : static_cast<CharacterId>(i);
        player.playerType = PLAYER_REAL_LOCAL;
        player.characterId = character;
        player.kartId = pick.picked ? pick.kart : static_cast<KartId>(GetCharacterWeightClass(character));
    }
}

// InitRace+0x14 replaces "addi r3, r3, 0xc10" (r3 = r31 = the racedata); the prologue saved LR and
// r31, and r3 is set again from r31 after the call. RaceScene::OnEnter and OnReinit, and InitAwards,
// reach it.
asmFunc MenuLocalsLocalStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r31;
        bl MenuLocalsLocal;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        addi r3, r31, 0xc10;
        blr;)
}
kmCall(0x805302d8, MenuLocalsLocalStub);

}  // namespace SplitScreen8
