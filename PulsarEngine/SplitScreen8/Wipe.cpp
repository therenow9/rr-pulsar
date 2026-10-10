#include <kamek.hpp>
#include <core/System/SystemManager.hpp>
#include <MarioKartWii/UI/Layout/ControlLoader.hpp>
#include <MarioKartWii/UI/Page/Other/Wipe.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The Wipe page (0x3A) draws each local player's respawn fade and the partition lines. Its wipes are
// the section's 1P/2P/4P variants, so on a 3x2 or 4x2 grid they cover 4P quadrants and the lines
// cross the middle of tiles (D35, D36). Only race and demo sections build it, after InitRace has set
// raceScreenCount for that race.

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

// Section::CreatePageById, case 0x3A, replaces "li r3, 0x7a8", the size passed to new. CR0 is set again
// after new returns; the function saved LR. 0x7a8 is sizeof(Pages::Wipe).
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

typedef Pages::Wipe *(*WipeCtorFn)(Pages::Wipe *);
typedef void (*ControlCtorFn)(void *);
typedef void (*ControlDtorFn)(void *, s32);
static const WipeCtorFn wipeCtor = reinterpret_cast<WipeCtorFn>(0x80651e80);
static const ControlCtorFn wipeControlCtor = reinterpret_cast<ControlCtorFn>(0x80651c0c);
static const ControlDtorFn wipeControlDtor = reinterpret_cast<ControlDtorFn>(0x80651c48);
static const ControlCtorFn layoutControlCtor = reinterpret_cast<ControlCtorFn>(0x8063d798);
static const ControlDtorFn layoutControlDtor = reinterpret_cast<ControlDtorFn>(0x8063d8c0);

// The same case's "bl Pages::Wipe::__ct" (r3 = the new page, which the case returns in r3). A page
// allocated wide constructs its extras as the ctor's __construct_array constructs the game's.
static Pages::Wipe *WipeCtor(Pages::Wipe *page) {
    wipeCtor(page);
    if (raceScreenCount == 0)
        return page;
    for (u32 i = 0; i < kExtWipes; ++i) wipeControlCtor(&ExtWipe(*page, i));
    for (u32 i = 0; i < kExtLines; ++i) layoutControlCtor(&ExtLine(*page, i));
    widePage = page;
    return page;
}
kmCall(0x806238dc, WipeCtor);

// Pages::Wipe::__dt+0x38 calls LayoutUIControl::__dt(&partition_line, -1), before __destroy_array
// destroys the game's 4 wipes: the extras go with them.
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

// UIControl::PositionFromBasePosition: positionAndscale[1..3] from [0] through the parent group, which
// AddControl has set.
typedef void (*PositionFromBaseFn)(UIControl *);
static const PositionFromBaseFn positionFromBase = reinterpret_cast<PositionFromBaseFn>(0x8063d3cc);

// Layout units: the 2D screen is 832 x 456 at 16:9 and 608 x 456 at 4:3, centred on 0, y up; the wipe
// pane is 832 x 456 at scale 1. Tiles are numbered as Screens.cpp's GridRect numbers them, so a hud
// slot's wipe sits on its own tile.
static void PlaceOnTile(UIControl &control, u32 tile) {
    const float width = SystemManager::sInstance->isWideScreen == 1 ? 832.0f : 608.0f;
    const u32 cols = raceScreenCount / 2;
    const float tileWidth = width / cols;
    PositionAndScale &base = control.positionAndscale[0];
    base.position.x = -width / 2 + tileWidth * (tile % cols + 0.5f);
    base.position.y = tile < cols ? 114.0f : -114.0f;
    base.scale.x = tileWidth / 832.0f;
    base.scale.z = 0.5f;  // Vec2 names its second component z
    positionFromBase(&control);
}

static const char **const wipeAnims = reinterpret_cast<const char **>(0x80899f18);  // "Wipe", "Nothing", "Normal"

// The game's vertical line stays between the middle columns at 8 screens and is hidden at 6; the two
// extra lines are vertical only, between the outer columns.
static void PlaceLines(Pages::Wipe &page, u32 firstIdx) {
    const float width = SystemManager::sInstance->isWideScreen == 1 ? 832.0f : 608.0f;
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
        positionFromBase(&line);
    }
}

// After OnInit has loaded the game's wipes (idx = hud slot) and line: the extra wipes for hud slots
// 4.., then every wipe moved to its tile, then the lines.
static void WideWipeInit(Pages::Wipe *page, u32 gameWipes) {
    if (page != widePage)
        return;
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
