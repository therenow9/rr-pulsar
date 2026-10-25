#if defined(SS8_DEBUG_BOOT) || defined(SS8_MENU_SCRIPT)
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

// Boots past the menus into a VS race, or with SS8_BOOT_BATTLE a team Balloon round: SS8_BOOT_LOCALS locals on GC
// ports 1-4, then Wii channels 1-4, then CPUs.

namespace SplitScreen8 {

// Every local drives as a finished player does (RaceinfoPlayer::EndRace, 0x80534904): AI holder, CPU flag, post-race
// AI (0x8058F0E8, 0x8058F1D0). Run a second in: AI::Player::Init clears the AI's flag at the race's start.

void AutodriveLocals() {
    Input::Manager *input = Input::Manager::sInstance;
    Raceinfo *raceinfo = Raceinfo::sInstance;
    Kart::Manager *karts = Kart::Manager::sInstance;
    if (input == nullptr || raceinfo == nullptr || raceinfo->players == nullptr || karts == nullptr)
        return;
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    for (int id = 0; id < scenario.playerCount; ++id) {
        if (scenario.players[id].playerType != PLAYER_REAL_LOCAL)
            continue;
        Kart::Player *kart = karts->GetKartPlayer(id);
        raceinfo->players[id]->realControllerHolder = reinterpret_cast<Input::RealControllerHolder *>(&input->virtualControllerHolders[id]);
        kart->pointers.kartStatus->bitfield4 |= 1;
        // A battle's post-race AI holds the kart still (UpdatePostBattleDriving): there it drives as a battle CPU.
        AI::Player *ai = kart->kartSub->aiController->playerAI;
        if (scenario.settings.gamemode == MODE_BATTLE)
            ai->SetNextActions(&ai->cpuDrivingAction);
        else
            kart->kartSub->aiController->StartPostRaceAI();
    }
}

#ifdef SS8_DEBUG_BOOT
const int kBootGCLocals = SS8_BOOT_LOCALS < kGameLocal ? SS8_BOOT_LOCALS : kGameLocal;
// 5-8 locals race in the 4P section: the scenario's local count stops at 4.
const SectionId kBootSection = static_cast<SectionId>((SS8_BOOT_BATTLE ? SECTION_P1BATTLE : SECTION_P1VS) + kBootGCLocals - 1);

#if SS8_BOOT_LOCALS > 4
// Players 5..N hold Wii channels 0..N-5.
static void BindBootWiiHolders() {
    Input::Manager *input = Input::Manager::sInstance;
    if (input == nullptr)
        return;
    for (u32 id = kGameLocal; id < SS8_BOOT_LOCALS; ++id) BindPad(Holder(*input, id), &input->wiiControllers[id - kGameLocal]);
}

// Set as each section is requested, before its pages are built: the boot's races, intros and awards hold the 5-8 count;
// its between-race menus run as 1-4 games' (at 5-8 they draw menu models the boot never built); any other ends it.
static bool bootGame = true;
static void HoldBootCount() {
    const SectionId next = SectionMgr::sInstance->nextSectionId;
    if (!bootGame || next == SECTION_NONE)
        return;
    const bool race = next == kBootSection || next == SECTION_VS_RACE_INTRO || next == SECTION_BALLOON_BATTLE_INTRO || next == SECTION_VS_RACE_AWARD;
    menuLocalCount = race ? SS8_BOOT_LOCALS : 0;
    bootGame = race || next == SECTION_SINGLE_P_VS_NEXT_RACE || next == SECTION_SINGLE_P_BT_NEXT_BATTLE;
}
static FrameLoadHook holdBootCount(HoldBootCount);
#endif

// Runs inside SectionMgr::Init, after Racedata, RKSYS, CupsConfig and SectionParams exist. The menu
// scenario is filled as Title::PrepareDemo fills its no-menu race; InitRace copies it into the race.
SectionId DebugBootPrepare(SectionId section) {
    Racedata *racedata = Racedata::sInstance;
#ifdef SS8_VANILLA
    SectionMgr *sectionMgr = SectionMgr::sInstance;
    // A CourseId, which build_code.py checks.
    const CourseId track = static_cast<CourseId>(SS8_BOOT_TRACK);
    if (racedata == nullptr || sectionMgr == nullptr || sectionMgr->sectionParams == nullptr)
        return section;
#else
    Pulsar::CupsConfig *cups = Pulsar::CupsConfig::sInstance;
    SectionMgr *sectionMgr = SectionMgr::sInstance;
    const Pulsar::PulsarId track = static_cast<Pulsar::PulsarId>(SS8_BOOT_TRACK);
    if (racedata == nullptr || cups == nullptr || sectionMgr == nullptr || sectionMgr->sectionParams == nullptr)
        return section;
    if (!cups->IsValidTrack(track))
        return section;
#endif

    racedata->ResetScenarios();
    RacedataScenario &scenario = racedata->menusScenario;
    RacedataSettings &settings = scenario.settings;
    settings.gamemode = SS8_BOOT_BATTLE ? MODE_BATTLE : MODE_VS_RACE;
    settings.battleType = BATTLE_BALLOON;
    settings.gametype = GAMETYPE_DEFAULT;
    settings.engineClass = CC_150;
    settings.cpuMode = CPU_NORMAL;
    settings.itemMode = SS8_BOOT_ITEMS ? ITEMS_BALANCED : ITEMS_NONE;
    settings.lapCount = 3;
    settings.raceNumber = 0;
    // Mirror off; a Battle has teams, as BattleModeSelect sets them (0x8083B96C).
    settings.modeFlags = (settings.modeFlags & ~3) | (SS8_BOOT_BATTLE ? 2 : 0);

    // Characters 0-11 are distinct non-Mii drivers, and KartIds 0-2 are the three weight classes' Standard Karts.
    for (int i = 0; i < 12; ++i) {
        RacedataPlayer &player = scenario.players[i];
        const CharacterId character = static_cast<CharacterId>(i);
        player.playerType = i < SS8_BOOT_LOCALS ? PLAYER_REAL_LOCAL : PLAYER_CPU;
        player.characterId = character;
        player.kartId = static_cast<KartId>(GetCharacterWeightClass(character));
        if (SS8_BOOT_BATTLE)
            player.team = static_cast<Team>(i % 2);
    }
    // Player 0 keeps the pad RR's BootIntoSection registered; players 1-3 take GC ports 2-4 as GCN controller IDs.
    // SectionPad has no pad for players 5-8.
    for (int i = 1; i < kBootGCLocals; ++i) sectionMgr->pad.padInfos[i].controllerID = (i + 1) << 8 | 0x24;
#if SS8_BOOT_LOCALS > 4
    BindBootWiiHolders();
    // As the 5-8 Players button sets it: the race's 5-8 memory and pad paths key on it.
    menuLocalCount = SS8_BOOT_LOCALS;
#endif

#ifdef SS8_VANILLA
    // cupId stays as Racedata::Init leaves it: only a Grand Prix reads it.
    settings.courseId = track;
    SectionParams *params = sectionMgr->sectionParams;
    params->vsTracks[0] = track;
#else
    // The track goes through CupsConfig: RR's FormatTrackPath loads GetWinning(), not courseId.
    cups->SetWinning(track, SS8_BOOT_VARIANT);
    cups->SetSelected(track);
    cups->lastSelectedCup = Pulsar::CupsConfig::ConvertCup_PulsarTrackToCup(track);
    settings.courseId = cups->GetCorrectTrackSlot();
    settings.cupId = cups->lastSelectedCup % 8;
    SectionParams *params = sectionMgr->sectionParams;
    params->vsTracks[0] = static_cast<CourseId>(track);
    cups->vsTrackVariantIdx[0] = SS8_BOOT_VARIANT;
#endif
    params->vsRaceNumber = 0;
    params->vsRaceLimit = 4;

#ifndef SS8_VANILLA
    // RR gives offline CPUs custom skins only outside local multiplayer, at full LOD only, while a widened 1-local
    // boot loads the 4-screen LOD (+7 = 4): show the default skins a multi-local race would. RR's next settings save keeps it.
    if (Pulsar::Settings::Mgr::IsCreated())
        Pulsar::Settings::Mgr::Get().SetSettingValue(Pulsar::Settings::SETTING_DISPLAYCUSTOMSKINS, Pulsar::DISPLAYCUSTOMSKINS_DISABLED);
#if SS8_BOOT_SPEEDO
    if (Pulsar::Settings::Mgr::IsCreated())
        Pulsar::Settings::Mgr::Get().SetSettingValue(Pulsar::Settings::SETTING_SPEEDOMETER, Pulsar::SOM_DIGITS_0);
#endif
#if SS8_BOOT_BATTLE
    // RR's value 1 leaves PULSAR_FFA off (PulsarSystem.cpp), so the round is Red vs Blue.
    if (Pulsar::Settings::Mgr::IsCreated())
        Pulsar::Settings::Mgr::Get().SetSettingValue(Pulsar::Settings::SETTING_BATTLETEAMS, Pulsar::BATTLE_FFA_ENABLED);
#endif
#endif
    return kBootSection;
}

// The boot skips the title's and menus' pad binding, and RR's SetUpCorrectController binds pad 0 only to a leftover
// holder, so a connected pad would show "controller interrupted"; a port no holder carries gets the player's own.
static void BindBootPad(SectionPad &pad, u32 slot) {
    PadInfo &info = pad.padInfos[slot];
    Input::Manager *input = Input::Manager::sInstance;
    if (info.controllerHolder != nullptr && info.controllerIDActive == info.controllerID)
        return;
    const u32 channel = ((info.controllerID & 0xFF00) >> 8) - 1;
    if (input == nullptr || channel > 3)
        return;
    Input::Controller *controller =
      pad.GetType(info.controllerID) == GCN ? static_cast<Input::Controller *>(&input->gcnControllers[channel]) : static_cast<Input::Controller *>(&input->wiiControllers[channel]);
    Input::RealControllerHolder *holder = nullptr;
    for (int i = 0; i < 4 && holder == nullptr; ++i)
        if (input->realControllerHolders[i].curController == controller)
            holder = &input->realControllerHolders[i];
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

#if SS8_BOOT_RESPAWN
// Players respawn one by one through a squish respawn's fall path (ApplySquishRespawnDamage+0xA0), so the wipe,
// camera and Lakitu run; the order covers both rows and inner and outer tiles, then a CPU on a spare tile.
extern "C" void activateOOB(Kart::Collision *, u32, u32, u32);
static const u8 respawnOrder[] = {0, 1, 2, 4, 7, 5, 3, 6};
const u32 kRespawnFirst = 900;
const u32 kRespawnStep = 180;
const u32 kRespawnLocals = SS8_BOOT_LOCALS < 5 ? SS8_BOOT_LOCALS : 5;
const u32 kRespawns = kRespawnLocals + (SS8_BOOT_LOCALS < 12 ? 1 : 0);

static void RespawnScripted(u32 frame) {
    if (frame < kRespawnFirst || (frame - kRespawnFirst) % kRespawnStep != 0)
        return;
    const u32 k = (frame - kRespawnFirst) / kRespawnStep;
    if (k >= kRespawns)
        return;
    u32 id = SS8_BOOT_LOCALS;
    for (u32 i = 0, n = 0; i < sizeof(respawnOrder) && k < kRespawnLocals; ++i) {
        if (respawnOrder[i] >= SS8_BOOT_LOCALS)
            continue;
        if (n++ == k) {
            id = respawnOrder[i];
            break;
        }
    }
    Kart::Manager *karts = Kart::Manager::sInstance;
    if (karts == nullptr)
        return;
    OS::Report("ss8 boot: race %u respawn player %u at %u\n", bootRaces, id, frame);
    activateOOB(&karts->GetKartPlayer(id)->GetCollision(), 1, 0, 0);
}
const u32 kRespawnEnd = kRespawnFirst + kRespawns * kRespawnStep;
#else
const u32 kRespawnEnd = 0;
#endif

#if SS8_BOOT_USE_ITEM
// The last-placed racer uses the item (a Blooper, POW or Lightning reaches every tile), P5 a Bullet Bill, the last
// local a Mega Mushroom, star or Thundercloud; it is set into the inventory first, as each Use* removes one.
typedef void (*UseItemFn)(Item::Player *);
static const ItemId bootItems[] = {BLOOPER, POW_BLOCK, LIGHTNING, BULLET_BILL, MEGA_MUSHROOM, STAR, THUNDER_CLOUD};
extern "C" {
void useBlooper(Item::Player *);
void usePow(Item::Player *);
void useThunder(Item::Player *);
void useBullet(Item::Player *);
void useMegaMushroom(Item::Player *);
void useStar(Item::Player *);
void useTC(Item::Player *);
}
static const UseItemFn useItems[] = {useBlooper, usePow, useThunder, useBullet, useMegaMushroom, useStar, useTC};
const ItemId kBootItem = bootItems[SS8_BOOT_USE_ITEM - 1];
const u32 kItemFirst = 1200;
const u32 kItemStep = 600;
const u32 kItemUses = 3;

static void UseItemScripted(u32 frame) {
    if (frame < kItemFirst || (frame - kItemFirst) % kItemStep != 0 || (frame - kItemFirst) / kItemStep >= kItemUses)
        return;
    Raceinfo *raceinfo = Raceinfo::sInstance;
    Item::Manager *items = Item::Manager::sInstance;
    if (raceinfo == nullptr || raceinfo->playerIdInEachPosition == nullptr || items == nullptr)
        return;
    const u32 last = Racedata::sInstance->racesScenario.playerCount - 1;
    const bool own = kBootItem == MEGA_MUSHROOM || kBootItem == STAR || kBootItem == THUNDER_CLOUD;
    const u32 id = kBootItem == BULLET_BILL ? (SS8_BOOT_LOCALS > kGameLocal ? kGameLocal : 0) : own ? SS8_BOOT_LOCALS - 1 : raceinfo->playerIdInEachPosition[last];
    Item::Player &player = items->players[id];
    OS::Report("ss8 boot: race %u player %u uses item %u at %u\n", bootRaces, id, kBootItem, frame);
    player.inventory.SetItem(kBootItem, false);
    useItems[SS8_BOOT_USE_ITEM - 1](&player);
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
#ifdef SS8_VANILLA
    OS::Report("ss8 boot: race %u track %x variant %u locals %u\n", bootRaces, Racedata::sInstance->racesScenario.settings.courseId, 0, menuLocalCount);
#else
    const Pulsar::CupsConfig *cups = Pulsar::CupsConfig::sInstance;
    OS::Report("ss8 boot: race %u track %x variant %u locals %u\n", bootRaces, cups->GetWinning(), cups->GetCurVariantIdx(), menuLocalCount);
#endif
}
static RaceLoadHook bindBootPads(BindBootPads);

#if SS8_BOOT_BATTLE
// A round's end, once a race: page 0x33 parks the karts at their round-end spots, and 0x34 then waits for an A.
static u32 roundEndRace;

static void ReportRoundEnd() {
    const Raceinfo *raceinfo = Raceinfo::sInstance;
    if (raceinfo == nullptr || raceinfo->stage != RACESTAGE_FINISHED || roundEndRace == bootRaces)
        return;
    roundEndRace = bootRaces;
    OS::Report("ss8 boot: race %u round end at %u\n", bootRaces, raceFrames);
}
static RaceFrameHook reportRoundEnd(ReportRoundEnd);
#endif

// RecognizePad::CheckForConditions, reached only from its pages' vtables, returns 0 (li r3, 0; blr over its prologue):
// "controller interrupted" never opens in a debug boot, so a boot with no live pad reaches the race.
kmWrite32(0x8061C40C, 0x38600000);
kmWrite32(0x8061C410, 0x4E800020);

// A --boot-manual build, for a playtest with real pads, leaves out both scripts below.
#if !SS8_BOOT_MANUAL
// GC port 1 pauses about 25 s into each race: races 1-2 restart, race 3 quits ("Are you sure?" defaults to No,
// hence Up), then A once a second walks the menus into the next race.

static u32 PauseSteps(const u16 **steps) {
    static const u16 restart[] = {PAD::PAD_BUTTON_START, PAD::PAD_BUTTON_DOWN, PAD::PAD_BUTTON_A, PAD::PAD_BUTTON_UP, PAD::PAD_BUTTON_A};
    static const u16 quit[] = {PAD::PAD_BUTTON_START, PAD::PAD_BUTTON_DOWN, PAD::PAD_BUTTON_DOWN, PAD::PAD_BUTTON_A, PAD::PAD_BUTTON_UP, PAD::PAD_BUTTON_A};
    const bool quitting = bootRaces % 3 == 0;
    *steps = quitting ? quit : restart;
    return (quitting ? sizeof(quit) : sizeof(restart)) / sizeof(u16);
}

// The pause menu's steps, from its START to its last A.
static bool InPauseSteps(u32 frame) {
    const u16 *steps;
    return !SS8_BOOT_AUTODRIVE && frame >= kPauseFrame && (frame - kPauseFrame) / 60 < PauseSteps(&steps);
}

static u16 ScriptedButtons(u32 frame) {
    // A Battle boot presses nothing, so the round runs to its end and the scoreboard stays up.
    if (SS8_BOOT_BATTLE)
        return 0;
    const u16 *steps;
    const u32 count = PauseSteps(&steps);
    // An autodrive boot only presses A once a second: through the results into the next race.
    if (SS8_BOOT_AUTODRIVE)
        return frame % 60 < 5 ? PAD::PAD_BUTTON_A : 0;
    if (frame < kPauseFrame)
        return 0;
    const u32 step = (frame - kPauseFrame) / 60;
    if (step < count)
        return (frame - kPauseFrame) % 60 < 6 ? steps[step] : 0;
    return step >= count + 3 && frame % 60 < 5 ? PAD::PAD_BUTTON_A : 0;
}

// GCNController::UpdateImpl+0x70 calls CopyPADStatus(manager, channel, &padStatus), then reads padStatus and the
// result; RR's own pad hooks sit at the PADRead calls instead.
static u32 CopyPADStatusScripted(Input::Manager *input, u32 channel, PAD::Status *status) {
    const u32 result = input->CopyPADStatus(channel, status);
    if (channel == 0 && status != nullptr) {
        const u16 buttons = ScriptedButtons(raceFrames);
        if (buttons != 0 && ScriptedButtons(raceFrames - 1) != buttons)
            OS::Report("ss8 boot: race %u press %04x at %u by P%u\n", bootRaces, buttons, raceFrames, SS8_BOOT_PAUSE_BY && InPauseSteps(raceFrames) ? SS8_BOOT_PAUSE_BY : 1);
        if (!SS8_BOOT_PAUSE_BY || !InPauseSteps(raceFrames))
            status->buttons |= buttons;
#if SS8_BOOT_AUTODRIVE
        if (raceFrames == 60 && bootRaces != 0)
            AutodriveLocals();
#endif
#if SS8_BOOT_RESPAWN
        if (bootRaces != 0)
            RespawnScripted(raceFrames);
#endif
#if SS8_BOOT_USE_ITEM
        if (bootRaces != 0)
            UseItemScripted(raceFrames);
#endif
        ++raceFrames;
    }
    return result;
}
kmCall(0x80520220, CopyPADStatusScripted);

#if SS8_BOOT_LOCALS > 4
// Players 5 and 7 hold accelerate and 6 and 8 sit idle, so a frame shows which holders reach their karts; added
// after the controller's own read, so the channel's KPAD path still runs.
static void ScriptedWiiButtons(const u8 *controller, Input::State *state) {
    const u32 channel = *reinterpret_cast<const u32 *>(controller + 0x8d4);
    if (channel < SS8_BOOT_LOCALS - kGameLocal && channel % 2 == 0)
        state->buttonActions |= 1;
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

#if SS8_BOOT_PAUSE_BY
// PAD::PAD_BUTTON_* to WPAD_CL_BUTTON_*, for the buttons the pause steps use.
static u16 ClassicButtons(u16 pad) {
    return (pad & PAD::PAD_BUTTON_START ? 0x400 : 0) | (pad & PAD::PAD_BUTTON_A ? 0x10 : 0) | (pad & PAD::PAD_BUTTON_UP ? 0x1 : 0) | (pad & PAD::PAD_BUTTON_DOWN ? 0x4000 : 0);
}

// --boot-pause-by N: player N (5-8) drives the pause steps on its Classic Controller. WiiController::UpdateImpl+0x1E8
// calls UpdateStatesClassic, which takes held buttons from the status's +0x2A (WPADCLStatus) when the extension reads.
static void UpdateStatesClassicPauseBy(u8 *controller, u8 *status, void *state, void *uiState) {
    const u32 channel = *reinterpret_cast<const u32 *>(controller + 0x8d4);
    if (bootRaces != 0 && channel == SS8_BOOT_PAUSE_BY - 5 && InPauseSteps(raceFrames))
        *reinterpret_cast<u16 *>(status + 0x2a) |= ClassicButtons(ScriptedButtons(raceFrames));
    updateStatesClassic(controller, status, state, uiState);
}
kmCall(0x8051fe6c, UpdateStatesClassicPauseBy);
#endif
#endif

#if SS8_BOOT_NO_MAP
// RaceHUD::OnInit+0x40 replaces "mr r31, r3", the enabled-controls mask (RR owns VSMultiHUD's vf 0x68, 0x80633A00):
// a widened race drops the centre map (0x8). The control count and InitCtrlRaceBase both read r31; r12 is free.
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
#endif

}  // namespace SplitScreen8
#endif
