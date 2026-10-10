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
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Other/ModelRenderer.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuCharacterSelect.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
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
const u32 kButtons = 24;  // CtrlMenuCharacterSelect::buttonIdToCharacterId; the Mii row is C4c's
// LoadButton names a button's control "CharacterSelect%d_%d" from categoryCount (+0x1A8, 4) and
// idx / (2 * categoryCount) (0x807E29B8-0x807E29CC), so each control holds Button0-7.
const u32 kButtonsPerGroup = 8;
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

// 0x80868ABC's driver path for P5-8, on handles of their own: the player's last voice stopped, the
// character's confirm picker (DriverSoundManager + character * 0xC0 + 0x30) started into the handle
// through its vtable +0xC, then the volume. A Mii takes another path (0x80868C20), the Mii row's (C4c).
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
    };

    void LoadButton(u32 idx);
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
    s32 ButtonOf(CharacterId character) const;
    s32 Direction(u32 player, const Input::RealControllerHolder &holder);
    void Move(u32 player, s32 direction);
    void Hover(u32 player, u32 to);
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
};

Page *NewCharSelectPage() {
    return new CharSelectPage;
}

CharSelectPage::CharSelectPage() {
    this->nextPageId = PAGE_NONE;
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
    button.character = CtrlMenuCharacterSelect::buttonIdToCharacterId[idx];
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
// shows P1's skin (D84), so P1's skin toggle sets it again.
void CharSelectPage::SetIcon(Button &button) {
    LayoutUIControl &control = button.control;
    const char *icon = GetCharacterIconPaneName(button.character);
    static const char *const iconPanes[] = {"chara", "chara_shadow", "chara_light_01", "chara_light_02", "chara_c_down"};
    for (u32 i = 0; i < sizeof(iconPanes) / sizeof(iconPanes[0]); ++i) control.SetPicturePane(iconPanes[i], icon);
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
// give each player their own skin and model (Driver/LocalPlayerSkins.cpp), P5-8 through Previews.cpp.
void CharSelectPage::RequestPreview(u32 player) {
    const CharacterId character = this->buttons[this->cursors[player].button].character;
    if (player < kGameLocal) {
        Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
        if (renderer != nullptr)
            renderer->RequestCharacterModel(player, character);
    } else if (!ExtPreviewLoad(player, character, Pulsar::Driver::GetLocalPlayerSlot(player, character))) {
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

// The plate names the character the cursor is on in that player's skin (RR's names[hud], Driver/CustomCharacters.cpp).
void CharSelectPage::SetPlateName(u32 player) {
    const CharacterId character = this->buttons[this->cursors[player].button].character;
    this->plates[player]->SetMessage(Pulsar::UI::GetCharacterSlotNameBMGId(character, Pulsar::Driver::GetLocalPlayerSlot(player, character), false));
}

s32 CharSelectPage::ButtonOf(CharacterId character) const {
    for (u32 i = 0; i < kButtons; ++i)
        if (this->buttons[i].character == character)
            return i;
    return -1;
}

// P1-4 start on SectionParams' last picks, as CtrlMenuCharacterSelect::InitSelf (0x807E2F34); P5-8 on
// theirs or D62's default. A last pick that is a Mii (the Mii row is C4c's) starts on D62's default.
void CharSelectPage::OnActivate() {
    this->nextPageId = PAGE_NONE;
    this->leaving = false;
    this->leaveFrames = -1;
    this->frame = 0;
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
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
    OS::Report("ss8 select: character page, %u players\n", menuLocalCount);
    for (u32 p = 0; p < menuLocalCount; ++p) OS::Report("ss8 select: character player %u on %#x\n", p + 1, this->buttons[this->cursors[p].button].character);
}

void CharSelectPage::OnDeactivate() {
    ExtPreviewsHide();
}

// RR's skin toggle input (PageBeforeControlUpdate, Driver/CustomCharacters.cpp), which runs only on the
// vanilla page, for all 8 (D81): L and R on GameCube and Classic, C and Z on a Nunchuk, B and A on a
// sideways Wii Remote, whose 2 and 1 then become its A and B so that A toggles rather than confirms.
void CharSelectPage::BeforeControlUpdate() {
    for (u32 p = 0; p < kMaxLocal; ++p) this->skinSteps[p] = 0;
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
    for (u32 p = 0; p < count; ++p) {
        const Input::RealControllerHolder *holder = PlayerHolder(p);
        if (holder == nullptr)
            continue;
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
// place with no button.
void CharSelectPage::Move(u32 player, s32 direction) {
    static const s8 dCol[4] = {0, 0, -1, 1};
    static const s8 dRow[4] = {-1, 1, 0, 0};
    const Button &from = this->buttons[this->cursors[player].button];
    s32 col = from.col, row = from.row;
    for (u32 step = 0; step < kButtons; ++step) {
        col = (col + dCol[direction] + this->cols) % this->cols;
        row = (row + dRow[direction] + this->rows) % this->rows;
        for (u32 i = 0; i < kButtons; ++i) {
            if (this->buttons[i].col == col && this->buttons[i].row == row) {
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
void CharSelectPage::Hover(u32 player, u32 to) {
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
    const CharacterId character = this->buttons[to].character;
    this->SetPlateName(player);
    this->RequestPreview(player);
    OS::Report("ss8 select: character player %u on %#x\n", player + 1, character);
}

// As PushButton::HandleClick (0x805BE358) and CharacterSelect::OnButtonDriverClick offline (0x8083DFA8):
// P1-4's pick goes where the vanilla page writes it, P5-8's to extPicks, with a kart InitRace can take.
void CharSelectPage::Confirm(u32 player) {
    Cursor &cursor = this->cursors[player];
    cursor.confirmed = true;
    LayoutUIControl &control = this->buttons[cursor.button].control;
    const CharacterId character = this->buttons[cursor.button].character;
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
    if (player < kGameLocal)
        confirmVoice(*confirmVoiceOwner, player, character);
    else
        ExtConfirmVoice(player, character);
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
