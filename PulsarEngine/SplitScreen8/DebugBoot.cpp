#ifdef SS8_DEBUG_BOOT
#include <kamek.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Settings/Settings.hpp>
#include <SlotExpansion/CupsConfig.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Debug build only: boots past the menus into a VS race with SS8_BOOT_LOCALS local players on GC
// ports 1..N and CPUs after them, so load crashes and render checks need no input.
// docs/plans/m2-automation.md has the design.

namespace SplitScreen8 {

// Runs inside SectionMgr::Init, after Racedata, RKSYS, CupsConfig and SectionParams exist. The menu
// scenario is filled as Title::PrepareDemo fills its no-menu race; InitRace copies it into the race.
SectionId DebugBootPrepare(SectionId section) {
    Racedata *racedata = Racedata::sInstance;
    Pulsar::CupsConfig *cups = Pulsar::CupsConfig::sInstance;
    SectionMgr *sectionMgr = SectionMgr::sInstance;
    const Pulsar::PulsarId track = static_cast<Pulsar::PulsarId>(SS8_BOOT_TRACK);
    if (racedata == nullptr || cups == nullptr || sectionMgr == nullptr || sectionMgr->sectionParams == nullptr) return section;
    if (!cups->IsValidTrack(track)) return section;

    racedata->ResetScenarios();
    RacedataScenario &scenario = racedata->menusScenario;
    RacedataSettings &settings = scenario.settings;
    settings.gamemode = MODE_VS_RACE;
    settings.gametype = GAMETYPE_DEFAULT;
    settings.engineClass = CC_150;
    settings.cpuMode = CPU_NORMAL;
    settings.itemMode = ITEMS_NONE;
    settings.lapCount = 3;
    settings.raceNumber = 0;
    settings.modeFlags &= ~3;  // mirror, teams

    // Characters 0..11 are distinct non-Mii drivers; each takes the Standard Kart of its weight
    // class (KartId = weight class for the first three karts). Player 0 is Mario on Standard Kart M.
    for (int i = 0; i < 12; ++i) {
        RacedataPlayer &player = scenario.players[i];
        const CharacterId character = static_cast<CharacterId>(i);
        player.playerType = i < SS8_BOOT_LOCALS ? PLAYER_REAL_LOCAL : PLAYER_CPU;
        player.characterId = character;
        player.kartId = static_cast<KartId>(GetCharacterWeightClass(character));
    }
    // Player 0 keeps the pad RR's BootIntoSection registered; the others take GC ports 2..N, as
    // GCN controller IDs (port + 1) << 8 | 0x24.
    for (int i = 1; i < SS8_BOOT_LOCALS; ++i) sectionMgr->pad.padInfos[i].controllerID = (i + 1) << 8 | 0x24;

    // The track goes through CupsConfig: RR's FormatTrackPath loads GetWinning(), not courseId.
    cups->SetWinning(track, 0);
    cups->SetSelected(track);
    cups->lastSelectedCup = Pulsar::CupsConfig::ConvertCup_PulsarTrackToCup(track);
    settings.courseId = cups->GetCorrectTrackSlot();
    settings.cupId = cups->lastSelectedCup % 8;
    SectionParams *params = sectionMgr->sectionParams;
    params->vsTracks[0] = static_cast<CourseId>(track);
    cups->vsTrackVariantIdx[0] = 0;
    params->vsRaceNumber = 0;
    params->vsRaceLimit = 4;

    // RR gives offline CPUs custom skins only outside local multiplayer, and ships them at full LOD
    // only, while a widened 1-local boot loads the 4-screen LOD (+7 = 4): the boot shows the default
    // skins a multi-local race would. RAM only; RR saves settings from its settings page.
    if (Pulsar::Settings::Mgr::IsCreated())
        Pulsar::Settings::Mgr::Get().SetSettingValue(Pulsar::Settings::SETTING_DISPLAYCUSTOMSKINS, Pulsar::DISPLAYCUSTOMSKINS_DISABLED);
    return static_cast<SectionId>(SECTION_P1VS + SS8_BOOT_LOCALS - 1);
}

// The title's "press A" binds pad 0 to a holder and the menus bind the others; the boot skips
// both. InitControllerHolders gives connected controllers the holders in order, and RR's
// SetUpCorrectController binds pad 0 only to a leftover holder, so a connected pad stays unbound
// and the race shows "controller interrupted". A port no holder carries (the boot's ports 2-4
// hold the dummy) is given the player's own holder, as SetUpCorrectController does.
static void BindBootPad(SectionPad &pad, u32 slot) {
    PadInfo &info = pad.padInfos[slot];
    Input::Manager *input = Input::Manager::sInstance;
    if (info.controllerHolder != nullptr && info.controllerIDActive == info.controllerID) return;
    const u32 channel = ((info.controllerID & 0xFF00) >> 8) - 1;
    if (input == nullptr || channel > 3) return;
    Input::Controller *controller = pad.GetType(info.controllerID) == GCN
                                        ? static_cast<Input::Controller *>(&input->gcnControllers[channel])
                                        : static_cast<Input::Controller *>(&input->wiiControllers[channel]);
    Input::RealControllerHolder *holder = nullptr;
    for (int i = 0; i < 4 && holder == nullptr; ++i)
        if (input->realControllerHolders[i].curController == controller) holder = &input->realControllerHolders[i];
    if (holder == nullptr) {
        holder = &input->realControllerHolders[slot];
        holder->SetController(controller, nullptr);
    }
    info.controllerHolder = holder;
    info.controllerIDActive = info.controllerID;
    info.status = PadInfo::STATUS_CONNECTED;
}

// Races loaded since boot, and pad reads since the latest one; the scripted presses count from them.
static u32 bootRaces;
static u32 raceFrames;

static void BindBootPads() {
    SectionPad &pad = SectionMgr::sInstance->pad;
    for (int i = 0; i < SS8_BOOT_LOCALS; ++i) BindBootPad(pad, i);
    ++bootRaces;
    raceFrames = 0;
}
static RaceLoadHook bindBootPads(BindBootPads);

// GC port 1 drives the pause menu about 25 s into each race: races 1 and 2 restart (START, Down to
// "Restart", A, Up to "Yes", A) and race 3 quits (START, Down, Down to "Quit", A, Up, A). A press
// once a second then walks the menus into the next race, so the restart and menu-to-race paths run
// with no one at the pads. "Are you sure?" defaults to No.
// GCNController::UpdateImpl+0x70 calls Input::Manager::CopyPADStatus(manager, channel, &padStatus)
// and reads padStatus after it, and its result (0 connected, 1 error, 2 no controller); RR's own
// pad hooks sit at the PADRead calls instead.
typedef u32 (*CopyPADStatusFn)(Input::Manager *, u32, PAD::Status *);
static const CopyPADStatusFn copyPADStatus = reinterpret_cast<CopyPADStatusFn>(0x80524628);

static u16 ScriptedButtons(u32 frame) {
    static const u16 restart[] = {PAD::PAD_BUTTON_START, PAD::PAD_BUTTON_DOWN, PAD::PAD_BUTTON_A, PAD::PAD_BUTTON_UP,
                                  PAD::PAD_BUTTON_A};
    static const u16 quit[] = {PAD::PAD_BUTTON_START, PAD::PAD_BUTTON_DOWN, PAD::PAD_BUTTON_DOWN, PAD::PAD_BUTTON_A,
                               PAD::PAD_BUTTON_UP, PAD::PAD_BUTTON_A};
    const bool quitting = bootRaces % 3 == 0;
    const u16 *steps = quitting ? quit : restart;
    const u32 count = (quitting ? sizeof(quit) : sizeof(restart)) / sizeof(u16);
    if (frame < 1500) return 0;
    const u32 step = (frame - 1500) / 60;
    if (step < count) return (frame - 1500) % 60 < 6 ? steps[step] : 0;
    return step >= count + 3 && frame % 60 < 5 ? PAD::PAD_BUTTON_A : 0;
}

static u32 CopyPADStatusScripted(Input::Manager *input, u32 channel, PAD::Status *status) {
    const u32 result = copyPADStatus(input, channel, status);
    if (channel == 0 && status != nullptr) {
        const u16 buttons = ScriptedButtons(raceFrames);
        if (buttons != 0 && ScriptedButtons(raceFrames - 1) != buttons)
            OS::Report("ss8 boot: race %u press %04x at %u\n", bootRaces, buttons, raceFrames);
        status->buttons |= buttons;
        ++raceFrames;
    }
    return result;
}
kmCall(0x80520220, CopyPADStatusScripted);

}  // namespace SplitScreen8
#endif
