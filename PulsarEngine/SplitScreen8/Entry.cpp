#include <kamek.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/Scene/GameScene.hpp>
#include <MarioKartWii/UI/Page/Menu/MainMenu.hpp>
#include <MarioKartWii/UI/Page/Menu/MultiPlayer.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The main menu's added buttons show at every player count; everything after them acts only while menuLocalCount is set.

namespace SplitScreen8 {

u8 menuLocalCount;

const s32 kEntryButtonId = 8;  // 5-8 Players; the main menu's own ids are 0-7
const u32 kEntryButtons = kMaxLocal - kGameLocal;
const s32 kFourPlayersBmg = 2304;  // ButtonMulti4P's "4\nPlayers"
// The 5-8 Players button last clicked, for the focus when the main menu comes back.
static u32 lastEntryCount;
static PushButton *entryButtons[kEntryButtons];

typedef void (*PushButtonCtorFn)(PushButton *);
typedef void (*CropMovieFn)(LayoutUIControl *, const char *, float, float, float, float);
typedef void (*SoundIdFn)(PushButton *, u32);
typedef void (*ResetParamsFn)(Pages::MainMenu *, u8);
typedef void (*SelectButtonFn)(Pages::MainMenu *, PushButton *);
static const PushButtonCtorFn pushButtonCtor = reinterpret_cast<PushButtonCtorFn>(0x805bd3a8);
static const CropMovieFn cropMovie = reinterpret_cast<CropMovieFn>(0x8063e5c4);
static const SoundIdFn setSelectionSoundId = reinterpret_cast<SoundIdFn>(0x805be430);
static const ResetParamsFn resetSectionParams = reinterpret_cast<ResetParamsFn>(0x808516bc);

// Vanilla's three buttons sit at x -54, 0, 54 (16:9, scale 1.05 x 0.93), about 50.6 units wide with 3.4 between.
const float kPanelScale = 1.5f;
const float kButtonWidth = 50.6f;
const float kButtonGap = 3.4f;
const float kButtonScaleX = 1.05f;
const u32 kRowButtons = 3 + kEntryButtons;
// The multiplayer clip is top_menu.thp's top-right quadrant: vanilla splits it across three buttons, this across seven.
const float kMovieStrip = 0.5f / kRowButtons;

// A CtrlMenuMovieButton built as CreateControl builds its own (0x80850750): handler, PushButton at +0x18, both vtables.
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

// BMG_TEXT's string is redrawn every frame, so it must outlive the call. It is the 4 Players button's own
// message with its digit swapped, so the font-scale escape is the one the loaded BMG gives 2-4 Players.
static wchar_t buttonLabels[kEntryButtons][32];

static void SetButtonDigit(PushButton &button, wchar_t digit) {
    const BMGHolder &bmg = button.curFileBmgs;
    const wchar_t *text = bmg.GetMsgByMsgId(bmg.GetMsgId(kFourPlayersBmg));
    if (text == nullptr)
        return;
    wchar_t *label = buttonLabels[digit - L'5'];
    const u32 max = sizeof(buttonLabels[0]) / sizeof(buttonLabels[0][0]) - 1;
    u32 i = 0;
    // An escape is copied whole: its size in bytes is its second unit's high byte, and it may hold a zero word.
    for (u32 escapeEnd = 0; i < max && (text[i] != 0 || i < escapeEnd); ++i) {
        if (i >= escapeEnd && text[i] == 0x1a)
            escapeEnd = i + (text[i + 1] >> 9);
        label[i] = i >= escapeEnd && text[i] == L'4' ? digit : text[i];
    }
    label[i] = 0;
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
    button.PositionFromBasePosition();
    cropMovie(&button, "black_base", 0.0f, 0.5f, 0.5f + index * kMovieStrip, 0.5f + (index + 1) * kMovieStrip);
}

// TopMenuMultiWaku is all picture panes but its heading; widening the pictures keeps the text's size.
static void WidenPanel(LayoutUIControl &panel) {
    static const char *const panes[] = {"touch", "black", "borderline", "waku_sha", "waku_co", "Picture_00", "waku_light", "Picture_02", "Picture_01"};
    for (u32 i = 0; i < sizeof(panes) / sizeof(panes[0]); ++i) {
        nw4r::lyt::Pane *pane = panel.layout.GetPaneByName(panes[i]);
        if (pane != nullptr)
            pane->size.x *= kPanelScale;
    }
}

// Four more buttons built as the 4 Players one is (0x80850A2C), then all seven placed in the widened row.
static void AddEntryButtons(Pages::MainMenu *page, LayoutUIControl *panel) {
    for (u32 i = 0; i < kEntryButtons; ++i) {
        PushButton *button = NewMovieButton();
        panel->AddControl(3 + i, button);
        button->Load("button", "TopMenuMulti", "ButtonMulti4P", 1, 0, false);
        static_cast<Pages::MenuInteractable *>(page)->SetButtonHandlers(*button);
        button->buttonId = kEntryButtonId + i;
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
kmWrite32(0x80850744, 0x38800007);  // the Multiplayer panel's InitControlGroup: li r4, 7 for vanilla's 3
kmBranch(0x80850a28, AddEntryButtonsStub);

// As the 2-4 Players buttons click (0x808511C0), with the game's count kept at 4.
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

// Every game starts here, so a 5-8 player game's state ends here, holders 4-7 on the dummy that TrySetController's
// scans read; focus returns to the button last clicked, as OnActivate does for ids 0-7 (vtable 0x78).
static void EntryActivate(Pages::MainMenu *page) {
    menuLocalCount = 0;
    ClearExtPads();
    for (u32 i = 0; i < kMaxLocal - kGameLocal; ++i) extPicks[i].picked = false;
    // The buttons' text is laid out after CreateControl, so the digits are set here.
    for (u32 i = 0; i < kEntryButtons; ++i) {
        if (entryButtons[i] != nullptr)
            SetButtonDigit(*entryButtons[i], L'5' + i);
    }
    if (lastEntryCount <= kGameLocal || SectionMgr::sInstance->sectionParams->category != 3)
        return;
    PushButton *button = entryButtons[lastEntryCount - kGameLocal - 1];
    lastEntryCount = 0;
    if (button == nullptr)
        return;
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

static_assert(Pulsar::UI::PULPAGE_SS8JOIN == 0x115, "RegisteredPadsOrJoin loads the join page's id as 0x115");

// Each replaces "li r4, 0x61" (RegisteredPads) at section 0x54's first page and the multiplayer menu's Back button
// and B; r0, r3 and r5 stay live (0x8084DB8C sits before "stw r0").
asmFunc RegisteredPadsOrJoin() {
    ASM(
        nofralloc;
        lis r12, menuLocalCount @ha;
        lbz r12, menuLocalCount @l(r12);
        li r4, 0x61;
        cmplwi r12, 4;
        blelr;
        li r4, 0x115;
        blr;)
}
kmCall(0x80630984, RegisteredPadsOrJoin);
kmCall(0x8084d52c, RegisteredPadsOrJoin);
kmCall(0x8084db8c, RegisteredPadsOrJoin);

// OnInit titles the page from the game's count of 4 (0x7EC + count). Section 0x54 is built again for every game,
// so a page that hid Battle never serves a 1-4 player one.
static void MultiActivate(Pages::MultiPlayer *page) {
    if (menuLocalCount <= kGameLocal)
        return;
    for (int i = 0; i < page->externControlCount; ++i) {
        PushButton *button = page->externControls[i];
        if (button == nullptr || button->buttonId != 1)
            continue;  // Battle
        button->isHidden = true;
        button->manipulator.inaccessible = true;
    }
    if (page->titleText == nullptr)
        return;
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

CharacterId ExtCharacter(u32 player) {
    const ExtPick &pick = extPicks[player - kGameLocal];
    return pick.picked ? pick.character : static_cast<CharacterId>(player);
}

// A Mii's id is past selectedSlots' row, so it reads 0 and writes nothing.
u32 SkinSlot(u32 hud, u32 character) {
    return hud < kMaxLocal && character < Pulsar::Driver::CHARACTER_COUNT ? Pulsar::Driver::selectedSlots[hud][character] : 0;
}

void SetSkinSlot(u32 hud, u32 character, u32 slot) {
    if (hud < kMaxLocal && character < Pulsar::Driver::CHARACTER_COUNT)
        Pulsar::Driver::selectedSlots[hud][character] = slot;
}

#ifndef SS8_PAGES
void ResetExtPreviews() {
}
#endif

// A 5-8 player section 0x54 has ~1.2 MB less MEM1 free, too little for RR's 0xE1000 per skin load. A load keeps its file
// plus at most 51,628 bytes (Rosalina's 6th skin, 244,948); a model that outgrows the reserve faults inside game code.
const u32 kSkinLoadReserve = 0x60000;

u32 SkinLoadReserve(u32 reserve) {
    return menuLocalCount > kGameLocal ? kSkinLoadReserve : reserve;
}

// The menus CPU-fill slots 4+: players 5..N are made local here, with their pick or default, before RacedataScenario::Init
// counts locals. Not for the awards, whose InitAwards has already given every local a slot, in finish order.
static void MenuLocalsLocal(Racedata *racedata) {
    if (menuLocalCount <= kGameLocal || racedata->menusScenario.settings.gamemode == MODE_AWARD)
        return;
    for (u32 i = kGameLocal; i < menuLocalCount; ++i) {
        RacedataPlayer &player = racedata->menusScenario.players[i];
        if (player.playerType == PLAYER_REAL_LOCAL)
            continue;
        const ExtPick &pick = extPicks[i - kGameLocal];
        const CharacterId character = ExtCharacter(i);
        player.playerType = PLAYER_REAL_LOCAL;
        player.characterId = character;
        player.kartId = pick.picked ? pick.kart : static_cast<KartId>(GetCharacterWeightClass(character));
    }
}

// InitRace+0x14 replaces "addi r3, r3, 0xc10" (r3 = r31 = the racedata); the prologue saved LR and r31,
// and r3 is set again from r31 after the call.
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
