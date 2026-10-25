#include <kamek.hpp>
#include <MarioKartWii/3D/GameScreenEffects/GameScreenEffects.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Page/Other/Wipe.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

const u32 kExtWipes = kMaxLocal - kGameLocal;
const u32 kExtLines = 2;
// WipeControl and LayoutUIControl sizes; a widened page holds its extras past the game's 0x7A8.
const u32 kWipeControlSize = 0x178;
const u32 kLineControlSize = 0x174;
const u32 kExtOffset = sizeof(Pages::Wipe);
const u32 kWidePageSize = kExtOffset + kExtWipes * kWipeControlSize + kExtLines * kLineControlSize;
// WipePageSize's li writes it as a literal.
typedef char WidePageSizeIs0x1070[kWidePageSize == 0x1070 ? 1 : -1];

// The one widened Wipe page alive, or null. Its extras are destroyed with it, so they are tracked by
// the page, not by raceScreenCount, which the next race's InitRace may change first.
static Pages::Wipe *widePage;

static WipeControl &ExtWipe(Pages::Wipe &page, u32 i) {
    return *reinterpret_cast<WipeControl *>(reinterpret_cast<u8 *>(&page) + kExtOffset + i * kWipeControlSize);
}

static LayoutUIControl &ExtLine(Pages::Wipe &page, u32 i) {
    return *reinterpret_cast<LayoutUIControl *>(reinterpret_cast<u8 *>(&page) + kExtOffset + kExtWipes * kWipeControlSize + i * kLineControlSize);
}

// Section::CreatePageById case 0x3A, "li r3, 0x7a8" (sizeof(Pages::Wipe)) for new; only race and demo sections build the page,
// after InitRace has set raceScreenCount. CR0 is set again after new returns; the function saved LR.
asmFunc WipePageSize() {
    ASM(
        nofralloc;
        lis r3, raceScreenCount @ha;
        lbz r3, raceScreenCount @l(r3);
        cmpwi r3, 0;
        li r3, 0x7a8;
        beqlr;
        li r3, 0x1070;
        blr;)
}
kmCall(0x806238cc, WipePageSize);

extern "C" {
Pages::Wipe *wipeCtor(Pages::Wipe *);
void wipeControlCtor(void *);
void wipeControlDtor(void *, s32);
void layoutControlDtor(void *, s32);
extern const char *wipeAnims[];  // "Wipe", "Nothing", "Normal"
}

// The same case's "bl Pages::Wipe::__ct" (r3 = the new page, which the case returns in r3).
static Pages::Wipe *WipeCtor(Pages::Wipe *page) {
    wipeCtor(page);
    if (raceScreenCount == 0)
        return page;
    for (u32 i = 0; i < kExtWipes; ++i) wipeControlCtor(&ExtWipe(*page, i));
    for (u32 i = 0; i < kExtLines; ++i) layoutCtor(&ExtLine(*page, i));
    widePage = page;
    return page;
}
kmCall(0x806238dc, WipeCtor);

// Pages::Wipe::__dt+0x38, LayoutUIControl::__dt(&partition_line, -1), before __destroy_array destroys the game's 4 wipes.
static void WipeLineDtor(LayoutUIControl *line, s32 flag) {
    layoutControlDtor(line, flag);
    Pages::Wipe *page = reinterpret_cast<Pages::Wipe *>(reinterpret_cast<u8 *>(line) - offsetof(Pages::Wipe, partition_line));
    if (page != widePage)
        return;
    for (u32 i = 0; i < kExtLines; ++i) layoutControlDtor(&ExtLine(*page, i), -1);
    for (u32 i = 0; i < kExtWipes; ++i) wipeControlDtor(&ExtWipe(*page, i), -1);
    widePage = nullptr;
}
kmCall(0x80651f24, WipeLineDtor);

// Wipes past the game's 4: locals 5-8 race in the 4P section, whose page builds 4 (r31).
static u32 ExtWipeCount(u32 gameWipes) {
    return gameWipes == kGameLocal && raceLocalCount > kGameLocal ? raceLocalCount - kGameLocal : 0;
}

// Controls of the page: the game's wipes and partition line, then the extra wipes and lines.
static u32 WipeGroupSize(const Pages::Wipe *page, u32 gameWipes) {
    if (page != widePage)
        return gameWipes + 1;
    return gameWipes + 1 + ExtWipeCount(gameWipes) + kExtLines;
}

// OnInit+0x19C replaces "addi r4, r31, 1", the control count passed to InitControlGroup (r3 = r30 =
// the page, r31 = the section's wipe count). Only r3 and r4 are read before the call; OnInit saved LR.
asmFunc WipeGroupSizeStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r30;
        mr r4, r31;
        bl WipeGroupSize;
        mr r4, r3;
        mr r3, r30;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        blr;)
}
kmCall(0x80652114, WipeGroupSizeStub);

// The wipe pane is 832 x 456 at scale 1.
// Tiles are numbered as Screens.cpp's GridRect numbers them, so a hud slot's wipe sits on its own tile.
static void PlaceOnTile(UIControl &control, u32 tile) {
    const float width = GameScreenEffectsMgr::GetRootScreenWidth();
    const u32 cols = raceScreenCount / 2;
    const float tileWidth = width / cols;
    PositionAndScale &base = control.positionAndscale[0];
    base.position.x = -width / 2 + tileWidth * (tile % cols + 0.5f);
    base.position.y = tile < cols ? 114.0f : -114.0f;
    base.scale.x = tileWidth / 832.0f;
    base.scale.z = 0.5f;  // Vec2 names its second component z
    control.PositionFromBasePosition();
}

// The game's vertical line stays between the middle columns at 8 screens and is hidden at 6; the two
// extra lines are vertical only, between the outer columns.
static void PlaceLines(Pages::Wipe &page, u32 firstIdx) {
    const float width = GameScreenEffectsMgr::GetRootScreenWidth();
    page.partition_line.SetPaneVisibility("tate_line", raceScreenCount == 8);
    page.partition_line.SetPaneVisibility("yoko_line", true);
    for (u32 i = 0; i < kExtLines; ++i) {
        LayoutUIControl &line = ExtLine(page, i);
        page.AddControl(firstIdx + i, line, 0);
        ControlLoader loader(&line);
        loader.Load("game_image", "partition_line", "partition_line", nullptr);
        line.SetPaneVisibility("tate_line", true);
        line.SetPaneVisibility("yoko_line", false);
        const float offset = width / (raceScreenCount == 8 ? 4 : 6);
        line.positionAndscale[0].position.x = i == 0 ? -offset : offset;
        line.PositionFromBasePosition();
    }
}

// Runs after OnInit has loaded the game's wipes (idx = hud slot) and line. The game's 2-4 screen wipes fill the layout width,
// so a wider screen record (WiiCompiled) stretches them over it; the partition line's yoko_line already spans it.
static void WideWipeInit(Pages::Wipe *page, u32 gameWipes) {
    if (page != widePage) {
        const float stretch = GameScreenEffectsMgr::GetRootScreenWidth() / LayoutWidth();
        for (u32 i = 0; gameWipes > 1 && stretch > 1.0f && i < gameWipes; ++i) {
            PositionAndScale &base = page->wipeControls[i].positionAndscale[0];
            base.position.x *= stretch;
            base.scale.x *= stretch;
            page->wipeControls[i].PositionFromBasePosition();
        }
        return;
    }
    const u32 ext = ExtWipeCount(gameWipes);
    for (u32 i = 0; i < ext; ++i) {
        WipeControl &wipe = ExtWipe(*page, i);
        page->AddControl(gameWipes + 1 + i, wipe, 0);
        wipe.idx = kGameLocal + i;
        ControlLoader loader(&wipe);
        loader.Load("game_image", "Wipe", "Wipe1P4Div", wipeAnims);
    }
    for (u32 i = 0; i < gameWipes; ++i) PlaceOnTile(page->wipeControls[i], i);
    for (u32 i = 0; i < ext; ++i) PlaceOnTile(ExtWipe(*page, i), kGameLocal + i);
    PlaceLines(*page, gameWipes + 1 + ext);
}

// OnInit+0x338 replaces "lmw r24, 0x80(r1)", where every path ends (r30 = the page, r31 = the
// section's wipe count). The lmw runs after this frame is popped; OnInit saved LR.
asmFunc WideWipeInitStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r30;
        mr r4, r31;
        bl WideWipeInit;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lmw r24, 0x80(r1);
        blr;)
}
kmCall(0x806522b0, WideWipeInitStub);

}  // namespace SplitScreen8
