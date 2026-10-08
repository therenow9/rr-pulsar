#ifdef SS8_DEBUG_BOOT
#include <kamek.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/ItemPlayer.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Settings/Settings.hpp>
#include <SlotExpansion/CupsConfig.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Debug build only: boots past the menus into a VS race with SS8_BOOT_LOCALS local players on GC
// ports 1..4, then Wii Remote channels 1..4 (D24), and CPUs after them, so load crashes and render
// checks need no input. docs/plans/m2-automation.md has the design.

namespace SplitScreen8 {

const int kBootGCLocals = SS8_BOOT_LOCALS < kGameLocal ? SS8_BOOT_LOCALS : kGameLocal;

#if SS8_BOOT_LOCALS > 4
// Players 5..N hold Wii channels 0..N-5, bound as TrySetController binds a pad (0x80523FCC):
// current and previous controller, the +0xC copy InitControllers restores each race, and params.
typedef void (*FillParamsFn)(Input::ControllerParams *, const Input::Controller *);
static const FillParamsFn fillParams = reinterpret_cast<FillParamsFn>(0x80522364);

static void BindBootWiiHolders() {
    Input::Manager *input = Input::Manager::sInstance;
    if (input == nullptr) return;
    for (u32 id = kGameLocal; id < SS8_BOOT_LOCALS; ++id) {
        Input::RealControllerHolder &holder = Holder(*input, id);
        Input::Controller *controller = &input->wiiControllers[id - kGameLocal];
        if (holder.curController != controller) holder.SetController(controller, nullptr);
        holder.controller3 = holder.curController;
        fillParams(&holder.params, holder.curController);
    }
}
#endif

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
    settings.itemMode = SS8_BOOT_ITEMS ? ITEMS_BALANCED : ITEMS_NONE;
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
    // Player 0 keeps the pad RR's BootIntoSection registered; players 1-3 take GC ports 2..4, as
    // GCN controller IDs (port + 1) << 8 | 0x24. SectionPad has no pad for players 5-8 (M4).
    for (int i = 1; i < kBootGCLocals; ++i) sectionMgr->pad.padInfos[i].controllerID = (i + 1) << 8 | 0x24;
#if SS8_BOOT_LOCALS > 4
    BindBootWiiHolders();
#endif

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
#if SS8_BOOT_SPEEDO
    if (Pulsar::Settings::Mgr::IsCreated())
        Pulsar::Settings::Mgr::Get().SetSettingValue(Pulsar::Settings::SETTING_SPEEDOMETER, Pulsar::SOM_DIGITS_0);
#endif
    // 5-8 locals race in the 4P section: the scenario's local count stops at 4 (D25).
    return static_cast<SectionId>(SECTION_P1VS + kBootGCLocals - 1);
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

#if SS8_BOOT_AUTODRIVE
// Every local is driven as a finished player is: its kart reads its AI holder (RaceinfoPlayer::EndRace,
// 0x80534904), is flagged CPU-controlled, and its AI switches to post-race driving (0x8058F0E8 and
// 0x8058F1D0, from EndRace). All of them finish, and the results and next-race paths run with no one at
// the pads. Run a second into the race: AI::Player::Init clears the AI's flag at the race's start.
typedef void (*StartPostRaceAIFn)(void *kartAIController);
static const StartPostRaceAIFn startPostRaceAI = reinterpret_cast<StartPostRaceAIFn>(0x807263a8);

static void AutodriveLocals() {
    Input::Manager *input = Input::Manager::sInstance;
    Raceinfo *raceinfo = Raceinfo::sInstance;
    Kart::Manager *karts = Kart::Manager::sInstance;
    if (input == nullptr || raceinfo == nullptr || raceinfo->players == nullptr || karts == nullptr) return;
    for (int id = 0; id < SS8_BOOT_LOCALS; ++id) {
        Kart::Player *kart = karts->GetKartPlayer(id);
        raceinfo->players[id]->realControllerHolder =
            reinterpret_cast<Input::RealControllerHolder *>(&input->virtualControllerHolders[id]);
        kart->pointers.kartStatus->bitfield4 |= 1;
        startPostRaceAI(*reinterpret_cast<void **>(reinterpret_cast<u8 *>(kart->kartSub) + 0x20));
    }
}
#endif

#if SS8_BOOT_RESPAWN
// Players respawn one by one through the game's own fall path, as a squish respawn does
// (Kart::Damage::ApplySquishRespawnDamage+0xA0: Collision::ActivateOOB through its thunk with 1, 0, 0),
// so the wipe, the camera and Lakitu run as after a real fall. The order puts players on both rows and
// on inner and outer tiles first, then the first CPU, which in a widened race holds a spare tile.
typedef void (*ActivateOOBFn)(Kart::Collision *, u32, u32, u32);
typedef Kart::Collision *(*GetCollisionFn)(Kart::Link *);
static const ActivateOOBFn activateOOB = reinterpret_cast<ActivateOOBFn>(0x80573ec4);
static const GetCollisionFn getCollision = reinterpret_cast<GetCollisionFn>(0x8059084c);
static const u8 respawnOrder[] = {0, 1, 2, 4, 7, 5, 3, 6};
const u32 kRespawnFirst = 900;
const u32 kRespawnStep = 180;
const u32 kRespawnLocals = SS8_BOOT_LOCALS < 5 ? SS8_BOOT_LOCALS : 5;
const u32 kRespawns = kRespawnLocals + (SS8_BOOT_LOCALS < 12 ? 1 : 0);

static void RespawnScripted(u32 frame) {
    if (frame < kRespawnFirst || (frame - kRespawnFirst) % kRespawnStep != 0) return;
    const u32 k = (frame - kRespawnFirst) / kRespawnStep;
    if (k >= kRespawns) return;
    u32 id = SS8_BOOT_LOCALS;
    for (u32 i = 0, n = 0; i < sizeof(respawnOrder) && k < kRespawnLocals; ++i) {
        if (respawnOrder[i] >= SS8_BOOT_LOCALS) continue;
        if (n++ == k) {
            id = respawnOrder[i];
            break;
        }
    }
    Kart::Manager *karts = Kart::Manager::sInstance;
    if (karts == nullptr) return;
    OS::Report("ss8 boot: race %u respawn player %u at %u\n", bootRaces, id, frame);
    activateOOB(getCollision(karts->GetKartPlayer(id)), 1, 0, 0);
}
const u32 kRespawnEnd = kRespawnFirst + kRespawns * kRespawnStep;
#else
const u32 kRespawnEnd = 0;
#endif

#if SS8_BOOT_USE_ITEM
// The last-placed racer uses the item (a Blooper, POW or Lightning then reaches every tile), P5 a
// Bullet Bill, or the last local a Mega Mushroom, a star or a Thundercloud (on the last tile), set into
// its inventory first as a pickup would: each Use* removes one.
typedef void (*UseItemFn)(Item::Player *);
typedef void (*SetItemFn)(Item::PlayerInventory *, ItemId, bool);
static const ItemId bootItems[] = {BLOOPER, POW_BLOCK, LIGHTNING, BULLET_BILL, MEGA_MUSHROOM, STAR, THUNDER_CLOUD};
// Item::Player::UseBlooper, UsePow, UseThunder, UseBullet, UseMegaMushroom, UseStar and UseTC, in bootItems' order.
static const u32 useItems[] = {0x807a81b4, 0x807b1b2c, 0x807b7b7c, 0x807a9afc, 0x807a9e50, 0x807b706c, 0x807af1bc};
static const SetItemFn setItem = reinterpret_cast<SetItemFn>(0x807bc940);
const ItemId kBootItem = bootItems[SS8_BOOT_USE_ITEM - 1];
const u32 kItemFirst = 1200;
const u32 kItemStep = 600;
const u32 kItemUses = 3;

static void UseItemScripted(u32 frame) {
    if (frame < kItemFirst || (frame - kItemFirst) % kItemStep != 0 || (frame - kItemFirst) / kItemStep >= kItemUses) return;
    Raceinfo *raceinfo = Raceinfo::sInstance;
    Item::Manager *items = Item::Manager::sInstance;
    if (raceinfo == nullptr || raceinfo->playerIdInEachPosition == nullptr || items == nullptr) return;
    const u32 last = Racedata::sInstance->racesScenario.playerCount - 1;
    const bool own = kBootItem == MEGA_MUSHROOM || kBootItem == STAR || kBootItem == THUNDER_CLOUD;
    const u32 id = kBootItem == BULLET_BILL ? (SS8_BOOT_LOCALS > kGameLocal ? kGameLocal : 0)
                   : own                    ? SS8_BOOT_LOCALS - 1
                                            : raceinfo->playerIdInEachPosition[last];
    Item::Player &player = items->players[id];
    OS::Report("ss8 boot: race %u player %u uses item %u at %u\n", bootRaces, id, kBootItem, frame);
    setItem(&player.inventory, kBootItem, false);
    reinterpret_cast<UseItemFn>(useItems[SS8_BOOT_USE_ITEM - 1])(&player);
}
const u32 kItemEnd = kItemFirst + kItemUses * kItemStep;
#else
const u32 kItemEnd = 0;
#endif

// The scripted pause comes after the respawns and item uses.
const u32 kScriptEnd = kRespawnEnd > kItemEnd ? kRespawnEnd : kItemEnd;
const u32 kPauseFrame = kScriptEnd != 0 ? kScriptEnd + 300 : 1500;

static void BindBootPads() {
    SectionPad &pad = SectionMgr::sInstance->pad;
    for (int i = 0; i < kBootGCLocals; ++i) BindBootPad(pad, i);
#if SS8_BOOT_LOCALS > 4
    BindBootWiiHolders();
#endif
    ++bootRaces;
    raceFrames = 0;
}
static RaceLoadHook bindBootPads(BindBootPads);

// "Controller interrupted" never opens in a debug boot, Dolphin and --boot-manual included, so a boot
// with no live pad (WiiCompiled reports an empty GC port as unplugged) reaches the race; a real pad
// dropping out no longer pauses either. RecognizePad::CheckForConditions, reached only from the
// RecognizePad pages' vtables, returns 0 (li r3, 0; blr over its stwu/mflr prologue).
kmWrite32(0x8061C40C, 0x38600000);
kmWrite32(0x8061C410, 0x4E800020);

// A --boot-manual build, for a playtest with real pads, leaves out both scripts below.
#if !SS8_BOOT_MANUAL
// GC port 1 drives the pause menu about 25 s into each race (later with --boot-respawn or --boot-use-item): races 1 and 2 restart (START, Down to
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
    // An autodrive boot only presses A once a second: through the results into the next race.
    if (SS8_BOOT_AUTODRIVE) return frame % 60 < 5 ? PAD::PAD_BUTTON_A : 0;
    if (frame < kPauseFrame) return 0;
    const u32 step = (frame - kPauseFrame) / 60;
    if (step < count) return (frame - kPauseFrame) % 60 < 6 ? steps[step] : 0;
    return step >= count + 3 && frame % 60 < 5 ? PAD::PAD_BUTTON_A : 0;
}

static u32 CopyPADStatusScripted(Input::Manager *input, u32 channel, PAD::Status *status) {
    const u32 result = copyPADStatus(input, channel, status);
    if (channel == 0 && status != nullptr) {
        const u16 buttons = ScriptedButtons(raceFrames);
        if (buttons != 0 && ScriptedButtons(raceFrames - 1) != buttons)
            OS::Report("ss8 boot: race %u press %04x at %u\n", bootRaces, buttons, raceFrames);
        status->buttons |= buttons;
#if SS8_BOOT_AUTODRIVE
        if (raceFrames == 60 && bootRaces != 0) AutodriveLocals();
#endif
#if SS8_BOOT_RESPAWN
        if (bootRaces != 0) RespawnScripted(raceFrames);
#endif
#if SS8_BOOT_USE_ITEM
        if (bootRaces != 0) UseItemScripted(raceFrames);
#endif
        ++raceFrames;
    }
    return result;
}
kmCall(0x80520220, CopyPADStatusScripted);

#if SS8_BOOT_LOCALS > 4
// Players 5 and 7 (Wii channels 0 and 2) hold accelerate and players 6 and 8 sit idle, so a frame
// shows which holders reach their karts. Added after the controller's own read, so the channel's
// KPAD path still runs.
static void ScriptedWiiButtons(const u8 *controller, Input::State *state) {
    const u32 channel = *reinterpret_cast<const u32 *>(controller + 0x8d4);
    if (channel < SS8_BOOT_LOCALS - kGameLocal && channel % 2 == 0) state->buttonActions |= 1;
}

// WiiController::UpdateImpl+0x31C replaces "addi r11, r1, 0x70" before _rest_gpr_24, where every path
// ends (r26 = the controller, +0x8D4 its channel; r27 = the State it fills); the prologue saved LR.
asmFunc ScriptedWiiButtonsStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r26;
        mr r4, r27;
        bl ScriptedWiiButtons;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        addi r11, r1, 0x70;
        blr;)
}
kmCall(0x8051ffa0, ScriptedWiiButtonsStub);
#endif
#endif

#if SS8_BOOT_NO_MAP
// RaceHUD::OnInit+0x40 replaces "mr r31, r3", the enabled-controls mask from the page's vf 0x68 (RR
// owns VSMultiHUD's, 0x80633A00): a widened race leaves out the centre map (0x8). Both the control
// count and InitCtrlRaceBase read r31, so they stay in step; r12 is free after the bctrl.
asmFunc NoCentreMap() {
    ASM(
        nofralloc;
        mr r31, r3;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        li r12, 8;
        andc r31, r31, r12;
        blr;)
}
kmCall(0x808562b8, NoCentreMap);
#endif

}  // namespace SplitScreen8
#endif
