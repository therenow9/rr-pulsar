#ifdef SS8_MENU_SCRIPT
#include <kamek.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <SplitScreen8/SplitScreen8.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Race/CustomCharacters.hpp>
#include <Race/CustomCharacterVoice.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>

#ifdef SS8_DEBUG_BOOT
#error "a menu script and the debug boot both hook Input::Manager::CopyPADStatus's call (0x80520220)"
#endif

// Debug build only: presses GC pad buttons on ports 1-4, and Classic Controller buttons on Wii channels
// 1-4 for ports 5-8, at fixed frames from boot, so menu paths (the 3P character select, M4's join
// screen) run with no one at the pads. tools/build_code.py --menu-script turns a script file into
// SS8_MENU_STEPS; docs/toolchain.md has the format.

namespace SplitScreen8 {

struct MenuStep {
    u32 frame;  // channel 0's pad reads since boot
    u32 port;  // 0-3 GC ports, 4-7 Wii channels 0-3
    u32 buttons;  // PAD::PAD_BUTTON_* on a GC port, WPAD_CL_BUTTON_* on a Wii channel
    u32 hold;  // frames
};
static const MenuStep menuSteps[] = {SS8_MENU_STEPS};
static u32 menuFrames;
static void ReportHeaps(const char *when);
static void ReportSkins();

// GCNController::UpdateImpl+0x70 calls Input::Manager::CopyPADStatus(manager, channel, &padStatus) for
// each plugged-in port every frame, and reads padStatus after it, as DebugBoot.cpp's race script does.
typedef u32 (*CopyPADStatusFn)(Input::Manager *, u32, PAD::Status *);
static const CopyPADStatusFn copyPADStatus = reinterpret_cast<CopyPADStatusFn>(0x80524628);

static u32 CopyPADStatusMenuScript(Input::Manager *input, u32 channel, PAD::Status *status) {
    const u32 result = copyPADStatus(input, channel, status);
    if (status == nullptr)
        return result;
    for (u32 i = 0; i < sizeof(menuSteps) / sizeof(menuSteps[0]); ++i) {
        const MenuStep &step = menuSteps[i];
        if (step.port != channel || menuFrames < step.frame || menuFrames >= step.frame + step.hold)
            continue;
        if (menuFrames == step.frame)
            OS::Report("ss8 menu: step %u frame %u port %u press %04x\n", i, menuFrames, channel + 1, step.buttons);
        status->buttons |= step.buttons;
    }
    if (channel == 0)
        ReportSkins();
    if (channel == 0 && ++menuFrames % 300 == 0)
        ReportHeaps("tick");
    return result;
}
kmCall(0x80520220, CopyPADStatusMenuScript);

// WiiController::UpdateImpl+0x1E8 calls UpdateStatesClassic(controller, status, state, uiState) for a
// channel with a Classic Controller; with the extension readable it takes the held buttons from the
// status's +0x2A (WPADCLStatus), and the controller's +0x8F8 holds the last frame's. +0x8D4 is the channel.
typedef void (*UpdateStatesClassicFn)(u8 *controller, u8 *status, void *state, void *uiState);
static const UpdateStatesClassicFn updateStatesClassic = reinterpret_cast<UpdateStatesClassicFn>(0x8051f410);

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

// Every EGG heap's free bytes, in EGG's own list (0x80384320), at each section load and every 300 frames:
// a menu page's allocations land on the scene heaps (D57's method). An ExpHeap reports its total free
// bytes, any other heap its largest block.
static nw4r::ut::List *const heapList = reinterpret_cast<nw4r::ut::List *>(0x80384320);

static void ReportHeaps(const char *when) {
    u32 index = 0;
    for (void *node = nw4r::ut::List_GetNext(heapList, nullptr); node != nullptr; node = nw4r::ut::List_GetNext(heapList, node)) {
        EGG::Heap *heap = static_cast<EGG::Heap *>(node);
        const u32 kind = heap->getHeapKind();
        const u32 free = kind == EGG::HEAP_TYPE_EXP ? static_cast<EGG::ExpHeap *>(heap)->getTotalFreeSize() : heap->getAllocatableSize(4);
        OS::Report("ss8 heap: %s frame %u #%u %08x kind %u free %u name %s\n", when, menuFrames, index++, heap, kind, free, heap->name != nullptr ? heap->name : "-");
    }
}

// Each menu player's character, skin slot and whether they show the character's shared model or a
// preview of their own (RR's Driver/LocalPlayerSkins.cpp, D72), when any of the three changes.
static u32 reportedSkins[kGameLocal];

static void ReportSkins() {
    const MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    const MenuDriverModelMgr *mgr = modelMgr != nullptr && modelMgr->isActive ? modelMgr->driverModels : static_cast<const MenuDriverModelMgr *>(nullptr);
    for (u32 hud = 0; mgr != nullptr && hud < mgr->playerCount && hud < kGameLocal; ++hud) {
        const u32 character = static_cast<u32>(mgr->players[hud].id);
        if (character >= Pulsar::Driver::CHARACTER_COUNT)
            continue;
        const bool shared = mgr->players[hud].playerModel == &mgr->models[character];
        const u32 slot = Pulsar::Driver::GetLocalPlayerSlot(hud, mgr->players[hud].id);
        const u32 key = 0x80000000 | character << 16 | slot << 1 | (shared ? 1 : 0);
        if (reportedSkins[hud] == key)
            continue;
        reportedSkins[hud] = key;
        OS::Report("ss8 skin: hud %u character %#x slot %u %s\n", hud, character, slot, shared ? "shared" : "own");
    }
}

// A script's "pick" lines fill P5-8's extPicks in a 5-8 player game, which phase C's select pages will
// write; the main menu clears them (Entry.cpp), and section 0x54 loads after it.
#ifdef SS8_MENU_PICKS
struct MenuPick {
    u32 slot;  // 0-3 for P5-8
    u32 character;
    u32 kart;
};
static const MenuPick menuPicks[] = {SS8_MENU_PICKS};
#endif

// The race scenario's first 8 players, once per race at its first frame (InitRace has run by then),
// with the skin slot RR's race readers get for each and the character whose voice groups RR lends
// that player (-1 for none).
static bool racePlayersReported;

static void ReportRacePlayers() {
    if (racePlayersReported || Racedata::sInstance == nullptr)
        return;
    racePlayersReported = true;
    for (u32 i = 0; i < kMaxLocal; ++i) {
        const RacedataPlayer &player = Racedata::sInstance->racesScenario.players[i];
        const u32 slot = Pulsar::Race::GetPlayerCustomCharacterSlot(i, player.characterId);
        OS::Report("ss8 race player %u: type %d character %#x kart %#x skin %u voice %d\n", i, player.playerType, player.characterId, player.kartId, slot, Pulsar::Race::GetPlayerVoiceAlias(i));
    }
    // Each holder's drift type as RealControllerHolder::SetDriftType stores it (+0xC0, 0x80520F30), and
    // its controller's copy (+0x51), which the race reads (Kart::Status, 0x805944F4).
    for (u32 i = 0; Input::Manager::sInstance != nullptr && i < kMaxLocal; ++i) {
        const Input::RealControllerHolder &holder = Holder(*Input::Manager::sInstance, i);
        const u8 *controller = reinterpret_cast<const u8 *>(holder.curController);
        OS::Report("ss8 holder %u: drift %u controller %d\n", i, *reinterpret_cast<const u16 *>(reinterpret_cast<const u8 *>(&holder) + 0xc0), controller != nullptr ? controller[0x51] : -1);
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
