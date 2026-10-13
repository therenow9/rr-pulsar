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

// Presses scripted GC buttons on ports 1-4 and Classic Controller buttons on Wii channels 1-4 at fixed frames from boot.

namespace SplitScreen8 {

struct MenuStep {
    u32 frame;  // channel 0's pad reads since boot
    u32 port;  // 0-3 GC ports, 4-7 Wii channels 0-3
    u32 buttons;  // PAD::PAD_BUTTON_* on a GC port, WPAD_CL_BUTTON_* on a Wii channel; 0 unplugs the port
    u32 hold;  // frames
};
static const MenuStep menuSteps[] = {SS8_MENU_STEPS};
static u32 menuFrames;
#ifdef SS8_MENU_OPEN_MII_C
bool menuOpenMiiC = true;
#else
bool menuOpenMiiC = false;
#endif
static void ReportHeaps(const char *when);
static void ReportSkins();

// GCNController::UpdateImpl+0x70 calls Input::Manager::CopyPADStatus(manager, channel, &padStatus) for each
// plugged-in port every frame, and reads padStatus after it; a result of 2 (no controller) disconnects it (+0x514).
static u32 CopyPADStatusMenuScript(Input::Manager *input, u32 channel, PAD::Status *status) {
    u32 result = input->CopyPADStatus(channel, status);
    if (status == nullptr)
        return result;
    for (u32 i = 0; i < sizeof(menuSteps) / sizeof(menuSteps[0]); ++i) {
        const MenuStep &step = menuSteps[i];
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
    for (u32 i = 0; i < sizeof(menuSteps) / sizeof(menuSteps[0]); ++i) {
        const MenuStep &step = menuSteps[i];
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
    for (u32 i = 0; i < sizeof(menuSteps) / sizeof(menuSteps[0]); ++i) {
        const MenuStep &step = menuSteps[i];
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

// A script's "pick" lines fill P5-8's extPicks at each section load; the main menu clears them (Entry.cpp).
#ifdef SS8_MENU_PICKS
struct MenuPick {
    u32 slot;  // 0-3 for P5-8
    u32 character;
    u32 kart;
};
static const MenuPick menuPicks[] = {SS8_MENU_PICKS};
#endif

// Every race scenario player, CPUs included, once per race at its first frame (after InitRace), with the skin slot
// RR's race readers get and the character whose voice groups RR lends it (-1 for none).
static bool racePlayersReported;

static void ReportRacePlayers() {
    if (racePlayersReported || Racedata::sInstance == nullptr)
        return;
    racePlayersReported = true;
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    for (u32 i = 0; i < sizeof(scenario.players) / sizeof(scenario.players[0]); ++i) {
        const RacedataPlayer &player = scenario.players[i];
        const u32 slot = Pulsar::Race::GetPlayerCustomCharacterSlot(i, player.characterId);
        OS::Report("ss8 race player %u: type %d hud %d character %#x kart %#x skin %u voice %d\n", i, player.playerType, player.hudSlotId, player.characterId, player.kartId, slot,
          Pulsar::Race::GetPlayerVoiceAlias(i));
    }
    // Each holder's drift type as RealControllerHolder::SetDriftType stores it (+0xC0, 0x80520F30), and
    // its controller's copy (+0x51), which the race reads (Kart::Status, 0x805944F4).
    for (u32 i = 0; Input::Manager::sInstance != nullptr && i < kMaxLocal; ++i) {
        const Input::RealControllerHolder &holder = Holder(*Input::Manager::sInstance, i);
        const u8 *controller = reinterpret_cast<const u8 *>(holder.curController);
        OS::Report("ss8 holder %u: drift %u controller %d transmission %d\n", i, holder.isDriftAuto, controller != nullptr ? controller[0x51] : -1, Pulsar::UI::GetSelectedTransmission(i));
    }
}
static RaceFrameHook reportRacePlayers(ReportRacePlayers);

static void OnSectionLoad() {
    racePlayersReported = false;
    memset(reportedSkins, 0, sizeof(reportedSkins));
#ifdef SS8_MENU_PICKS
    for (u32 i = 0; menuLocalCount > kGameLocal && i < sizeof(menuPicks) / sizeof(menuPicks[0]); ++i) {
        ExtPick &pick = extPicks[menuPicks[i].slot];
        pick.character = static_cast<CharacterId>(menuPicks[i].character);
        pick.kart = static_cast<KartId>(menuPicks[i].kart);
        pick.picked = true;
    }
#endif
    ReportHeaps("section");
}
static SectionLoadHook menuScriptSectionLoad(OnSectionLoad);

}  // namespace SplitScreen8
#endif
