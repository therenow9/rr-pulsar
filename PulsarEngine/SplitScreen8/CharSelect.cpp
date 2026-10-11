#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/WPAD.hpp>
#include <core/nw4r/snd/BasicSound.hpp>
#include <core/nw4r/snd/SoundHandle.hpp>
#include <MarioKartWii/Audio/RSARPlayer.hpp>
#include <MarioKartWii/Input/Controller.hpp>
#include <MarioKartWii/Input/ControllerHolder.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/RKSYS/RKSYSMgr.hpp>
#include <MarioKartWii/System/Random.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Menu/KartSelect.hpp>
#include <MarioKartWii/UI/Page/Other/ModelRenderer.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuCharacterSelect.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Network/Rating/PlayerRating.hpp>
#include <Network/Settings/SelectionRestrictions.hpp>
#include <Race/CustomCharacters.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The character page of a 5-8 player game (D67, D71): all players pick at once on the vanilla driver grid.
// The vanilla page cannot serve 8: its buttons are PushButtons, which need the page's 5-slot
// ControlsManipulatorManager, and CtrlMenuCharacterSelect calls back into RR's real 0x6B page and keeps
// 4-wide state. So each button is a plain LayoutUIControl and this page moves the cursors, colours them
// (D79), shows the OK markers (D80) and writes the picks itself. docs/plans/m4-menu-flow.md, phase C, C4a.

namespace SplitScreen8 {

typedef void (*PositionFromBaseFn)(UIControl *);
typedef void (*CopyMiisFn)(SectionParams *);
typedef bool (*ConfirmVoiceFn)(void *, u32, CharacterId);
typedef void (*FillCpusFn)();
typedef void (*ControlFn)(LayoutUIControl *);
typedef void (*DummyBackLoadFn)(LayoutUIControl *, const char *, const char *, const char *, u32);
static const PositionFromBaseFn positionFromBase = reinterpret_cast<PositionFromBaseFn>(0x8063d3cc);
static const CopyMiisFn copyMiisToRacedata = reinterpret_cast<CopyMiisFn>(0x805e39d8);  // SectionParams::CopyMiisToRacedata
// ButtonDriver::OnButtonClick's confirm voice (0x807E36D0); it returns at once for hud 4+ (0x80868974),
// whose driver path (0x80868ABC) keeps one sound handle per hud at DriverSoundManager+0x2430.
static const ConfirmVoiceFn confirmVoice = reinterpret_cast<ConfirmVoiceFn>(0x80868974);
static u8 *const *const confirmVoiceOwner = reinterpret_cast<u8 *const *>(0x809c4740);
typedef void *(*StartVoiceFn)(void *, nw4r::snd::SoundHandle *);
const u32 kVoiceSetStride = 0xc0;  // one character's voice pickers
const u32 kVoicePicker = 0x30;  // the confirm voice's picker in its set
const float kVoiceVolume = 0.85f;  // 0x808B1FC4
static nw4r::snd::SoundHandle extVoices[kMaxLocal - kGameLocal];
// Character select's last confirm gives every CPU from GetLocalPlayerCount (read once, 0x8083EC38) a
// random character no earlier slot has (0x8083EC28); it takes no argument and keeps the slots below the
// count in a 14-word array (0x8083EC80).
static const FillCpusFn fillCpuCharacters = reinterpret_cast<FillCpusFn>(0x8083ec28);
static const ControlFn layoutCtor = reinterpret_cast<ControlFn>(0x8063d798);
static const DummyBackLoadFn dummyBackLoad = reinterpret_cast<DummyBackLoadFn>(0x807e9620);
typedef void (*ShowModelFn)(Pages::ModelRenderer *, u32, bool, u32);
typedef GX::TexObj *(*ModelTexObjFn)(Pages::ModelRenderer *, u32);
typedef void (*TexMapSetFn)(nw4r::lyt::TexMap *, const GX::TexObj *);
// DriverModelControl::OnUpdate's per-frame calls (0x805F3618, 0x805F3624): the first ends in
// MenuDriverModelMgr::TogglePlayerModel, which writes players[hud] (4 wide), so P1-4 only.
static const ShowModelFn showModel = reinterpret_cast<ShowModelFn>(0x805f59ac);
static const ModelTexObjFn modelTexObj = reinterpret_cast<ModelTexObjFn>(0x805f5a4c);
static const TexMapSetFn texMapSet = reinterpret_cast<TexMapSetFn>(0x80082400);

const u32 kTitleBmg = 0xd4a;  // CharacterSelect's titleBmg (0x80626C90)
const u32 kDummyBackSize = 0x184;
const u32 kDriverButtons = 24;  // CtrlMenuCharacterSelect::buttonIdToCharacterId
// Then Mii A, B and C, as RR builds the vanilla page (li r30, 27 at 0x807E2714). Their button ids are
// GetCorrectButtonDriverId's 0x1E + 2 * outfit (0x807E3DBC); the character a player gets is their Mii's.
const u32 kButtons = kDriverButtons + 3;
// LoadButton names a button's control "CharacterSelect%d_%d" from categoryCount (+0x1A8, 4) and
// idx / (2 * categoryCount) (0x807E29B8-0x807E29CC), so each control holds Button0-7, and the Mii row is
// the fourth control's Button0-2.
const u32 kButtonsPerGroup = 8;
const u32 kMiiOutfits = 3;  // A, B and C, two ids apart (MII_S_A_MALE ... MII_S_C_FEMALE)
const float kGridShift = 80.0f;  // ButtonDriver::SetPositionAnim takes InitSelf's 80 off every button's x (0x807E2E38)
const u32 kCursorFrames = 30;  // D79: each player on a button shows for 30 frames in turn
const u32 kOkBmg = 0x9fb;  // P1's coloured "OK", then P2-4's
// A button nobody is on takes the menus' colour, which GetHudSlotIdColor gives any hud that is no local
// (palette entry 0, 0x805F0440; RR's GetHUDSlotColor falls through to its own).
const u8 kNoPlayer = 0xff;
// The menus' hold repeat (ControlButtonInfo::IsADirectionHeldForMultipleOf15, 0x805EFA70) and stick
// thresholds (ButtonInfo::Update, 0x80895CB0): a direction starts past 0.67 and holds past 0.33.
const s32 kRepeatFrames = 15;
const float kStickStart = 0.67f;
const float kStickHold = 0.33f;
// D67's previews and name plates, four down each side (P1/P3/P5/P7 left): each preview small and just
// above its plate (D100).
const float kPlateX = 240.0f;
const float kPlateY[kMaxLocal / 2] = {115.0f, 30.0f, -55.0f, -140.0f};
const float kPlateScale = 0.5f;
const float kPreviewX = 240.0f;
const float kPreviewDY = 40.0f;  // above its plate
const float kPreviewScale = 0.4f;  // of the 256 x 256 model_set_p
// RR's Randomize Combo roulette (ExpVR::randomDuration, ExpCharacterSelect::BeforeControlUpdate): 60 frames, a
// hop on each frame whose count left is 1 modulo 4 (the last onto the roll), every player confirming at 0.
const s32 kRollFrames = 60;
const s32 kRollHop = 4;

// PushButton's animation names (0x808B7690), so the groups and animations keep PushButton's ids.
static const char *kButtonAnims[] = {
    "Loop", "Loop", nullptr, "Select", "Free", "FreeToSelect", "Select", "SelectToFree", nullptr, "SelectIn", "SelectIn", "SelectStop", nullptr, "OK", "OK", "OKStop", nullptr, nullptr};
enum ButtonGroup {
    GROUP_LOOP,
    GROUP_SELECT,
    GROUP_SELECT_IN,
    GROUP_OK
};
enum SelectAnim {
    ANIM_FREE,
    ANIM_FREE_TO_SELECT,
    ANIM_SELECT,
    ANIM_SELECT_TO_FREE
};
const u32 kAnimStop = 1;  // SelectIn's SelectStop and OK's OKStop

// A player's Mii is SectionParams.playerMiis' entry for them, a group of 12 (Init at 0x805E2F9C), which
// CopyMiisToRacedata copies to the race's players. P5-8's index among the console's and the default Miis
// is kept here, as P1-4's at SectionParams+0x154 (4 wide). Vanilla zeroes P1-4's at every license click
// (ResetLicenseParams, 0x805E4118); P5-8's go back to 0 only when the page is built under another license.
static u32 extMiiIdx[kMaxLocal - kGameLocal];
static s32 extMiiLicense = -1;
// The player 5-8 MiiSelect is open for, else 0xFF; read by MiiPickPlayer.
static u8 extMiiPlayer = kNoPlayer;
const u32 kMiiSelectClicked = 0xd94;  // MiiSelect::clickedButtonId, -1 for none
const u32 kMiiSelectHud = 0xda0;  // MiiSelect::activeHudSlotId
static u8 *const *const miiManager = reinterpret_cast<u8 *const *>(0x809bd710);  // MiiManager::sInstance
const u32 kOfficialMiiCount = 0x2d4;  // its count of the console's Miis, read as a byte (0x8083D6DC)

typedef void (*LoadMiiFn)(MiiGroup *, u8, u32);
typedef void (*MiiVoiceFn)(void *, Mii *, u32);
typedef void (*MiiConfirmVoiceFn)(void *, u32);
typedef void (*MiiModelsFn)(MenuDriverModelMgr *, u32, Mii *);
static const LoadMiiFn loadOfficialMii = reinterpret_cast<LoadMiiFn>(0x805fa678);
static const LoadMiiFn loadDefaultMii = reinterpret_cast<LoadMiiFn>(0x805fa748);
// OnButtonDriverClick's Mii voice (0x8083E094, 0x8083E0A0): the Mii's voice set for the hud, then its
// confirm line through 0x80868C20; both keep per-hud records 4 wide (DriverSoundManager+0x4260, +0x2430).
static const MiiVoiceFn miiVoice = reinterpret_cast<MiiVoiceFn>(0x80869118);
static const MiiConfirmVoiceFn miiConfirmVoice = reinterpret_cast<MiiConfirmVoiceFn>(0x80868d3c);
static const MiiModelsFn rebuildMiiModels = reinterpret_cast<MiiModelsFn>(0x80831534);

// MiiSelect::OnButtonClick's section 0x54 branch, "lwz r3, 0x44(r28)" (0x80848A44): r30 is the clicking
// slot, which indexes playerMiis in the copy and flag that follow (0x80848A60, 0x80848A78, 0x80848A98).
// P5-8 drive MiiSelect through slot 4, so slot 4 becomes the player it was opened for. r12 and CR0 are free
// (the bl at 0x80848A50 is next to read either); OnButtonClick saved LR.
asmFunc MiiPickPlayer() {
    ASM(
        nofralloc;
        cmpwi r30, 4;
        bne done;
        lis r12, extMiiPlayer @ha;
        lbz r12, extMiiPlayer @l(r12);
        cmplwi r12, 8;
        bge done;
        mr r30, r12;
        done :;
        lwz r3, 0x44(r28);
        blr;)
}
kmCall(0x80848a44, MiiPickPlayer);

// The same branch's "bl 0x80831534" (0x80848ABC) rebuilds the hud's Mii models, MiiSelect's hud, in
// MenuDriverModelMgr's 4-wide arrays: P5-8 come in as hud 4 and skip it (their preview is Previews.cpp's).
static void RebuildMiiModels(MenuDriverModelMgr *models, u32 hud, Mii *mii) {
    if ((hud & 0xff) < kGameLocal)
        rebuildMiiModels(models, hud, mii);
}
kmCall(0x80848abc, RebuildMiiModels);

// SectionPad::Update's "stw r0, 0x58(r26)" (0x8061AF60) gives the reconnect check (GetFirstDeactivatedPad, its
// only reader) the top page's player mask, MiiSelect's 0x10 while a player 5-8 drives it through slot 4, whose
// pad record is the dummy's: slot 4 leaves the mask then. r12 and CR0 are written before they are next read;
// Update saved LR.
asmFunc MiiSelectReconnectMask() {
    ASM(
        nofralloc;
        lis r12, extMiiPlayer @ha;
        lbz r12, extMiiPlayer @l(r12);
        cmplwi r12, 8;
        bge store;
        rlwinm r0, r0, 0, 28, 26;
        store :;
        stw r0, 0x58(r26);
        blr;)
}
kmCall(0x8061af60, MiiSelectReconnectMask);

// 0x80868ABC's driver path for P5-8, on handles of their own: the player's last voice stopped, the
// character's confirm picker (DriverSoundManager + character * 0xC0 + 0x30) started into the handle
// through its vtable +0xC, then the volume. A Mii takes another path (0x80868C20, ExtMiiConfirmVoice).
static void ExtConfirmVoice(u32 player, CharacterId character) {
    u8 *manager = *confirmVoiceOwner;
    if (manager == nullptr || player < kGameLocal || player >= kMaxLocal || static_cast<u32>(character) >= Pulsar::Driver::CHARACTER_COUNT)
        return;
    nw4r::snd::SoundHandle &handle = extVoices[player - kGameLocal];
    if (handle.basicSound != nullptr)
        handle.basicSound->Stop(2);
    void *picker = manager + character * kVoiceSetStride + kVoicePicker;
    const StartVoiceFn start = *reinterpret_cast<StartVoiceFn *>(*reinterpret_cast<u8 **>(picker) + 0xc);
    if (start(picker, &handle) != nullptr && handle.basicSound != nullptr)
        handle.basicSound->SetVolume(0, kVoiceVolume);
}

// A Mii's voice record, as 0x80869344 fills DriverSoundManager+0x4260 + hud * 0x10 from the Mii: its voice type,
// size and gender, which pick one of the shared pickers at +0x42A8 (0x8086622C), and its pitch.
struct MiiVoice {
    s32 type;
    s32 size;
    s32 gender;
    float pitch;
};
static MiiVoice extMiiVoices[kMaxLocal - kGameLocal];
typedef void (*MiiVoiceRecordFn)(void *, Mii *, MiiVoice *, u32);
typedef void (*MiiVoicePickerFn)(s32 *, const MiiVoice *);
static const MiiVoiceRecordFn miiVoiceRecord = reinterpret_cast<MiiVoiceRecordFn>(0x80869344);
static const MiiVoicePickerFn miiVoicePicker = reinterpret_cast<MiiVoicePickerFn>(0x8086622c);
const u32 kMiiVoicePickers = 0x42a8;
const u32 kMiiVoicePickerStride = 0x30;
const float kMiiVoiceVolume = 0.6f;  // 0x808B1FC8
const float kMiiVoicePan = 0.0f;  // 0x808B1FCC
typedef void (*SetPanFn)(nw4r::snd::detail::BasicSound *, float);
static const SetPanFn setPan = reinterpret_cast<SetPanFn>(0x8008f620);  // BasicSound::SetPan, not in Kamek's externals

// A Mii's confirm voice for P5-8 on their own records and handles, as 0x80869118 then 0x80868C20 play P1-4's: the
// record, the old voice stopped, the picker started into the handle, then volume, pan and pitch. 0x80869118's
// 3-4 player step (0x808693B8), which gives each local Mii a voice id the others lack in shared 4-player sets, is
// left out, so a P5-8 Mii can repeat another's voice.
static void ExtMiiConfirmVoice(u32 player, Mii *mii) {
    u8 *manager = *confirmVoiceOwner;
    if (manager == nullptr || mii == nullptr || player < kGameLocal || player >= kMaxLocal)
        return;
    MiiVoice &record = extMiiVoices[player - kGameLocal];
    miiVoiceRecord(manager, mii, &record, 1);
    s32 picker = -1;
    miiVoicePicker(&picker, &record);
    if (picker < 0)
        return;
    nw4r::snd::SoundHandle &handle = extVoices[player - kGameLocal];
    if (handle.basicSound != nullptr)
        handle.basicSound->Stop(2);
    void *pickerObj = manager + kMiiVoicePickers + picker * kMiiVoicePickerStride;
    const StartVoiceFn start = *reinterpret_cast<StartVoiceFn *>(*reinterpret_cast<u8 **>(pickerObj) + 0xc);
    if (start(pickerObj, &handle) == nullptr || handle.basicSound == nullptr)
        return;
    handle.basicSound->SetVolume(0, kMiiVoiceVolume);
    setPan(handle.basicSound, kMiiVoicePan);
    handle.basicSound->SetPitch(record.pitch);
}

static void PlayerColours(u32 player, RGBA16 *primary, RGBA16 *secondary) {
    if (player < kGameLocal)
        ControlManipulator::GetHudSlotIdColor(player, primary, secondary);  // RR's local colours (UIColor.cpp)
    else
        SlotPalette(player, primary, secondary);
}

static void SetTev(nw4r::lyt::Material *material, u32 idx, s16 r, s16 g, s16 b, s16 a) {
    material->tevColours[idx].r = r;
    material->tevColours[idx].g = g;
    material->tevColours[idx].b = b;
    material->tevColours[idx].a = a;
}

static void SetVisible(nw4r::lyt::Pane *pane, bool visible) {
    if (pane != nullptr)
        pane->flag = visible ? (pane->flag | 1) : (pane->flag & ~1);
}

// 1 - the share of the current animation played, so a reversed transition starts where the other stopped.
static float Remaining(const AnimationGroup &group) {
    const u16 size = group.animations[group.curAnimation].transform->GetFrameSize();
    return 1.0f - group.curFrame / static_cast<float>(size);
}

class CharSelectPage : public Page {
public:
    CharSelectPage();
    ~CharSelectPage() override;
    PageId GetNextPage() const override {
        return this->nextPageId;
    }
    void OnInit() override;
    void OnActivate() override;
    void OnDeactivate() override;
    void BeforeControlUpdate() override;
    void AfterControlUpdate() override;
    void OnResume() override;

private:
    struct Cursor {
        u8 button;
        bool confirmed;
        s32 held[4];  // frames each of up, down, left and right has been held, -1 when not
    };
    struct Button {
        LayoutUIControl control;
        CharacterId character;
        u8 col, row;
        nw4r::lyt::Pane *frame;  // fuchi_pattern
        nw4r::lyt::Pane *base;  // black_base
        nw4r::lyt::Pane *markers[kMaxLocal];  // ok_null_1p..8p
        bool coloured;
        s8 outfit;  // the Mii row's 0, 1 and 2 for A, B and C, -1 for a driver
        bool locked;  // Mii C below RR's 300 VR, its icon a "?" and no cursor stopping on it
    };

    void LoadButton(u32 idx);
    void LoadExtMiis();
    Mii *PlayerMii(u32 player) const;
    CharacterId CharacterOn(u32 player, u32 idx) const;
    void OpenMiiSelect(u32 player);
    void LoadPlate(u32 player);
    void LoadPreview(u32 player);
    void RequestPreview(u32 player);
    void PosePreview(u32 player, bool confirmed);
    void UpdatePreviews();
    void SetPlateName(u32 player);
    void SetIcon(Button &button);
    void CycleSkin(u32 player, s32 direction);
    void SyncSharedModels();
    void FillCpus();
    CharacterId RollCharacter(Random &random, u32 player, CharacterId avoid) const;
    bool Roll();
    void Roulette();
    void FlushSkinModels();
    s32 ButtonOf(CharacterId character) const;
    s32 Direction(u32 player, const Input::RealControllerHolder &holder);
    void Move(u32 player, s32 direction);
    void Hover(u32 player, u32 to, bool preview = true);
    void Confirm(u32 player);
    void Unconfirm(u32 player);
    bool PlayerOn(u32 player, u32 button) const;
    void UpdateButton(u32 idx);
    void Leave(u32 next, u32 animDirection);

    CtrlMenuPageTitleText *titleText;
    LayoutUIControl *backButton;  // a CtrlMenuDummyBack, the visual only, as Join's
    LayoutUIControl *grid;  // CtrlMenuCharacterSelect's own parent, "CharacterSelectNULL"
    Button *buttons;
    LayoutUIControl *plates[kMaxLocal];
    LayoutUIControl *previews[kMaxLocal];  // CharacterSelectModel, the model's texture on model_set_p
    PageManipulatorManager manager;
    Cursor cursors[kMaxLocal];
    s8 skinSteps[kMaxLocal];  // this frame's skin toggle per player, -1, 0 or 1
    u8 cols, rows;
    PageId nextPageId;
    u32 frame;
    s32 leaveFrames;  // counts down once everyone has confirmed, -1 before
    bool leaving;
    u8 miiPlayer;  // the player MiiSelect is open over the page for, else kNoPlayer
    bool miiResumed;  // MiiSelect closed this frame, so its last press reads as no input here
    u32 savedDrift;  // SectionParams' driftType[0] while MiiSelect borrows it for a player 5-8
    s32 rolling;  // the roulette's frames left, -1 when none runs
    CharacterId rolled[kMaxLocal];
};

Page *NewCharSelectPage() {
    return new CharSelectPage;
}

CharSelectPage::CharSelectPage() {
    this->nextPageId = PAGE_NONE;
    this->miiPlayer = kNoPlayer;
    this->miiResumed = false;
    this->rolling = -1;
    this->titleText = new CtrlMenuPageTitleText;
    this->backButton = static_cast<LayoutUIControl *>(::operator new(kDummyBackSize));
    layoutCtor(this->backButton);
    *reinterpret_cast<u32 *>(this->backButton) = 0x808d3710;
    this->grid = new LayoutUIControl;
    this->buttons = new Button[kButtons];
    for (int i = 0; i < kMaxLocal; ++i) {
        this->plates[i] = new LayoutUIControl;
        this->previews[i] = new LayoutUIControl;
    }
}

CharSelectPage::~CharSelectPage() {
    delete this->titleText;
    delete this->backButton;
    delete this->grid;
    delete[] this->buttons;
    for (int i = 0; i < kMaxLocal; ++i) {
        delete this->plates[i];
        delete this->previews[i];
    }
}

void CharSelectPage::OnInit() {
    this->InitControlGroup(3 + 2 * kMaxLocal);
    this->AddControl(0, *this->titleText, 0);
    this->titleText->Load(false);
    this->titleText->SetMessage(kTitleBmg);
    this->AddControl(1, *this->backButton, 0);
    dummyBackLoad(this->backButton, Pulsar::UI::buttonFolder, "Back", "ButtonBack", 1);

    // The grid as CtrlMenuCharacterSelect::Load (0x807E26D4) builds it with 3+ players, its x less the
    // 80 every vanilla button takes off its own.
    this->AddControl(2, *this->grid, 0);
    ControlLoader gridLoader(this->grid);
    gridLoader.Load("control", "CharacterSelectNULL", "CharacterSelectNULLCenter", nullptr);
    this->grid->positionAndscale[0].position.x -= kGridShift;
    positionFromBase(this->grid);
    this->grid->InitControlGroup(kButtons);
    for (u32 i = 0; i < kButtons; ++i) this->LoadButton(i);

    // The grid's columns and rows from the buttons' own places, which RR's ReplacedAssets may move.
    float xs[kButtons], ys[kButtons];
    this->cols = 0;
    this->rows = 0;
    for (u32 i = 0; i < kButtons; ++i) {
        const Vec3 &at = this->buttons[i].control.positionAndscale[0].position;
        u8 c = 0, r = 0;
        while (c < this->cols && xs[c] != at.x) ++c;
        if (c == this->cols)
            xs[this->cols++] = at.x;
        while (r < this->rows && ys[r] != at.y) ++r;
        if (r == this->rows)
            ys[this->rows++] = at.y;
    }
    for (u32 i = 0; i < kButtons; ++i) {
        const Vec3 &at = this->buttons[i].control.positionAndscale[0].position;
        u8 col = 0, row = 0;
        for (u32 k = 0; k < this->cols; ++k) col += xs[k] < at.x;
        for (u32 k = 0; k < this->rows; ++k) row += ys[k] > at.y;
        this->buttons[i].col = col;
        this->buttons[i].row = row;
    }

    for (u32 p = 0; p < kMaxLocal; ++p) {
        this->LoadPreview(p);
        this->LoadPlate(p);
    }
    this->LoadExtMiis();

    // As C3's pages: a PageManipulatorManager with nothing to select, every player's input read from the
    // holders, and SectionPad's reconnect overlay watching P1-4 (P5-8's box is D69's, C6).
    this->manager.Init(1, false);
    this->SetManipulatorManager(this->manager);
    this->manager.UpdatePlayerBitfield2((1 << kGameLocal) - 1);
}

// As CtrlMenuCharacterSelect::LoadButton (0x807E2928) with every character unlocked, as RR has them
// (Extra/MiiOutfitC.cpp), on the controls the install generates (tools/assets/gen_charselect_multi.py), whose
// OK markers each take a sixth of the button with 5-6 players and an eighth with 7-8 (D80, D95). Section 0x54
// is built for each game, after menuLocalCount is set.
void CharSelectPage::LoadButton(u32 idx) {
    Button &button = this->buttons[idx];
    LayoutUIControl &control = button.control;
    this->grid->AddControl(idx, &control);
    char ctrName[0x20];
    char variant[0x10];
    snprintf(ctrName, sizeof(ctrName), "CharacterSelect4_%u_Multi%u", idx / kButtonsPerGroup, menuLocalCount <= 6 ? 6 : 8);
    snprintf(variant, sizeof(variant), "Button%u", idx % kButtonsPerGroup);
    ControlLoader loader(&control);
    loader.Load(Pulsar::UI::buttonFolder, ctrName, variant, kButtonAnims);
    button.outfit = idx < kDriverButtons ? -1 : static_cast<s8>(idx - kDriverButtons);
    button.locked = false;
    button.character = button.outfit < 0 ? CtrlMenuCharacterSelect::buttonIdToCharacterId[idx] : static_cast<CharacterId>(MII_M_A_MALE + 2 * button.outfit);
    button.frame = control.layout.GetPaneByName("fuchi_pattern");
    button.base = control.layout.GetPaneByName("black_base");
    for (u32 p = 0; p < kMaxLocal; ++p) {
        char name[0x10];
        snprintf(name, sizeof(name), "ok_null_%up", p + 1);
        button.markers[p] = control.layout.GetPaneByName(name);
    }
    button.coloured = true;  // so the first UpdateButton resets it
    this->SetIcon(button);

    // P1-4's "OK" carries its colour in its message, 0x9FB + hud (ButtonDriver::OnMultiplayerMiiSelected,
    // 0x807E3BB0); P5-8's copies say "OK" plainly in the slot palette's colour (D80).
    for (u32 p = 0; p < kMaxLocal; ++p) {
        char name[0x10];
        snprintf(name, sizeof(name), "ok_text_%up", p + 1);
        if (p < kGameLocal) {
            control.SetTextBoxMessage(name, kOkBmg + p);
            continue;
        }
        Text::Info info;
        info.strings[0] = const_cast<wchar_t *>(L"OK");
        control.SetTextBoxMessage(name, Pulsar::UI::BMG_TEXT, &info);
        nw4r::lyt::Pane *text = control.layout.GetPaneByName(name);
        nw4r::lyt::Material *material = text != nullptr ? text->GetMaterial() : static_cast<nw4r::lyt::Material *>(nullptr);
        if (material == nullptr)
            continue;
        RGBA16 primary, secondary;
        PlayerColours(p, &primary, &secondary);
        material->tevColours[1].r = primary.red;
        material->tevColours[1].g = primary.green;
        material->tevColours[1].b = primary.blue;
    }
}

// LoadButton's five icon panes (0x807E2AE8-0x807E2B38), then the body of RR's SetCharacterSelectIcon
// (UI/CustomCharacters.cpp), which reads a PushButton's buttonId and so cannot take this control: the icon
// shows P1's skin (D84), so P1's skin toggle sets it again. A Mii button has no skin; a locked Mii C shows
// RR's "?" (UI/ChangeCombo/ChangeCombo.cpp, ExpCharacterSelect::OnActivate).
void CharSelectPage::SetIcon(Button &button) {
    LayoutUIControl &control = button.control;
    const char *icon = button.locked ? "cha_26_hatena" : GetCharacterIconPaneName(button.character);
    static const char *const iconPanes[] = {"chara", "chara_shadow", "chara_light_01", "chara_light_02", "chara_c_down"};
    for (u32 i = 0; i < sizeof(iconPanes) / sizeof(iconPanes[0]); ++i) control.SetPicturePane(iconPanes[i], icon);
    if (button.outfit >= 0)
        return;
    Pulsar::Race::LoadCustomCharacterIcon(button.character, Pulsar::Driver::selectedSlots[button.character], control.layout.GetPaneByName("chara"), control.layout.GetPaneByName("chara_shadow"),
      control.layout.GetPaneByName("chara_light_01"));
    *control.layout.GetPaneByName("chara_light_02")->GetMaterial()->GetTexMapAry() = *control.layout.GetPaneByName("chara")->GetMaterial()->GetTexMapAry();
    *control.layout.GetPaneByName("chara_c_down")->GetMaterial()->GetTexMapAry() = *control.layout.GetPaneByName("chara")->GetMaterial()->GetTexMapAry();
}

// The 4P name plate (CharacterSelect::CreateControl, 0x8083D9C0) at half size, coloured as
// CharaName::InitSelf colours it (0x8083F09C): each channel the mean of the player's two colours.
// RR renames the control the game loads to RharacterSelectName (Extra.cpp), its own layout with the skin
// toggle's L and R at the plate's ends (D81); a build without RR would load CharacterSelectName.
void CharSelectPage::LoadPlate(u32 player) {
    LayoutUIControl &plate = *this->plates[player];
    this->AddControl(3 + player, plate, 0);
    ControlLoader loader(&plate);
    loader.Load("control", "RharacterSelectName", "CharacterSelectName1P4", nullptr);
    plate.drawPriority = -1.0f;  // as CreateControl with 3+ players (0x8083D9F4)
    PositionAndScale &base = plate.positionAndscale[0];
    base.position.x = player % 2 == 0 ? -kPlateX : kPlateX;
    base.position.y = kPlateY[player / 2];
    base.scale.x = kPlateScale;
    base.scale.z = kPlateScale;  // Vec2 names its second component z
    positionFromBase(&plate);
    RGBA16 primary, secondary;
    PlayerColours(player, &primary, &secondary);
    static const char *const parts[] = {"black_parts_t_00", "black_parts_t_01"};
    for (u32 i = 0; i < 2; ++i) {
        nw4r::lyt::Pane *pane = plate.layout.GetPaneByName(parts[i]);
        nw4r::lyt::Material *material = pane != nullptr ? pane->GetMaterial() : static_cast<nw4r::lyt::Material *>(nullptr);
        if (material == nullptr)
            continue;
        for (u32 tev = 0; tev < 2; ++tev)
            SetTev(material, tev, (primary.red + secondary.red) / 2, (primary.green + secondary.green) / 2, (primary.blue + secondary.blue) / 2, (primary.alpha + secondary.alpha) / 2);
    }
}

// As DriverModelControl::Load loads it (0x805F32DC, from CharacterSelect::CreateControl 0x8083D8E8): its
// animation names (0x80895DC4; a null table faults in ControlLoader) and its one group started on Player,
// full opacity. Placed here rather than by ModelRendererValuesHolder, whose variants stop at 4 players.
void CharSelectPage::LoadPreview(u32 player) {
    static const char *kModelAnims[] = {"Ghost", "Player", "Ghost", nullptr, nullptr};
    LayoutUIControl &preview = *this->previews[player];
    this->AddControl(3 + kMaxLocal + player, preview, 0);
    ControlLoader loader(&preview);
    loader.Load("model", "CharacterSelectModel", "1P_4Div", kModelAnims);
    preview.animator.GetAnimationGroupById(0).PlayAnimationAtFrame(0, 0.0f);
    preview.drawPriority = -2.0f;  // under the plates
    PositionAndScale &base = preview.positionAndscale[0];
    base.position.x = player % 2 == 0 ? -kPreviewX : kPreviewX;
    base.position.y = kPlateY[player / 2] + kPreviewDY;
    base.scale.x = kPreviewScale;
    base.scale.z = kPreviewScale;
    positionFromBase(&preview);
}

// The player's model for the character their cursor is on: P1-4 through ModelRenderer, which RR's hooks
// give each player their own skin and model (Driver/LocalPlayerSkins.cpp), P5-8 through Previews.cpp. A P5-8
// skin that could not be built leaves the stock model, and the slot goes back to 0 as CycleSkin puts it.
void CharSelectPage::RequestPreview(u32 player) {
    const CharacterId character = this->CharacterOn(player, this->cursors[player].button);
    if (player < kGameLocal) {
        Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
        if (renderer != nullptr)
            renderer->RequestCharacterModel(player, character);
    } else if (!ExtPreviewLoad(player, character, Pulsar::Driver::GetLocalPlayerSlot(player, character))) {
        if (ExtPreviewStock(player, character))
            Pulsar::Driver::SetLocalPlayerSlot(player, character, 0);
        OS::Report("ss8 select: character player %u preview %#x failed\n", player + 1, character);
    }
}

// As OnActivate (0x8083DD58), OnButtonDriverClick (0x8083E140) and OnBackPress (0x8083EA30) switch a P1-4
// model with its hud; P5-8's pass -1 (Previews.cpp).
void CharSelectPage::PosePreview(u32 player, bool confirmed) {
    if (player >= kGameLocal) {
        ExtPreviewPose(player, confirmed);
        return;
    }
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    MenuDriverModel *model = modelMgr != nullptr && modelMgr->driverModels != nullptr ? modelMgr->driverModels->players[player].playerModel : static_cast<MenuDriverModel *>(nullptr);
    if (model != nullptr)
        model->SwitchState(player, confirmed ? MenuDriverModel::MENUDRIVERMODEL_STATE_ONCHARSELECT : MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
}

// What DriverModelControl::OnUpdate does each frame (0x805F356C), for 8: the model shown, its texture on
// model_set_p (TexMap::Set, then no palette, 0x805F36AC-0x805F375C), and a P1-4 model a kart page left in
// its kart pose set back to idle (0x805F37C0).
void CharSelectPage::UpdatePreviews() {
    Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    for (u32 p = 0; p < menuLocalCount; ++p) {
        GX::TexObj *texObj = nullptr;
        if (p >= kGameLocal) {
            texObj = ExtPreviewTexObj(p);
        } else if (renderer != nullptr) {
            showModel(renderer, p, true, 0);
            texObj = modelTexObj(renderer, p);
            MenuDriverModel *model = modelMgr != nullptr && modelMgr->driverModels != nullptr ? modelMgr->driverModels->players[p].playerModel : static_cast<MenuDriverModel *>(nullptr);
            if (model != nullptr && model->state == MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT)
                model->SwitchState(p, MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
        }
        nw4r::lyt::Pane *pane = this->previews[p]->layout.GetPaneByName("model_set_p");
        nw4r::lyt::Material *material = pane != nullptr ? pane->GetMaterial() : static_cast<nw4r::lyt::Material *>(nullptr);
        if (texObj == nullptr || material == nullptr)
            continue;
        nw4r::lyt::TexMap texMap;
        texMapSet(&texMap, texObj);
        texMap.palette = nullptr;
        texMap.paletteEntryNum = 0;
        texMap.bits.paletteFormat = 0;
        *material->GetTexMapAry() = texMap;
    }
}

// The plate names the character the cursor is on in that player's skin (RR's names[hud], Driver/CustomCharacters.cpp),
// or on the Mii row the player's Mii, as OnButtonDriverSelect names it (0x8083E69C-0x8083E6D0) with outfit A's id,
// since GetCharacterBMGId has no name for C's (RR remaps only its own calls, Extra/MiiOutfitC.cpp).
void CharSelectPage::SetPlateName(u32 player) {
    const Button &button = this->buttons[this->cursors[player].button];
    if (button.outfit >= 0) {
        Text::Info info;
        info.miis[0] = this->PlayerMii(player);
        const CharacterId outfitA = static_cast<CharacterId>(this->CharacterOn(player, this->cursors[player].button) - 2 * button.outfit);
        this->plates[player]->SetMessage(GetCharacterBMGId(outfitA, info.miis[0] != nullptr), &info);
        return;
    }
    const CharacterId character = button.character;
    this->plates[player]->SetMessage(Pulsar::UI::GetCharacterSlotNameBMGId(character, Pulsar::Driver::GetLocalPlayerSlot(player, character), false));
}

Mii *CharSelectPage::PlayerMii(u32 player) const {
    MiiGroup &miis = SectionMgr::sInstance->sectionParams->playerMiis;
    return player < miis.miiCount ? miis.GetMii(player) : static_cast<Mii *>(nullptr);
}

// The character a player confirms on a button: a driver's own, or on the Mii row their Mii's size and gender in
// that outfit, as OnButtonDriverSelect (0x8083E62C-0x8083E668) and RR's MiiOutfitC2 (0x8083E64C) work it out.
CharacterId CharSelectPage::CharacterOn(u32 player, u32 idx) const {
    const Button &button = this->buttons[idx];
    const Mii *mii = button.outfit >= 0 ? this->PlayerMii(player) : static_cast<Mii *>(nullptr);
    if (mii == nullptr)
        return button.character;
    const s32 outfitA = static_cast<s32>(GetMiiCharacterId(*mii));
    return outfitA < 0 ? button.character : static_cast<CharacterId>(outfitA + 2 * button.outfit);
}

// P5-8's Miis as CharacterSelect::OnInit loads P1-4's from SectionParams+0x154 (0x8083D6D0-0x8083D710): an index
// below the console's Mii count is one of its Miis, any other one of the default Miis.
void CharSelectPage::LoadExtMiis() {
    MiiGroup &miis = SectionMgr::sInstance->sectionParams->playerMiis;
    if (*miiManager == nullptr)
        return;
    const RKSYS::Mgr *rksys = RKSYS::Mgr::sInstance;
    const s32 license = rksys != nullptr ? static_cast<s32>(rksys->curLicenseId) : -1;
    if (license != extMiiLicense) {
        memset(extMiiIdx, 0, sizeof(extMiiIdx));
        extMiiLicense = license;
    }
    const u32 official = (*miiManager)[kOfficialMiiCount];
    for (u32 p = kGameLocal; p < menuLocalCount && p < miis.miiCount; ++p) {
        const u32 idx = extMiiIdx[p - kGameLocal];
        if (idx < official)
            loadOfficialMii(&miis, p, idx);
        else
            loadDefaultMii(&miis, p, idx - official);
    }
}

// As OnMiiButtonClick opens MiiSelect for a player (0x807E394C-0x807E3978). P5-8 drive it through slot 4, as
// a pause by them does (Pause.cpp): its masks are 1 << hud (0x808485F4), and its player index, SectionParams+0x154
// read at 0x8084862C and written at 0x808488E8, lands for hud 4 on driftType[0], which holds that player's
// index until OnResume puts both back. Nothing reads driftType before the drift page.
void CharSelectPage::OpenMiiSelect(u32 player) {
    Page *miiSelect = SectionMgr::sInstance->curSection->pages[PAGE_MII_SELECT];
    if (miiSelect == nullptr)
        return;
    SectionParams *params = SectionMgr::sInstance->sectionParams;
    u8 *fields = reinterpret_cast<u8 *>(miiSelect);
    *reinterpret_cast<u32 *>(fields + kMiiSelectHud) = player < kGameLocal ? player : kGameLocal;
    if (player >= kGameLocal) {
        this->savedDrift = params->driftType[0];
        params->driftType[0] = extMiiIdx[player - kGameLocal];
        extMiiPlayer = player;
        SetMenuSlotProxy(PlayerHolder(player));
    }
    this->miiPlayer = player;
    this->AddPageLayer(PAGE_MII_SELECT, 0);
    OS::Report("ss8 select: character player %u mii select\n", player + 1);
}

// MiiSelect closed. A pick is already in playerMiis[player] (its section 0x54 click, 0x80848A70, through
// MiiPickPlayer for P5-8), and the vanilla page confirms right after one (OnMultiplayerMiiSelected, 0x807E3BD0).
void CharSelectPage::OnResume() {
    if (this->miiPlayer == kNoPlayer)
        return;
    const u32 player = this->miiPlayer;
    this->miiPlayer = kNoPlayer;
    this->miiResumed = true;
    if (player >= kGameLocal) {
        SectionParams *params = SectionMgr::sInstance->sectionParams;
        extMiiIdx[player - kGameLocal] = params->driftType[0];
        params->driftType[0] = this->savedDrift;
        extMiiPlayer = kNoPlayer;
        SetMenuSlotProxy(nullptr);
    }
    const Page *miiSelect = SectionMgr::sInstance->curSection->pages[PAGE_MII_SELECT];
    const s32 clicked = miiSelect != nullptr ? *reinterpret_cast<const s32 *>(reinterpret_cast<const u8 *>(miiSelect) + kMiiSelectClicked) : -1;
    OS::Report("ss8 select: character player %u mii %s\n", player + 1, clicked < 0 ? "back" : "picked");
    if (clicked < 0)
        return;
    this->SetPlateName(player);
    if (player >= kGameLocal)
        ExtPreviewDrop(player);  // P1-4's models were rebuilt by the click (RebuildMiiModels)
    this->RequestPreview(player);
    this->Confirm(player);
}

s32 CharSelectPage::ButtonOf(CharacterId character) const {
    if (character >= MII_S_A_MALE && character <= MII_L_C_FEMALE) {
        const u32 idx = kDriverButtons + (character - MII_S_A_MALE) % (2 * kMiiOutfits) / 2;
        return this->buttons[idx].locked ? -1 : static_cast<s32>(idx);
    }
    for (u32 i = 0; i < kButtons; ++i)
        if (this->buttons[i].character == character)
            return i;
    return -1;
}

// P1-4 start on SectionParams' last picks, as CtrlMenuCharacterSelect::InitSelf (0x807E2F34); P5-8 on
// theirs or D62's default; a Mii on its outfit's button. Mii C is open as RR opens it on the vanilla page
// without character restrictions (ExpCharacterSelect::OnActivate); a last pick on a locked C starts on
// D62's default.
void CharSelectPage::OnActivate() {
    this->nextPageId = PAGE_NONE;
    this->leaving = false;
    this->leaveFrames = -1;
    this->frame = 0;
    this->miiPlayer = kNoPlayer;
    this->miiResumed = false;
    this->rolling = -1;
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    bool miiCOpen = Pulsar::Restrictions::IsOnlyMiiOutfitCEnabled();
    const RKSYS::Mgr *rksys = RKSYS::Mgr::sInstance;
    if (!miiCOpen && rksys != nullptr)
        miiCOpen = Pulsar::PointRating::GetUserVR(rksys->curLicenseId) >= 300.0f;
#ifdef SS8_MENU_SCRIPT
    miiCOpen = miiCOpen || menuOpenMiiC;
#endif
    Button &miiC = this->buttons[kButtons - 1];
    miiC.locked = !miiCOpen;
    this->SetIcon(miiC);
    for (u32 p = 0; p < kMaxLocal; ++p) {
        Cursor &cursor = this->cursors[p];
        s32 button = this->ButtonOf(p < kGameLocal ? params->characters[p] : ExtCharacter(p));
        if (button < 0)
            button = this->ButtonOf(static_cast<CharacterId>(p));
        cursor.button = button;
        cursor.confirmed = false;
        for (int d = 0; d < 4; ++d) cursor.held[d] = -1;
        this->plates[p]->isHidden = p >= menuLocalCount;
        this->previews[p]->isHidden = p >= menuLocalCount;
    }
    if (!ExtPreviewsShow(menuLocalCount))
        OS::Report("ss8 select: P5-8 preview textures failed\n");
    for (u32 p = 0; p < menuLocalCount; ++p) {
        this->RequestPreview(p);
        this->PosePreview(p, false);
        this->SetPlateName(p);
    }
    // Every group starts as PushButton::Init (0x805BDBE0) starts it; a loaded group has no animation yet.
    for (u32 i = 0; i < kButtons; ++i) {
        bool hovered = false;
        for (u32 p = 0; p < menuLocalCount; ++p) hovered |= this->cursors[p].button == i;
        UIAnimator &animator = this->buttons[i].control.animator;
        animator.GetAnimationGroupById(GROUP_LOOP).PlayAnimationAtFrameAndDisable(0, 0.0f);
        animator.GetAnimationGroupById(GROUP_SELECT).PlayAnimationAtFrame(hovered ? ANIM_SELECT : ANIM_FREE, 0.0f);
        animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(kAnimStop, 0.0f);
        animator.GetAnimationGroupById(GROUP_OK).PlayAnimationAtFrame(kAnimStop, 0.0f);
        this->UpdateButton(i);
    }
    OS::Report("ss8 select: character page, %u players, mii C %s\n", menuLocalCount, miiC.locked ? "locked" : "open");
    for (u32 p = 0; p < menuLocalCount; ++p) OS::Report("ss8 select: character player %u on %#x\n", p + 1, this->CharacterOn(p, this->cursors[p].button));
}

void CharSelectPage::OnDeactivate() {
    ExtPreviewsHide();
}

// RR's skin toggle input (PageBeforeControlUpdate, Driver/CustomCharacters.cpp), which runs only on the
// vanilla page, for all 8 (D81): L and R on GameCube and Classic, C and Z on a Nunchuk, B and A on a
// sideways Wii Remote, whose 2 and 1 then become its A and B so that A toggles rather than confirms.
void CharSelectPage::BeforeControlUpdate() {
    for (u32 p = 0; p < kMaxLocal; ++p) this->skinSteps[p] = 0;
    if (this->miiPlayer != kNoPlayer)
        return;  // a page under a layer still updates (0x80622610); a sideways Wii Remote's swap would reach MiiSelect
    for (u32 p = 0; p < menuLocalCount; ++p) {
        Input::RealControllerHolder *holder = PlayerHolder(p);
        if (holder == nullptr || holder->curController == nullptr)
            continue;
        Input::UIState &state = holder->uiinputStates[0];
        const u16 raw = state.rawButtons;
        const u16 pressed = raw & ~holder->uiinputStates[1].rawButtons;
        u16 previousButton, nextButton;
        switch (holder->curController->GetType()) {
            case GCN:
                previousButton = PAD::PAD_BUTTON_L;
                nextButton = PAD::PAD_BUTTON_R;
                break;
            case CLASSIC:
                previousButton = WPAD::WPAD_CL_TRIGGER_L;
                nextButton = WPAD::WPAD_CL_TRIGGER_R;
                break;
            case NUNCHUCK:
                previousButton = WPAD::WPAD_BUTTON_C;
                nextButton = WPAD::WPAD_BUTTON_Z;
                if (raw & WPAD::WPAD_BUTTON_C)
                    state.buttonActions &= ~0x100;
                break;
            default:
                previousButton = WPAD::WPAD_BUTTON_B;
                nextButton = WPAD::WPAD_BUTTON_A;
                state.buttonActions &= ~0x3;
                if (raw & WPAD::WPAD_BUTTON_2)
                    state.buttonActions |= 0x1;
                if (raw & WPAD::WPAD_BUTTON_1)
                    state.buttonActions |= 0x2;
                break;
        }
        const u16 step = pressed & (previousButton | nextButton);
        if (step == previousButton)
            this->skinSteps[p] = -1;
        else if (step == nextButton)
            this->skinSteps[p] = 1;
    }
}

// RR's skin cycle (PageAfterControlUpdate) for one player: the next or previous slot the character has,
// only while the preview stands idle, reloaded where that player's model is (LoadLocalPlayerModel for
// P1-4, Previews.cpp for P5-8) and back to slot 0 if the load fails.
void CharSelectPage::CycleSkin(u32 player, s32 direction) {
    Button &button = this->buttons[this->cursors[player].button];
    const CharacterId character = button.character;
    if (button.outfit >= 0)
        return;
    if (player < kGameLocal) {
        MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
        MenuDriverModel *model = modelMgr != nullptr && modelMgr->driverModels != nullptr ? modelMgr->driverModels->players[player].playerModel : static_cast<MenuDriverModel *>(nullptr);
        if (model == nullptr || model->state != MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE)
            return;
    } else if (!ExtPreviewIdle(player)) {
        return;
    }
    const u32 previousSlot = Pulsar::Driver::GetLocalPlayerSlot(player, character);
    u32 slot = previousSlot;
    for (u32 tries = 0; tries <= Pulsar::Driver::MAX_CUSTOM_CHARACTER_SLOTS; ++tries) {
        if (direction < 0)
            slot = slot == 0 ? Pulsar::Driver::MAX_CUSTOM_CHARACTER_SLOTS : slot - 1;
        else
            slot = slot == Pulsar::Driver::MAX_CUSTOM_CHARACTER_SLOTS ? 0 : slot + 1;
        if (Pulsar::Driver::characterTables[character][slot])
            break;
    }
    if (slot == previousSlot)
        return;
    Pulsar::Driver::SetLocalPlayerSlot(player, character, slot);
    const bool loaded = player < kGameLocal ? Pulsar::Driver::LoadLocalPlayerModel(player, character, slot) : ExtPreviewLoad(player, character, slot);
    if (!loaded)
        Pulsar::Driver::SetLocalPlayerSlot(player, character, 0);
    else
        Audio::RSARPlayer::PlaySoundById(direction < 0 ? SOUND_ID_LEFT_ARROW_PRESS : SOUND_ID_RIGHT_ARROW_PRESS, 0, 0);
    if (player == 0)
        this->SetIcon(button);
    this->SetPlateName(player);
    OS::Report("ss8 select: character player %u skin %#x slot %u%s\n", player + 1, character, Pulsar::Driver::GetLocalPlayerSlot(player, character), loaded ? "" : " failed");
}

// RR's shared-model pass (PageAfterControlUpdate): each character's shared model holds the skin of the
// P1-4 player standing on it. LoadDriverBRRES returns at once when that slot is already loaded.
void CharSelectPage::SyncSharedModels() {
    const u32 count = menuLocalCount < kGameLocal ? menuLocalCount : kGameLocal;
    for (u32 c = 0; c < Pulsar::Driver::CHARACTER_COUNT; ++c) {
        const CharacterId character = static_cast<CharacterId>(c);
        bool focused = false;
        for (u32 p = 0; p < count; ++p) focused |= this->buttons[this->cursors[p].button].character == character;
        u32 standingHud;
        const u32 slot = Pulsar::Driver::GetSharedModelSlot(character, focused, standingHud);
        if (!Pulsar::Driver::LoadDriverBRRES(character, slot))
            Pulsar::Driver::SetLocalPlayerSlot(standingHud, character, 0);
    }
}

void CharSelectPage::AfterControlUpdate() {
    this->UpdatePreviews();
    if (this->currentState != STATE_ACTIVE || this->leaving)
        return;
    ++this->frame;
    const u32 count = menuLocalCount;
    const bool roulette = this->rolling >= 0;
    if (roulette)
        this->Roulette();
    const bool input = this->miiPlayer == kNoPlayer && !this->miiResumed && !roulette;
    this->miiResumed = false;
    for (u32 p = 0; input && p < count; ++p) {
        const Input::RealControllerHolder *holder = PlayerHolder(p);
        if (holder == nullptr)
            continue;
        // ExpCharacterSelect::OnStartPress: P1's START, until everyone has confirmed (D108).
        if (p == 0 && this->leaveFrames < 0 && UIPressed(*holder, START_PRESS) && this->Roll())
            break;
        Cursor &cursor = this->cursors[p];
        if (!cursor.confirmed && this->skinSteps[p] != 0)
            this->CycleSkin(p, this->skinSteps[p]);
        if (UIPressed(*holder, BACK_PRESS)) {
            if (cursor.confirmed) {
                this->Unconfirm(p);
            } else if (p == 0) {
                this->Leave(PAGE_VS_MODE_SELECT, 1);
                return;
            }
        } else if (!cursor.confirmed && UIPressed(*holder, FORWARD_PRESS)) {
            if (this->buttons[cursor.button].outfit >= 0) {
                this->OpenMiiSelect(p);
                break;  // everyone else waits (D106)
            }
            this->Confirm(p);
        } else if (!cursor.confirmed) {
            const s32 direction = this->Direction(p, *holder);
            if (direction >= 0)
                this->Move(p, direction);
        }
    }
    this->SyncSharedModels();
    for (u32 i = 0; i < kButtons; ++i) this->UpdateButton(i);

    u32 confirmed = 0;
    for (u32 p = 0; p < count; ++p) confirmed += this->cursors[p].confirmed;
    if (confirmed < count) {
        this->leaveFrames = -1;
        return;
    }
    // CharacterSelect::OnButtonDriverClick waits for the OK animation, the same on every button, before
    // leaving (PushButton::GetAnimationFrameSize, 0x8083E0D4); a player's B in that time takes it back.
    if (this->leaveFrames < 0) {
        const AnimationGroup &ok = this->buttons[this->cursors[0].button].control.animator.GetAnimationGroupById(GROUP_OK);
        this->leaveFrames = static_cast<s32>(ok.animations[0].transform->GetFrameSize() / ok.animations[0].speed / ok.unknown_0x40);
        this->FillCpus();
    }
    if (this->leaveFrames-- == 0)
        this->Leave(Pulsar::UI::PULPAGE_SS8KARTSELECT, 0);
}

// D98: no CPU takes a local's pick. For the fill's one call SectionParams' count is the true one and P5-8's
// picks sit in their scenario slots, so the fill keeps all 8 and gives CPUs only the slots from 8 up.
// InitRace makes slots 4-7 local with the same picks (Entry.cpp).
void CharSelectPage::FillCpus() {
    SectionParams *params = SectionMgr::sInstance->sectionParams;
    RacedataScenario &scenario = Racedata::sInstance->menusScenario;
    for (u32 p = kGameLocal; p < menuLocalCount; ++p) scenario.players[p].characterId = ExtCharacter(p);
    const u32 gameCount = params->localPlayerCount;
    params->localPlayerCount = menuLocalCount;
    fillCpuCharacters();
    params->localPlayerCount = gameCount;
}

// RR's GetRandomEnabledCharacter (UI/ChangeCombo/ChangeCombo.cpp), which is file-static and finds a Mii in
// localPlayerMiis by hud, so is copied over this page's buttons: the drivers and Mii A and B in the character
// mask (its bits are the buttons, Restrictions::GetCharacterSlot), a Mii button giving the player's own Mii.
CharacterId CharSelectPage::RollCharacter(Random &random, u32 player, CharacterId avoid) const {
    CharacterId choices[kButtons];
    u32 count = 0;
    const u32 mask = Pulsar::Restrictions::GetCharacterMask();
    for (u32 i = 0; i < kButtons - 1; ++i) {  // never Mii C, as RR (BUTTON_MII_C)
        if (((mask >> i) & 1) == 0)
            continue;
        const CharacterId character = this->CharacterOn(player, i);
        if (character != avoid)
            choices[count++] = character;
    }
    return count == 0 ? avoid : choices[random.NextLimited(count)];
}

// RR's RandomizeCombo for every player (D108), nothing while a selection restriction is on: a character, a skin
// among its slots and a kart of its weight class, drawn as RR draws them. The kart is where the kart page
// and InitRace take it (D110). combos[] is not written: 2 wide, and read only by online code.
bool CharSelectPage::Roll() {
    if (Pulsar::Restrictions::IsCharacterRestrictionEnabled() || Pulsar::Restrictions::IsVehicleRestrictionEnabled())
        return false;
    Random random;
    SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 p = 0; p < menuLocalCount; ++p) {
        if (this->cursors[p].confirmed) {
            this->cursors[p].confirmed = false;
            this->PosePreview(p, false);
        }
        const CharacterId character = this->RollCharacter(random, p, CHARACTER_NONE);
        u32 slot = 0;
        if (static_cast<u32>(character) < Pulsar::Driver::CHARACTER_COUNT) {  // RR's loop would read past the table for a Mii
            u32 slots = 0;
            for (u32 s = 0; s <= Pulsar::Driver::MAX_CUSTOM_CHARACTER_SLOTS; ++s)
                if (Pulsar::Driver::characterTables[character][s] && random.NextLimited(++slots) == 0)
                    slot = s;
            Pulsar::Driver::SetLocalPlayerSlot(p, character, slot);
            if (p == 0)
                this->SetIcon(this->buttons[this->ButtonOf(character)]);  // P1's skin on the icon (D84)
        }
        const u32 weight = GetCharacterWeightClass(character);
        const u16 vehicles = Pulsar::Restrictions::GetVehicleMask(weight);
        u8 positions[Pulsar::Restrictions::VEHICLES_PER_WEIGHT];
        u32 enabled = 0;
        for (u32 v = 0; v < Pulsar::Restrictions::VEHICLES_PER_WEIGHT; ++v)
            if ((vehicles >> v) & 1)
                positions[enabled++] = v;
        const u32 position = enabled == 0 ? Pulsar::Restrictions::GetFirstEnabledVehiclePosition(weight) : positions[random.NextLimited(enabled)];
        const KartId kart = kartsSortedByWeight[weight][position];
        if (p < kGameLocal) {
            params->karts[p] = kart;
        } else {
            ExtPick &pick = extPicks[p - kGameLocal];
            pick.kart = kart;  // kept by Confirm's KartForCharacter: one of the character's 12
            pick.picked = true;
        }
        this->rolled[p] = character;
        OS::Report("ss8 select: character player %u roll %#x slot %u kart %#x\n", p + 1, character, slot, kart);
    }
    this->rolling = kRollFrames;
    return true;
}

// RR's roulette (ExpCharacterSelect::BeforeControlUpdate): each cursor hops to another character, the last hop onto
// the roll, with nobody's input read. Only that hop loads a preview (D115): a load per hop fragments the skin heaps.
// All confirm on one frame (D112), on the Mii row with the plate's Mii and no MiiSelect (D111).
void CharSelectPage::Roulette() {
    const s32 roulette = this->rolling--;
    if (roulette == 0) {
        for (u32 p = 0; p < menuLocalCount; ++p) this->Confirm(p);
        return;
    }
    if (roulette % kRollHop != 1)
        return;
    if (roulette == 1)
        this->FlushSkinModels();
    Random random;
    for (u32 p = 0; p < menuLocalCount; ++p) {
        const CharacterId character = roulette == 1 ? this->rolled[p] : this->RollCharacter(random, p, this->CharacterOn(p, this->cursors[p].button));
        const s32 button = this->ButtonOf(character);
        if (button >= 0)
            this->Hover(p, button, roulette == 1);
    }
}

// Before a roll's last hop loads every player's model, P1-4's skin models leave the structs heaps, so those loads
// start from the heap the page opened with, not one every earlier roll fragmented (D122). A player on one of RR's
// previews goes back on its character's shared model, as SetPlayerCharacter puts it, before RR drops the previews.
void CharSelectPage::FlushSkinModels() {
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    MenuDriverModelMgr *mgr = modelMgr != nullptr ? modelMgr->driverModels : static_cast<MenuDriverModelMgr *>(nullptr);
    if (mgr == nullptr)
        return;
    for (u32 hud = 0; hud < mgr->playerCount && hud < kGameLocal; ++hud) {
        MenuDriverModelMgr::Player &player = mgr->players[hud];
        const bool own = player.playerModel < mgr->models || player.playerModel >= mgr->models + mgr->modelCount;
        if (own && static_cast<u32>(player.id) < Pulsar::Driver::CHARACTER_COUNT)
            player.playerModel = &mgr->models[player.id];
    }
    Pulsar::Driver::DestroyLocalPlayerPreviews();
    for (u32 c = 0; c < Pulsar::Driver::CHARACTER_COUNT; ++c) Pulsar::Driver::LoadDriverBRRES(static_cast<CharacterId>(c), 0);
}

// The direction this frame's input moves the player's cursor in, -1 for none, as the menus read it:
// the D-pad's direction actions first, else the stick, each repeating while held.
s32 CharSelectPage::Direction(u32 player, const Input::RealControllerHolder &holder) {
    const Input::UIState &state = holder.uiinputStates[0];
    const u16 actions = state.buttonActions;
    const bool pad = (actions & ((1 << UP_PRESS) | (1 << DOWN_PRESS) | (1 << LEFT_PRESS) | (1 << RIGHT_PRESS))) != 0;
    const float axes[4] = {state.stickY, -state.stickY, -state.stickX, state.stickX};
    Cursor &cursor = this->cursors[player];
    s32 direction = -1;
    for (s32 d = 0; d < 4; ++d) {
        bool on;
        if (pad)
            on = (actions & (1 << (UP_PRESS + d))) != 0;
        else
            on = axes[d] > kStickStart || (axes[d] > kStickHold && cursor.held[d] >= 0);
        cursor.held[d] = on ? cursor.held[d] + 1 : -1;
        const s32 held = cursor.held[d];
        if (direction < 0 && (held == 0 || (held >= kRepeatFrames && (held - kRepeatFrames) % kRepeatFrames == 0)))
            direction = d;
    }
    return direction;
}

// One step up, down, left or right, wrapping on both axes as 0x6B's SetDistanceFunc(0) does, past any
// place with no button or a locked one.
void CharSelectPage::Move(u32 player, s32 direction) {
    static const s8 dCol[4] = {0, 0, -1, 1};
    static const s8 dRow[4] = {-1, 1, 0, 0};
    const Button &from = this->buttons[this->cursors[player].button];
    s32 col = from.col, row = from.row;
    for (u32 step = 0; step < kButtons; ++step) {
        col = (col + dCol[direction] + this->cols) % this->cols;
        row = (row + dRow[direction] + this->rows) % this->rows;
        for (u32 i = 0; i < kButtons; ++i) {
            if (this->buttons[i].col == col && this->buttons[i].row == row && !this->buttons[i].locked) {
                this->Hover(player, i);
                return;
            }
        }
    }
}

bool CharSelectPage::PlayerOn(u32 player, u32 button) const {
    return player < menuLocalCount && this->cursors[player].button == button;
}

// PushButton::HandleDeselect (0x805BE130) on the button left, if nobody else is on it, and HandleSelect
// (0x805BDFFC) on the one reached, with its sound; the plate names the hovered character
// (CharacterSelect::OnButtonDriverSelect, 0x8083E6E0).
void CharSelectPage::Hover(u32 player, u32 to, bool preview) {
    const u32 from = this->cursors[player].button;
    this->cursors[player].button = to;
    bool shared = false;
    for (u32 p = 0; p < kMaxLocal; ++p) shared |= this->PlayerOn(p, from);
    if (!shared) {
        AnimationGroup &select = this->buttons[from].control.animator.GetAnimationGroupById(GROUP_SELECT);
        if (select.curAnimation == ANIM_FREE_TO_SELECT)
            select.PlayAnimationAtPercent(ANIM_SELECT_TO_FREE, Remaining(select));
        else if (select.curAnimation == ANIM_SELECT)
            select.PlayAnimationAtFrame(ANIM_SELECT_TO_FREE, 0.0f);
    }
    LayoutUIControl &control = this->buttons[to].control;
    AnimationGroup &select = control.animator.GetAnimationGroupById(GROUP_SELECT);
    if (select.curAnimation == ANIM_FREE)
        select.PlayAnimationAtFrame(ANIM_FREE_TO_SELECT, 0.0f);
    else if (select.curAnimation == ANIM_SELECT_TO_FREE)
        select.PlayAnimationAtPercent(ANIM_FREE_TO_SELECT, Remaining(select));
    control.animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(0, 0.0f);
    control.PlaySound(SOUND_ID_BUTTON_SELECT, player);
    const CharacterId character = this->CharacterOn(player, to);
    this->SetPlateName(player);
    if (preview)
        this->RequestPreview(player);
    OS::Report("ss8 select: character player %u on %#x\n", player + 1, character);
}

// As PushButton::HandleClick (0x805BE358) and CharacterSelect::OnButtonDriverClick offline (0x8083DFA8):
// P1-4's pick goes where the vanilla page writes it, P5-8's to extPicks, with a kart InitRace can take.
void CharSelectPage::Confirm(u32 player) {
    Cursor &cursor = this->cursors[player];
    cursor.confirmed = true;
    LayoutUIControl &control = this->buttons[cursor.button].control;
    const CharacterId character = this->CharacterOn(player, cursor.button);
    control.animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(0, 0.0f);
    control.animator.GetAnimationGroupById(GROUP_OK).PlayAnimationAtFrame(0, 0.0f);
    control.PlaySound(SOUND_ID_BUTTON_PRESS, player);
    SectionParams *params = SectionMgr::sInstance->sectionParams;
    copyMiisToRacedata(params);
    this->PosePreview(player, true);
    if (player < kGameLocal) {
        params->characters[player] = character;
        RacedataPlayer &racer = Racedata::sInstance->menusScenario.players[player];
        racer.playerType = PLAYER_REAL_LOCAL;
        if (racer.characterId != character)
            racer.kartId = static_cast<KartId>(-1);
        racer.characterId = character;
    } else {
        ExtPick &pick = extPicks[player - kGameLocal];
        const KartId last = pick.picked ? pick.kart : static_cast<KartId>(GetCharacterWeightClass(character));
        pick.character = character;
        pick.kart = KartForCharacter(character, last);
        pick.picked = true;
    }
    if (player < kGameLocal && this->buttons[cursor.button].outfit >= 0) {
        miiVoice(*confirmVoiceOwner, this->PlayerMii(player), player);
        miiConfirmVoice(*confirmVoiceOwner, player);
    } else if (player < kGameLocal) {
        confirmVoice(*confirmVoiceOwner, player, character);
    } else if (this->buttons[cursor.button].outfit >= 0) {
        ExtMiiConfirmVoice(player, this->PlayerMii(player));
    } else {
        ExtConfirmVoice(player, character);
    }
    OS::Report("ss8 select: character player %u confirm %#x\n", player + 1, character);
}

// As CharacterSelect::OnBackPress for a confirmed player (0x8083EA3C-0x8083EAA8): the plate names the
// character again and the back sound plays.
void CharSelectPage::Unconfirm(u32 player) {
    Cursor &cursor = this->cursors[player];
    cursor.confirmed = false;
    this->PosePreview(player, false);
    this->PlaySound(SOUND_ID_BACK_PRESS, -1);
    OS::Report("ss8 select: character player %u unconfirm\n", player + 1);
}

// What PushButton::Update (0x805BDD98) and ButtonDriver's colours do each frame, for up to 8 players:
// the frame and all four corners take one player's colours at a time (D79), and each confirmed player's
// marker shows (D80). The corners are SetButtonColours' (0x807E4130) and ResetButtonColours' (0x807E3EFC).
void CharSelectPage::UpdateButton(u32 idx) {
    Button &button = this->buttons[idx];
    LayoutUIControl &control = button.control;
    AnimationGroup &select = control.animator.GetAnimationGroupById(GROUP_SELECT);
    AnimationGroup &loop = control.animator.GetAnimationGroupById(GROUP_LOOP);
    if (select.curAnimation != ANIM_FREE) {
        if (loop.animations[loop.curAnimation].transform != nullptr)
            loop.isActive = true;
        control.drawPriority = 10.0f;
    } else {
        loop.PlayAnimationAtFrameAndDisable(0, 30.0f);
        control.drawPriority = 0.0f;
    }

    u8 on[kMaxLocal];
    u32 n = 0;
    for (u32 p = 0; p < kMaxLocal; ++p) {
        if (this->PlayerOn(p, idx))
            on[n++] = p;
        SetVisible(button.markers[p], this->PlayerOn(p, idx) && this->cursors[p].confirmed);
    }
    if (button.base == nullptr || button.frame == nullptr)
        return;
    nw4r::lyt::Material *base = button.base->GetMaterial();
    nw4r::lyt::Material *frame = button.frame->GetMaterial();
    if (n == 0) {
        RGBA16 primary, secondary;
        ControlManipulator::GetHudSlotIdColor(kNoPlayer, &primary, &secondary);
        SetTev(frame, 0, primary.red, primary.green, primary.blue, primary.alpha);
        SetTev(frame, 1, secondary.red, secondary.green, secondary.blue, secondary.alpha);
        if (button.coloured) {
            SetTev(base, 0, 0xff, 0xff, 0xff, 0xff);
            SetTev(base, 1, 0xff, 0xff, 0xff, 0xff);
            for (u32 corner = 0; corner < 4; ++corner) button.base->SetVtxColor(corner, nw4r::ut::Color(0x14, 0x14, 0x14, 0xff));
            button.base->alpha = 0x80;
            button.coloured = false;
        }
        return;
    }
    RGBA16 primary, secondary;
    PlayerColours(on[(this->frame / kCursorFrames) % n], &primary, &secondary);
    SetTev(frame, 0, primary.red, primary.green, primary.blue, primary.alpha);
    SetTev(frame, 1, secondary.red, secondary.green, secondary.blue, secondary.alpha);
    SetTev(base, 0, 200, 200, 200, 240);
    SetTev(base, 1, 200, 200, 200, 240);
    const nw4r::ut::Color corner((primary.red + secondary.red) / 2, (primary.green + secondary.green) / 2, (primary.blue + secondary.blue) / 2, 0xb4);
    for (u32 i = 0; i < 4; ++i) button.base->SetVtxColor(i, corner);
    button.coloured = true;
}

void CharSelectPage::Leave(u32 next, u32 animDirection) {
    this->leaving = true;
    this->nextPageId = static_cast<PageId>(next);
    OS::Report("ss8 select: character page -> %#x\n", next);
    this->EndStateAnimated(animDirection, 0.0f);
}

}  // namespace SplitScreen8
#endif
