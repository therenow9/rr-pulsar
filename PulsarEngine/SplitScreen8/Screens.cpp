#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/3D/Scn/GameScreen.hpp>
#ifdef SS8_DEBUG_SCREENS
#include <MarioKartWii/Input/InputManager.hpp>
#endif
#include <SplitScreen8/SplitScreen8.hpp>

// 6/8 race screens. Only RaceScene::GetScreenCount reports the wide count, so the views, cameras and
// renderers built from it widen, while RacedataScenario::screenCount stays at the game's 4 for its
// ~100 readers (item screens, thresholds). docs/plans/m2-engine-widening.md has the site list.

namespace SplitScreen8 {

u8 raceScreenCount;
// Player id per hud slot kGameLocal..kMaxLocal-1; lower slots stay in RacedataSettings::hudPlayerIds.
static s8 hudPlayerIdsExt[kMaxLocal];

// Only the debug build widens: 5-8 local players have no menu path and their controller holders
// exist only there (D24, D26). A debug boot can race 1 local player (--boot-locals 1), so it widens
// from 1; menu-driven debug builds keep 1P vanilla.
#ifdef SS8_DEBUG_BOOT
#define SS8_DEBUG_MIN_LOCALS 1
#else
#define SS8_DEBUG_MIN_LOCALS 2
#endif

#ifdef SS8_DEBUG_SCREENS
u8 raceLocalCount;

static u32 LocalPlayerCount(const RacedataScenario &scenario) {
    u32 locals = 0;
    for (int i = 0; i < 12; ++i)
        if (scenario.players[i].playerType == PLAYER_REAL_LOCAL) ++locals;
    return locals;
}
#endif

// 5-6 local players race on 6 screens and 7-8 on 8; fewer take the debug build's forced count.
static u32 WideScreenCount(const RacedataScenario &scenario) {
#ifdef SS8_DEBUG_SCREENS
    if (scenario.settings.gamemode != MODE_VS_RACE) return 0;
    const u32 locals = LocalPlayerCount(scenario);
    if (locals > kMaxLocal) return 0;
    if (locals > 6) return 8;
    if (locals > kGameLocal) return SS8_DEBUG_SCREENS > 6 ? SS8_DEBUG_SCREENS : 6;
    return locals >= SS8_DEBUG_MIN_LOCALS ? SS8_DEBUG_SCREENS : 0;
#else
    return 0;
#endif
}

#ifdef SS8_DEBUG_SCREENS
// ComputePlayerCounts+0x194 replaces its last store, "stb r10, 0(r6)" (r10 = local players,
// unclamped), before its blr: the scenario's count stops at the game's 4 (D25), for Init's call
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
#endif

#ifdef SS8_DEBUG_SCREENS
// ComputePlayerCounts+0x118 replaces "cmplwi r9, 3" (r9 = screen count, r10 = local count, r3 =
// scenario). A widened race reports the game's full 4 so slots 0-3 stay on game storage; the
// predicate matches WideScreenCount. Leaf function, so branch out and back; r0 is dead here.
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

// RacedataScenario::Init+0x274 calls InitScreens, its only caller; Init runs on the menu scenario
// that InitRace then copies to the race scenario. screenCount is the final one, after Init's own
// overrides (a CPU-only demo race is 1), and the game's per-screen storage is sized by it, so only
// a race that keeps the game's full 4 widens. Locals past the 4th are hidden from InitScreens as
// PLAYER_NONE (it would index hudPlayerIds and the holders by their slot) and take hud slot and
// controller id 4.. here (D27); spare CPUs take the slots after them, into the side table.
static void InitScreensWide(RacedataScenario &scenario, u8 screenCount) {
    const u32 wide = screenCount == kGameLocal ? WideScreenCount(scenario) : 0;
    raceScreenCount = wide;
    for (int i = 0; i < kMaxLocal; ++i) hudPlayerIdsExt[i] = -1;
#ifdef SS8_DEBUG_SCREENS
    const u32 localCount = LocalPlayerCount(scenario);
    raceLocalCount = localCount <= kMaxLocal && (wide != 0 || localCount > kGameLocal) ? localCount : 0;
    s8 extLocals[kMaxLocal - kGameLocal];
    u32 extCount = 0;
    for (int i = 0, locals = 0; i < 12 && raceLocalCount > kGameLocal; ++i) {
        RacedataPlayer &player = scenario.players[i];
        if (player.playerType != PLAYER_REAL_LOCAL || locals++ < kGameLocal) continue;
        extLocals[extCount++] = i;
        player.playerType = PLAYER_NONE;
    }
#endif
    scenario.InitScreens(screenCount);
    u32 hud = kGameLocal;
#ifdef SS8_DEBUG_SCREENS
    for (u32 k = 0; k < extCount; ++k) {
        RacedataPlayer &player = scenario.players[extLocals[k]];
        player.playerType = PLAYER_REAL_LOCAL;
        // A scene that does not widen: "Next Race" (0x8085AEA4) loads a one-screen gametype 5 scene
        // the AI drives. Its locals 5-8 keep no slot, controller or holder.
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
#endif
    for (int i = 0; i < 12 && hud < wide; ++i) {
        RacedataPlayer &player = scenario.players[i];
        if (player.playerType == PLAYER_NONE || player.hudSlotId != -1) continue;
        player.hudSlotId = hud;
        hudPlayerIdsExt[hud] = i;
        ++hud;
    }
}
kmCall(0x8052fe04, InitScreensWide);

// Kart::BRRESHandle::__ct+0xDC replaces "cmpwi r30, 0" (r30 = vanilla 3P), reached for a hud slot
// >= 0; its bne skips "stb 0x12". +0x12 asks for the second kart archive, which RaceScene::OnEnter
// loads only for local players, and the 3P path sets nothing at all for a hud slot other than 3, so
// a CPU on a spare tile takes vanilla 3P's spare-tile flag +0x11 and the bne. +0x14 is the ctor's
// "is local" byte. r5 is dead here; the epilogue restores LR.
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

// DriverMgr::IsPlayerComputer+0x100 replaces "extsb. r0, r3" (r3 = hud slot), reached for CPU, online
// and empty player types (the 0x19 mask at +0xEC); the bge after it answers 1 ("watched": the driver's model/model_lod pair) for a hud
// slot >= 0. The 4-screen kart archives a CPU loads hold model_cpu instead, and vanilla 3P answers 0
// for its spare-tile CPU, so a widened race reads the slot as -1. r4 is dead; the epilogue restores LR.
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

// RaceScene::GetScreenCount, reached only through the RaceScene vtable (0x808b426c); its result
// becomes the static screen count ScnMgr::InitScn stores at 0x808b4bf0.
static u32 GetRaceScreenCount() {
#ifdef SS8_DEBUG_SCREENS
    const RacedataScenario &race = Racedata::sInstance->racesScenario;
    OS::Report("ss8 screens: wide %u, race players %u screens %u locals %u (%u) mode %u\n", raceScreenCount, race.playerCount,
               race.screenCount, race.localPlayerCount, raceLocalCount, race.settings.gamemode);
#endif
    if (raceScreenCount != 0) return raceScreenCount;
    return Racedata::sInstance->racesScenario.screenCount;
}
kmBranch(0x80554f68, GetRaceScreenCount);

// RouteHolder::Init copies each camera route once per screen of racesScenario.screenCount (+0x25)
// and sizes its table by that count, while AutoCameraMoverRace looks a copy up by the camera's
// screen: a widened race takes its own count at both reads (+0xC0 sets the table size, +0x14/+0xE;
// +0xDC the copies per screen, +0x17). r12 is reloaded before every call in Init; the prologue saved LR.
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

// Racedata::GetPlayerIdOfLocalPlayer. Retro Rewind's KO spectating fix owns the function's blr
// (KOMisc.cpp, 0x80531f7c), so leave through it with the id in r3.
static s32 HudPlayerId(const Racedata &racedata, u32 hud) {
    if (hud >= kGameLocal && hud < raceScreenCount) return hudPlayerIdsExt[hud];
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

// isLiveView (MAP calls 0x80531fc8 LoadNextGPTrack) +0x28 replaces "add r4, r3, r4; lbz r0,
// 0xb84(r4)": r0 = hudPlayerIds[r4], and a non-local player there means a live (TV) camera. In a
// widened race every spare tile follows its CPU instead (D22): each hud below the count reads -1,
// which isLiveView answers "not live", as it answers a local player. A leaf, so branch out and back
// (one blr, for kmPatchExitPoint); only r0 and r3 are read after it, and r5 is free.
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

// io holds the full view size [w, h] on entry and the tile's [x, y, w, h] on return: 3x2 for 6
// screens, 4x2 for 8, numbered left to right then top to bottom.
static void GridRect(const GameScreen &screen, u32 screenCount, float *io) {
    const u32 cols = screenCount / 2;
    const float w = io[0] / cols;
    const float h = io[1] / 2;
    io[0] = w * (screen.idx % cols);
    io[1] = h * (screen.idx / cols);
    io[2] = w;
    io[3] = h;
}

// GameScreen::CalcDimensions+0xD8 replaces "cmpwi r4, 2", reached for counts other than 3-4 with
// r4 = screen count, f4/f5 = full width/height. Its tail stores f6/f1 as position and f4/f5 as size
// (0x80566f48); the re-done compare sends 6 and 8 there. CalcDimensions is a leaf: LR is saved here.
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
