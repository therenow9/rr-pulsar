#ifdef SS8_MENU_SCRIPT
#include <kamek.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/KPAD.hpp>
#include <core/rvl/WPAD.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/nw4r/snd/BasicSound.hpp>
#include <core/nw4r/snd/WsdFile.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <PulsarSystem.hpp>
#include <IO/IO.hpp>
#include <Settings/Settings.hpp>
#include <UI/ExtendedTeamSelect/ExtendedTeamManager.hpp>
#include <SplitScreen8/SplitScreen8.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Race/CustomCharacters.hpp>
#include <Race/CustomCharacterVoice.hpp>
#include <UI/TransmissionSelect/TransmissionSelect.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>

#ifdef SS8_DEBUG_BOOT
#error "a menu script and the debug boot both hook Input::Manager::CopyPADStatus's call (0x80520220)"
#endif

// Presses scripted GC buttons on ports 1-4 and Classic Controller buttons on Wii channels 1-4 at fixed frames from boot,
// as the copy's ss8-menu.bin says (install_8p.py script writes it into the NAND's mod folder).

namespace SplitScreen8 {

struct MenuStep {
    u32 frame;  // channel 0's pad reads since boot
    u32 port;  // 0-3 GC ports, 4-7 Wii channels 0-3
    u32 buttons;  // PAD::PAD_BUTTON_* on a GC port, WPAD_CL_BUTTON_* on a Wii channel; 0 unplugs the port
    u32 hold;  // frames
};
// A script's "pick" lines fill P5-8's extPicks at each section load; the main menu clears them (Entry.cpp).
struct MenuPick {
    u32 slot;  // 0-3 for P5-8
    u32 character;
    u32 kart;
};
// A script's "setting ID VALUE" lines, set at each section load; RR's next settings save writes them to the copy's file.
struct MenuSetting {
    u32 id;  // Pulsar::Settings::SettingId
    u32 value;
};
enum MenuFlags {
    MENU_OPEN_MII_C = 1,  // Mii C open on the 8-player character page whatever the license's VR
    MENU_AUTODRIVE = 2  // every local's kart to the AI a second into each race
};
// The file's layout, big-endian, whole: install_8p.py's script subcommand packs the same fields.
struct MenuScript {
    u32 stepCount;
    u32 pickCount;
    u32 settingCount;
    u32 flags;
    MenuStep steps[1024];
    MenuPick picks[4];
    MenuSetting settings[8];
};
static alignas(0x20) MenuScript menuScript;
static u32 menuFrames;
bool menuOpenMiiC;
static void ReportHeaps(const char *when);
static void ReportSkins();

// GCNController::UpdateImpl+0x70 calls Input::Manager::CopyPADStatus(manager, channel, &padStatus) for each
// plugged-in port every frame, and reads padStatus after it; a result of 2 (no controller) disconnects it (+0x514).
static u32 CopyPADStatusMenuScript(Input::Manager *input, u32 channel, PAD::Status *status) {
    u32 result = input->CopyPADStatus(channel, status);
    if (status == nullptr)
        return result;
    for (u32 i = 0; i < menuScript.stepCount; ++i) {
        const MenuStep &step = menuScript.steps[i];
        if (step.port != channel || menuFrames < step.frame || menuFrames >= step.frame + step.hold)
            continue;
        if (menuFrames == step.frame)
            OS::Report("ss8 menu: step %u frame %u port %u press %04x\n", i, menuFrames, channel + 1, step.buttons);
        status->buttons |= step.buttons;
        if (step.buttons == 0) {
            status->error = PAD::PAD_ERR_NO_CONTROLLER;
            result = 2;
        }
    }
    if (channel == 0)
        ReportSkins();
    if (channel == 0 && ++menuFrames % 300 == 0)
        ReportHeaps("tick");
    return result;
}
kmCall(0x80520220, CopyPADStatusMenuScript);

// WiiController::UpdateImpl+0x1E8 calls UpdateStatesClassic for a Classic Controller channel; it takes held buttons
// from the status's +0x2A (WPADCLStatus). The controller's +0x8D4 is its channel, +0x8F8 the last frame's buttons.
static void UpdateStatesClassicMenuScript(u8 *controller, u8 *status, void *state, void *uiState) {
    const u32 port = 4 + *reinterpret_cast<const u32 *>(controller + 0x8d4);
    for (u32 i = 0; i < menuScript.stepCount; ++i) {
        const MenuStep &step = menuScript.steps[i];
        if (step.port != port || menuFrames < step.frame || menuFrames >= step.frame + step.hold)
            continue;
        if (menuFrames == step.frame)
            OS::Report("ss8 menu: step %u frame %u port %u press %04x\n", i, menuFrames, port + 1, step.buttons);
        *reinterpret_cast<u16 *>(status + 0x2a) |= step.buttons;
    }
    updateStatesClassic(controller, status, state, uiState);
}
kmCall(0x8051fe6c, UpdateStatesClassicMenuScript);

// An unplug step on a Wii channel: WiiController::UpdateImpl+0x44 probes the channel and +0xAC reads it; a failed probe
// and no reads take the game's own disconnect path (the controller's +0x50 cleared, its type -1).
static bool Unplugged(u32 port, bool report) {
    for (u32 i = 0; i < menuScript.stepCount; ++i) {
        const MenuStep &step = menuScript.steps[i];
        if (step.port != port || step.buttons != 0 || menuFrames < step.frame || menuFrames >= step.frame + step.hold)
            continue;
        if (report && menuFrames == step.frame)
            OS::Report("ss8 menu: step %u frame %u port %u press %04x\n", i, menuFrames, port + 1, step.buttons);
        return true;
    }
    return false;
}

static s32 ProbeMenuScript(s32 channel, u32 *type) {
    return Unplugged(4 + channel, true) ? -1 : WPAD::Probe(channel, type);
}
kmCall(0x8051fcc8, ProbeMenuScript);

static s32 ReadMenuScript(s32 channel, KPAD::Status *status, u32 count) {
    return Unplugged(4 + channel, false) ? 0 : KPAD::Read(channel, status, count);
}
kmCall(0x8051fd30, ReadMenuScript);

// Script presses never reach WPAD, so its 5-minute idle sleep would disconnect every scripted remote mid-lane:
// WPADiCheckContInputs loads the sleep minutes here (lbz r0, -0x5fc6(r13)) and skips the sleep on 0.
kmWrite32(0x801be8a8, 0x38000000);

// Every EGG heap (EGG's list, 0x80384320) at each section load and every 300 frames. "free" is an ExpHeap's total
// free or another heap's largest block; "largest" is every heap's largest block, the one a skin load needs.
static nw4r::ut::List *const heapList = reinterpret_cast<nw4r::ut::List *>(0x80384320);

static void ReportHeaps(const char *when) {
    u32 index = 0;
    for (void *node = nw4r::ut::List_GetNext(heapList, nullptr); node != nullptr; node = nw4r::ut::List_GetNext(heapList, node)) {
        EGG::Heap *heap = static_cast<EGG::Heap *>(node);
        const u32 kind = heap->getHeapKind();
        const u32 free = kind == EGG::HEAP_TYPE_EXP ? static_cast<EGG::ExpHeap *>(heap)->getTotalFreeSize() : heap->getAllocatableSize(4);
        OS::Report(
          "ss8 heap: %s frame %u #%u %08x kind %u free %u largest %u name %s\n", when, menuFrames, index++, heap, kind, free, heap->getAllocatableSize(4), heap->name != nullptr ? heap->name : "-");
    }
}

// Each menu player's character, skin slot and shared or own model, logged when any of the three changes.
static u32 reportedSkins[kGameLocal];

static void ReportSkins() {
    const MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    const MenuDriverModelMgr *mgr = modelMgr != nullptr && modelMgr->isActive ? modelMgr->driverModels : static_cast<const MenuDriverModelMgr *>(nullptr);
    for (u32 hud = 0; mgr != nullptr && hud < mgr->playerCount && hud < kGameLocal; ++hud) {
        const u32 character = static_cast<u32>(mgr->players[hud].id);
        if (character >= Pulsar::Driver::CHARACTER_COUNT)
            continue;
        const bool shared = mgr->players[hud].playerModel == &mgr->models[character];
        const u32 slot = SkinSlot(hud, mgr->players[hud].id);
        const u32 key = 0x80000000 | character << 16 | slot << 1 | (shared ? 1 : 0);
        if (reportedSkins[hud] == key)
            continue;
        reportedSkins[hud] = key;
        OS::Report("ss8 skin: hud %u character %#x slot %u %s\n", hud, character, slot, shared ? "shared" : "own");
    }
}

// The picker (0x80868ABC) plays at 0x80868B84 (mulli r3, r28, 0xc0), r28 the character and r31 the type.
static u32 LogMenuVoicePlay() {
    register u32 character, type;
    asm(mr character, r28;);
    asm(mr type, r31;);
    OS::Report("ss8 voice: type %u plays character %#x frame %u\n", type, character, menuFrames);
    return character * 0xc0;
}
kmCall(0x80868b84, LogMenuVoicePlay);

// 0x80868AE4 (li r3, 0): the character is negative, a silent skin or a skin whose own trick line RR's hook
// already started, and the picker returns with no sound.
static u32 LogMenuVoiceNone() {
    register u32 type;
    asm(mr type, r31;);
    OS::Report("ss8 voice: type %u no sound frame %u\n", type, menuFrames);
    return 0;
}
kmCall(0x80868ae4, LogMenuVoiceNone);

// 0x800A2D60 (bl WsdFileReader::ReadWaveInfo in WsdCallback::GetWaveSoundData): a note looks its wave up in
// the wave archive the player resolved for its file id; a skin's trick line reads RR's trimmed archive.
static u32 waveLogs;
static bool LogReadWaveInfo(void *reader, int waveIndex, void *info, const void *origin) {
    const bool ok = static_cast<const nw4r::snd::detail::WsdFileReader *>(reader)->ReadWaveInfo(waveIndex, static_cast<nw4r::snd::detail::WaveInfo *>(info), origin);
    if (waveLogs < 48 && origin != nullptr) {
        ++waveLogs;
        OS::Report("ss8 wave: index %d archive %p ok %d frame %u\n", waveIndex, origin, ok, menuFrames);
    }
    return ok;
}
kmCall(0x800a2d60, LogReadWaveInfo);

// Every race scenario player, CPUs included, once per race at its first frame (after InitRace), with the skin slot
// RR's race readers get and the character whose voice groups RR lends it (-1 for none).
static bool racePlayersReported;

static void ReportRacePlayers() {
    if (racePlayersReported || Racedata::sInstance == nullptr)
        return;
    racePlayersReported = true;
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    Pulsar::UI::ExtendedTeamManager *teams = Pulsar::UI::ExtendedTeamManager::sInstance;
    for (u32 i = 0; i < sizeof(scenario.players) / sizeof(scenario.players[0]); ++i) {
        const RacedataPlayer &player = scenario.players[i];
        const u32 slot = Pulsar::Race::GetPlayerCustomCharacterSlot(i, player.characterId);
        OS::Report("ss8 race player %u: type %d hud %d ch %d character %#x kart %#x skin %u voice %d team %d ext %d\n", i, player.playerType, player.hudSlotId, player.realControllerChannel,
          player.characterId, player.kartId, slot, Pulsar::Race::GetPlayerVoiceAlias(i), player.team, teams != nullptr ? static_cast<s32>(teams->GetPlayerTeam(i)) : -1);
    }
    // RR's race modes as UpdateContext set them for this race, and the series length a KO race forces.
    const Pulsar::System *system = Pulsar::System::sInstance;
    OS::Report("ss8 context: gamemode %d gametype %d battle %d ffa %d elim %d ko %d lapko %d kofinal %d royale %d ott %d extteams %d vsRaceCount %u komgr %d race %u\n", scenario.settings.gamemode,
      scenario.settings.gametype, scenario.settings.battleType, system->IsContext(Pulsar::PULSAR_FFA), system->IsContext(Pulsar::PULSAR_ELIMINATION), system->IsContext(Pulsar::PULSAR_MODE_KO),
      system->IsContext(Pulsar::PULSAR_MODE_LAPKO), system->IsContext(Pulsar::PULSAR_KOFINAL), system->IsContext(Pulsar::PULSAR_MODE_BATTLEROYALE), system->IsContext(Pulsar::PULSAR_MODE_OTT),
      system->IsContext(Pulsar::PULSAR_EXTENDEDTEAMS), SectionMgr::sInstance->sectionParams->vsRaceCount, system->koMgr != nullptr, scenario.settings.raceNumber);
    // Each holder's drift type as RealControllerHolder::SetDriftType stores it (+0xC0, 0x80520F30), and
    // its controller's copy (+0x51), which the race reads (Kart::Status, 0x805944F4).
    for (u32 i = 0; Input::Manager::sInstance != nullptr && i < kMaxLocal; ++i) {
        const Input::RealControllerHolder &holder = Holder(*Input::Manager::sInstance, i);
        const u8 *controller = reinterpret_cast<const u8 *>(holder.curController);
        OS::Report("ss8 holder %u: drift %u controller %d transmission %d pad %#x\n", i, holder.isDriftAuto, controller != nullptr ? controller[0x51] : -1, Pulsar::UI::GetSelectedTransmission(i),
          calculateID(&holder));
    }
}
static RaceFrameHook reportRacePlayers(ReportRacePlayers);

// Every racer's battle score and team once the round has ended.
static bool battleScoresReported;

static void ReportBattleScores() {
    const Raceinfo *raceinfo = Raceinfo::sInstance;
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    if (battleScoresReported || raceinfo == nullptr || raceinfo->stage != RACESTAGE_FINISHED || scenario.settings.gamemode != MODE_BATTLE)
        return;
    battleScoresReported = true;
    for (u32 i = 0; i < scenario.playerCount; ++i)
        OS::Report("ss8 battle player %u: type %d hud %d team %d score %u frame %u\n", i, scenario.players[i].playerType, scenario.players[i].hudSlotId, scenario.players[i].team,
          raceinfo->players[i]->battleScore, menuFrames);
}
static RaceFrameHook reportBattleScores(ReportBattleScores);

// Each change of a Battle race's top page, so a lane can shoot the scoreboards (0x33, 0x34) and the round's menu
// (0x23, or 0x24 after the last round): they are pages, not sections.
static PageId battleTopPage;

static void ReportBattlePages() {
    const Page *page = SectionMgr::sInstance->curSection->GetTopLayerPage();
    if (page == nullptr || page->pageId == battleTopPage || Racedata::sInstance->racesScenario.settings.gamemode != MODE_BATTLE)
        return;
    battleTopPage = page->pageId;
    OS::Report("ss8 battle page %#x frame %u\n", battleTopPage, menuFrames);
}
static RaceFrameHook reportBattlePages(ReportBattlePages);

// A script's "autodrive" line: every local's kart goes to the AI a second into each race, so a lane reaches the
// results; the awards and the one-screen "Next Race" scene are other gametypes.
static void AutodriveMenuRace() {
    const Raceinfo *raceinfo = Raceinfo::sInstance;
    if ((menuScript.flags & MENU_AUTODRIVE) != 0 && raceinfo != nullptr && raceinfo->raceFrames == 60 && Racedata::sInstance->racesScenario.settings.gametype == GAMETYPE_DEFAULT)
        AutodriveLocals();
}
static RaceFrameHook autodriveMenuRace(AutodriveMenuRace);

// The first section load is past System::Init's IO setup and long before the first scripted press (frame ~900).
// A missing or short file reads as no script.
static bool menuScriptRead;

static void OnSectionLoad() {
    if (!menuScriptRead) {
        menuScriptRead = true;
        char path[IOS::ipcMaxPath];
        snprintf(path, IOS::ipcMaxPath, "%s/ss8-menu.bin", Pulsar::System::sInstance->GetModFolder());
        if (Pulsar::IO::sInstance->ReadFolderFileFromPath(&menuScript, path, sizeof(menuScript)) != sizeof(menuScript)) {
            memset(&menuScript, 0, sizeof(menuScript));
            OS::Report("ss8 menu: no script at %s frame %u\n", path, menuFrames);
        } else
            OS::Report(
              "ss8 menu: script %s steps %u picks %u settings %u flags %#x frame %u\n", path, menuScript.stepCount, menuScript.pickCount, menuScript.settingCount, menuScript.flags, menuFrames);
        menuOpenMiiC = (menuScript.flags & MENU_OPEN_MII_C) != 0;
    }
    racePlayersReported = false;
    battleScoresReported = false;
    battleTopPage = PAGE_NONE;
    memset(reportedSkins, 0, sizeof(reportedSkins));
    OS::Report("ss8 section %#x frame %u locals %u\n", SectionMgr::sInstance->curSection->sectionId, menuFrames, menuLocalCount);
    for (u32 i = 0; Pulsar::Settings::Mgr::IsCreated() && i < menuScript.settingCount; ++i)
        Pulsar::Settings::Mgr::Get().SetSettingValue(static_cast<Pulsar::Settings::SettingId>(menuScript.settings[i].id), menuScript.settings[i].value);
    for (u32 i = 0; menuLocalCount > kGameLocal && i < menuScript.pickCount; ++i) {
        ExtPick &pick = extPicks[menuScript.picks[i].slot];
        pick.character = static_cast<CharacterId>(menuScript.picks[i].character);
        pick.kart = static_cast<KartId>(menuScript.picks[i].kart);
        pick.picked = true;
    }
    ReportHeaps("section");
}
static SectionLoadHook menuScriptSectionLoad(OnSectionLoad);

}  // namespace SplitScreen8
#endif
