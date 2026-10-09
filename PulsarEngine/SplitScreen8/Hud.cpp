#include <kamek.hpp>
#include <include/c_stdio.h>
#include <core/System/SystemManager.hpp>
#include <core/nw4r/lyt/TextBox.hpp>
#include <MarioKartWii/UI/Page/RaceHUD/RaceHUD.hpp>
#include <Settings/Settings.hpp>
#include <UI/CtrlRaceBase/CustomCtrlRaceBase.hpp>
#include <UI/CtrlRaceBase/Speedometer.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The race HUD of a widened race: every local tile gets its own controls (D44), each loaded from a 4P
// variant and moved onto its tile by the owner's rule (D42), plus a speedometer (D43). The game's own
// per-player code builds and drives slots 4-7: RR patches inside BeforeControlUpdate's per-player loop,
// so the loop is widened, not copied. docs/plans/m3-hud-gameplay.md, phase B, has the rule and the sites.

namespace SplitScreen8 {

// hudHasPlayer for slots 4.. (RaceHUD+0x60 holds 4, and +0x64 follows it); indexed by slot.
static u8 hudHasPlayerExt[kMaxLocal];

// RaceHUD's reads of racesScenario.localPlayerCount (+0x26), which stays 4 in a widened race (D25): the
// race's own local count instead. Each stub replaces "lbz rD, 0x26(rB)", writes only rD and r12, and is
// followed by a cmpwi that sets CR0 again; every host saved LR.
// InitCtrlRaceBase+0x24, BeforeControlUpdate+0xD8, AfterControlUpdate+0x1CC and +0x468: r3 from r3.
asmFunc HudCountR3() {
    ASM(
        nofralloc;
        lbz r3, 0x26(r3);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r3, r12;
        blr;)
}
kmCall(0x80857ce4, HudCountR3);
kmCall(0x80856dc4, HudCountR3);
kmCall(0x80857684, HudCountR3);
kmCall(0x80857920, HudCountR3);

// GetCtrlRaceBaseCount+0x34 and AfterControlUpdate+0x2C: r0 from r5.
asmFunc HudCountR0R5() {
    ASM(
        nofralloc;
        lbz r0, 0x26(r5);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r0, r12;
        blr;)
}
kmCall(0x80857b3c, HudCountR0R5);
kmCall(0x808574e4, HudCountR0R5);

// UpdateRaceBalloons+0x44 and OnItemObjTargeting+0x3C: r0 from r3.
asmFunc HudCountR0R3() {
    ASM(
        nofralloc;
        lbz r0, 0x26(r3);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lis r12, raceLocalCount @ha;
        lbz r12, raceLocalCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r0, r12;
        blr;)
}
kmCall(0x80858b28, HudCountR0R3);
kmCall(0x80858a8c, HudCountR0R3);

// RaceBalloons::OnItemObjTargeting reads the targeting item's distance from its balloon's screen as
// "lfs fN, 0xc(r3)", r3 = the item's ClipInfo + screen * 4 (r0): screens 4+ read the shadow's, at
// r3 + 0x47F0 (Culling.cpp's kClipShadow - 4 * 4). r3 and r12 are written before they are read after
// each site, CR0 is set again, and the function saved LR. +0xC4 and +0x118 load f0, +0x140 f1.
asmFunc BalloonDistanceF0() {
    ASM(
        nofralloc;
        cmplwi r0, 16;
        blt load;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq load;
        addi r3, r3, 0x47f0;
        load :;
        lfs f0, 0xc(r3);
        blr;)
}
kmCall(0x807f1e78, BalloonDistanceF0);
kmCall(0x807f1ecc, BalloonDistanceF0);

asmFunc BalloonDistanceF1() {
    ASM(
        nofralloc;
        cmplwi r0, 16;
        blt load;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq load;
        addi r3, r3, 0x47f0;
        load :;
        lfs f1, 0xc(r3);
        blr;)
}
kmCall(0x807f1ef4, BalloonDistanceF1);

// RaceHUD's accesses of hudHasPlayer[slot], each "op rV, 0x60(page + slot)": slots 0-3 stay on the page,
// 4.. go to hudHasPlayerExt. Only r12 and CR0 are written; CR0 is set again before it is read at each.
// InitCtrlRaceBase+0x3DC: "stb r0, 0x60(r3)", the page in r15.
asmFunc HasPlayerStoreR0R3() {
    ASM(
        nofralloc;
        subf r12, r15, r3;
        cmplwi r12, 4;  // kGameLocal
        bge ext;
        stb r0, 0x60(r3);
        blr;
        ext :;
        addis r12, r12, hudHasPlayerExt @ha;
        stb r0, hudHasPlayerExt @l(r12);
        blr;)
}
kmCall(0x8085809c, HasPlayerStoreR0R3);

// BeforeControlUpdate+0x128: "lbz r0, 0x60(r22)", the page in r15.
asmFunc HasPlayerLoadR0R22() {
    ASM(
        nofralloc;
        subf r12, r15, r22;
        cmplwi r12, 4;  // kGameLocal
        bge ext;
        lbz r0, 0x60(r22);
        blr;
        ext :;
        addis r12, r12, hudHasPlayerExt @ha;
        lbz r0, hudHasPlayerExt @l(r12);
        blr;)
}
kmCall(0x80856e14, HasPlayerLoadR0R22);

// BeforeControlUpdate+0x1B8, +0x1DC and +0x5BC: "stb r26, 0x60(r22)", the page in r15.
asmFunc HasPlayerStoreR26R22() {
    ASM(
        nofralloc;
        subf r12, r15, r22;
        cmplwi r12, 4;  // kGameLocal
        bge ext;
        stb r26, 0x60(r22);
        blr;
        ext :;
        addis r12, r12, hudHasPlayerExt @ha;
        stb r26, hudHasPlayerExt @l(r12);
        blr;)
}
kmCall(0x80856ea4, HasPlayerStoreR26R22);
kmCall(0x80856ec8, HasPlayerStoreR26R22);
kmCall(0x808572a8, HasPlayerStoreR26R22);

// AfterControlUpdate+0x498: "lbz r0, 0x60(r3)", the page in r27.
asmFunc HasPlayerLoadR0R3() {
    ASM(
        nofralloc;
        subf r12, r27, r3;
        cmplwi r12, 4;  // kGameLocal
        bge ext;
        lbz r0, 0x60(r3);
        blr;
        ext :;
        addis r12, r12, hudHasPlayerExt @ha;
        lbz r0, hudHasPlayerExt @l(r12);
        blr;)
}
kmCall(0x80857950, HasPlayerLoadR0R3);

// The 4P variant whose anchoring a tile takes: the top or bottom row, and the right half of the grid
// (mirrored, like 4P); at 6 screens the middle column anchors left. Arithmetic only: map_%d's caller
// passes no slot.
static u32 Quadrant(u32 tile) {
    const u32 cols = raceScreenCount / 2;
    const u32 col = tile % cols;
    return (tile < cols ? 0 : 2) + (2 * col + 1 > cols ? 1 : 0);
}

// The bl snprintf naming a control's variant, "<Ctrl>_%d_%d" (screens, slot): with the count above 4
// the game would ask for _8_5, so a widened race always loads _4_<quadrant>. RankNum, Lap, ItemWindow
// and CountGO in InitCtrlRaceBase's per-player loop, and map_%d after it (count only).
static int HudVariantName(char *buf, u32 size, const char *format, u32 screens, u32 slot) {
    if (raceScreenCount != 0) {
        screens = kGameLocal;
        slot = Quadrant(slot);
    }
    return snprintf(buf, size, format, screens, slot);
}
kmCall(0x80857ddc, HudVariantName);
kmCall(0x80857e50, HudVariantName);
kmCall(0x80857ec4, HudVariantName);
kmCall(0x80857fe4, HudVariantName);
kmCall(0x808581f8, HudVariantName);

// The name balloon's "balloon_%d_%d" (screens, screen) and the item balloon's "chase_icon_%d"
// (screens): a count above 4 occurs only in a widened race. The balloon keeps its own screen in +0x175.
static int BalloonVariantName(char *buf, u32 size, const char *format, u32 screens, u32 screen) {
    if (screens > kGameLocal) {
        screens = kGameLocal;
        screen %= kGameLocal;
    }
    return snprintf(buf, size, format, screens, screen);
}
kmCall(0x807efda8, BalloonVariantName);
kmCall(0x807f1fd8, BalloonVariantName);

// VSMultiHUD::GetCtrlRaceNameBalloonCount ("li r3, 4; blr"): one name balloon per local screen, as
// AddNameBalloon gives balloon i to RaceBalloons[i % locals]. RR's kmBranch at 0x80633A00 is the next
// function.
static u32 VSMultiNameBalloonCount(const Pages::RaceHUD *) {
    return raceScreenCount != 0 && raceLocalCount > kGameLocal ? raceLocalCount : kGameLocal;
}
kmBranch(0x806339f8, VSMultiNameBalloonCount);

// Layout units: 832 x 456 at 16:9, 608 x 456 at 4:3, centred on 0, y up; each row of tiles is centred
// 114 above or below 0, as a 4P quadrant is. Element scale 0.50 at 8 screens and 0.65 at 6 (the
// owner's picks, D42).
static float LayoutWidth() { return SystemManager::sInstance->isWideScreen == 1 ? 832.0f : 608.0f; }

typedef void (*PositionFromBaseFn)(UIControl *);
static const PositionFromBaseFn positionFromBase = reinterpret_cast<PositionFromBaseFn>(0x8063d3cc);

// Moves a control loaded from its tile's 4P variant onto the tile. A countdown is centred on the tile;
// any other control keeps its 4P distance from the tile's outer edge, scaled, and its offset from the
// row's middle, scaled by at least 0.6.
static void PlaceOnTile(UIControl &control, u32 tile, bool centred) {
    const float width = LayoutWidth();
    const float factor = raceScreenCount == 8 ? 0.5f : 0.65f;
    const u32 cols = raceScreenCount / 2;
    const float tileWidth = width / cols;
    const float left = -width / 2 + tileWidth * (tile % cols);
    const float rowMid = tile < cols ? 114.0f : -114.0f;
    const u32 q = Quadrant(tile);
    const bool rightSide = (q & 1) != 0;
    PositionAndScale &base = control.positionAndscale[0];
    if (centred) {
        base.position.x = left + tileWidth / 2;
        base.position.y = rowMid;
    } else {
        const float fromEdge = rightSide ? width / 2 - base.position.x : base.position.x + width / 2;
        base.position.x = rightSide ? left + tileWidth - fromEdge * factor : left + fromEdge * factor;
        const float quadrantMid = q < 2 ? 114.0f : -114.0f;
        base.position.y = rowMid + (base.position.y - quadrantMid) * (factor > 0.6f ? factor : 0.6f);
    }
    base.scale.x *= factor;
    base.scale.z *= factor;  // Vec2 names its second component z
    positionFromBase(&control);
}

// Vtables InitCtrlRaceBase stores after LayoutUIControl::__ct; each of their Loads stores the slot in
// CtrlRaceBase::hudSlotId (+0x190).
const u32 kRankNumVtable = 0x808d3e98;
const u32 kLapVtable = 0x808d3d18;
const u32 kItemWindowVtable = 0x808d3cc8;

typedef char CtrlRaceCountIs0x198[sizeof(CtrlRaceCount) == 0x198 ? 1 : -1];

// The count pairs (the countdown digits and FINISH) zoom in from many times their settled size, across
// every tile. A widened race draws them through a copy of CtrlRaceCount's vtable (0x808D3C18, shared with
// TTSplits and the team leaderboard) whose Draw (+0x14) scales that zoom down to start at the tile.
const u32 kCountVtableWords = 0x5c / 4;
static u32 countVtable[kCountVtableWords];
static CtrlRaceCount *countArray;
static float countSettledScale;
static float countZoomStart[2 * kMaxLocal];

typedef void (*LayoutDrawFn)(LayoutUIControl *, u32);
static const LayoutDrawFn layoutDraw = reinterpret_cast<LayoutDrawFn>(0x8063db84);
typedef void (*AnimateFn)(MainLayout *);
static const AnimateFn layoutAnimate = reinterpret_cast<AnimateFn>(0x805e91a8);

// An estimate of the text's width at pane scale 1 from the text box's font: each character's width in
// font units times fontSizeX / the font's width, plus the character spacing. MKW's tags (0x1A, then a
// u16 whose high byte is the tag's length in bytes) are skipped. MKW draws through its own text
// handler, so the drawn word can be wider; the caller keeps a margin.
static float TextWidth(const nw4r::lyt::TextBox &text) {
    if (text.font == nullptr || text.stringBuf == nullptr || text.font->GetWidth() == 0) return 0.0f;
    const float perUnit = text.fontSizeX / text.font->GetWidth();
    float width = 0.0f;
    for (const wchar_t *c = text.stringBuf; *c != 0;) {
        if (*c == 0x1a) {
            const u32 tagChars = (static_cast<u16>(c[1]) >> 8) / 2;
            if (tagChars < 2) break;
            c += tagChars;
            continue;
        }
        width += text.font->GetCharWidth(*c) * perUnit + text.charSpace;
        ++c;
    }
    return width;
}

// The text's scale runs from its zoom's start down to the settled scale; the zoom is remapped to start
// where the text fills the tile (its height, or its width for a long word) through count_down_null, the
// text's parent, which no animation scales. MainLayout::Animate applies the current frame only, so
// applying it here and again in Draw gives the same values.
static void ZoomFromTile(CtrlRaceCount &count, u32 index) {
    layoutAnimate(&count.layout);
    nw4r::lyt::TextBox *text = static_cast<nw4r::lyt::TextBox *>(count.layout.GetPaneByName("text_00"));
    nw4r::lyt::Pane *parent = count.layout.GetPaneByName("count_down_null");
    if (text == nullptr || parent == nullptr) return;
    const float scale = text->scale.x;
    float ratio = 1.0f;
    if (scale > countSettledScale) {
        if (scale > countZoomStart[index]) countZoomStart[index] = scale;
        const float controlScale = count.positionAndscale[0].scale.z;
        float fit = 228.0f / (text->fontSizeY * controlScale);  // a tile is 228 high
        const float width = TextWidth(*text);
        // FINISH! draws about 15% wider than the estimate at 8 screens, so the width fits 80% of the tile.
        const float fitWidth = 0.8f * LayoutWidth() / (raceScreenCount / 2) / (width * controlScale);
        if (width > 0.0f && fitWidth < fit) fit = fitWidth;
        if (fit < countSettledScale) fit = countSettledScale;
        const float start = countZoomStart[index];
        if (fit < start) ratio = (countSettledScale + (scale - countSettledScale) * (fit - countSettledScale) / (start - countSettledScale)) / scale;
    } else
        countZoomStart[index] = 0.0f;
    parent->scale.x = ratio;
    parent->scale.z = ratio;
}

static void CountDraw(CtrlRaceCount *count, u32 zIdx) {
    ZoomFromTile(*count, count - countArray);
    layoutDraw(count, zIdx);
}

// The settled scale is the text pane's own, read before any animation has run.
static void UseCountDraw(CtrlRaceCount *counts, u32 count) {
    const u32 *game = *reinterpret_cast<u32 *const *>(counts);
    for (u32 i = 0; i < kCountVtableWords; ++i) countVtable[i] = game[i];
    countVtable[0x14 / 4] = reinterpret_cast<u32>(CountDraw);
    countArray = counts;
    nw4r::lyt::Pane *text = counts[0].layout.GetPaneByName("text_00");
    countSettledScale = text != nullptr ? text->scale.x : 0.0f;
    for (u32 i = 0; i < count; ++i) {
        *reinterpret_cast<u32 **>(&counts[i]) = countVtable;
        countZoomStart[i] = 0.0f;
    }
}

typedef void (*InitCtrlRaceBaseFn)(Pages::RaceHUD *, u32);
static const InitCtrlRaceBaseFn initCtrlRaceBase = reinterpret_cast<InitCtrlRaceBaseFn>(0x80857cc0);

// RaceHUD::OnInit+0x64, "bl InitCtrlRaceBase": the controls exist once it returns (RR's custom ones
// were built at OnInit+0x58), and are placed before InitControls, whose CtrlRaceBase::InitSelf takes
// the pause slide from the position. The countdown pair of slot i is ctrlRaceCountArray[2i, 2i + 1]:
// CtrlRaceCount::Load ignores the slot it is passed.
static void InitCtrlRaceBaseWide(Pages::RaceHUD *page, u32 bitField) {
    for (u32 i = 0; i < kMaxLocal; ++i) hudHasPlayerExt[i] = 0;
    initCtrlRaceBase(page, bitField);
    if (raceScreenCount == 0 || raceLocalCount == 0) return;
    const ControlGroup &group = page->controlGroup;
    for (u32 i = 0; i < group.controlCount; ++i) {
        UIControl *control = group.controlArray[i];
        if (control == nullptr) continue;
        const u32 vtable = *reinterpret_cast<const u32 *>(control);
        if (vtable != kRankNumVtable && vtable != kLapVtable && vtable != kItemWindowVtable) continue;
        const u8 slot = static_cast<CtrlRaceBase *>(control)->hudSlotId;
        if (slot < raceLocalCount) PlaceOnTile(*control, slot, false);
    }
    if (page->ctrlRaceCountArray != nullptr) {
        for (u32 i = 0; i < 2u * raceLocalCount; ++i) PlaceOnTile(page->ctrlRaceCountArray[i], i / 2, true);
        UseCountDraw(page->ctrlRaceCountArray, 2u * raceLocalCount);
    }
}
kmCall(0x808562dc, InitCtrlRaceBaseWide);

// A speedometer per local tile (D43), when RR's setting is on; RR's own builds one for a single local
// only, which a widened 1-local race keeps.
static u32 SpeedoCount() {
    if (raceScreenCount == 0 || raceLocalCount < 2) return 0;
    if (Pulsar::Settings::Mgr::Get().GetSettingValue(Pulsar::Settings::SETTING_SPEEDOMETER) == Pulsar::SOM_DISABLED) return 0;
    return raceLocalCount;
}

static void SpeedoCreate(Page &page, u32 index, u32 count) {
    for (u32 slot = 0; slot < count; ++slot) {
        Pulsar::UI::CtrlRaceSpeedo *speedo = new Pulsar::UI::CtrlRaceSpeedo;
        page.AddControl(index + slot, *speedo, 0);
        char variant[0x20];
        snprintf(variant, sizeof(variant), "Speedo_4_%u", Quadrant(slot));
        speedo->Load(variant, slot);
        PlaceOnTile(*speedo, slot, false);
    }
}
static Pulsar::UI::CustomCtrlBuilder wideSpeedos(SpeedoCount, SpeedoCreate);

}  // namespace SplitScreen8
