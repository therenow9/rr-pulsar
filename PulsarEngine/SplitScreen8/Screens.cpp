#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/3D/Scn/GameScreen.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

// Only RaceScene::GetScreenCount reports this; RacedataScenario::screenCount stays 4 for its ~100 readers (item screens, thresholds).
u8 raceScreenCount;
// Player id per hud slot kGameLocal..kMaxLocal-1; lower slots stay in RacedataSettings::hudPlayerIds.
static s8 hudPlayerIdsExt[kMaxLocal];

// A debug build also widens a race of 4 or fewer locals: from 1 in a debug boot, from 2 otherwise, so its 1P stays vanilla.
#ifdef SS8_DEBUG_BOOT
#define SS8_DEBUG_MIN_LOCALS 1
#else
#define SS8_DEBUG_MIN_LOCALS 2
#endif

u8 raceLocalCount;

static u32 LocalPlayerCount(const RacedataScenario &scenario) {
    u32 locals = 0;
    for (int i = 0; i < 12; ++i)
        if (scenario.players[i].playerType == PLAYER_REAL_LOCAL)
            ++locals;
    return locals;
}

// A VS race or a Battle of 5-6 local players races on 6 screens and 7-8 on 8; 4 or fewer keep the game's count.
static u32 WideScreenCount(const RacedataScenario &scenario) {
    const GameMode mode = scenario.settings.gamemode;
    if (mode != MODE_VS_RACE && mode != MODE_BATTLE)
        return 0;
    const u32 locals = LocalPlayerCount(scenario);
    if (locals > kMaxLocal || (mode == MODE_BATTLE && locals <= kGameLocal))  // a debug build widens only a VS below 5
        return 0;
    if (locals > 6)
        return 8;
#ifdef SS8_DEBUG_SCREENS
    if (locals > kGameLocal)
        return SS8_DEBUG_SCREENS > 6 ? SS8_DEBUG_SCREENS : 6;
    return locals >= SS8_DEBUG_MIN_LOCALS ? SS8_DEBUG_SCREENS : 0;
#else
    return locals > kGameLocal ? 6 : 0;
#endif
}

// ComputePlayerCounts+0x194, its last store "stb r10, 0(r6)" (r10 = unclamped locals): the count stops at 4 for Init's call
// through RR's NonGhostPlayerCount and for RR's own calls. Branched to, so this blr returns.
asmFunc ClampLocalPlayerCount() {
    ASM(
        nofralloc;
        cmplwi r10, 4;
        ble store;
        li r10, 4;
        store :;
        stb r10, 0(r6);
        blr;)
}
kmBranch(0x8052f91c, ClampLocalPlayerCount);

#ifdef SS8_DEBUG_SCREENS
// ComputePlayerCounts+0x118, "cmplwi r9, 3" (r9 = screens, r10 = locals, r3 = scenario): a widened race reports 4 so slots 0-3
// stay on game storage, by WideScreenCount's predicate. A leaf, so branch out and back; r0 is dead here.
asmFunc DebugWideScreenCount() {
    ASM(
        nofralloc;
        cmplwi r10, SS8_DEBUG_MIN_LOCALS;
        blt end;
        lwz r0, 0xb50(r3);
        cmpwi r0, 1;
        bne end;
        li r9, 4;
        end :;
        cmplwi r9, 3;
        blr;)
}
kmBranch(0x8052f8a0, DebugWideScreenCount);
kmPatchExitPoint(DebugWideScreenCount, 0x8052f8a4);
#endif

// RacedataScenario::Init+0x274, on the menu scenario InitRace copies, after Init's overrides; game storage is sized by screenCount,
// so only 4 widens. Locals past the 4th hide as PLAYER_NONE from InitScreens, which indexes by slot, and take slots 4+ here.
static void InitScreensWide(RacedataScenario &scenario, u8 screenCount) {
    const u32 wide = screenCount == kGameLocal ? WideScreenCount(scenario) : 0;
    raceScreenCount = wide;
    for (int i = 0; i < kMaxLocal; ++i) hudPlayerIdsExt[i] = -1;
    const u32 localCount = LocalPlayerCount(scenario);
    raceLocalCount = localCount <= kMaxLocal && (wide != 0 || localCount > kGameLocal) ? localCount : 0;
    s8 extLocals[kMaxLocal - kGameLocal];
    u32 extCount = 0;
    for (int i = 0, locals = 0; i < 12 && raceLocalCount > kGameLocal; ++i) {
        RacedataPlayer &player = scenario.players[i];
        if (player.playerType != PLAYER_REAL_LOCAL || locals++ < kGameLocal)
            continue;
        extLocals[extCount++] = i;
        player.playerType = PLAYER_NONE;
    }
    scenario.InitScreens(screenCount);
    u32 hud = kGameLocal;
    for (u32 k = 0; k < extCount; ++k) {
        RacedataPlayer &player = scenario.players[extLocals[k]];
        player.playerType = PLAYER_REAL_LOCAL;
        // "Next Race" (0x8085AEA4) loads a one-screen gametype 5 scene the AI drives; its locals 5-8 keep no slot or holder.
        if (wide == 0) {
            player.hudSlotId = -1;
            player.realControllerChannel = -1;
            player.controllerType = static_cast<ControllerType>(-1);
            continue;
        }
        const Input::Controller *controller = Holder(*Input::Manager::sInstance, hud).curController;
        player.hudSlotId = hud;
        player.realControllerChannel = hud;
        player.controllerType = controller != nullptr ? controller->GetType() : static_cast<ControllerType>(-1);
        hudPlayerIdsExt[hud++] = extLocals[k];
    }
    for (int i = 0; i < 12 && hud < wide; ++i) {
        RacedataPlayer &player = scenario.players[i];
        if (player.playerType == PLAYER_NONE || player.hudSlotId != -1)
            continue;
        player.hudSlotId = hud;
        hudPlayerIdsExt[hud] = i;
        ++hud;
    }
}
kmCall(0x8052fe04, InitScreensWide);

// Kart::BRRESHandle::__ct+0xDC, "cmpwi r30, 0" (r30 = vanilla 3P), whose bne skips +0x12, the second kart archive OnEnter loads
// only for locals: a spare-tile CPU (+0x14 "is local" clear) takes 3P's spare-tile flag +0x11 and the bne. r5 dead; LR saved.
asmFunc SpareTileKartModel() {
    ASM(
        nofralloc;
        lis r5, raceScreenCount @ha;
        lbz r5, raceScreenCount @l(r5);
        cmpwi r5, 0;
        beq game;
        lbz r5, 0x14(r31);
        cmpwi r5, 0;
        bne game;
        li r5, 1;
        stb r5, 0x11(r31);
        cmpwi r5, 0;
        blr;
        game :;
        cmpwi r30, 0;
        blr;)
}
kmCall(0x80576c18, SpareTileKartModel);

// DriverMgr::IsPlayerComputer+0x100, "extsb. r0, r3" (r3 = hud slot), for CPU, online and empty players: a slot >= 0 would answer 1,
// model/model_lod, but a CPU's 4-screen archives hold model_cpu, so a widened race reads -1 as 3P does. r4 dead; LR saved.
asmFunc SpareTileDriverModel() {
    ASM(
        nofralloc;
        lis r4, raceScreenCount @ha;
        lbz r4, raceScreenCount @l(r4);
        cmpwi r4, 0;
        beq game;
        li r0, -1;
        cmpwi r0, 0;
        blr;
        game :;
        extsb.r0, r3;
        blr;)
}
kmCall(0x807bd6bc, SpareTileDriverModel);

// RaceScene::GetScreenCount, reached only through its vtable (0x808b426c); ScnMgr::InitScn stores the result at 0x808b4bf0.
static u32 GetRaceScreenCount() {
#ifdef SS8_DEBUG_SCREENS
    const RacedataScenario &race = Racedata::sInstance->racesScenario;
    OS::Report(
      "ss8 screens: wide %u, race players %u screens %u locals %u (%u) mode %u\n", raceScreenCount, race.playerCount, race.screenCount, race.localPlayerCount, raceLocalCount, race.settings.gamemode);
#endif
    if (raceScreenCount != 0)
        return raceScreenCount;
    return Racedata::sInstance->racesScenario.screenCount;
}
kmBranch(0x80554f68, GetRaceScreenCount);

// RouteHolder::Init copies each camera route per screen (+0x25), which AutoCameraMoverRace looks up by screen: the race's count at
// +0xC0 (table size) and +0xDC (copies). r12 is reloaded before every call in Init; the prologue saved LR.
asmFunc RouteCopiesForSize() {
    ASM(
        nofralloc;
        lbz r3, 0x25(r3);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r3, r12;
        blr;)
}
kmCall(0x806f0b98, RouteCopiesForSize);

asmFunc RouteCopiesPerScreen() {
    ASM(
        nofralloc;
        lbz r0, 0x25(r3);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r0, r12;
        blr;)
}
kmCall(0x806f0bb4, RouteCopiesPerScreen);

// Racedata::GetPlayerIdOfLocalPlayer: RR's KO spectating fix owns its blr (KOMisc.cpp, 0x80531f7c), so leave through it, id in r3.
static s32 HudPlayerId(const Racedata &racedata, u32 hud) {
    if (hud >= kGameLocal && hud < raceScreenCount)
        return hudPlayerIdsExt[hud];
    return static_cast<s8>(racedata.racesScenario.settings.hudPlayerIds[hud]);
}

// One blr, so kmPatchExitPoint catches every return of HudPlayerId.
asmFunc HudPlayerIdWrapper() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        bl HudPlayerId;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        blr;)
}
kmBranch(0x80531f70, HudPlayerIdWrapper);
kmPatchExitPoint(HudPlayerIdWrapper, 0x80531f7c);

u8 HudRacePlayer(u32 hud) {
    const Racedata *racedata = Racedata::sInstance;
    return hud < kGameLocal ? racedata->racesScenario.settings.hudPlayerIds[hud] : racedata->GetPlayerIdOfLocalPlayer(hud);
}

// isLiveView (MAP: LoadNextGPTrack)+0x28, "add r4, r3, r4; lbz r0, 0xb84(r4)": a spare tile follows its CPU, not a TV camera, so a
// hud below the count reads -1, "not live". A leaf, so branch out and back; only r0 and r3 are read after it, and r5 is free.
asmFunc IsLiveViewHudPlayerId() {
    ASM(
        nofralloc;
        add r5, r3, r4;
        lbz r0, 0xb84(r5);
        lis r5, raceScreenCount @ha;
        lbz r5, raceScreenCount @l(r5);
        cmplw r4, r5;
        bge end;
        li r0, -1;
        end :;
        blr;)
}
kmBranch(0x80531ff0, IsLiveViewHudPlayerId);
kmPatchExitPoint(IsLiveViewHudPlayerId, 0x80531ff8);

// io holds the view's [w, h] on entry and the tile's [x, y, w, h] on return; tiles run left to right, then top to bottom.
static void GridRect(const GameScreen &screen, u32 screenCount, float *io) {
    const u32 cols = screenCount / 2;
    const float w = io[0] / cols;
    const float h = io[1] / 2;
    io[0] = w * (screen.idx % cols);
    io[1] = h * (screen.idx / cols);
    io[2] = w;
    io[3] = h;
}

// GameScreen::CalcDimensions+0xD8, "cmpwi r4, 2" (r4 = screens, f4/f5 = full size) for counts other than 3-4: the re-done compare
// sends 6 and 8 to its tail (0x80566f48), which stores f6/f1 as position and f4/f5 as size. A leaf: LR is saved here.
asmFunc CalcDimensionsGrid() {
    ASM(
        nofralloc;
        cmpwi r4, 6;
        beq grid;
        cmpwi r4, 8;
        bne end;
        grid :;
        stwu r1, -0x30(r1);
        mflr r0;
        stw r0, 0x34(r1);
        stw r3, 0x8(r1);
        stw r4, 0xc(r1);
        stfs f4, 0x10(r1);
        stfs f5, 0x14(r1);
        addi r5, r1, 0x10;
        bl GridRect;
        lwz r3, 0x8(r1);
        lwz r4, 0xc(r1);
        lfs f6, 0x10(r1);
        lfs f1, 0x14(r1);
        lfs f4, 0x18(r1);
        lfs f5, 0x1c(r1);
        lwz r0, 0x34(r1);
        mtlr r0;
        addi r1, r1, 0x30;
        end :;
        cmpwi r4, 2;
        blr;)
}
kmBranch(0x80566f10, CalcDimensionsGrid);
kmPatchExitPoint(CalcDimensionsGrid, 0x80566f14);

}  // namespace SplitScreen8
