#ifdef SS8_DEBUG_BOOT
#include <kamek.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <SlotExpansion/CupsConfig.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Debug build only: boots past the menus into a VS race with 1 local player and 11 CPUs, so load
// crashes and render checks need no input. docs/plans/m2-automation.md has the design.

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
        player.playerType = i == 0 ? PLAYER_REAL_LOCAL : PLAYER_CPU;
        player.characterId = character;
        player.kartId = static_cast<KartId>(GetCharacterWeightClass(character));
    }

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
    params->vsRaceLimit = 1;
    return SECTION_P1VS;
}

// The title's "press A" binds pad 0 to a holder; the boot skips it. InitControllerHolders gives
// connected controllers the holders in order, and RR's SetUpCorrectController binds pad 0 only to a
// leftover holder, so a connected pad 0 stays unbound and the race shows "controller interrupted".
static void BindBootPad() {
    SectionPad &pad = SectionMgr::sInstance->pad;
    PadInfo &info = pad.padInfos[0];
    Input::Manager *input = Input::Manager::sInstance;
    if (info.controllerHolder != nullptr && info.controllerIDActive == info.controllerID) return;
    const u32 channel = ((info.controllerID & 0xFF00) >> 8) - 1;
    if (input == nullptr || channel > 3) return;
    const Input::Controller *controller = pad.GetType(info.controllerID) == GCN
                                              ? static_cast<Input::Controller *>(&input->gcnControllers[channel])
                                              : static_cast<Input::Controller *>(&input->wiiControllers[channel]);
    for (int i = 0; i < 4; ++i) {
        if (input->realControllerHolders[i].curController != controller) continue;
        info.controllerHolder = &input->realControllerHolders[i];
        info.controllerIDActive = info.controllerID;
        return;
    }
}
static RaceLoadHook bindBootPad(BindBootPad);

}  // namespace SplitScreen8
#endif
