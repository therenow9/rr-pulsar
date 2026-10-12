#include <kamek.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRaceResult.hpp>
#include <UI/UI.hpp>
#include <UI/ExtendedTeamSelect/ExtendedTeamManager.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

// A VS race shows the HUD pair only on name tags (the map hides it, the rest draw grey); the results pair is select_base's
// TEV colours 0 and 1. P1-4's rows are orange, blue, pink and green, so P5-8 avoid those hues.
struct PlayerColours {
    RGBA16 hud[2];
    u8 result[2][3];
};

static const PlayerColours kColours[kMaxLocal - kGameLocal] = {
    {{{150, 70, 255, 255}, {200, 150, 255, 255}}, {{120, 0, 220}, {190, 120, 255}}},  // purple
    {{{255, 255, 255, 255}, {170, 170, 170, 255}}, {{150, 150, 150}, {240, 240, 240}}},  // white
    {{{170, 100, 40, 255}, {225, 160, 90, 255}}, {{130, 70, 20}, {210, 140, 70}}},  // brown
    {{{0, 190, 170, 255}, {90, 245, 225, 255}}, {{0, 140, 130}, {60, 230, 210}}},  // teal
};

static bool IsWideLocalHud(u32 hud) {
    return raceLocalCount > kGameLocal && hud >= kGameLocal && hud < raceLocalCount;
}

void SlotPalette(u32 slot, RGBA16 *primary, RGBA16 *secondary) {
    *primary = kColours[slot - kGameLocal].hud[0];
    *secondary = kColours[slot - kGameLocal].hud[1];
}

bool HudSlotColour(u8 hud, RGBA16 *primary, RGBA16 *secondary) {
    if (!IsWideLocalHud(hud))
        return false;
    SlotPalette(hud, primary, secondary);
    return true;
}

// A results row plays animation <hud> of group 4, 0-3 P1-4's, 4 non-player, 5-6 teams: P5-8 play P(n-4)'s, then once it
// is bound (0x807F63A4, which RR wraps) select_base's colour track is unbound and rewritten, as RR's KO leaderboard does.
static u8 rowHud = 0xff;

static u8 ResultRowHud(const Racedata *racedata, u8 playerId) {
    const u8 hud = racedata->GetHudSlotId(playerId);
    if (raceLocalCount <= kGameLocal || hud < kGameLocal || hud == 0xff)
        return hud;
    if (!IsWideLocalHud(hud) || Pulsar::UI::ExtendedTeamManager::IsActivated())
        return kGameLocal;
    rowHud = hud;
    return hud - kGameLocal;
}
kmCall(0x807f6388, ResultRowHud);  // CtrlRaceResult::Fill+0x39C, a local's row

static void ResultRowColour(CtrlRaceResult *row) {
    const PlayerColours &colours = kColours[rowHud - kGameLocal];
    rowHud = 0xff;
    nw4r::lyt::Pane *base = row->layout.GetPaneByName("select_base");
    if (base == nullptr)
        return;
    nw4r::lyt::Material *material = base->GetMaterial();
    if (material == nullptr)
        return;
    Pulsar::UI::UnbindRLMC(material);
    for (int i = 0; i < 2; ++i) {
        material->tevColours[i].r = colours.result[i][0];
        material->tevColours[i].g = colours.result[i][1];
        material->tevColours[i].b = colours.result[i][2];
    }
}

// Fill+0x3BC replaces "addi r3, r29, 0x98" (r29 = the row) after the hud and team paths' group 4 play; r3-r5 and f1
// are set again before they are read, and the prologue saved LR.
asmFunc ResultRowColourHook() {
    ASM(
        nofralloc;
        lis r12, rowHud @ha;
        lbz r12, rowHud @l(r12);
        cmpwi r12, 0xff;
        beq done;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r29;
        bl ResultRowColour;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        done :;
        addi r3, r29, 0x98;
        blr;)
}
kmCall(0x807f63a8, ResultRowColourHook);

}  // namespace SplitScreen8
