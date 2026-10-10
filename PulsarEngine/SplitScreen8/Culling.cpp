#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Culling for screens 4-7 (D21). A ClipInfo keeps a distance and a flag byte per screen inline, for 4
// screens. In a widened race ClipInfoMgr allocates its pool twice over; entry i + kClipPool holds
// screens 4-7 of entry i in the same layout, so every reader reaches it by adding kClipShadow.
// docs/plans/m2-engine-widening.md (phase B, "Culling") has the reader scan.

namespace SplitScreen8 {

// ClipInfo and ClipScreenInfo as ClipInfoMgr::Update 0x80787774 uses them; the game header keeps
// both classes' members private.
struct Clip {
    const float *position;  // 0x0
    float radius;  // 0x4
    float farSquared;  // 0x8
    float distance[4];  // 0xC, per screen: squared, times the screen's scale
    u16 areaIds;  // 0x1C
    u8 areaType;  // 0x1E, 2 skips the area test
    u8 padding;
    u8 flags[4];  // 0x20, per screen
};  // 0x24

struct ClipScreen {
    float position[3];  // 0x0
    float unknown_0xC;
    float planes[6][3];  // 0x10
    float scale;  // 0x58
    u16 areaIds;  // 0x5C
    u8 padding[2];
};  // 0x60

// Per-screen flag bits. Init and the item code set the others (0x10, 0x20, 0x40) on all 4 bytes.
const u8 kCulled = 0x01;
const u8 kFar = 0x02;
const u8 kOtherPlanes = 0x10;  // tests planes 1 and 3, not 4 and 5
const u8 kSkip = 0x20;  // Update leaves the whole ClipInfo alone
const u8 kNever = 0x80;  // not on this screen at all (Init's only-screen argument)

// ClipInfoMgr::CreateInstance allocates 0x200 ClipInfos ("li r0, 0x200").
const u32 kClipPool = 0x200;
const u32 kClipShadow = kClipPool * sizeof(Clip);  // 0x4800

static Clip &Shadow(Clip &clip) {
    return *reinterpret_cast<Clip *>(reinterpret_cast<u8 *>(&clip) + kClipShadow);
}

static float Dot(const float *plane, const float *d) {
    return d[0] * plane[0] + d[1] * plane[1] + d[2] * plane[2];
}

// Update's per-screen body (+0x160..+0x304) for one screen: the new culled and far bits, and the
// distance. The comparisons keep the game's branch senses (bgt / ble after fcmpo), so a NaN takes the
// same path.
static u8 Classify(const Clip &clip, const ClipScreen &screen, u8 flags, float &distance) {
    const float d[3] = {clip.position[0] - screen.position[0], clip.position[1] - screen.position[1], clip.position[2] - screen.position[2]};
    distance = screen.scale * (d[2] * d[2] + d[0] * d[0] + d[1] * d[1]);
    if (!(distance <= clip.farSquared))
        return kCulled | kFar;
    if (clip.areaType != 2 && (clip.areaIds & screen.areaIds) != 0)
        return kCulled;
    const float r = clip.radius;
    if (Dot(screen.planes[0], d) > r || !(Dot(screen.planes[2], d) <= r)) {
        if ((flags & kOtherPlanes) == 0)
            return kCulled;
        if (Dot(screen.planes[1], d) > r || !(Dot(screen.planes[3], d) <= r))
            return kCulled;
        return 0;
    }
    if ((flags & kOtherPlanes) != 0)
        return 0;
    if (Dot(screen.planes[4], d) > r || !(Dot(screen.planes[5], d) <= r))
        return kCulled;
    return 0;
}

#ifdef SS8_DEBUG_SCREENS
// Proves Classify against the game: screens 0-3, which vanilla has just computed, recomputed here.
static u32 selfChecked;
static u32 selfMismatched;

static void SelfCheck(const Clip &clip, const ClipScreen *screens) {
    if (((clip.flags[0] | clip.flags[1] | clip.flags[2] | clip.flags[3]) & kSkip) != 0)
        return;
    for (u32 s = 0; s < kGameLocal; ++s) {
        const u8 flags = clip.flags[s];
        if ((flags & kNever) != 0)
            continue;
        float distance;
        const u8 bits = Classify(clip, screens[s], flags, distance);
        const float diff = distance - clip.distance[s];
        const float tolerance = (distance < 0 ? -distance : distance) * 1e-5f + 1e-6f;
        const bool same = (flags & (kCulled | kFar)) == bits && diff <= tolerance && -diff <= tolerance;
        ++selfChecked;
        if (!same && ++selfMismatched <= 8) {
            OS::Report("ss8 clip self-check: screen %u flags %02x mine %02x distance %08x mine %08x\n", s, flags, bits, *reinterpret_cast<const u32 *>(&clip.distance[s]),
              *reinterpret_cast<const u32 *>(&distance));
        }
        if (selfChecked % 0x40000 == 0) {
            OS::Report("ss8 clip self-check: %u checked, %u mismatched\n", selfChecked, selfMismatched);
        }
    }
}
#endif

// After Update's per-screen loop for one ClipInfo (screens 0-3), screens 4..N-1 into its shadow. The
// flags Init or the item code set uniformly are taken from screen 0; under the skip bit, as Update
// leaves the bytes alone, the shadow takes screen 0's whole byte.
static void ClipShadowUpdate(Clip &clip, const ClipScreen *screens) {
#ifdef SS8_DEBUG_SCREENS
    SelfCheck(clip, screens);
#endif
    Clip &shadow = Shadow(clip);
    const u8 *game = clip.flags;
    const bool skip = ((game[0] | game[1] | game[2] | game[3]) & kSkip) != 0;
    for (u32 s = kGameLocal; s < raceScreenCount; ++s) {
        const u32 i = s - kGameLocal;
        const u8 never = shadow.flags[i] & kNever;
        if (skip) {
            shadow.flags[i] = never | (game[0] & ~kNever);
        } else if (never != 0) {
            shadow.flags[i] = never | kCulled | (shadow.flags[i] & kFar) | (game[0] & ~(kNever | kCulled | kFar));
        } else {
            const u8 flags = game[0] & ~(kNever | kCulled | kFar);
            shadow.flags[i] = flags | Classify(clip, screens[s], flags, shadow.distance[i]);
        }
    }
}

// ClipInfoMgr::Update+0x318 replaces "addi r26, r26, 0x24", the per-ClipInfo loop's tail, reached
// both after the screen loop and from the skip test; r23 = the ClipInfo, r22 = ClipInfoMgr (+0x1C
// the ClipScreenInfos). r0 and r3-r12 are reloaded before they are read; the prologue saved LR.
asmFunc ClipShadowUpdateHook() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq done;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r23;
        lwz r4, 0x1c(r22);
        bl ClipShadowUpdate;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        done :;
        addi r26, r26, 0x24;
        blr;)
}
kmCall(0x80787a8c, ClipShadowUpdateHook);

// ClipInfoMgr::Update+0x110 replaces "andc. r0, r0, r5" (r0 = 0x02020202, r5 = the flag word): the
// area ids are refreshed unless every screen is far, so screens 4-7 count too. r31 = 0x02020000 and
// r23 = the ClipInfo; r12 is free in the loop. 0x4820 is kClipShadow + 0x20.
asmFunc ClipAreaRefreshAllScreens() {
    ASM(
        nofralloc;
        andc.r0, r0, r5;
        bnelr;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lwz r12, 0x4820(r23);
        and r12, r12, r5;
        addi r0, r31, 0x202;
        andc.r0, r0, r12;
        blr;)
}
kmCall(0x80787884, ClipAreaRefreshAllScreens);

// ClipInfo::Init leaves r3 (the ClipInfo) and r9 (its only screen, -1 for every screen) as they
// were, so the shadow starts as a copy of screen 0. Screens past the race's count stay "never".
static void ClipShadowInit(Clip &clip, s32 onlyScreen) {
    Clip &shadow = Shadow(clip);
    for (u32 s = kGameLocal; s < kMaxLocal; ++s) {
        const u32 i = s - kGameLocal;
        u8 flags = clip.flags[0] & ~kNever;
        if (s >= raceScreenCount || (onlyScreen != -1 && onlyScreen != static_cast<s32>(s)))
            flags |= kNever;
        shadow.flags[i] = flags;
        shadow.distance[i] = clip.distance[0];
    }
}

// ClipInfoMgr::Insert+0xA0 replaces "lwz r4, 0x18(r25)", just after the ClipInfo::Init call. Only
// r3 (reloaded from r31), r4 and r0 are read after it; the prologue saved LR.
asmFunc ClipShadowInitHook() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq done;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r4, r9;
        bl ClipShadowInit;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        done :;
        lwz r4, 0x18(r25);
        blr;)
}
kmCall(0x80787b58, ClipShadowInitHook);

// ClipInfoMgr::CreateInstance+0x48 replaces "mulli r3, r0, 0x24" (r0 = 0x200, stored next): the pool
// doubles in a widened race, and only the first half is constructed. r12 is free here.
asmFunc ClipPoolSize() {
    ASM(
        nofralloc;
        mulli r3, r0, 0x24;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        slwi r3, r3, 1;
        blr;)
}
kmCall(0x807874b4, ClipPoolSize);

// CreateInstance+0x7C "li r3, 0x190" and +0x9C "li r7, 4": 4 ClipScreenInfos (0x60 each, a 0x10
// array header), 8 (kMaxLocal) in a widened race, so Update's own first loop fills one per screen.
// r0 holds a value stored after the first; r12 is free at both.
asmFunc ClipScreensSize() {
    ASM(
        nofralloc;
        li r3, 0x190;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        li r3, 0x310;
        blr;)
}
kmCall(0x807874e8, ClipScreensSize);

asmFunc ClipScreensCount() {
    ASM(
        nofralloc;
        li r7, 4;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        li r7, 8;
        blr;)
}
kmCall(0x80787508, ClipScreensCount);

// The leaf 0x8055d2f4 reads a screen's distance as "add r6, r6, r0; lfs f1, 0xc(r6)" (r6 = the
// ClipInfo, r0 = screen r4 * 4) on each of its four paths. No stack frame, so branch out and back
// over both; r7 is never read in the leaf. For screens 4+ the shadow's is at r6 + 0x47F0
// (kClipShadow - 4 * 4) + 0xC.
asmFunc LeafDistanceA() {
    ASM(
        nofralloc;
        add r6, r6, r0;
        cmplwi r4, 4;
        blt load;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq load;
        addi r6, r6, 0x47f0;
        load :;
        lfs f1, 0xc(r6);
        blr;)
}
kmBranch(0x8055d364, LeafDistanceA);
kmPatchExitPoint(LeafDistanceA, 0x8055d36c);

asmFunc LeafDistanceB() {
    ASM(
        nofralloc;
        add r6, r6, r0;
        cmplwi r4, 4;
        blt load;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq load;
        addi r6, r6, 0x47f0;
        load :;
        lfs f1, 0xc(r6);
        blr;)
}
kmBranch(0x8055d394, LeafDistanceB);
kmPatchExitPoint(LeafDistanceB, 0x8055d39c);

asmFunc LeafDistanceC() {
    ASM(
        nofralloc;
        add r6, r6, r0;
        cmplwi r4, 4;
        blt load;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq load;
        addi r6, r6, 0x47f0;
        load :;
        lfs f1, 0xc(r6);
        blr;)
}
kmBranch(0x8055d3bc, LeafDistanceC);
kmPatchExitPoint(LeafDistanceC, 0x8055d3c4);

asmFunc LeafDistanceD() {
    ASM(
        nofralloc;
        add r6, r6, r0;
        cmplwi r4, 4;
        blt load;
        lis r7, raceScreenCount @ha;
        lbz r7, raceScreenCount @l(r7);
        cmpwi r7, 0;
        beq load;
        addi r6, r6, 0x47f0;
        load :;
        lfs f1, 0xc(r6);
        blr;)
}
kmBranch(0x8055d3e8, LeafDistanceD);
kmPatchExitPoint(LeafDistanceD, 0x8055d3f0);

// ModelDirector::SetDisableDrawScnOptionsFromClipInfo+0x54 replaces "lbz r0, 0x20(r5)" (r5 =
// ClipInfo + screen r4): bit 0 set hides the model on that screen. 0x481C is kClipShadow + 0x20 - 4.
// The next instruction sets CR0 from r0; r12 is reloaded before it is read.
asmFunc ClipByteForScreen() {
    ASM(
        nofralloc;
        cmplwi r4, 4;
        blt game;
        li r0, 0;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lbz r0, 0x481c(r5);
        blr;
        game :;
        lbz r0, 0x20(r5);
        blr;)
}
kmCall(0x8055d5a8, ClipByteForScreen);

// SetAllScnOptionsFromClipInfo decides "hidden on every screen" (then disables the model outright)
// from screens 0-3. In a widened race it takes every screen; the shadow's bytes past the race's count
// read culled, and the hidden mask's bits past the static count are set by ModelDirector::__ct(ClipInfo).
// +0x48 replaces "lwz r0, 0x20(r4)" (r4 = the ClipInfo; "andc. r0, 0x01010101, r0" follows).
asmFunc SetAllClipWord() {
    ASM(
        nofralloc;
        lwz r0, 0x20(r4);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lwz r12, 0x4820(r4);
        and r0, r0, r12;
        blr;)
}
kmCall(0x8055d490, SetAllClipWord);

// Effects::Player::UpdateValues+0x40 replaces "lwz r0, 0x20(r3)" (r3 = the kart's ClipInfo) ahead of
// "andc 0x01010101": +4 = culled on every screen, which skips the kart's item effects (UpdateItemEffect:
// star, thunder, POW). In a widened race it takes screens 4-7 as SetAllClipWord does. r12 is free.
asmFunc PlayerEffectsClipWord() {
    ASM(
        nofralloc;
        lwz r0, 0x20(r3);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lwz r12, 0x4820(r3);
        and r0, r0, r12;
        blr;)
}
kmCall(0x80693ed4, PlayerEffectsClipWord);

// Item::ObjHolder::UpdateModelPositions+0x38 replaces "lwz r0, 0x20(r4)" (r4 = the item's ClipInfo) ahead
// of "andc. 0x01010101": an item culled on every screen gets the light update (vtable +0x14) instead of
// its model placement (+0x10), so a Thundercloud over a kart seen only on tiles 5-8 never hovers there
// (PD8). In a widened race it takes screens 4-7 too. r12 is reloaded before it is read.
asmFunc ItemModelClipWord() {
    ASM(
        nofralloc;
        lwz r0, 0x20(r4);
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        lwz r12, 0x4820(r4);
        and r0, r0, r12;
        blr;)
}
kmCall(0x80796b68, ItemModelClipWord);

// +0x6C replaces "clrlwi. r0, r0, 28" (r0 = ~hidden mask): any screen not hidden.
asmFunc SetAllHiddenMask() {
    ASM(
        nofralloc;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        bne wide;
        clrlwi.r0, r0, 28;
        blr;
        wide :;
        clrlwi.r0, r0, 24;
        blr;)
}
kmCall(0x8055d4b4, SetAllHiddenMask);

// +0xD8 "ori r0, r0, 0x800f" and +0xE8 "rlwinm r3, r0, 0, 17, 27" set and clear bitfield2's
// per-screen bits along with bit 15; screens 4-7 are bits 4-7.
asmFunc SetAllScreenBitsOn() {
    ASM(
        nofralloc;
        ori r0, r0, 0x800f;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        ori r0, r0, 0xf0;
        blr;)
}
kmCall(0x8055d520, SetAllScreenBitsOn);

asmFunc SetAllScreenBitsOff() {
    ASM(
        nofralloc;
        rlwinm r3, r0, 0, 17, 27;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        rlwinm r3, r0, 0, 17, 23;
        blr;)
}
kmCall(0x8055d530, SetAllScreenBitsOff);

// The item-box (FIB) pass 0x807a7d90, called per drawn screen r4 from ScnMgrRace::Draw: +0x64
// replaces "lbz r0, 0x20(r3)" (r3 = ClipInfo + r4) and +0x7C "lfs f31, 0xc(r3)" (r3 = ClipInfo +
// r4 * 4). The prologue saved LR and f31; CR0 is set next from r0 or by fcmpo.
asmFunc FibClipByte() {
    ASM(
        nofralloc;
        cmplwi r4, 4;
        blt game;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq game;
        lbz r0, 0x481c(r3);
        blr;
        game :;
        lbz r0, 0x20(r3);
        blr;)
}
kmCall(0x807a7df4, FibClipByte);

asmFunc FibClipDistance() {
    ASM(
        nofralloc;
        cmplwi r4, 4;
        blt game;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq game;
        lfs f31, 0x47fc(r3);
        blr;
        game :;
        lfs f31, 0xc(r3);
        blr;)
}
kmCall(0x807a7e0c, FibClipDistance);

// The GX pass 0x8087a718 (drawn screen r22, from the renderer's GameScreen) tests each object's
// ClipInfo at +0x698, "lbz r0, 0x20(r3)" with r3 = ClipInfo + r22. The prologue saved LR; the next
// instruction sets CR0.
asmFunc GxPassClipByte() {
    ASM(
        nofralloc;
        cmplwi r22, 4;
        blt game;
        lis r12, raceScreenCount @ha;
        lbz r12, raceScreenCount @l(r12);
        cmpwi r12, 0;
        beq game;
        lbz r0, 0x481c(r3);
        blr;
        game :;
        lbz r0, 0x20(r3);
        blr;)
}
kmCall(0x8087adb0, GxPassClipByte);

// Objects::HighwayCar::__ct+0xCC replaces "stb r0, 0x15a(r26)" (r0 = the static screen count). Its
// one reader 0x806d9fcc keeps per-screen distances for its horn sounds in a 4-slot stack array, so
// spare tiles take none: the count stays at the game's 4. The prologue saved LR; CR0 is reset next.
asmFunc HighwayCarScreens() {
    ASM(
        nofralloc;
        cmplwi r0, 4;
        ble store;
        li r0, 4;
        store :;
        stb r0, 0x15a(r26);
        blr;)
}
kmCall(0x806d5fb0, HighwayCarScreens);

}  // namespace SplitScreen8
