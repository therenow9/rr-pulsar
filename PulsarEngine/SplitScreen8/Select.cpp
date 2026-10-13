#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/Input/Controller.hpp>
#include <MarioKartWii/Input/ControllerHolder.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/RKSYS/RKSYSMgr.hpp>
#include <MarioKartWii/System/Random.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Menu/KartSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/Menu.hpp>
#include <MarioKartWii/UI/Page/Other/ModelRenderer.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Network/Settings/SelectionRestrictions.hpp>
#include <PulsarSystem.hpp>
#include <UI/UI.hpp>
#include <UI/TransmissionSelect/TransmissionSelect.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

struct SelectStep {
    u32 id;
    u32 prev;
    u32 next;
    const wchar_t *title;  // English only
    const char *name;
};
static const SelectStep kSteps[] = {
    {Pulsar::UI::PULPAGE_SS8KARTSELECT, Pulsar::UI::PULPAGE_SS8CHARSELECT, Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT, L"Vehicles", "kart"},
    {Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT, Pulsar::UI::PULPAGE_SS8KARTSELECT, Pulsar::UI::PULPAGE_SS8DRIFTSELECT, L"Transmission", "transmission"},
    {Pulsar::UI::PULPAGE_SS8DRIFTSELECT, Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT, PAGE_CUP_SELECT, L"Drift", "drift"},
};

// MultiKartSelect::OnTextChange's kart name (0x8084A338) and OnUpDownClick's confirm voice (0x8084A1C4), which
// returns at once for hud 4 or more.
typedef u32 (*KartBmgFn)(KartId);
static const KartBmgFn kartBmg = reinterpret_cast<KartBmgFn>(0x808334e0);
typedef bool (*KartVoiceFn)(void *, u32);
static const KartVoiceFn kartVoice = reinterpret_cast<KartVoiceFn>(0x808689dc);

const float kCellX[kGameLocal] = {-240.0f, -80.0f, 80.0f, 240.0f};
const float kKartPlateY[2] = {10.0f, -165.0f};
const float kKartPlateScale = 0.5f;
const float kKartPreviewDY = 71.0f;  // the texture's middle; the kart sits low in it
const float kKartPreviewScale = 0.45f;
const float kArrowDX = 42.0f;
const float kArrowDY = 48.0f;
const float kArrowScale = 0.2f;
const s16 kArrowBaseAlpha = 70;
const float kLightPeakScale = 1.78f;  // chara_light_01's height at a change's peak (the left arrow's 1.74)
const float kLightRestOverPeak = 0.7f;  // chara_light_02's height at rest, the frame's, over its peak: 70 / 100
const float kButtonDY[2] = {-5.0f, -45.0f};  // the transmission and drift buttons, from the plate's place
const float kButtonScale = 0.5f;
// UpDownControl's change sounds (0x8063FD8C, 0x8063FC6C) and click sound (+0x5C4, set at 0x8063EB3C).
const u32 kSoundKartLeft = 0x11;
const u32 kSoundKartRight = 0x12;
const u32 kSoundKartConfirm = 0x13;
// SectionParams::driftType as MultiDriftSelect::OnButtonClick writes it (0x8084B5EC).
const u32 kDriftManual = 1;
const u32 kDriftAuto = 2;
// The menus' hold repeat and stick thresholds, as CharSelect.cpp reads them.
const s32 kRepeatFrames = 15;
const float kStickStart = 0.67f;
const float kStickHold = 0.33f;

enum ArrowSide {
    ARROW_LEFT,
    ARROW_RIGHT
};

KartId KartForCharacter(CharacterId character, KartId kart) {
    for (u8 k = 0; k < 12; ++k)
        if (CharacterIdToKartIdByIdx(character, k) == kart)
            return kart;
    return static_cast<KartId>(GetCharacterWeightClass(character));
}

// Written where the vanilla page's confirm writes P1-4's karts.
static void WriteKarts() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 i = 0; i < kGameLocal; ++i) {
        RacedataPlayer &player = Racedata::sInstance->menusScenario.players[i];
        player.kartId = KartForCharacter(player.characterId, params->karts[i]);
    }
    // The kart page's last confirm (0x8084745C, no arguments) gives every CPU from the game's local count of 4 a random
    // kart, P5-8 included until InitRace makes them local (MenuLocalsLocal, Entry.cpp).
    Pages::KartSelect::func_8084745c();
}

// As MultiKartSelect::FillPossibleKarts lists them (0x8084A3F8): the weight class in KartId order, unlocked with its
// 2 (0x8084A45C), and RR's vehicle restriction, which offline allows every kart.
static u32 KartChoices(CharacterId character, KartId *choices) {
    u32 count = 0;
    for (u8 k = 0; k < 12; ++k) {
        const KartId kart = CharacterIdToKartIdByIdx(character, k);
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

const u32 kDriverKart = 0x14;  // MenuDriverModel's kart (0x805F3F30)

// As UpDownControl::Update colours its hud (0x8063F304-0x8063F5A0): the frame both colours, the arrow the first at
// no alpha over the second, the base the second at alpha 70 (the game's table, 0x80895CC0).
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

// A control loaded with kButtonAnims, placed and in the player's colours as UpDownControl::Update and PushButton::Update
// (0x805BDE2C) colour them; a button has no color_yajirushi.
static void PlaceControl(LayoutUIControl &control, u32 player, float x, float y, float scale) {
    PositionAndScale &base = control.positionAndscale[0];
    base.position.x = x;
    base.position.y = y;
    base.scale.x = scale;
    base.scale.z = scale;  // Vec2 names its second component z
    control.PositionFromBasePosition();
    RGBA16 primary, secondary;
    MenuPlayerColours(player, &primary, &secondary);
    RGBA16 clear = primary;
    clear.alpha = 0;
    RGBA16 single = secondary;
    single.alpha = kArrowBaseAlpha;
    SetPaneColours(control, "fuchi_pattern", primary, secondary);
    SetPaneColours(control, "color_yajirushi", clear, secondary);
    SetPaneColours(control, "color_base", single, single);
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
    void AfterControlUpdate() override;

private:
    void Leave(u32 next, u32 animDirection);
    void LoadCell(u32 player);
    void ActivateKarts();
    void ActivateButtons();
    void StepKart(u32 player, s32 step);
    void LightArrow(u32 player, s32 step);
    void RequestKart(u32 player);
    void UpdateKartPreviews();
    void UpdateSides(u32 player);
    s32 Direction(u32 player, const Input::RealControllerHolder &holder, bool vertical);
    void Roulette();
    void Ready(u32 player);
    void Unready(u32 player);
    void WriteButton(u32 player);

    const SelectStep &step;
    const bool kartStep;
    CtrlMenuPageTitleText *titleText;
    PageManipulatorManager manager;
    PageId nextPageId;
    bool ready[kMaxLocal];
    bool leaving;
    s32 leaveFrames;  // counts down once everyone is ready, -1 before
    s32 rolling;  // the kart roulette's frames left, -1 when none runs
    KartId karts[kMaxLocal];
    LayoutUIControl *previews[kMaxLocal];  // CharacterSelectModel, the kart's texture on model_set_p
    LayoutUIControl *plates[kMaxLocal];  // the kart page's
    LayoutUIControl *sides[kMaxLocal][2];  // the arrows on the kart page, the two buttons on the others
    u8 buttons[kMaxLocal];  // the button each player is on, 0 or 1
    s32 held[kMaxLocal][2];  // frames each direction has been held, -1 when not
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
        this->previews[p] = new LayoutUIControl;
        this->plates[p] = this->kartStep ? new LayoutUIControl : static_cast<LayoutUIControl *>(nullptr);
        for (u32 side = 0; side < 2; ++side) this->sides[p][side] = new LayoutUIControl;
    }
}

SelectPage::~SelectPage() {
    delete this->titleText;
    for (u32 p = 0; p < kMaxLocal; ++p) {
        delete this->previews[p];
        delete this->plates[p];
        for (u32 side = 0; side < 2; ++side) delete this->sides[p][side];
    }
}

void SelectPage::OnInit() {
    this->InitControlGroup(1 + (this->kartStep ? 4 : 3) * kMaxLocal);
    this->AddControl(0, *this->titleText, 0);
    this->titleText->Load(false);
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(this->step.title);
    this->titleText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
    for (u32 p = 0; p < kMaxLocal; ++p) this->LoadCell(p);
    // No control to select, so A and B are read from the holders. The bitfield keeps SectionPad's reconnect
    // overlay on P1-4, as the vanilla pages' mask does with the game's count of 4.
    this->manager.Init(1, false);
    this->SetManipulatorManager(this->manager);
    this->manager.UpdatePlayerBitfield2((1 << kGameLocal) - 1);
}

// A cell: the preview, then the plate and UpDownControl's arrows for 3-4 (0x80849A84) or MultiDriftSelect's buttons
// (0x8084B0A0), as plain controls: the vanilla ones join the page's 5-slot ControlsManipulatorManager (0x8063EFC8).
void SelectPage::LoadCell(u32 player) {
    const float x = kCellX[player % kGameLocal];
    const float y = kKartPlateY[player / kGameLocal];
    this->AddControl(1 + player, *this->previews[player], 0);
    LoadModelPreview(*this->previews[player], x, y + kKartPreviewDY, kKartPreviewScale);
    u32 next = 1 + kMaxLocal;
    if (this->kartStep) {
        this->AddControl(next + player, *this->plates[player], 0);
        LoadNamePlate(*this->plates[player], player, x, y, kKartPlateScale);
        ShowSkinToggleIcons(*this->plates[player], CONTROLLER_TYPE_NONE);  // the skin toggle buttons do nothing here
        next += kMaxLocal;
    }
    for (u32 side = 0; side < 2; ++side) {
        LayoutUIControl &control = *this->sides[player][side];
        this->AddControl(next + 2 * player + side, control, 0);
        ControlLoader loader(&control);
        if (!this->kartStep) {
            loader.Load("button", "drift_4", side == 0 ? "ButtonAuto" : "ButtonManual", kButtonAnims);
            PlaceControl(control, player, x, y + kButtonDY[side], kButtonScale);
        } else {
            if (side == ARROW_LEFT)
                loader.Load("control", "MachineArrowLeft", "ButtonArrowLeft4", kButtonAnims);
            else
                loader.Load("control", "MachineArrowRight", "ButtonArrowRight4", kButtonAnims);
            PlaceControl(control, player, x + (side == ARROW_LEFT ? -kArrowDX : kArrowDX), y + kArrowDY, kArrowScale);
            // A change's light peaks past a small arrow's frame. Each light pane animates only its scale or only its
            // size, so the other, set once here, brings the peak to the resting height (the draw re-animates each frame).
            nw4r::lyt::Pane *light = control.layout.GetPaneByName("chara_light_01");
            if (light != nullptr)
                light->size.z /= kLightPeakScale;
            light = control.layout.GetPaneByName("chara_light_02");
            if (light != nullptr)
                light->scale.z = kLightRestOverPeak;
        }
        StartAnimations(control, false);
    }
    // MultiTransmissionSelect::OnInit's texts; the drift buttons keep the layout's.
    if (this->step.id == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT) {
        this->sides[player][0]->SetMessage(Pulsar::UI::BMG_OUTSIDE_TRANSMISSION);
        this->sides[player][1]->SetMessage(Pulsar::UI::BMG_INSIDE_TRANSMISSION);
    }
}

void SelectPage::OnActivate() {
    this->nextPageId = PAGE_NONE;
    this->leaving = false;
    this->leaveFrames = -1;
    this->rolling = -1;
    for (u32 i = 0; i < kMaxLocal; ++i) this->ready[i] = false;
    OS::Report("ss8 select: %s page, %u players\n", this->step.name, menuLocalCount);
    const u32 count = menuLocalCount;
    if (!ExtPreviewsShow(count) || !ExtKartsShow(count))
        OS::Report("ss8 select: P5-8 kart previews failed\n");
    for (u32 player = 0; player < kMaxLocal; ++player) {
        const bool shown = player < count;
        this->previews[player]->isHidden = !shown;
        for (u32 side = 0; side < 2; ++side) {
            this->sides[player][side]->isHidden = !shown;
            this->held[player][side] = -1;
        }
    }
    if (!this->kartStep) {
        this->ActivateButtons();
    } else {
        this->ActivateKarts();
        if (kartRoulette)
            this->rolling = kRollFrames;
        kartRoulette = false;
    }
}

void SelectPage::OnDeactivate() {
    ExtKartsHide();
    ExtPreviewsHide();
}

// P1-4's karts load as a character click loads them (0x805F570C) each time the page opens from the character page, their once-per-character
// flag (+0x9) cleared first: a skin, which the flag does not see, can change the archive.
void SelectPage::ActivateKarts() {
    Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 player = 0; player < kMaxLocal; ++player) {
        const bool shown = player < menuLocalCount;
        this->plates[player]->isHidden = !shown;
        for (u32 side = 0; side < 2; ++side) StartAnimations(*this->sides[player][side], true);  // as for a player who has not chosen
        if (!shown)
            continue;
        const CharacterId character = PlayerCharacter(player);
        const KartId last = player < kGameLocal ? params->karts[player] : extPicks[player - kGameLocal].picked ? extPicks[player - kGameLocal].kart : static_cast<KartId>(-1);
        this->karts[player] = KartForCharacter(character, last);
        this->StepKart(player, 0);
        // As VehicleModelControl::InitSelf leaves the confirmed pose on open (0x805F3CD8), for a Back from a later page,
        // whose archive is the one loaded: a reload would free the on-kart animations the driver is using.
        MenuDriverModel *model = player < kGameLocal ? GameDriverModel(player) : static_cast<MenuDriverModel *>(nullptr);
        if (model != nullptr && model->state == kStateKartChosen) {
            model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
        } else if (player < kGameLocal && renderer != nullptr) {
            renderer->params[player].unknown_0x9 = 0;
            renderer->LoadKartModelsByCharacter(player, character);
        }
    }
}

// As MultiTransmissionSelect::OnActivate and MultiDriftSelect::OnActivate (0x8084B2E0): each player on their stored
// choice, under the kart they chose, in its confirmed pose.
void SelectPage::ActivateButtons() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    const bool transmission = this->step.id == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT;
    for (u32 player = 0; player < menuLocalCount; ++player) {
        bool first;
        if (transmission)
            first = Pulsar::UI::GetSelectedTransmission(player) == Pulsar::TRANSMISSION_OUTSIDE;
        else
            first = player < kGameLocal ? params->driftType[player] == kDriftAuto : Holder(*Input::Manager::sInstance, player).isDriftAuto != 0;
        this->buttons[player] = first ? 0 : 1;
        for (u32 side = 0; side < 2; ++side) StartAnimations(*this->sides[player][side], side == this->buttons[player]);
        this->karts[player] = player < kGameLocal ? params->karts[player] : extPicks[player - kGameLocal].kart;
        this->RequestKart(player);
        if (player >= kGameLocal)
            ExtKartPose(player, true);
    }
}

// A step of 0 keeps the kart if it is a choice, else takes the first.
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

// A change lights that arrow, as UpDownControl's HandleLeftPress and HandleRightPress do.
void SelectPage::LightArrow(u32 player, s32 step) {
    LayoutUIControl &arrow = *this->sides[player][step < 0 ? ARROW_LEFT : ARROW_RIGHT];
    arrow.animator.GetAnimationGroupById(GROUP_SELECT_IN).PlayAnimationAtFrame(0, 0.0f);
    arrow.animator.GetAnimationGroupById(GROUP_OK).PlayAnimationAtFrame(0, 0.0f);
    arrow.PlaySound(step < 0 ? kSoundKartLeft : kSoundKartRight, player);
}

void SelectPage::RequestKart(u32 player) {
    if (player < kGameLocal)
        return;
    const CharacterId character = PlayerCharacter(player);
    ExtKartRequest(player, character, SkinSlot(player, character), this->karts[player]);
}

// VehicleModelControl::OnUpdate for P1-4 (0x805F3CF8): once loaded (0x805F3DBC) the kart asked for and the driver
// on it, in the confirmed pose (0x805F3F0C) once chosen, as on every page after the kart page.
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
        const bool chosen = !this->kartStep || this->ready[player];
        if (model == nullptr || (chosen && model->state == kStateKartChosen))
            continue;
        *reinterpret_cast<KartId *>(reinterpret_cast<u8 *>(model) + kDriverKart) = this->karts[player];
        model->SwitchState(player, chosen ? static_cast<MenuDriverModel::State>(kStateKartChosen) : MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
    }
}

// UpDownButton::Update (0x8063FFB0): the loop runs while selected. An arrow hides once a ready player's OK has played
// (0x8063F614); a button's partner at the confirm, as MultiDriftSelect::OnButtonClick hides it (0x8084B620).
void SelectPage::UpdateSides(u32 player) {
    for (u32 side = 0; side < 2; ++side) {
        LayoutUIControl &control = *this->sides[player][side];
        UIAnimator &animator = control.animator;
        AnimationGroup &loop = animator.GetAnimationGroupById(GROUP_LOOP);
        if (animator.GetAnimationGroupById(GROUP_SELECT).curAnimation != ANIM_FREE) {
            if (loop.animations[loop.curAnimation].transform != nullptr)
                loop.isActive = true;
        } else {
            loop.PlayAnimationAtFrameAndDisable(0, 0.0f);
        }
        if (this->kartStep) {
            control.isHidden = this->ready[player] && animator.GetAnimationGroupById(GROUP_OK).curAnimation != 0;
        } else
            control.isHidden = this->ready[player] && side != this->buttons[player];
    }
}

// -1, 1 or 0, left or up first: the D-pad first, else the stick, each repeating while held (CharSelect.cpp's Direction
// for one axis).
s32 SelectPage::Direction(u32 player, const Input::RealControllerHolder &holder, bool vertical) {
    const Input::UIState &state = holder.uiinputStates[0];
    const u16 actions = state.buttonActions;
    const u32 first = vertical ? UP_PRESS : LEFT_PRESS;
    const bool pad = (actions & (3 << first)) != 0;
    const float toFirst = vertical ? state.stickY : -state.stickX;
    const float axes[2] = {toFirst, -toFirst};
    s32 direction = 0;
    for (u32 side = 0; side < 2; ++side) {
        bool on;
        if (pad)
            on = (actions & (1 << (first + side))) != 0;
        else
            on = axes[side] > kStickStart || (axes[side] > kStickHold && this->held[player][side] >= 0);
        s32 &held = this->held[player][side];
        held = on ? held + 1 : -1;
        if (direction == 0 && (held == 0 || (held >= kRepeatFrames && (held - kRepeatFrames) % kRepeatFrames == 0)))
            direction = side == 0 ? -1 : 1;
    }
    return direction;
}

// RR's kart roulette (ExpKartSelect::BeforeControlUpdate) for all: plates and arrows hop to the rolled kart, which the
// previews show throughout (a load per hop would reload P5-8's archives), then everyone confirms on one frame.
void SelectPage::Roulette() {
    const s32 roulette = this->rolling--;
    if (roulette == 0) {
        for (u32 player = 0; player < menuLocalCount; ++player) this->Ready(player);
        return;
    }
    if (roulette % kRollHop != 1)
        return;
    Random random;
    for (u32 player = 0; player < menuLocalCount; ++player) {
        KartId choices[12];
        const u32 count = KartChoices(PlayerCharacter(player), choices);
        KartId shown = this->karts[player];
        if (roulette != 1 && count > 1) {
            do shown = choices[random.NextLimited(count)];
            while (shown == this->karts[player]);
        }
        this->plates[player]->SetMessage(kartBmg(shown));
        this->LightArrow(player, random.NextLimited(2) == 0 ? -1 : 1);
    }
}

// MultiKartSelect::OnUpDownClick offline (0x8084A100): the click, the confirmed pose (0x8084A288) and the kart voice
// (0x8084A1C4); the other pages as PushButton::HandleClick (0x805BE358) and their OnButtonClick.
void SelectPage::Ready(u32 player) {
    this->ready[player] = true;
    OS::Report("ss8 select: %s player %u ready\n", this->step.name, player + 1);
    if (!this->kartStep) {
        LayoutUIControl &button = *this->sides[player][this->buttons[player]];
        button.animator.GetAnimationGroupById(GROUP_OK).PlayAnimationAtFrame(0, 0.0f);
        button.PlaySound(SOUND_ID_BUTTON_PRESS, player);
        this->WriteButton(player);
        return;
    }
    this->plates[player]->PlaySound(kSoundKartConfirm, player);
    for (u32 side = 0; side < 2; ++side) SelectControl(*this->sides[player][side], false);
    if (player < kGameLocal) {
        if (MenuDriverModel *model = GameDriverModel(player))
            model->SwitchState(player, static_cast<MenuDriverModel::State>(kStateKartChosen));
        kartVoice(*driverSoundManager, player);
    } else {
        ExtKartPose(player, true);
        ExtKartVoice(player, PlayerCharacter(player));
    }
}

// As MultiKartSelect::OnBackPress for a player who has chosen (0x80849FB8-0x8084A020), and MultiDriftSelect's
// (0x8084B850), whose partner button comes back.
void SelectPage::Unready(u32 player) {
    this->ready[player] = false;
    OS::Report("ss8 select: %s player %u unready\n", this->step.name, player + 1);
    if (this->kartStep) {
        if (player < kGameLocal) {
            if (MenuDriverModel *model = GameDriverModel(player))
                model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
        } else {
            ExtKartPose(player, false);
        }
        for (u32 side = 0; side < 2; ++side) SelectControl(*this->sides[player][side], true);
    }
    this->PlaySound(SOUND_ID_BACK_PRESS, -1);
}

// MultiTransmissionSelect's and MultiDriftSelect's OnButtonClick (0x8084B500), the first button outside or automatic.
// Drift goes to the holder the race reads (0x805944F4), to SectionParams for P1-4 and the license for P1, as vanilla.
void SelectPage::WriteButton(u32 player) {
    const bool first = this->buttons[player] == 0;
    OS::Report("ss8 select: %s player %u on %s\n", this->step.name, player + 1, first ? "first" : "second");
    if (this->step.id == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT) {
        Pulsar::UI::SetSelectedTransmission(player, first ? Pulsar::TRANSMISSION_OUTSIDE : Pulsar::TRANSMISSION_INSIDE);
        return;
    }
    Holder(*Input::Manager::sInstance, player).SetDriftType(first);
    if (player >= kGameLocal)
        return;
    const u32 driftType = first ? kDriftAuto : kDriftManual;
    SectionMgr::sInstance->sectionParams->driftType[player] = driftType;
    RKSYS::Mgr *rksys = RKSYS::Mgr::sInstance;
    if (player == 0 && rksys != nullptr && static_cast<s16>(rksys->curLicenseId) >= 0) {
        rksys->licenses[rksys->curLicenseId].defaultDrift = driftType;
        SectionMgr::sInstance->rksysRequester->NotifyNewLicenseContent();
    }
}

void SelectPage::AfterControlUpdate() {
    this->UpdateKartPreviews();
    for (u32 player = 0; player < menuLocalCount; ++player) this->UpdateSides(player);
    if (this->currentState != STATE_ACTIVE || this->leaving)
        return;
    if (this->rolling >= 0)
        this->Roulette();
    const u32 count = menuLocalCount;
    u32 readyCount = 0;
    for (u32 player = 0; player < count; ++player) {
        const Input::RealControllerHolder *holder = this->rolling >= 0 || SectionMgr::sInstance->curSection->skipsActions ? static_cast<Input::RealControllerHolder *>(nullptr) : PlayerHolder(player);
        const s32 step = holder != nullptr ? this->Direction(player, *holder, !this->kartStep) : 0;
        if (holder != nullptr && UIPressed(*holder, FORWARD_PRESS) && !this->ready[player]) {
            this->Ready(player);
        } else if (holder != nullptr && !this->ready[player] && step != 0) {
            if (this->kartStep) {
                this->StepKart(player, step);
                this->LightArrow(player, step);
            } else {
                const u8 to = step < 0 ? 0 : 1;  // Up the first button, Down the second, no wrap
                if (to != this->buttons[player]) {
                    SelectControl(*this->sides[player][this->buttons[player]], false);
                    SelectControl(*this->sides[player][to], true);
                    this->buttons[player] = to;
                    this->sides[player][to]->PlaySound(SOUND_ID_BUTTON_SELECT, player);
                }
            }
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
    // OnUpDownClick leaves with the delay its value text's OK takes (vtable +0x88, 0x8084A1E8), and a button's click
    // with its OK's; the plate has none, so the arrows' OK sets it. A player's B in that time takes it back.
    if (this->leaveFrames < 0) {
        const AnimationGroup &ok = this->sides[0][0]->animator.GetAnimationGroupById(GROUP_OK);
        this->leaveFrames = static_cast<s32>(ok.animations[0].transform->GetFrameSize() / ok.animations[0].speed / ok.unknown_0x40);
    }
    if (this->leaveFrames-- > 0)
        return;
    if (this->kartStep)
        WriteKarts();
    this->Leave(this->step.next, 0);
}

// Menu::LoadNextPageById stores a Menu caller's id as the next Menu's prevPageId (0x808377DC); these pages are not
// Menus, so cup select gets it here. The transmission page is skipped as RR's LoadMultiTransmissionSelectBeforeDrift.
void SelectPage::Leave(u32 next, u32 animDirection) {
    if (next == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT && Pulsar::UI::ShouldSkipTransmissionSelect(Pulsar::System::sInstance))
        next = this->kartStep ? Pulsar::UI::PULPAGE_SS8DRIFTSELECT : Pulsar::UI::PULPAGE_SS8KARTSELECT;
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

// VSModeSelect::OnButtonClick's "bl Menu::LoadNextPageById" for Team VS (0x80852A6C) and VS (0x80852A90); r3-r5 are
// the call's own arguments, and the code after it sets r0 and r3 before reading them.
static void VSModeNextPage(Pages::Menu &page, PageId id, PushButton &button) {
    page.LoadNextPageById(menuLocalCount > kGameLocal ? static_cast<PageId>(Pulsar::UI::PULPAGE_SS8CHARSELECT) : id, button);
}
kmCall(0x80852a6c, VSModeNextPage);
kmCall(0x80852a90, VSModeNextPage);

}  // namespace SplitScreen8
#endif
