#include <kamek.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRaceResult.hpp>
#include <UI/UI.hpp>
#include <UI/ExtendedTeamSelect/ExtendedTeamManager.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Players 5-8's colours: a HUD pair, which a VS race shows only on the name tags (the map hides its
// markers' light; rank, lap, item window and speedometer take it but draw grey), and a results-row pair (select_base's
// TEV colours 0 and 1). P1-4's rows are orange, blue, pink and green, so P5-8 avoid those hues.

namespace SplitScreen8 {

struct PlayerColours {
    RGBA16 hud[2];
    u8 result[2][3];
};

// The owner's palette (D49, docs/plans/m3-hud-gameplay.md).
static const PlayerColours kColours[kMaxLocal - kGameLocal] = {
    {{{150, 70, 255, 255}, {200, 150, 255, 255}}, {{120, 0, 220}, {190, 120, 255}}},  // purple
    {{{255, 255, 255, 255}, {170, 170, 170, 255}}, {{150, 150, 150}, {240, 240, 240}}},  // white
    {{{170, 100, 40, 255}, {225, 160, 90, 255}}, {{130, 70, 20}, {210, 140, 70}}},  // brown
    {{{0, 190, 170, 255}, {90, 245, 225, 255}}, {{0, 140, 130}, {60, 230, 210}}},  // teal
};

static bool IsWideLocalHud(u32 hud) {
    return raceLocalCount > kGameLocal && hud >= kGameLocal && hud < raceLocalCount;
}

bool HudSlotColour(u8 hud, RGBA16 *primary, RGBA16 *secondary) {
    if (!IsWideLocalHud(hud)) return false;
    *primary = kColours[hud - kGameLocal].hud[0];
    *secondary = kColours[hud - kGameLocal].hud[1];
    return true;
}

// A local's results row plays animation <hud> of group 4: 0-3 are P1-4's looks, 4 the non-player
// look, 5-6 the team colours. P5-8 play P(n-4)'s look, and once it is bound (0x807F63A4, which RR
// wraps) select_base's colour track is unbound and its colours written, as RR's KO leaderboard does.
// Both the race results page and the points total fill their rows here.
typedef u8 (*GetHudSlotIdFn)(const void *racedata, u8 playerId);
static const GetHudSlotIdFn getHudSlotId = reinterpret_cast<GetHudSlotIdFn>(0x80531f18);
static u8 rowHud = 0xff;

static u8 ResultRowHud(const void *racedata, u8 playerId) {
    const u8 hud = getHudSlotId(racedata, playerId);
    if (raceLocalCount <= kGameLocal || hud < kGameLocal || hud == 0xff) return hud;
    if (!IsWideLocalHud(hud) || Pulsar::UI::ExtendedTeamManager::IsActivated()) return kGameLocal;
    rowHud = hud;
    return hud - kGameLocal;
}
kmCall(0x807f6388, ResultRowHud);  // CtrlRaceResult::Fill+0x39C, a local's row

static void ResultRowColour(CtrlRaceResult *row) {
    const PlayerColours &colours = kColours[rowHud - kGameLocal];
    rowHud = 0xff;
    nw4r::lyt::Pane *base = row->layout.GetPaneByName("select_base");
    if (base == nullptr) return;
    nw4r::lyt::Material *material = base->GetMaterial();
    if (material == nullptr) return;
    Pulsar::UI::UnbindRLMC(material);
    for (int i = 0; i < 2; ++i) {
        material->tevColours[i].r = colours.result[i][0];
        material->tevColours[i].g = colours.result[i][1];
        material->tevColours[i].b = colours.result[i][2];
    }
}

// Fill+0x3BC replaces "addi r3, r29, 0x98" (r29 = the row), reached from the hud path and the team
// path just after their group 4 play; r3-r5 and f1 are set again before they are read, and the
// prologue saved LR.
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
