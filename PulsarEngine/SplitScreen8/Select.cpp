#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/Input/Controller.hpp>
#include <MarioKartWii/Input/ControllerHolder.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Menu/Menu.hpp>
#include <MarioKartWii/UI/Page/Other/ModelRenderer.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Network/Settings/SelectionRestrictions.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The kart and drift pages of a 5-8 player game (D67, D71): each player readies with A and un-readies with B, P1's
// B with nobody ready goes back, and the last ready moves on. The kart page is D127's grid, the karts stepped by
// Left/Right and the skin toggle buttons (D142); the character page is CharSelect.cpp's. Only with --ss8-pages
// (SS8_PAGES); normal builds keep phase B's flow. docs/plans/m4-menu-flow.md, C3, C5a and C5b.

namespace SplitScreen8 {

struct SelectStep {
    u32 id;
    u32 prev;
    u32 next;
    const wchar_t *title;  // English only until C4's pages take the game's own titles
    const char *name;
};
static const SelectStep kSteps[] = {
    {Pulsar::UI::PULPAGE_SS8KARTSELECT, Pulsar::UI::PULPAGE_SS8CHARSELECT, Pulsar::UI::PULPAGE_SS8DRIFTSELECT, L"Vehicles", "kart"},
    {Pulsar::UI::PULPAGE_SS8DRIFTSELECT, Pulsar::UI::PULPAGE_SS8KARTSELECT, PAGE_CUP_SELECT, L"Drift", "drift"},
};

// The vanilla kart page's last confirm gives every CPU from the game's count (4, D60) a random kart
// (KartSelect, 0x8084745C); it reads no argument. P5-8 are among them until InitRace makes them local
// (MenuLocalsLocal, Entry.cpp).
typedef void (*FillCpusFn)();
static const FillCpusFn fillCpuKarts = reinterpret_cast<FillCpusFn>(0x8084745c);
typedef KartId (*KartByIdxFn)(CharacterId, u8);
static const KartByIdxFn characterIdToKartIdByIdx = reinterpret_cast<KartByIdxFn>(0x8081cef4);  // not in Kamek's externals
typedef void (*PositionFromBaseFn)(UIControl *);
static const PositionFromBaseFn positionFromBase = reinterpret_cast<PositionFromBaseFn>(0x8063d3cc);
typedef GX::TexObj *(*ModelTexObjFn)(Pages::ModelRenderer *, u32);
static const ModelTexObjFn modelTexObj = reinterpret_cast<ModelTexObjFn>(0x805f5a4c);
// MultiKartSelect::OnTextChange's kart name (0x8084A338) and OnUpDownClick's P1-4 confirm voice (0x8084A1C4), which
// returns at once for hud 4+; neither is in Kamek's externals.
typedef u32 (*KartBmgFn)(KartId);
static const KartBmgFn kartBmg = reinterpret_cast<KartBmgFn>(0x808334e0);
typedef bool (*KartVoiceFn)(void *, u32);
static const KartVoiceFn kartVoice = reinterpret_cast<KartVoiceFn>(0x808689dc);
static u8 *const *const voiceOwner = reinterpret_cast<u8 *const *>(0x809c4740);

// D127's grid: P1-4 over P5-8, columns at the layout's x, rows 175 apart, each kart above its plate at the size the
// decision page's renders showed. Their 0.65 was of the prototype's preview control; this CharacterSelectModel
// takes 0.45 for the same kart, its middle 48 above the plate.
const float kCellX[kGameLocal] = {-240.0f, -80.0f, 80.0f, 240.0f};
const float kKartPlateY[2] = {10.0f, -165.0f};
const float kKartPlateScale = 0.5f;
const float kKartPreviewDY = 71.0f;  // the texture's middle; the kart sits low in it
const float kKartPreviewScale = 0.45f;
// The vanilla arrows beside each kart (the 4P page's MachineArrowLeft/Right, ButtonArrowLeft4/Right4), smaller to
// fit a column.
const float kArrowDX = 42.0f;
const float kArrowDY = 48.0f;
const float kArrowScale = 0.32f;
const s16 kArrowBaseAlpha = 70;
// UpDownControl's sounds: a change's 0x11 and 0x12 (HandleLeftPress 0x8063FD8C, HandleRightPress 0x8063FC6C) and a
// click's 0x13 (+0x5C4, set in its constructor at 0x8063EB3C; MultiKartSelect keeps it).
const u32 kSoundKartLeft = 0x11;
const u32 kSoundKartRight = 0x12;
const u32 kSoundKartConfirm = 0x13;
// The menus' hold repeat and stick thresholds, as CharSelect.cpp reads them.
const s32 kRepeatFrames = 15;
const float kStickStart = 0.67f;
const float kStickHold = 0.33f;

// UpDownButton's animation names (0x808BF108, the same as PushButton's), so its group and animation ids hold.
static const char *kArrowAnims[] = {
    "Loop", "Loop", nullptr, "Select", "Free", "FreeToSelect", "Select", "SelectToFree", nullptr, "SelectIn", "SelectIn", "SelectStop", nullptr, "OK", "OK", "OKStop", nullptr, nullptr};
enum ArrowGroup {
    GROUP_LOOP,
    GROUP_SELECT,
    GROUP_SELECT_IN,
    GROUP_OK
};
enum ArrowSelect {
    ANIM_FREE,
    ANIM_FREE_TO_SELECT,
    ANIM_SELECT,
    ANIM_SELECT_TO_FREE
};
const u32 kAnimStop = 1;  // SelectIn's SelectStop and OK's OKStop
enum ArrowSide {
    ARROW_LEFT,
    ARROW_RIGHT
};

KartId KartForCharacter(CharacterId character, KartId kart) {
    for (u8 k = 0; k < 12; ++k)
        if (characterIdToKartIdByIdx(character, k) == kart)
            return kart;
    return static_cast<KartId>(GetCharacterWeightClass(character));
}

// P1-4's karts, from SectionParams as the kart page leaves them, written where the vanilla page's confirm
// writes them.
static void WriteKarts() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 i = 0; i < kGameLocal; ++i) {
        RacedataPlayer &player = Racedata::sInstance->menusScenario.players[i];
        player.kartId = KartForCharacter(player.characterId, params->karts[i]);
    }
    fillCpuKarts();
}

// A character's karts as MultiKartSelect::FillPossibleKarts lists them (0x8084A3F8): its weight class in KartId
// order, unlocked as that page asks (IsKartUnlocked with its 2, 0x8084A45C), and RR's vehicle restriction, which
// offline allows every kart (GetVehicleMask needs a friend room's PULSAR_VEHICLERESTRICT).
static u32 KartChoices(CharacterId character, KartId *choices) {
    u32 count = 0;
    for (u8 k = 0; k < 12; ++k) {
        const KartId kart = characterIdToKartIdByIdx(character, k);
        if (IsKartUnlocked(kart, static_cast<u32>(2)) && Pulsar::Restrictions::IsVehicleEnabled(kart))
            choices[count++] = kart;
    }
    return count;
}

static CharacterId PlayerCharacter(u32 player) {
    return player < kGameLocal ? SectionMgr::sInstance->sectionParams->characters[player] : ExtCharacter(player);
}

static MenuDriverModel *GameDriverModel(u32 player) {
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    return modelMgr != nullptr && modelMgr->driverModels != nullptr ? modelMgr->driverModels->players[player].playerModel : static_cast<MenuDriverModel *>(nullptr);
}

typedef void (*ShowModelFn)(Pages::ModelRenderer *, u32, bool, u32);
// VehicleModelControl::OnUpdate's per-frame calls (0x805F3DAC, 0x805F3E30); they end in
// MenuDriverModelMgr::TogglePlayerModel, which writes players[hud] (4 wide), so P1-4 only.
static const ShowModelFn showModel = reinterpret_cast<ShowModelFn>(0x805f59ac);
const u32 kDriverKart = 0x14;  // MenuDriverModel's kart (0x805F3F30)

// UpDownControl::Update's colours for its hud (0x8063F304-0x8063F5A0): each arrow's frame (fuchi_pattern) the
// player's two colours, its arrow (color_yajirushi) the first at no alpha over the second, and its base
// (color_base) the slot's single colour, which the game's table (0x80895CC0) makes the second at alpha 70.
static void SetPaneColours(LayoutUIControl &control, const char *name, const RGBA16 &first, const RGBA16 &second) {
    nw4r::lyt::Pane *pane = control.layout.GetPaneByName(name);
    nw4r::lyt::Material *material = pane != nullptr ? pane->GetMaterial() : static_cast<nw4r::lyt::Material *>(nullptr);
    if (material == nullptr)
        return;
    const RGBA16 *colours[2] = {&first, &second};
    for (u32 i = 0; i < 2; ++i) {
        material->tevColours[i].r = colours[i]->red;
        material->tevColours[i].g = colours[i]->green;
        material->tevColours[i].b = colours[i]->blue;
        material->tevColours[i].a = colours[i]->alpha;
    }
}

class SelectPage : public Page {
public:
    explicit SelectPage(const SelectStep &step);
    ~SelectPage() override;
    PageId GetNextPage() const override {
        return this->nextPageId;
    }
    void OnInit() override;
    void OnActivate() override;
    void OnDeactivate() override;
    void BeforeControlUpdate() override;
    void AfterControlUpdate() override;

private:
    void Leave(u32 next, u32 animDirection);
    void LoadKartCell(u32 player);
    void LoadArrow(u32 player, ArrowSide side);
    void ActivateKarts();
    void StepKart(u32 player, s32 step);
    void RequestKart(u32 player);
    void UpdateKartPreviews();
    void UpdateArrows(u32 player);
    void SelectArrows(u32 player, bool select);
    s32 KartDirection(u32 player, const Input::RealControllerHolder &holder);
    void Ready(u32 player);
    void Unready(u32 player);

    const SelectStep &step;
    const bool kartStep;
    CtrlMenuPageTitleText *titleText;
    PageManipulatorManager manager;
    PageId nextPageId;
    bool ready[kMaxLocal];
    bool leaving;
    s32 leaveFrames;  // counts down once everyone is ready on the kart page, -1 before
    // On the kart page:
    KartId karts[kMaxLocal];
    LayoutUIControl *previews[kMaxLocal];  // CharacterSelectModel, the kart's texture on model_set_p
    LayoutUIControl *plates[kMaxLocal];
    LayoutUIControl *arrows[kMaxLocal][2];
    s32 held[kMaxLocal][2];  // frames Left and Right have been held, -1 when not
    s8 toggleSteps[kMaxLocal];  // this frame's skin toggle press, -1, 0 or 1 (D142)
};

Page *NewSelectPage(u32 id) {
    if (id == Pulsar::UI::PULPAGE_SS8CHARSELECT)
        return NewCharSelectPage();
    for (u32 i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i)
        if (kSteps[i].id == id)
            return new SelectPage(kSteps[i]);
    return nullptr;
}

SelectPage::SelectPage(const SelectStep &step) : step(step), kartStep(step.id == Pulsar::UI::PULPAGE_SS8KARTSELECT) {
    this->nextPageId = PAGE_NONE;
    this->titleText = new CtrlMenuPageTitleText;
    for (u32 p = 0; p < kMaxLocal; ++p) {
        this->previews[p] = this->kartStep ? new LayoutUIControl : static_cast<LayoutUIControl *>(nullptr);
        this->plates[p] = this->kartStep ? new LayoutUIControl : static_cast<LayoutUIControl *>(nullptr);
        for (u32 side = 0; side < 2; ++side) this->arrows[p][side] = this->kartStep ? new LayoutUIControl : static_cast<LayoutUIControl *>(nullptr);
    }
}

SelectPage::~SelectPage() {
    delete this->titleText;
    for (u32 p = 0; p < kMaxLocal; ++p) {
        delete this->previews[p];
        delete this->plates[p];
        for (u32 side = 0; side < 2; ++side) delete this->arrows[p][side];
    }
}

void SelectPage::OnInit() {
    this->InitControlGroup(this->kartStep ? 1 + 4 * kMaxLocal : 1);
    this->AddControl(0, *this->titleText, 0);
    this->titleText->Load(false);
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(this->step.title);
    this->titleText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
    for (u32 p = 0; this->kartStep && p < kMaxLocal; ++p) this->LoadKartCell(p);
    // A page with no control to select takes a PageManipulatorManager; every player's A and B are read
    // from the holders. SectionPad's reconnect overlay watches P1-4, as the vanilla pages' mask does with
    // D60's count of 4; P5-8's box is D69's (C6).
    this->manager.Init(1, false);
    this->SetManipulatorManager(this->manager);
    this->manager.UpdatePlayerBitfield2((1 << kGameLocal) - 1);
}

// A cell of D127's grid: the preview, then the plate over it, then the arrows.
void SelectPage::LoadKartCell(u32 player) {
    const float x = kCellX[player % kGameLocal];
    const float y = kKartPlateY[player / kGameLocal];
    this->AddControl(1 + player, *this->previews[player], 0);
    LoadModelPreview(*this->previews[player], x, y + kKartPreviewDY, kKartPreviewScale);
    this->AddControl(1 + kMaxLocal + player, *this->plates[player], 0);
    LoadNamePlate(*this->plates[player], player, x, y, kKartPlateScale);
    this->LoadArrow(player, ARROW_LEFT);
    this->LoadArrow(player, ARROW_RIGHT);
}

// UpDownControl::Load's buttons (0x8063ECF8-0x8063EDC8) as MultiKartSelect::CreateControl names them for 3-4
// players (0x80849A84), coloured as UpDownControl::Update colours them. Plain LayoutUIControls, since an
// UpDownControl joins the page's 5-slot ControlsManipulatorManager (0x8063EFC8), which cannot serve 8.
void SelectPage::LoadArrow(u32 player, ArrowSide side) {
    LayoutUIControl &arrow = *this->arrows[player][side];
    this->AddControl(1 + 2 * kMaxLocal + 2 * player + side, arrow, 0);
    ControlLoader loader(&arrow);
    if (side == ARROW_LEFT)
        loader.Load("control", "MachineArrowLeft", "ButtonArrowLeft4", kArrowAnims);
    else
        loader.Load("control", "MachineArrowRight", "ButtonArrowRight4", kArrowAnims);
    PositionAndScale &base = arrow.positionAndscale[0];
    base.position.x = kCellX[player % kGameLocal] + (side == ARROW_LEFT ? -kArrowDX : kArrowDX);
    base.position.y = kKartPlateY[player / kGameLocal] + kArrowDY;
    base.scale.x = kArrowScale;
    base.scale.z = kArrowScale;  // Vec2 names its second component z
    positionFromBase(&arrow);
    RGBA16 primary, secondary;
    MenuPlayerColours(player, &primary, &secondary);
    RGBA16 clear = primary;
    clear.alpha = 0;
    RGBA16 single = secondary;
    single.alpha = kArrowBaseAlpha;
    SetPaneColours(arrow, "fuchi_pattern", primary, secondary);
    SetPaneColours(arrow, "color_yajirushi", clear, secondary);
    SetPaneColours(arrow, "color_base", single, single);
}

void SelectPage::OnActivate() {
    this->nextPageId = PAGE_NONE;
    this->leaving = false;
    this->leaveFrames = -1;
    for (u32 i = 0; i < kMaxLocal; ++i) this->ready[i] = false;
    OS::Report("ss8 select: %s page, %u players\n", this->step.name, menuLocalCount);
    if (this->kartStep)
        this->ActivateKarts();
}

void SelectPage::OnDeactivate() {
    if (!this->kartStep)
        return;
    ExtKartsHide();
    ExtPreviewsHide();
}

// Each player starts on their last kart if their character has it, else the class's first choice. P1-4's karts
// load as a character click loads them (LoadKartModelsByCharacter, 0x805F570C), each time the page opens: their
// once-per-character flag (+0x9) is cleared first, since a skin, which the flag does not see, can change the archive.
void SelectPage::ActivateKarts() {
    const u32 count = menuLocalCount;
    if (!ExtPreviewsShow(count) || !ExtKartsShow(count))
        OS::Report("ss8 select: P5-8 kart previews failed\n");
    Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 player = 0; player < kMaxLocal; ++player) {
        const bool shown = player < count;
        this->previews[player]->isHidden = !shown;
        this->plates[player]->isHidden = !shown;
        // Every arrow group as UpDownButton::Init (0x8063FF18) starts it, then selected, as for a player who has not chosen.
        for (u32 side = 0; side < 2; ++side) {
            LayoutUIControl &arrow = *this->arrows[player][side];
            arrow.isHidden = !shown;
            arrow.animator.GetAnimationGroupById(GROUP_LOOP).PlayAnimationAtFrameAndDisable(0, 0.0f);
            arrow.animator.GetAnimationGroupById(GROUP_SELECT).PlayAnimationAtFrame(ANIM_SELECT, 0.0f);
            arrow.animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(kAnimStop, 0.0f);
            arrow.animator.GetAnimationGroupById(GROUP_OK).PlayAnimationAtFrame(kAnimStop, 0.0f);
            this->held[player][side] = -1;
        }
        if (!shown)
            continue;
        const CharacterId character = PlayerCharacter(player);
        const KartId last = player < kGameLocal ? params->karts[player] : extPicks[player - kGameLocal].picked ? extPicks[player - kGameLocal].kart : static_cast<KartId>(-1);
        this->karts[player] = KartForCharacter(character, last);
        this->StepKart(player, 0);
        // VehicleModelControl::InitSelf takes each hud out of the confirmed pose as the page opens (0x805F3CD8), which a
        // Back from the drift page needs.
        MenuDriverModel *model = player < kGameLocal ? GameDriverModel(player) : static_cast<MenuDriverModel *>(nullptr);
        if (model != nullptr && model->state == kStateKartChosen)
            model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
        if (player < kGameLocal && renderer != nullptr) {
            renderer->params[player].unknown_0x9 = 0;
            renderer->LoadKartModelsByCharacter(player, character);
        }
    }
}

// A step of 0 keeps the kart if it is a choice, else takes the first. The plate names the kart, as OnTextChange
// does (0x8084A338).
void SelectPage::StepKart(u32 player, s32 step) {
    KartId choices[12];
    const CharacterId character = PlayerCharacter(player);
    const s32 count = static_cast<s32>(KartChoices(character, choices));
    if (count == 0)
        return;
    s32 at = 0;
    for (s32 k = 0; k < count; ++k)
        if (choices[k] == this->karts[player])
            at = k;
    at = ((at + step) % count + count) % count;
    this->karts[player] = choices[at];
    if (player < kGameLocal) {
        SectionMgr::sInstance->sectionParams->karts[player] = choices[at];
    } else {
        ExtPick &pick = extPicks[player - kGameLocal];
        pick.character = character;
        pick.kart = choices[at];
        pick.picked = true;
    }
    this->plates[player]->SetMessage(kartBmg(choices[at]));
    this->RequestKart(player);
    OS::Report("ss8 select: kart player %u on %#x\n", player + 1, choices[at]);
}

void SelectPage::RequestKart(u32 player) {
    if (player < kGameLocal)
        return;
    const CharacterId character = PlayerCharacter(player);
    ExtKartRequest(player, character, SkinSlot(player, character), this->karts[player]);
}

// What VehicleModelControl::OnUpdate does each frame for P1-4 (0x805F3CF8): the model shown, and once its karts
// have loaded (params' isVisible, 0x805F3DBC) the kart asked for and the driver put on it, unless it is in the
// confirmed pose (state 3, 0x805F3F0C). Then every player's texture on their preview.
void SelectPage::UpdateKartPreviews() {
    Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    if (renderer == nullptr || modelMgr == nullptr || modelMgr->driverModels == nullptr)
        return;
    for (u32 player = 0; player < menuLocalCount; ++player) {
        if (player >= kGameLocal) {
            SetPreviewTexture(*this->previews[player], ExtPreviewTexObj(player));
            continue;
        }
        showModel(renderer, player, false, 0);
        SetPreviewTexture(*this->previews[player], modelTexObj(renderer, player));
        if (!renderer->params[player].isVisible)
            continue;
        showModel(renderer, player, true, 1);
        renderer->RequestKartModel(player, this->karts[player]);
        MenuDriverModel *model = modelMgr->driverModels->players[player].playerModel;
        if (model == nullptr || model->state == kStateKartChosen)
            continue;
        *reinterpret_cast<KartId *>(reinterpret_cast<u8 *>(model) + kDriverKart) = this->karts[player];
        model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
    }
}

// UpDownButton::Update (0x8063FFB0): the arrow's loop runs while it is selected, and it shows while the player can
// change the kart or its OK still plays. UpDownControl::Update gives each arrow that "can change" (+0x174,
// 0x8063F614-0x8063F678) from the control's own (+0x211, which a confirm clears), the page's karts wrapping.
void SelectPage::UpdateArrows(u32 player) {
    for (u32 side = 0; side < 2; ++side) {
        LayoutUIControl &arrow = *this->arrows[player][side];
        UIAnimator &animator = arrow.animator;
        AnimationGroup &loop = animator.GetAnimationGroupById(GROUP_LOOP);
        if (animator.GetAnimationGroupById(GROUP_SELECT).curAnimation != ANIM_FREE) {
            if (loop.animations[loop.curAnimation].transform != nullptr)
                loop.isActive = true;
        } else {
            loop.PlayAnimationAtFrameAndDisable(0, 0.0f);
        }
        arrow.isHidden = this->ready[player] && animator.GetAnimationGroupById(GROUP_OK).curAnimation != 0;
    }
}

// UpDownButton::ToggleSelect (0x8064019C) on both arrows: a player who chooses leaves them, as the 4P page's
// confirm does by taking the player's holder off the control (InitHolders, 0x8084A164), and one who un-chooses
// comes back to them.
void SelectPage::SelectArrows(u32 player, bool select) {
    for (u32 side = 0; side < 2; ++side) {
        UIAnimator &animator = this->arrows[player][side]->animator;
        AnimationGroup &group = animator.GetAnimationGroupById(GROUP_SELECT);
        const u16 size = group.animations[group.curAnimation].transform->GetFrameSize();
        const float remaining = 1.0f - group.curFrame / static_cast<float>(size);
        if (select && group.curAnimation == ANIM_FREE)
            group.PlayAnimationAtFrame(ANIM_FREE_TO_SELECT, 0.0f);
        else if (select && group.curAnimation == ANIM_SELECT_TO_FREE)
            group.PlayAnimationAtPercent(ANIM_FREE_TO_SELECT, remaining);
        else if (!select && group.curAnimation == ANIM_FREE_TO_SELECT)
            group.PlayAnimationAtPercent(ANIM_SELECT_TO_FREE, remaining);
        else if (!select && group.curAnimation == ANIM_SELECT)
            group.PlayAnimationAtFrame(ANIM_SELECT_TO_FREE, 0.0f);
        if (select)
            animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(0, 0.0f);
    }
}

// Left or Right this frame, -1, 1 or 0, as the menus read a direction: the D-pad first, else the stick, each
// repeating while held (CharSelect.cpp's Direction for two directions).
s32 SelectPage::KartDirection(u32 player, const Input::RealControllerHolder &holder) {
    const Input::UIState &state = holder.uiinputStates[0];
    const u16 actions = state.buttonActions;
    const bool pad = (actions & ((1 << LEFT_PRESS) | (1 << RIGHT_PRESS))) != 0;
    const float axes[2] = {-state.stickX, state.stickX};
    s32 direction = 0;
    for (u32 side = 0; side < 2; ++side) {
        bool on;
        if (pad)
            on = (actions & (1 << (LEFT_PRESS + side))) != 0;
        else
            on = axes[side] > kStickStart || (axes[side] > kStickHold && this->held[player][side] >= 0);
        s32 &held = this->held[player][side];
        held = on ? held + 1 : -1;
        if (direction == 0 && (held == 0 || (held >= kRepeatFrames && (held - kRepeatFrames) % kRepeatFrames == 0)))
            direction = side == ARROW_LEFT ? -1 : 1;
    }
    return direction;
}

// As MultiKartSelect::OnUpDownClick offline (0x8084A100): the click's sound, the kart written (StepKart already
// has), the driver in the confirmed pose (0x8084A288) and the kart's voice for the player's character (0x8084A1C4).
void SelectPage::Ready(u32 player) {
    this->ready[player] = true;
    OS::Report("ss8 select: %s player %u ready\n", this->step.name, player + 1);
    if (!this->kartStep)
        return;
    this->plates[player]->PlaySound(kSoundKartConfirm, player);
    this->SelectArrows(player, false);
    if (player < kGameLocal) {
        if (MenuDriverModel *model = GameDriverModel(player))
            model->SwitchState(player, static_cast<MenuDriverModel::State>(kStateKartChosen));
        kartVoice(*voiceOwner, player);
    } else {
        ExtKartPose(player, true);
        ExtKartVoice(player, PlayerCharacter(player));
    }
}

// As MultiKartSelect::OnBackPress for a player who has chosen (0x80849FB8-0x8084A020): the driver back in the kart
// pose, the arrows back, the back sound.
void SelectPage::Unready(u32 player) {
    this->ready[player] = false;
    OS::Report("ss8 select: %s player %u unready\n", this->step.name, player + 1);
    if (!this->kartStep)
        return;
    if (player < kGameLocal) {
        if (MenuDriverModel *model = GameDriverModel(player))
            model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
    } else {
        ExtKartPose(player, false);
    }
    this->SelectArrows(player, true);
    this->PlaySound(SOUND_ID_BACK_PRESS, -1);
}

// The skin toggle buttons, read before the page's actions as the character page reads them (D81, D142), with
// each player's toggle icons on their plate.
void SelectPage::BeforeControlUpdate() {
    for (u32 p = 0; p < kMaxLocal; ++p) this->toggleSteps[p] = 0;
    if (!this->kartStep)
        return;
    for (u32 p = 0; p < menuLocalCount; ++p) {
        Input::RealControllerHolder *holder = PlayerHolder(p);
        if (holder == nullptr || holder->curController == nullptr)
            continue;
        this->toggleSteps[p] = static_cast<s8>(SkinToggleStep(*holder));
        ShowSkinToggleIcons(*this->plates[p], holder->curController->GetType());
    }
}

void SelectPage::AfterControlUpdate() {
    if (this->kartStep) {
        this->UpdateKartPreviews();
        for (u32 player = 0; player < menuLocalCount; ++player) this->UpdateArrows(player);
    }
    if (this->currentState != STATE_ACTIVE || this->leaving)
        return;
    const u32 count = menuLocalCount;
    u32 readyCount = 0;
    for (u32 player = 0; player < count; ++player) {
        const Input::RealControllerHolder *holder = PlayerHolder(player);
        const s32 direction = holder != nullptr && this->kartStep ? this->KartDirection(player, *holder) : 0;
        const s32 step = direction != 0 ? direction : this->toggleSteps[player];
        if (holder != nullptr && UIPressed(*holder, FORWARD_PRESS) && !this->ready[player]) {
            this->Ready(player);
        } else if (holder != nullptr && this->kartStep && !this->ready[player] && step != 0) {
            this->StepKart(player, step);
            LayoutUIControl &arrow = *this->arrows[player][step < 0 ? ARROW_LEFT : ARROW_RIGHT];
            arrow.animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(0, 0.0f);
            arrow.animator.GetAnimationGroupById(GROUP_OK).PlayAnimationAtFrame(0, 0.0f);
            arrow.PlaySound(step < 0 ? kSoundKartLeft : kSoundKartRight, player);
        } else if (holder != nullptr && UIPressed(*holder, BACK_PRESS)) {
            if (this->ready[player]) {
                this->Unready(player);
            } else if (player == 0) {
                this->Leave(this->step.prev, 1);
                return;
            }
        }
        if (this->ready[player])
            ++readyCount;
    }
    if (readyCount < count) {
        this->leaveFrames = -1;
        return;
    }
    // OnUpDownClick leaves with the delay its value text's OK takes (vtable +0x88, 0x8084A1E8); the plate has none,
    // so the arrows' OK sets it. A player's B in that time takes it back.
    if (this->kartStep && this->leaveFrames < 0) {
        const AnimationGroup &ok = this->arrows[0][ARROW_LEFT]->animator.GetAnimationGroupById(GROUP_OK);
        this->leaveFrames = static_cast<s32>(ok.animations[0].transform->GetFrameSize() / ok.animations[0].speed / ok.unknown_0x40);
    }
    if (this->kartStep && this->leaveFrames-- > 0)
        return;
    if (this->kartStep)
        WriteKarts();
    this->Leave(this->step.next, 0);
}

// Menu::LoadNextPageById (0x80837720) stores a Menu caller's id as the next Menu's prevPageId (0x808377DC),
// where that Menu's Back goes (LoadPrevPage). These pages are not Menus, so cup select gets it here.
void SelectPage::Leave(u32 next, u32 animDirection) {
    this->leaving = true;
    this->nextPageId = static_cast<PageId>(next);
    if (next == PAGE_CUP_SELECT && animDirection == 0) {
        Page *cupSelect = SectionMgr::sInstance->curSection->pages[PAGE_CUP_SELECT];
        if (cupSelect != nullptr)
            static_cast<Pages::Menu *>(cupSelect)->prevPageId = static_cast<PageId>(this->step.id);
    }
    OS::Report("ss8 select: %s page -> %#x\n", this->step.name, next);
    this->EndStateAnimated(animDirection, 0.0f);
}

// VSModeSelect::OnButtonClick's two "bl Menu::LoadNextPageById" with r4 = 0x6B: Team VS (0x80852A6C,
// the team bit set) and VS (0x80852A90, cleared). r3-r5 are the call's own arguments, and the code after
// it sets r0 and r3 before reading them. A 5-8 player game opens the character page in both; the team bit
// is not read until phase D.
static void VSModeNextPage(Pages::Menu &page, PageId id, PushButton &button) {
    page.LoadNextPageById(menuLocalCount > kGameLocal ? static_cast<PageId>(Pulsar::UI::PULPAGE_SS8CHARSELECT) : id, button);
}
kmCall(0x80852a6c, VSModeNextPage);
kmCall(0x80852a90, VSModeNextPage);

}  // namespace SplitScreen8
#endif
