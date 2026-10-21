#include <kamek.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>
#include <MarioKartWii/CourseMgr.hpp>
#include <MarioKartWii/KMP/KMPManager.hpp>
#include <core/nw4r/math/Triangular.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

// ModelDirector's screen bit is 0x10000 << screen in bitfield +4; 4-7 would hit bits 20-23, which the game uses, so a
// widened race uses 28-31, which nothing reads. Otherwise this is exactly slw: a 6-bit amount, 32-63 giving 0 (hud -1).
static u32 ScreenBit(u32 screen) {
    if (!IsWideHud(screen)) {
        screen &= 0x3f;
        return screen < 32 ? 0x10000u << screen : 0;
    }
    return screen < kMaxLocal ? 0x10000000u << (screen - kGameLocal) : 0;
}

// The five screen-bit leaves (5-7 instructions, no stack frame), replaced whole.
static void EnableScreen(ModelDirector &director, u32 screen) {
    director.bitfield |= ScreenBit(screen);
}
kmBranch(0x8055cce0, EnableScreen);
kmBranch(0x8055cd30, EnableScreen);  // EnableScreen2, the same body

static void DisableScreen(ModelDirector &director, u32 screen) {
    director.bitfield &= ~ScreenBit(screen);
}
kmBranch(0x8055ccf8, DisableScreen);

static bool IsVisibleOnScreen(const ModelDirector &director, u32 screen) {
    return (director.bitfield & ScreenBit(screen)) != 0;
}
kmBranch(0x8055cd10, IsVisibleOnScreen);
kmBranch(0x8055cd48, IsVisibleOnScreen);  // IsVisibleOnScreen2, the same body

// ModelDirector::UpdateVisibility+0x40 replaces "slw r0, r0, r4" (r0 = 0x10000, r4 = screen), ScreenBit's rule in asm.
// r12 is reloaded before it is read; the prologue saved LR. 4 is kGameLocal, 8 kMaxLocal.
asmFunc UpdateVisibilityScreenBit() {
    ASM(
        nofralloc;
        cmplwi r4, 4;
        blt game;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq game;
        li r0, 0;
        cmplwi r4, 8;
        bgelr;
        lis r0, 0x1000;
        addi r12, r4, -4;
        slw r0, r0, r12;
        blr;
        game :;
        slw r0, r0, r4;
        blr;)
}
kmCall(0x8055cc4c, UpdateVisibilityScreenBit);

// Leaf 0x8055d2f4 tests the screen bit inline at +0x30 and +0x48 ("lis r0, 1; slw r0, r0, r4; and. r0, r5, r0").
// No stack frame, so branch out and back past the and.; r7 is never read in the leaf. One blr each, for kmPatchExitPoint.
asmFunc LeafScreenBitHide() {
    ASM(
        nofralloc;
        lis r0, 1;
        cmplwi r4, 4;
        blt game;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq game;
        li r0, 0;
        cmplwi r4, 8;
        bge test;
        lis r0, 0x1000;
        addi r7, r4, -4;
        slw r0, r0, r7;
        b test;
        game :;
        slw r0, r0, r4;
        test :;
        and.r0, r5, r0;
        blr;)
}
kmBranch(0x8055d324, LeafScreenBitHide);
kmPatchExitPoint(LeafScreenBitHide, 0x8055d330);

asmFunc LeafScreenBitShow() {
    ASM(
        nofralloc;
        lis r0, 1;
        cmplwi r4, 4;
        blt game;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq game;
        li r0, 0;
        cmplwi r4, 8;
        bge test;
        lis r0, 0x1000;
        addi r7, r4, -4;
        slw r0, r0, r7;
        b test;
        game :;
        slw r0, r0, r4;
        test :;
        and.r0, r5, r0;
        blr;)
}
kmBranch(0x8055d33c, LeafScreenBitShow);
kmPatchExitPoint(LeafScreenBitShow, 0x8055d348);

// Effects::Mgr's Sub9d8 pointers (per-player culling per screen) cannot widen: inline per-screen blocks end where
// they begin. Screens 4+ read this stand-in: every player near (0) and on screen (1).
struct Sub9d8Visible {
    const u8 *distance;  // 0x0
    void *unknown_0x4;
    void *unknown_0x8;
    const s32 *onScreen;  // 0xC
};
static const u8 visibleDistance[12] = {};
static const s32 visibleOnScreen[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static const Sub9d8Visible sub9d8Visible = {visibleDistance, nullptr, nullptr, visibleOnScreen};

// Effects::Mgr::Draw+0x6B8 replaces "lwzx r5, r18, r3" (r3 = the Sub9d8 array, r18 = screen * 4), its only read.
// r3 is rewritten next; the prologue saved LR. 16 is kGameLocal * 4.
asmFunc Sub9d8ForGameScreens() {
    ASM(
        nofralloc;
        cmplwi r18, 16;
        bge wide;
        lwzx r5, r18, r3;
        blr;
        wide :;
        lis r5, sub9d8Visible @ha;
        addi r5, r5, sub9d8Visible @l;
        blr;)
}
kmCall(0x8067d62c, Sub9d8ForGameScreens);

// Audio::ItemWarningMgr::GetItemWarning, a leaf replaced whole: warnings[4] (0xC at +0x14) indexed by a shell target's
// hud, 4-7 for a spare CPU. Those get hud -1, on which PlayTargetedWarning returns at once (+0x5C).
struct ItemWarning {
    s8 hud;
    u8 unknown_0x1[0xC - 0x1];
};
static ItemWarning noItemWarning = {-1};

static ItemWarning *GetItemWarning(u8 *mgr, u32 hud) {
    if (IsWideHud(hud))
        return &noItemWarning;
    return reinterpret_cast<ItemWarning *>(mgr + 0x14 + hud * 0xC);
}
kmBranch(0x806f8210, GetItemWarning);

// Audio::KartActor::Link+0x6C replaces "stb r3, 0xb3(r31)", the hud slot that also marks the kart local. A local's hud 4+
// is kept (Audio.cpp redirects its 4-wide readers); a spare CPU's becomes -1. CR0, r11, r12 are rewritten; LR saved.
asmFunc KartSoundHud() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq store;
        extsb r12, r3;
        cmpwi r12, 4;
        blt store;
        lis r11, raceLocalCount @ha;
        lbz r11, raceLocalCount @l(r11);
        cmpw r12, r11;
        blt store;
        li r3, -1;
        store :;
        stb r3, 0xb3(r31);
        blr;)
}
kmCall(0x807075a0, KartSoundHud);

// A Battle round's end moves each local's kart to its screen's MSPT (0x805847B0), and an arena has at most 4 (Wii Rainbow
// Road 1): a screen past them parks beside spot screen % count, at the first offset clear of walls and over floor.
static const float kBesideSpot = 400.0f;
static const s8 kBesideOffsets[][2] = {{1, 0}, {-1, 0}, {0, -1}, {0, 1}, {2, 0}, {-2, 0}, {1, -1}, {-1, -1}};
static const u32 kWallKcl = KCL_BITFIELD_WALL | KCL_BITFIELD_INVISIBLE_WALL | KCL_BITFIELD_WALL_3 | KCL_BITFIELD_PLAYER_ONLY_WALL | KCL_BITFIELD_WEAK_WALL | KCL_BITFIELD_SPECIAL_WALL
  | KCL_BITFIELD_WALL5 | KCL_BITFIELD_FALL_BOUNDARY;
static const u32 kFloorKcl = 0x20e80dff;  // the floor mask 0x8058473C samples under the spot

// An arena's object KCLs read info->objInfo and ret unchecked (0x8081323C, 0x8081340C): neither is null.
static bool Collides(const Vec3 &pos, const Vec3 &from, u32 kcl, float radius) {
    CollisionInfo info;
    info.objInfo = nullptr;
    KCLTypeHolder ret;
    return CourseMgr::sInstance->IsCollidingAddEntry(pos, from, static_cast<KCLBitfield>(kcl), &info, &ret, 0, radius);
}

static KMP::Holder<MSPT> *ScreenMspt(const KMP::Manager &kmp, u16 index) {
    const u16 count = kmp.msptSection != nullptr ? kmp.msptSection->pointCount : 0;
    if (raceScreenCount == 0 || index < count || count == 0 || index >= kMaxLocal)
        return kmp.GetHolder<MSPT>(index);
    static MSPT beside;
    static KMP::Holder<MSPT> besideHolder = {&beside};
    // Screens count..index are placed in order, each off the spots and the karts placed before it.
    Vec3 taken[kMaxLocal];
    for (u16 s = 0; s < count; ++s) taken[s] = kmp.GetHolder<MSPT>(s)->raw->position;
    for (u16 s = count; s <= index; ++s) {
        beside = *kmp.GetHolder<MSPT>(s % count)->raw;
        float sin = 0.0f;
        float cos = 0.0f;
        nw4r::math::SinCosFIdx(&sin, &cos, beside.angle.y * (256.0f / 360.0f));
        const Vec3 base = beside.position;
        for (u32 i = 0; i < sizeof(kBesideOffsets) / sizeof(kBesideOffsets[0]); ++i) {
            const float side = kBesideSpot * kBesideOffsets[i][0];
            const float ahead = kBesideSpot * kBesideOffsets[i][1];
            Vec3 pos(base.x + side * cos + ahead * sin, base.y, base.z - side * sin + ahead * cos);
            bool clear = !Collides(Vec3(pos.x, pos.y + kBesideSpot / 2, pos.z), Vec3(pos.x, pos.y + kBesideSpot / 2, pos.z), kWallKcl, kBesideSpot / 2);
            clear = clear && Collides(Vec3(pos.x, pos.y - kBesideSpot / 2, pos.z), Vec3(pos.x, pos.y + kBesideSpot, pos.z), kFloorKcl, kBesideSpot / 8);
            for (u16 t = 0; clear && t < s; ++t) {
                const Vec3 &k = taken[t];
                clear = (k.x - pos.x) * (k.x - pos.x) + (k.z - pos.z) * (k.z - pos.z) >= kBesideSpot * kBesideSpot * 0.5f;
            }
            if (clear || i == 0)
                beside.position = pos;
            if (clear)
                break;
        }
        taken[s] = beside.position;
    }
    return &besideHolder;
}
kmCall(0x805847b0, ScreenMspt);

}  // namespace SplitScreen8
