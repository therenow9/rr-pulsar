#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/rvl/OS/OSCache.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/WPAD.hpp>
#include <core/rvl/dvd/dvd.hpp>
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
#include <MarioKartWii/UI/Page/Menu/BattleKartSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/CharacterSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/CourseSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/DriftSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/KartSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/MultiDriftSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/MultiKartSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/Menu.hpp>
#include <MarioKartWii/UI/Page/Menu/MultiPlayerTeamSelect.hpp>
#include <MarioKartWii/UI/Page/Menu/StartRace.hpp>
#include <MarioKartWii/UI/Page/Menu/VSTeamsView.hpp>
#include <MarioKartWii/UI/Page/Other/ModelRenderer.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/CountDown.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Network/Settings/SelectionRestrictions.hpp>
#include <PulsarSystem.hpp>
#include <Settings/Settings.hpp>
#include <SlotExpansion/CupsConfig.hpp>
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
    {Pulsar::UI::PULPAGE_SS8TEAMSELECT, Pulsar::UI::PULPAGE_SS8CHARSELECT, Pulsar::UI::PULPAGE_SS8KARTSELECT, L"Team Settings", "team"},
};

// RR's free-for-all Battle (PulsarSystem.cpp's isTeamBattle), from the setting: RR's context is set at scene entry,
// before Battle is picked.
bool MenuFreeForAll() {
#ifdef SS8_VANILLA
    return false;
#else
    return Racedata::sInstance->menusScenario.settings.gamemode == MODE_BATTLE && Pulsar::Settings::Mgr::Get().GetSettingValue(Pulsar::Settings::SETTING_BATTLETEAMS) == Pulsar::BATTLE_FFA_DISABLED;
#endif
}

// VSModeSelect's Team VS sets bit 1 (0x80852A64); Battle sets it too (0x8083B96C), and RR's free-for-all clears it
// only at the race (BattleFFA.cpp).
bool MenuTeamVS() {
    return !MenuFreeForAll() && (Racedata::sInstance->menusScenario.settings.modeFlags & 2) != 0;
}

void WithMenuLocalCount(void (*fill)()) {
    SectionParams *params = SectionMgr::sInstance->sectionParams;
    const u32 gameCount = params->localPlayerCount;
    params->localPlayerCount = menuLocalCount;
    fill();
    params->localPlayerCount = gameCount;
}

s32 OkLeaveFrames(const AnimationGroup &ok) {
    if (Page::transitionDelay == 0.0f)
        return 0;
    return static_cast<s32>(ok.animations[0].transform->GetFrameSize() / ok.animations[0].speed / ok.unknown_0x40);
}

// MultiKartSelect::OnTextChange's kart name (0x8084A338) and OnUpDownClick's confirm voice (0x8084A1C4), which
// returns at once for hud 4 or more.
extern "C" {
u32 kartBmg(KartId);
bool kartVoice(void *, u32);
}
// MultiTeamSelect::OnButtonClick's team voice (0x8084C8BC), which returns at once for hud 4 or more.
extern "C" bool PlayDriverTeamVoice(u8 *manager, u32 hud, u32 team);

const float kCellX[kGameLocal] = {-240.0f, -80.0f, 80.0f, 240.0f};
#ifdef SS8_VANILLA
const float kKartPlateY[2] = {65.0f, -95.0f};  // clear of the game's bottom bar, which RR's background lacks
#else
const float kKartPlateY[2] = {10.0f, -165.0f};
#endif
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
// At 4:3 the game draws its widescreen-adjusted panes wider in layout units, so the cells shrink too.
const float kNarrowSize = 0.8f;
const float kNarrowTeamSize = 0.75f;  // the team buttons keep their offsets: each grows into the gap between them
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
// The team page's cells: the driver beside MultiTeamSelect's buttons (0x8084C290), Red first. A team of six is full
// (the game's team views hold six a side), and its button fades for the players still choosing.
const float kTeamCellX[kGameLocal] = {-225.0f, -75.0f, 75.0f, 225.0f};
const float kTeamRowY[2] = {70.0f, -100.0f};
const float kTeamDriverDX = -38.0f;
const float kTeamDriverScale = 0.4f;
const float kTeamButtonDX[2] = {-5.0f, 38.0f};
const float kTeamButtonScale = 0.6f;
const u32 kTeamMessages[2] = {0xdb3, 0xdb2};  // "Red Team", "Blue Team" (0x8084C2C4)
const char *const kTeamFlags[2] = {"flag_red", "flag_blue"};
const u32 kTeamFull = 6;
// SectionParams+0x174's last team per hud for P5-8, seeded as ResetLicenseParams seeds P1-4's (0x805E4184): odd huds Blue.
static u8 extLastTeam[kMaxLocal - kGameLocal] = {TEAM_RED, TEAM_BLUE, TEAM_RED, TEAM_BLUE};

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
    // The kart page's last confirm gives every slot from the game's local count of 4 a kart, P5-8 included until InitRace
    // makes them local (MenuLocalsLocal, Entry.cpp): in Battle a standard one, else a CPU's random one (0x8084A22C).
    if (Racedata::sInstance->menusScenario.settings.gamemode == MODE_BATTLE)
        Pages::BattleKartSelect::func_8083ad54();
    else
        Pages::KartSelect::func_8084745c();
}

// As MultiKartSelect::FillPossibleKarts lists them (0x8084A3F8): the weight class in KartId order, unlocked with its
// 2 (0x8084A45C), and RR's vehicle restriction, which offline allows every kart.
static u32 KartChoices(CharacterId character, KartId *choices) {
    u32 count = 0;
    for (u8 k = 0; k < 12; ++k) {
        const KartId kart = CharacterIdToKartIdByIdx(character, k);
#ifdef SS8_VANILLA
        if (IsKartUnlocked(kart, static_cast<u32>(2)))
#else
        if (IsKartUnlocked(kart, static_cast<u32>(2)) && Pulsar::Restrictions::IsVehicleEnabled(kart))
#endif
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

// The look of a control a 5-8 player game cannot use.
const u8 kFadedAlpha = 100;

void Fade(LayoutUIControl &control, u8 alpha) {
    // LayoutUIControl::Update gives the root [3]'s opacity each frame; SetPositionAnim makes [3] from [1] while the page animates.
    control.positionAndscale[1].opacity = alpha;
    control.positionAndscale[3].opacity = alpha;
    nw4r::lyt::Pane &root = *control.layout.layout.rootPane;
    root.alpha = alpha;
    root.flag |= 2;  // influenced alpha: the panes below take the root's
}

#ifndef SS8_VANILLA
// Mystic Tangle faults at load with 8 locals (its tree models take 4.3 MB of MEM1), so 5-8 player games leave it out.
// Battle Royale's 1 MB Mii heap goes on MEM2 after the archive, so it also leaves out archives above a line per tile count.
bool TrackOffForLocals(u32 track, u32 variant) {
    if (menuLocalCount <= kGameLocal)
        return false;
    if (track == 0x1e4)
        return true;
    const Pulsar::PulsarId id = static_cast<Pulsar::PulsarId>(track);
    const bool royale = Pulsar::Settings::Mgr::Get().GetSettingValue(Pulsar::Settings::SETTING_KOROYALEENABLED) == Pulsar::KOROYALESETTING_ENABLED;
    if (!royale || Pulsar::CupsConfig::IsReg(id))
        return false;
    char path[0x80];
    Pulsar::CupsConfig::sInstance->FormatCoursePath(path, sizeof(path) - 4, id, static_cast<u8>(variant));
    strcat(path, ".szs");
    DVD::FileInfo info;
    if (!DVD::Open(path, &info))
        return false;
    // DVD reads need a 32-byte aligned buffer, which Kamek's link does not give a static.
    u8 raw[0x40];
    u32 *header = reinterpret_cast<u32 *>((reinterpret_cast<u32>(raw) + 0x1f) & ~0x1f);
    OS::DCInvalidateRange(header, 0x20);
    const bool read = DVD::ReadPrio(&info, header, 0x20, 0, 2) == 0x20;
    DVD::Close(&info);
    const u32 line = menuLocalCount > 6 ? 83 * 0x100000 / 10 : 9 * 0x100000;  // 8.3 MiB on 8 tiles, 9.0 on 6
    return read && header[0] == 0x59617a30 && header[1] > line;  // "Yaz0", then the decoded size
}

// The pages keep their buttons, so a 5-8 player game sets both ways. Variant -1 is the course page, whose button goes only
// with every variant of its track; the variant page's own control also runs that InitSelf and is left alone there.
bool OffCourseButton(CtrlMenuCourseSelectCourse &course, u32 row, u32 track, s32 variant) {
    if (menuLocalCount <= kGameLocal)
        return false;
    bool off = true;
    if (variant >= 0) {
        off = TrackOffForLocals(track, variant);
    } else {
        const Pages::CourseSelect *page = SectionMgr::sInstance->curSection->Get<Pages::CourseSelect>();
        if (page == nullptr || &page->CtrlMenuCourseSelectCourse != &course)
            return false;
        const Pulsar::PulsarId id = static_cast<Pulsar::PulsarId>(track);
        const u32 variants = Pulsar::CupsConfig::IsReg(id) ? 1 : Pulsar::CupsConfig::sInstance->GetTrack(id).variantCount + 1;
        for (u32 i = 0; off && i < variants; ++i) off = TrackOffForLocals(track, i);
    }
    course.courseButtons[row].manipulator.inaccessible = off;
    Fade(course.courseButtons[row], off ? kFadedAlpha : 255);
    return off;
}
#endif

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
    void OnResume() override;

private:
    void Leave(u32 next, u32 animDirection);
    void LoadCell(u32 player);
    void ActivateKarts();
    void ActivateButtons();
    void ActivateTeams();
    void UpdateTeamCap(bool moveOff);
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
    const bool teamStep;
    CtrlMenuPageTitleText *titleText;
    PageManipulatorManager manager;
    PageId nextPageId;
    SectionId raceSection;  // while Start? is up, the section its OK opens
    bool ready[kMaxLocal];
    bool leaving;
    s32 leaveFrames;  // counts down once everyone is ready, -1 before
    s32 rolling;  // the kart roulette's frames left, -1 when none runs
    KartId karts[kMaxLocal];
    s32 opened[kMaxLocal];  // each player's kart or button as the page opened, their last pick
    LayoutUIControl *previews[kMaxLocal];  // CharacterSelectModel, the kart's texture on model_set_p
    LayoutUIControl *plates[kMaxLocal];  // the kart page's
    LayoutUIControl *sides[kMaxLocal][2];  // the arrows on the kart page, the two buttons on the others
    LayoutUIControl *clock;  // the countdown's, null without one
    u8 buttons[kMaxLocal];  // the button each player is on, 0 or 1
    s32 held[kMaxLocal][2];  // frames each direction has been held, -1 when not
    bool teamFull[2];  // the team page's teams six players have chosen
};

Page *NewSelectPage(u32 id) {
    if (id == Pulsar::UI::PULPAGE_SS8CHARSELECT)
        return NewCharSelectPage();
    for (u32 i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i)
        if (kSteps[i].id == id)
            return new SelectPage(kSteps[i]);
    return nullptr;
}

SelectPage::SelectPage(const SelectStep &step) : step(step), kartStep(step.id == Pulsar::UI::PULPAGE_SS8KARTSELECT), teamStep(step.id == Pulsar::UI::PULPAGE_SS8TEAMSELECT) {
    this->nextPageId = PAGE_NONE;
    this->titleText = new CtrlMenuPageTitleText;
    for (u32 p = 0; p < kMaxLocal; ++p) {
        this->previews[p] = new LayoutUIControl;
        this->plates[p] = this->kartStep ? new LayoutUIControl : static_cast<LayoutUIControl *>(nullptr);
        for (u32 side = 0; side < 2; ++side) this->sides[p][side] = new LayoutUIControl;
    }
    this->clock = nullptr;
}

SelectPage::~SelectPage() {
    delete this->titleText;
    for (u32 p = 0; p < kMaxLocal; ++p) {
        delete this->previews[p];
        delete this->plates[p];
        for (u32 side = 0; side < 2; ++side) delete this->sides[p][side];
    }
    delete this->clock;
}

void SelectPage::OnInit() {
    const u32 cells = 1 + (this->kartStep ? 4 : 3) * kMaxLocal;
    const bool clock = !this->teamStep && SelectTimerSeconds() != 0;
    this->InitControlGroup(cells + clock);
    if (clock)
        this->clock = AddSelectClock(*this, cells, nullptr);
    this->AddControl(0, *this->titleText, 0);
    this->titleText->Load(false);
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(this->step.title);
    this->titleText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
    for (u32 p = 0; p < kMaxLocal; ++p) this->LoadCell(p);
    // No control to select, so A and B are read from the holders. The bitfield keeps SectionPad's reconnect
    // overlay on P1-4, as the vanilla pages' mask does with the game's count of 4; multiplayer, so it draws their colours.
    this->manager.Init(1, true);
    this->SetManipulatorManager(this->manager);
    this->manager.UpdatePlayerBitfield2((1 << kGameLocal) - 1);
}

// A cell: the preview, then the plate and UpDownControl's arrows for 3-4 (0x80849A84) or MultiDriftSelect's buttons
// (0x8084B0A0), as plain controls: the vanilla ones join the page's 5-slot ControlsManipulatorManager (0x8063EFC8).
void SelectPage::LoadCell(u32 player) {
    const bool narrow = LayoutWidth() < 832.0f;
    const float fx = narrow ? kNarrowX : 1.0f;
    const float fs = narrow ? kNarrowSize : 1.0f;
    const float teamSize = narrow ? kNarrowTeamSize : 1.0f;
    const float x = kCellX[player % kGameLocal] * fx;
    const float y = kKartPlateY[player / kGameLocal];
    const float teamX = kTeamCellX[player % kGameLocal] * fx;
    this->AddControl(1 + player, *this->previews[player], 0);
    if (this->teamStep)
        LoadModelPreview(*this->previews[player], teamX + kTeamDriverDX, kTeamRowY[player / kGameLocal], kTeamDriverScale * teamSize);
    else
        LoadModelPreview(*this->previews[player], x, y + kKartPreviewDY * fs, kKartPreviewScale * fs);
    u32 next = 1 + kMaxLocal;
    if (this->kartStep) {
        this->AddControl(next + player, *this->plates[player], 0);
        LoadNamePlate(*this->plates[player], player, x, y, kKartPlateScale * fs);
        ShowSkinToggleIcons(*this->plates[player], CONTROLLER_TYPE_NONE);  // the skin toggle buttons do nothing here
        next += kMaxLocal;
    }
    for (u32 side = 0; side < 2; ++side) {
        LayoutUIControl &control = *this->sides[player][side];
        this->AddControl(next + 2 * player + side, control, 0);
        ControlLoader loader(&control);
        if (this->teamStep) {
            loader.Load("button", "TeamSelect_4", side == 0 ? "ButtonRed" : "ButtonBlue", kButtonAnims);
            PlaceControl(control, player, teamX + kTeamButtonDX[side], kTeamRowY[player / kGameLocal], kTeamButtonScale * teamSize);
            control.SetTextBoxMessage("text", kTeamMessages[side]);
            control.SetPicturePane("flag_set_p", kTeamFlags[side]);
        } else if (!this->kartStep) {
            loader.Load("button", "drift_4", side == 0 ? "ButtonAuto" : "ButtonManual", kButtonAnims);
            PlaceControl(control, player, x, y + kButtonDY[side] * fs, kButtonScale * fs);
        } else {
            if (side == ARROW_LEFT)
                loader.Load("control", "MachineArrowLeft", "ButtonArrowLeft4", kButtonAnims);
            else
                loader.Load("control", "MachineArrowRight", "ButtonArrowRight4", kButtonAnims);
            PlaceControl(control, player, x + (side == ARROW_LEFT ? -kArrowDX : kArrowDX) * fs, y + kArrowDY * fs, kArrowScale * fs);
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
    this->raceSection = SECTION_NONE;
    this->leaving = false;
    this->leaveFrames = -1;
    this->rolling = -1;
    for (u32 i = 0; i < kMaxLocal; ++i) this->ready[i] = false;
    OS::Report("ss8 select: %s page, %u players\n", this->step.name, menuLocalCount);
    const u32 count = menuLocalCount;
    if (!ExtPreviewsShow(count) || (!this->teamStep && !ExtKartsShow(count)))
        OS::Report("ss8 select: P5-8 kart previews failed\n");
    for (u32 player = 0; player < kMaxLocal; ++player) {
        const bool shown = player < count;
        this->previews[player]->isHidden = !shown;
        for (u32 side = 0; side < 2; ++side) {
            this->sides[player][side]->isHidden = !shown;
            this->held[player][side] = -1;
        }
    }
    if (this->teamStep) {
        this->ActivateTeams();
    } else if (!this->kartStep) {
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
        this->opened[player] = this->karts[player];
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

// As MultiTeamSelect::OnActivate (0x8084C444): each player on their last team (SectionParams+0x174, 0x8084C638),
// their driver idle (0x8084C6E4).
void SelectPage::ActivateTeams() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 player = 0; player < menuLocalCount; ++player) {
        const u32 last = player < kGameLocal ? params->isOddLicense[player] : extLastTeam[player - kGameLocal];
        this->buttons[player] = last == TEAM_BLUE ? 1 : 0;
        for (u32 side = 0; side < 2; ++side) StartAnimations(*this->sides[player][side], side == this->buttons[player]);
        if (player >= kGameLocal) {
            ExtPreviewPose(player, false);
        } else if (MenuDriverModel *model = GameDriverModel(player)) {
            model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
        }
    }
}

// Free-for-all skips the team page, but a Battle's race still needs a team per player (its AI reads a null kart with
// none, 0x80728D50): the teams the page's untouched confirm writes, six a side at most, the CPUs balanced against them.
void AssignLastTeams() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    u32 chosen[2] = {0, 0};
    for (u32 player = 0; player < menuLocalCount; ++player) {
        const u32 last = player < kGameLocal ? params->isOddLicense[player] : extLastTeam[player - kGameLocal];
        u32 side = last == TEAM_BLUE ? 1 : 0;
        if (chosen[side] == kTeamFull)
            side ^= 1;
        ++chosen[side];
        Racedata::sInstance->menusScenario.players[player].team = side == 0 ? TEAM_RED : TEAM_BLUE;
    }
    WithMenuLocalCount(Pages::MultiTeamSelect::AssignCPUTeams);
}

// A team six players have chosen fades for the players still choosing, and moves them off it when moveOff.
void SelectPage::UpdateTeamCap(bool moveOff) {
    u32 chosen[2] = {0, 0};
    for (u32 player = 0; player < menuLocalCount; ++player)
        if (this->ready[player])
            ++chosen[this->buttons[player]];
    for (u32 side = 0; side < 2; ++side) this->teamFull[side] = chosen[side] >= kTeamFull;
    for (u32 player = 0; player < menuLocalCount; ++player) {
        if (this->ready[player])
            continue;
        for (u32 side = 0; side < 2; ++side) {
            const bool full = this->teamFull[side];
            Fade(*this->sides[player][side], full ? kFadedAlpha : 255);
            if (moveOff && full && this->buttons[player] == side) {
                SelectControl(*this->sides[player][side], false);
                SelectControl(*this->sides[player][side ^ 1], true);
                this->buttons[player] = side ^ 1;
            }
        }
    }
}

static bool StoredDriftAuto(u32 player) {
    return player < kGameLocal ? SectionMgr::sInstance->sectionParams->driftType[player] == kDriftAuto : Holder(*Input::Manager::sInstance, player).isDriftAuto != 0;
}

// As MultiTransmissionSelect::OnActivate and MultiDriftSelect::OnActivate (0x8084B2E0): each player on their stored
// choice, under the kart they chose, in its confirmed pose.
void SelectPage::ActivateButtons() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    const bool transmission = this->step.id == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT;
    for (u32 player = 0; player < menuLocalCount; ++player) {
        bool first;
#ifndef SS8_VANILLA
        if (transmission)
            first = Pulsar::UI::GetSelectedTransmission(player) == Pulsar::TRANSMISSION_OUTSIDE;
        else
#endif
            first = StoredDriftAuto(player);
        this->buttons[player] = first ? 0 : 1;
        this->opened[player] = this->buttons[player];
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
        if (this->teamStep) {
            if (player < kGameLocal)
                PlayDriverTeamVoice(*driverSoundManager, player, this->buttons[player]);
            else
                ExtMenuVoice(player, PlayerCharacter(player), this->buttons[player] == 0 ? kVoiceTeamRed : kVoiceTeamBlue);
        }
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
        ExtMenuVoice(player, PlayerCharacter(player), kVoiceKart);
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

// Drift goes to the holder the race reads (0x805944F4), to SectionParams for P1-4 and the license for P1, as vanilla.
static void WriteDrift(u32 player, bool automatic) {
    Holder(*Input::Manager::sInstance, player).SetDriftType(automatic);
    if (player >= kGameLocal)
        return;
    const u32 driftType = automatic ? kDriftAuto : kDriftManual;
    SectionMgr::sInstance->sectionParams->driftType[player] = driftType;
    RKSYS::Mgr *rksys = RKSYS::Mgr::sInstance;
    if (player == 0 && rksys != nullptr && static_cast<s16>(rksys->curLicenseId) >= 0) {
        rksys->licenses[rksys->curLicenseId].defaultDrift = driftType;
        SectionMgr::sInstance->rksysRequester->NotifyNewLicenseContent();
    }
}

// MultiTransmissionSelect's and MultiDriftSelect's OnButtonClick (0x8084B500), the first button outside or automatic.
void SelectPage::WriteButton(u32 player) {
    const bool first = this->buttons[player] == 0;
    OS::Report("ss8 select: %s player %u on %s\n", this->step.name, player + 1, first ? "first" : "second");
    if (this->teamStep) {
        const Team team = first ? TEAM_RED : TEAM_BLUE;
        Racedata::sInstance->menusScenario.players[player].team = team;
        if (player < kGameLocal)
            SectionMgr::sInstance->sectionParams->isOddLicense[player] = team;
        else
            extLastTeam[player - kGameLocal] = team;
        return;
    }
#ifndef SS8_VANILLA
    if (this->step.id == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT) {
        Pulsar::UI::SetSelectedTransmission(player, first ? Pulsar::TRANSMISSION_OUTSIDE : Pulsar::TRANSMISSION_INSIDE);
        return;
    }
#endif
    WriteDrift(player, first);
}

void SelectPage::AfterControlUpdate() {
    if (this->teamStep) {
        UpdateDriverPreviews(this->previews);
        this->UpdateTeamCap(true);
    } else {
        this->UpdateKartPreviews();
    }
    for (u32 player = 0; player < menuLocalCount; ++player) this->UpdateSides(player);
    if (this->currentState != STATE_ACTIVE || this->leaving)
        return;
    const u32 count = menuLocalCount;
#ifndef SS8_VANILLA
    // Entered by the cup page's Back once everyone has picked: pass back, as PassDriftPage does the game's page.
    if (this->step.id == Pulsar::UI::PULPAGE_SS8DRIFTSELECT && this->animationDirection == 1 && this->curStateDuration == 0 && SkipDrift(count)) {
        OS::Report("ss8 select: drift page passed back\n");
        this->Leave(this->step.prev, 1);
        return;
    }
#endif
    if (this->rolling >= 0)
        this->Roulette();
    // At the countdown's zero, or in Same as Last Time's pass, everyone still choosing confirms what they are on, as the
    // game's pages do, a player who has picked since the title on what the page opened on.
    const bool timeUp = this->rolling < 0 && (SamePassing() || (this->clock != nullptr && SelectTimeUp(*this->clock)));
    u32 readyCount = 0;
    for (u32 player = 0; player < count; ++player) {
        if (timeUp && !this->ready[player]) {
            if (this->teamStep) {
                this->UpdateTeamCap(true);  // last teams from other counts can fill one past six
            } else if (DriftChosenBy(player + 1) && (this->kartStep ? this->karts[player] : this->buttons[player]) != this->opened[player]) {
                OS::Report("ss8 select: %s player %u back on the last pick\n", this->step.name, player + 1);
                if (this->kartStep) {
                    this->karts[player] = static_cast<KartId>(this->opened[player]);
                    this->StepKart(player, 0);
                } else {
                    SelectControl(*this->sides[player][this->buttons[player]], false);
                    this->buttons[player] = static_cast<u8>(this->opened[player]);
                    SelectControl(*this->sides[player][this->buttons[player]], true);
                }
            }
            this->Ready(player);
        }
        const Input::RealControllerHolder *holder = this->rolling >= 0 || SectionMgr::sInstance->curSection->skipsActions ? static_cast<Input::RealControllerHolder *>(nullptr) : PlayerHolder(player);
        const s32 step = holder != nullptr ? this->Direction(player, *holder, !this->kartStep && !this->teamStep) : 0;
        if (holder != nullptr && UIPressed(*holder, FORWARD_PRESS) && !this->ready[player]) {
            if (this->teamStep && this->teamFull[this->buttons[player]])
                continue;  // filled earlier this frame; next frame's cap moves this player off it
            this->Ready(player);
            if (this->teamStep)
                this->UpdateTeamCap(false);  // a later confirm on this frame sees the count
        } else if (holder != nullptr && !this->ready[player] && step != 0) {
            if (this->kartStep) {
                this->StepKart(player, step);
                this->LightArrow(player, step);
            } else {
                const u8 to = step < 0 ? 0 : 1;  // Up or Left the first button, Down or Right the second, no wrap
                if (to != this->buttons[player] && !(this->teamStep && this->teamFull[to])) {
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
        this->leaveFrames = OkLeaveFrames(ok);
    }
    if (this->leaveFrames-- > 0)
        return;
    if (this->kartStep)
        WriteKarts();
    if (this->teamStep)  // MultiTeamSelect's confirm balances the CPUs against the locals' teams (0x8084CA78)
        WithMenuLocalCount(Pages::MultiTeamSelect::AssignCPUTeams);
    if (this->step.id == Pulsar::UI::PULPAGE_SS8DRIFTSELECT)
        DriftChosen(count);
    this->Leave(this->step.next, 0);
}

// Menu::LoadNextPageById stores a Menu caller's id as the next Menu's prevPageId (0x808377DC); these pages are not
// Menus, so cup select gets it here. The transmission page is skipped as RR's LoadMultiTransmissionSelectBeforeDrift.
void SelectPage::Leave(u32 next, u32 animDirection) {
    if (next == Pulsar::UI::PULPAGE_SS8CHARSELECT && this->kartStep && MenuTeamVS())
        next = Pulsar::UI::PULPAGE_SS8TEAMSELECT;
#ifdef SS8_VANILLA
    if (next == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT)  // transmissions are RR's
#else
    if (next == Pulsar::UI::PULPAGE_SS8TRANSMISSIONSELECT && Pulsar::UI::ShouldSkipTransmissionSelect(Pulsar::System::sInstance))
#endif
        next = this->kartStep ? Pulsar::UI::PULPAGE_SS8DRIFTSELECT : Pulsar::UI::PULPAGE_SS8KARTSELECT;
#ifndef SS8_VANILLA
    if (next == Pulsar::UI::PULPAGE_SS8DRIFTSELECT && SkipDrift(menuLocalCount)) {
        for (u32 player = 0; player < menuLocalCount; ++player) WriteDrift(player, StoredDriftAuto(player));
        OS::Report("ss8 select: drift page skipped\n");
        next = PAGE_CUP_SELECT;
    }
#endif
    if (animDirection == 1 || next == PAGE_CUP_SELECT)
        EndSamePass();
#ifdef SS8_VANILLA
    const bool battle = Racedata::sInstance->menusScenario.settings.gamemode == MODE_BATTLE;
    // The game's arenas have their own cup page (MultiDriftSelect::OnButtonClick, 0x8084B7BC); RR's cup page holds them.
    if (next == PAGE_CUP_SELECT && battle)
        next = PAGE_BATTLE_CUP_SELECT;
    // With the multiplayer rules' courses on Random (index 2 or 3, 0x80860A28; Random is 1) the game's drift page draws
    // them and asks Start? in place of the cup page (0x8084B6D8-0x8084B798).
    const RKSYS::Mgr *rksys = RKSYS::Mgr::sInstance;
    if ((next == PAGE_CUP_SELECT || next == PAGE_BATTLE_CUP_SELECT) && animDirection == 0 && static_cast<s16>(rksys->curLicenseId) >= 0
      && rksys->licenses[rksys->curLicenseId].courseChoice[battle ? 3 : 2] == 1) {
        SectionParams *params = SectionMgr::sInstance->sectionParams;
        RacedataSettings &settings = Racedata::sInstance->menusScenario.settings;
        if (battle) {
            params->RandomizeBattleArenas();
            settings.courseId = params->battleArenas[0];
        } else {
            params->RandomizeVSTracks();
            settings.courseId = params->vsTracks[0];
        }
        this->raceSection = battle ? SECTION_BALLOON_BATTLE_INTRO : SECTION_VS_RACE_INTRO;
        this->leaving = true;
        OS::Report("ss8 select: %s page -> start race on %#x\n", this->step.name, settings.courseId);
        this->AddPageLayer(PAGE_START_RACE, 0);
        return;
    }
#endif
    this->leaving = true;
    this->nextPageId = static_cast<PageId>(next);
    if ((next == PAGE_CUP_SELECT || next == PAGE_BATTLE_CUP_SELECT) && animDirection == 0) {
        Page *cupSelect = SectionMgr::sInstance->curSection->pages[next];
        if (cupSelect != nullptr)
            static_cast<Pages::Menu *>(cupSelect)->prevPageId = static_cast<PageId>(this->step.id);
    }
    OS::Report("ss8 select: %s page -> %#x\n", this->step.name, next);
    this->EndStateAnimated(animDirection, 0.0f);
}

// As Menu::OnResume once Start? closes (0x80837D44): its OK changes section, and its Back clears every player's
// choice, as MultiDriftSelect::OnResume does (0x8084B8C4).
void SelectPage::OnResume() {
    const SectionId section = this->raceSection;
    if (section == SECTION_NONE)
        return;
    this->raceSection = SECTION_NONE;
    if (SectionMgr::sInstance->curSection->Get<Pages::StartRace>()->hasButtonBeenPressed) {
        this->ChangeSectionBySceneChange(section, 0, 0.0f);
        return;
    }
    OS::Report("ss8 select: %s page back from start race\n", this->step.name);
    for (u32 player = 0; player < kMaxLocal; ++player) this->ready[player] = false;
    this->leaving = false;
    this->leaveFrames = -1;
}

// P5-8's karts on the cup, course, version, Battle cup and arena pages: the page's model control (NoteModelControl::Load, 0x805F4348) showing
// their preview, as the character and kart pages do.
class CupKart : public LayoutUIControl {
public:
    void InitSelf() override;
    void OnUpdate() override;
    void SetPositionAnim(PositionAndScale &positionAndScale, float curFrame) override;
    u32 player;
};

// One column of four a side, P1 and P2 at the top, at one size per page.
#ifdef SS8_VANILLA
const float kCupKartX[2][2] = {{-240.0f, 240.0f}, {-240.0f, 240.0f}};  // the game's course-list box fills the cup page's middle
#else
const float kCupKartX[2][2] = {{-101.0f, 229.0f}, {-240.0f, 240.0f}};  // [course page][odd player]
#endif
const float kCupKartY[2][4] = {{35.0f, -10.0f, -55.0f, -100.0f}, {110.0f, 40.0f, -30.0f, -100.0f}};  // the cup page's below a selected arrow
const float kCupKartSize[2] = {0.4f, 0.55f};

// The model pane keeps its offset, which NoteModelControl::SetPositionAnim (0x805F4CFC) writes while the page
// animates, so the control goes where that offset lands the kart on the column.
static void PlaceCupKart(LayoutUIControl &control, u32 player, bool course) {
    const nw4r::lyt::Pane *model = control.layout.GetPaneByName("model_null");
    PositionAndScale &at = control.positionAndscale[1];
    at.scale.x = kCupKartSize[course] / model->scale.x;
    at.scale.z = at.scale.x;
    at.position.x = kCupKartX[course][player & 1] - model->trans.x * at.scale.x;
    at.position.y = kCupKartY[course][player / 2] - model->trans.y * at.scale.x;
    // [3] as SetPositionAnim (0x8063D194) makes it, which the course page runs only while it animates.
    const PositionAndScale &slide = control.positionAndscale[2];
    PositionAndScale &shown = control.positionAndscale[3];
    shown.position.x = at.position.x + slide.position.x;
    shown.position.y = at.position.y + slide.position.y;
    shown.scale.x = at.scale.x * slide.scale.x;
    shown.scale.z = at.scale.z * slide.scale.z;
}

void CupKart::InitSelf() {
    this->isHidden = true;
    // Page::Init runs this too, as the section is built (0x80601A8C): the karts belong to the page's activation only.
    if (this->parentGroup->parentPage->currentState != STATE_ACTIVATING)
        return;
    // ExtKartsShow clears every pose, so P5's control, initialised first, shows them all.
    if (this->player == kGameLocal && !(ExtPreviewsShow(menuLocalCount) && ExtKartsShow(menuLocalCount)))
        OS::Report("ss8 cup: P5-8 previews failed\n");
    const CharacterId character = ExtCharacter(this->player);
    const u32 slot = SkinSlot(this->player, character);
    ExtPreviewLoad(this->player, character, slot);
    ExtKartRequest(this->player, character, slot, extPicks[this->player - kGameLocal].kart);
    ExtKartPose(this->player, true);
}

void CupKart::OnUpdate() {
    const GX::TexObj *texObj = ExtPreviewTexObj(this->player);
    SetPreviewTexture(*this, texObj);
    this->isHidden = texObj == nullptr;
    Pages::Menu *page = static_cast<Pages::Menu *>(this->parentGroup->parentPage);
    const bool course = page->pageId != PAGE_CUP_SELECT;  // the version, Battle cup and arena pages take the course page's columns
    PlaceCupKart(*this, this->player, course);
    if (this->player == kGameLocal)  // P1-4's each frame, over RR's one-time place (ExpansionUIMisc.cpp) or the game's
        for (u32 i = 0; i < kGameLocal; ++i) PlaceCupKart(page->modelPosition[i], i, course);
}

// As NoteModelControl::SetPositionAnim: still while the menu changes page, as Menu::BeforeExitAnimations and
// BeforeEntranceAnimations decide for P1-4's controls (0x80838ACC, 0x80838A14).
void CupKart::SetPositionAnim(PositionAndScale &positionAndScale, float curFrame) {
    const Pages::Menu *page = static_cast<const Pages::Menu *>(this->parentGroup->parentPage);
    LayoutUIControl::SetPositionAnim(positionAndScale, page->modelPosition[0].animationType != 0 ? curFrame : 0.0f);
}

// Each player's choice as a game select page opens, which is their last pick, to go back to before the page confirms
// everyone at zero. A control's selection set here is applied by the next frame's CheckActions, before the controls.
class LastPicks {
public:
    void Open(Pages::MenuInteractable &page);
    void Return(Pages::MenuInteractable &page) const;

private:
    ControlManipulator *opened[kGameLocal];
    u32 options[kGameLocal];  // the 2-4 kart page's, whose arrows are one control a player
};

void LastPicks::Open(Pages::MenuInteractable &page) {
    for (u32 p = 0; p < kGameLocal; ++p) {
        this->opened[p] = page.controlsManipulatorManager.holders[p].curManipulator;
        if (page.pageId == PAGE_MULTIPLAYER_KART_SELECT)
            this->options[p] = static_cast<Pages::MultiKartSelect &>(page).arrows[p].curSelectedOption;
    }
}

// A player still choosing who has picked since the title.
void LastPicks::Return(Pages::MenuInteractable &page) const {
    ControlsManipulatorManager &manager = page.controlsManipulatorManager;
    for (u32 p = 0; p < SectionMgr::sInstance->sectionParams->localPlayerCount; ++p) {
        if (!DriftChosenBy(p + 1) || !manager.holders[p].info.enabled)
            continue;
        if (page.pageId == PAGE_MULTIPLAYER_KART_SELECT) {
            UpDownControl &arrows = static_cast<Pages::MultiKartSelect &>(page).arrows[p];
            if (arrows.curSelectedOption == this->options[p])
                continue;
            arrows.SelectInitial(this->options[p]);
        } else if (this->opened[p] != nullptr && manager.holders[p].curManipulator != this->opened[p]) {
            manager.SetChildControlManipulator(p, *this->opened[p], static_cast<u32>(-1));
        } else {
            continue;
        }
        OS::Report("ss8 select: page %#x player %u back on the last pick\n", page.pageId, p + 1);
    }
}

// The game's online clock. It shows only once AnimateCurrentCountDown has played its show animation, which the page's
// activation undoes, so it plays every frame, as RR's ChooseNextTrack does.
class SelectClock : public CountDownTimerControl {
public:
    explicit SelectClock(CountDown **timer) : picks(nullptr), timer(timer), seconds(0), started(false), returned(false) {
    }
    void OnUpdate() override;
    bool TimeUp() const {
        return this->seconds != 0 && this->count.countdown <= 0.0f;
    }
    CountDown count;
    const LastPicks *picks;  // a game page's, null on ours

private:
    CountDown **timer;
    u32 seconds;  // this visit's length, 0 once the row is Off
    bool started;
    bool returned;  // this visit's players put back on their last picks
};

// A fresh count each time the page opens, set before PassDriftPage saves the drift page's timer and after RR copies the
// kart page's into the transmission page's. The row is read then, as Settings can change it within the section.
void SelectClock::OnUpdate() {
    const Page &page = *this->parentGroup->parentPage;
    if (page.currentState > STATE_ACTIVE) {
        this->started = false;
    } else if (page.currentState < STATE_ACTIVE || !this->started) {
        if (!this->started)
            OS::Report("ss8 select: page %#x clock %u s%s\n", page.pageId, SelectTimerSeconds(), page.currentState == STATE_ACTIVE ? " late" : "");
        this->seconds = SelectTimerSeconds();
        this->count.SetInitial(static_cast<float>(this->seconds));
        this->count.isActive = true;
        if (this->timer != nullptr)
            *this->timer = this->seconds != 0 ? &this->count : static_cast<CountDown *>(nullptr);
        this->started = true;
        this->returned = false;
    } else if (this->seconds != 0 && SectionMgr::sInstance->curSection->IsPageTopLayer(page)) {
        this->count.Update();  // a layer over the page pauses it
        if (this->picks != nullptr && !this->returned && this->count.countdown <= 0.0f) {
            // A frame more, so the next frame's CheckActions moves players back before the page's zero step.
            this->picks->Return(static_cast<Pages::MenuInteractable &>(*this->parentGroup->parentPage));
            this->returned = true;
            this->count.countdown = 1.0f / 60.0f;
            this->count.isActive = true;  // Update clears it at zero
        }
    }
    this->isHidden = this->seconds == 0;
    if (this->isHidden)
        return;  // the update plays its zero sound from a hidden control too
    this->AnimateCurrentCountDown();
    CountDownTimerControl::OnUpdate();
}

LayoutUIControl *AddSelectClock(Page &page, u32 index, CountDown **timer) {
    SelectClock *clock = new SelectClock(timer);
    page.AddControl(index, *clock, 0);
    clock->Load(clock->count);
    clock->positionAndscale[0].position.x = 0.0f;  // centred in the title band, at the layout's height and size
    clock->PositionFromBasePosition();
    return clock;
}

bool SelectTimeUp(const LayoutUIControl &clock) {
    return static_cast<const SelectClock &>(clock).TimeUp();
}

// The game's select pages' timers, whose own step confirms everyone's highlighted choice at zero; the character page's
// is its list's, as the page's own also turns its Back button off. Battle's kart page confirms only online.
static CountDown **GameSelectTimer(Page &page) {
    switch (page.pageId) {
        case PAGE_CHARACTER_SELECT:
            return &static_cast<Pages::CharacterSelect &>(page).ctrlMenuCharSelect.timer;
        case PAGE_KART_SELECT:
            return &static_cast<Pages::KartSelect &>(page).timer;
        case PAGE_MULTIPLAYER_KART_SELECT:
            return &static_cast<Pages::MultiKartSelect &>(page).timer;
        case PAGE_DRIFT_SELECT:
            return &static_cast<Pages::DriftSelect &>(page).timer;
        case PAGE_MULTIPLAYER_DRIFT_SELECT:
            return &static_cast<Pages::MultiDriftSelect &>(page).timer;
    }
    if (page.pageId == static_cast<PageId>(Pulsar::UI::PULPAGE_TRANSMISSIONSELECT))
        return &static_cast<Pages::DriftSelect &>(page).timer;
    if (page.pageId == static_cast<PageId>(Pulsar::UI::PULPAGE_MULTITRANSMISSIONSELECT))
        return &static_cast<Pages::MultiDriftSelect &>(page).timer;
    return nullptr;
}

static bool samePass;

bool SamePassing() {
    return samePass;
}

void EndSamePass() {
    if (samePass)
        OS::Report("ss8 select: same as last time ends\n");
    samePass = false;
}

// The menus' local count; the game's 1-4 player menus leave ours at 0.
static u32 SelectLocals() {
    return menuLocalCount != 0 ? menuLocalCount : SectionMgr::sInstance->sectionParams->localPlayerCount;
}

static const u32 kSameHoldFrames = 180;

static CountDown &PassZero() {
    static CountDown zero;  // here, not at file scope: Kamek runs those constructors before StaticR.rel, which has CountDown's
    return zero;
}

// A game page's timer at zero, which its own step confirms everyone on, then put back at the next Restore: the step
// runs in that frame's AfterControlUpdate, or in the next frame's update of the character list, a control before ours.
class PassTimer {
public:
    explicit PassTimer(CountDown **timer) : timer(timer), saved(nullptr), set(false) {
    }
    void Zero() {
        CountDown &zero = PassZero();
        if (*this->timer == &zero)
            this->saved = nullptr;  // RR's transmission pages take the kart page's as it leaves (CopyKartTimerToTransmission)
        else if (*this->timer != nullptr && (*this->timer)->countdown <= 0.0f)
            return;  // Keep Last's PassDriftPage, which puts its own back
        else
            this->saved = *this->timer;
        zero.SetInitial(0.0f);
        zero.isActive = true;
        *this->timer = &zero;
        this->set = true;
    }
    void Restore() {
        if (this->set && *this->timer == &PassZero())
            *this->timer = this->saved;
        this->set = false;
    }
    CountDown **timer;  // null on the team pages and ours

private:
    CountDown *saved;
    bool set;
};

// Same as Last Time's hint in the bottom bar (tools/assets/gen_same_button.py's control), once everyone in the count
// has chosen since the title: "Hold", P1's button, the words. Holding it starts the pass, which confirms this page too.
class SameHint : public LayoutUIControl {
public:
    explicit SameHint(CountDown **timer) : pass(timer), held(0), gcn(-1) {
    }
    void OnUpdate() override;
    LastPicks picks;  // on the game's page

private:
    PassTimer pass;
    u32 held;
    s32 gcn;  // whether the icon shown is a GameCube pad's Z, -1 before the first frame
};

void SameHint::OnUpdate() {
    const Page &page = *this->parentGroup->parentPage;
    if (this->pass.timer != nullptr)
        this->pass.Restore();
    if (page.currentState < STATE_ACTIVE)
        EndSamePass();  // one cut short, as by a section change
    if (this->pass.timer != nullptr && page.currentState == STATE_ACTIVE && page.curStateDuration == 0)
        this->picks.Open(static_cast<Pages::MenuInteractable &>(*this->parentGroup->parentPage));
    const Input::RealControllerHolder *holder = PlayerHolder(0);
    const Input::Controller *controller = holder != nullptr ? holder->curController : static_cast<Input::Controller *>(nullptr);
    const ControllerType type = controller != nullptr ? controller->GetType() : CONTROLLER_TYPE_NONE;
    if ((type == GCN) != this->gcn) {
        this->gcn = type == GCN;
        this->SetPaneVisibility("ss8_z", type == GCN);
        this->SetPaneVisibility("ss8_minus", type != GCN);
        // English only; the spaces are the icon's room, which the generator measured for these words.
        Text::Info info;
        info.strings[0] = const_cast<wchar_t *>(type == GCN ? L"Hold        : Same as Last Time" : L"Hold      : Same as Last Time");
        this->SetTextBoxMessage("text", Pulsar::UI::BMG_TEXT, &info);
    }
    this->isHidden = samePass || !SameAsLastTime() || !DriftChosenBy(SelectLocals());
    // Raw, so a GameCube pad's X (the same menu action as Z) does nothing; a Classic's raw word holds its own buttons.
    const u16 button = type == GCN ? PAD::PAD_BUTTON_Z : type == CLASSIC ? WPAD::WPAD_CL_BUTTON_MINUS : WPAD::WPAD_BUTTON_MINUS;
    if (this->isHidden || controller == nullptr || page.currentState != STATE_ACTIVE || !SectionMgr::sInstance->curSection->IsPageTopLayer(page)
      || (holder->uiinputStates[0].rawButtons & button) == 0) {
        this->held = 0;
        return;
    }
    if (++this->held < kSameHoldFrames)
        return;
    samePass = true;
    OS::Report("ss8 select: same as last time by %u players\n", SelectLocals());
    if (this->pass.timer != nullptr) {
        this->picks.Return(static_cast<Pages::MenuInteractable &>(*this->parentGroup->parentPage));
        this->pass.Zero();
    }
}

LayoutUIControl *AddSameHint(Page &page, u32 index, CountDown **timer) {
    SameHint *hint = new SameHint(timer);
    page.AddControl(index, *hint, 0);
    ControlLoader loader(hint);
    loader.Load("bg", "SS8SameHint", "SS8SameHint", nullptr);
    hint->isHidden = true;
    return hint;
}

// The pass on each game page after the character page, at its first active frame: its timer at zero, or on a team page,
// which has none, each player's last team clicked. Its drift pages keep who has chosen since the title (DriftChosen).
class SamePass : public UIControl {
public:
    explicit SamePass(CountDown **timer) : pass(timer) {
    }
    void OnUpdate() override;
    LastPicks picks;

private:
    PassTimer pass;
};

void SamePass::OnUpdate() {
    Pages::Menu &page = *static_cast<Pages::Menu *>(this->parentGroup->parentPage);
    const PageId id = page.pageId;
    if (this->pass.timer != nullptr)
        this->pass.Restore();
    if (page.currentState < STATE_ACTIVE)
        return;
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    // Forward: to a page (exiting, direction 0), or to a section (ChangeSectionById locks it with no next page; a race's
    // Start? is then a layer over this page, which still updates).
    const bool forward = (page.currentState == STATE_EXITING && page.animationDirection == 0) || (page.isLocked && page.nextPageId == PAGE_NONE);
    if ((id == PAGE_DRIFT_SELECT || id == PAGE_MULTIPLAYER_DRIFT_SELECT) && forward) {
        DriftChosen(params->localPlayerCount);
        EndSamePass();
    }
    if (page.currentState == STATE_EXITING && page.animationDirection == 1)
        EndSamePass();
    if (page.currentState != STATE_ACTIVE || page.curStateDuration != 0)
        return;
    if (this->pass.timer != nullptr)
        this->picks.Open(static_cast<Pages::MenuInteractable &>(page));
    if (!samePass)
        return;
    OS::Report("ss8 select: page %#x passed, same as last time\n", id);
    if (this->pass.timer != nullptr) {
        this->pass.Zero();
        return;
    }
    // Check Teams goes on from any button but Back (0x8084FED0); MultiTeamSelect has two a player, id 2 * hud plus
    // the team, Red first (0x8084C770), and goes on once each has clicked one.
    for (u32 i = 0; i < page.externControlCount; ++i) {
        PushButton &button = *page.externControls[i];
        if (button.buttonId < 0)
            continue;
        const u32 hud = static_cast<u32>(button.buttonId) / 2;
        if (id == PAGE_VS_TEAMS_VIEW) {
            button.HandleClick(0, -1);
            return;
        }
        if (static_cast<u32>(button.buttonId) % 2 == (params->isOddLicense[hud] == TEAM_BLUE ? 1 : 0))
            button.HandleClick(hud, -1);
    }
}

// Menu::OnInit's, MultiDriftSelect's and MultiTeamSelect's "bl Page::InitControlGroup" (r3-r4 its own): after the page's
// controls, P5-8's karts on a 5-8 game's cup, course and arena pages, or a select page's clock and Same as Last Time's.
static void MenuControlGroup(Page &page, u32 count) {
    const PageId id = page.pageId;
    CountDown **timer = GameSelectTimer(page);
    if (timer != nullptr || id == PAGE_VS_TEAMS_VIEW || id == PAGE_MULTIPLAYER_TEAM_SELECT) {
        const u32 clock = timer != nullptr && SelectTimerSeconds() != 0;
        page.InitControlGroup(count + clock + 1);
        LastPicks *picks;
        if (id == PAGE_CHARACTER_SELECT) {
            picks = &static_cast<SameHint *>(AddSameHint(page, count + clock, timer))->picks;
        } else {
            SamePass *pass = new SamePass(timer);
            page.AddControl(count + clock, *pass, 0);
            picks = &pass->picks;
        }
        if (clock)
            static_cast<SelectClock *>(AddSelectClock(page, count, timer))->picks = picks;
        return;
    }
    const bool karts =
      (id == PAGE_CUP_SELECT || id == PAGE_COURSE_SELECT || id == static_cast<PageId>(Pulsar::UI::PULPAGE_VARIANTSELECT) || id == PAGE_BATTLE_CUP_SELECT || id == PAGE_BATTLE_STAGE_SELECT)
      && menuLocalCount > kGameLocal;
    const u32 extra = karts ? menuLocalCount - kGameLocal : 0;
    page.InitControlGroup(count + extra);
    for (u32 i = 0; i < extra; ++i) {
        static const char *const kVariants[] = {"1P_4Div", "2P_4Div", "3P_4Div", "4P_4Div"};
        static const char *kAnims[] = {"Ghost", "Player", "Ghost", nullptr, nullptr};  // NoteModelControl::Load's
        CupKart *kart = new CupKart;
        kart->player = kGameLocal + i;
        page.AddControl(count + i, *kart, 0);
        ControlLoader loader(kart);
        loader.Load("model", "PadEasyModel", kVariants[i], kAnims);
        kart->animator.GetAnimationGroupById(0).PlayAnimationAtFrame(0, 0.0f);
        kart->drawPriority = -11.0f;
    }
}
kmCall(0x80836cfc, MenuControlGroup);
kmCall(0x80836dc8, MenuControlGroup);
kmCall(0x8084ab2c, MenuControlGroup);  // MultiDriftSelect::OnInit+0x68, RR's multi transmission page's too
kmCall(0x8084bb9c, MenuControlGroup);  // MultiTeamSelect::OnInit+0x68

// VSModeSelect's (0x80852A6C, 0x80852A90) and BattleModeSelect's (0x8083B924, 0x8083B944) bl Menu::LoadNextPageById to
// the character page; r3-r5 are its arguments, and the code after each sets the registers it reads.
static void ModeNextPage(Pages::Menu &page, PageId id, PushButton &button) {
    page.LoadNextPageById(menuLocalCount > kGameLocal ? static_cast<PageId>(Pulsar::UI::PULPAGE_SS8CHARSELECT) : id, button);
}
kmCall(0x80852a6c, ModeNextPage);
kmCall(0x80852a90, ModeNextPage);
kmCall(0x8083b924, ModeNextPage);
kmCall(0x8083b944, ModeNextPage);

}  // namespace SplitScreen8
