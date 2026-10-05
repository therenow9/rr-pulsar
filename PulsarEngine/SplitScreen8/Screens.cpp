#include <kamek.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/3D/Scn/GameScreen.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// 6/8 race screens. Only RaceScene::GetScreenCount reports the wide count, so the views, cameras and
// renderers built from it widen, while RacedataScenario::screenCount stays at the game's 4 for its
// ~100 readers (item screens, thresholds). docs/plans/m2-engine-widening.md has the site list.

namespace SplitScreen8 {

// 0 while a race uses the game's own screen count, else 6 or 8.
static u8 raceScreenCount;
// Player id per hud slot kGameLocal..kMaxLocal-1; lower slots stay in RacedataSettings::hudPlayerIds.
static s8 hudPlayerIdsExt[kMaxLocal];

// Only the debug build widens: InitScreens' local-player branch still overflows past 4 locals. The
// debug boot races 1 local player, so it widens from 1; menu-driven debug builds keep 1P vanilla.
#ifdef SS8_DEBUG_BOOT
#define SS8_DEBUG_MIN_LOCALS 1
#else
#define SS8_DEBUG_MIN_LOCALS 2
#endif

static u32 WideScreenCount(const RacedataScenario &scenario) {
#ifdef SS8_DEBUG_SCREENS
    if (scenario.settings.gamemode != MODE_VS_RACE) return 0;
    u32 locals = 0;
    for (int i = 0; i < 12; ++i)
        if (scenario.players[i].playerType == PLAYER_REAL_LOCAL) ++locals;
    return locals >= SS8_DEBUG_MIN_LOCALS ? SS8_DEBUG_SCREENS : 0;
#else
    return 0;
#endif
}

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
// that InitRace then copies to the race scenario. Spare hud slots past the game's 4 are filled in
// player order, as InitScreens' own spare-screen loop would, into the side table.
static void InitScreensWide(RacedataScenario &scenario, u8 screenCount) {
    const u32 wide = WideScreenCount(scenario);
    raceScreenCount = wide;
    for (int i = 0; i < kMaxLocal; ++i) hudPlayerIdsExt[i] = -1;
    scenario.InitScreens(screenCount);
    u32 hud = kGameLocal;
    for (int i = 0; i < 12 && hud < wide; ++i) {
        RacedataPlayer &player = scenario.players[i];
        if (player.playerType == PLAYER_NONE || player.hudSlotId != -1) continue;
        player.hudSlotId = hud;
        hudPlayerIdsExt[hud] = i;
        ++hud;
    }
}
kmCall(0x8052fe04, InitScreensWide);

// RaceScene::GetScreenCount, reached only through the RaceScene vtable (0x808b426c); its result
// becomes the static screen count ScnMgr::InitScn stores at 0x808b4bf0.
static u32 GetRaceScreenCount() {
    if (raceScreenCount != 0) return raceScreenCount;
    return Racedata::sInstance->racesScenario.screenCount;
}
kmBranch(0x80554f68, GetRaceScreenCount);

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
// 0xb84(r4)": r0 = hudPlayerIds[r4]. A leaf, so branch out and back; only r0 and r3 are read
// after it, and r5 is free. 4 is kGameLocal.
asmFunc IsLiveViewHudPlayerId() {
    ASM(
        nofralloc;
        add r5, r3, r4;
        lbz r0, 0xb84(r5);
        cmplwi r4, 4;
        blt end;
        lis r5, raceScreenCount @ha;
        lbz r5, raceScreenCount @l(r5);
        cmplw r4, r5;
        bge end;
        lis r5, hudPlayerIdsExt @ha;
        addi r5, r5, hudPlayerIdsExt @l;
        lbzx r0, r5, r4;
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
