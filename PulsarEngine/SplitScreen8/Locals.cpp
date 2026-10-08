#ifdef SS8_DEBUG_SCREENS
#include <kamek.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Game objects that keep one entry per local player, 4 wide, reached by players 5-8 just by racing.
// docs/plans/m2-engine-widening.md, Phase C, has the site list.

namespace SplitScreen8 {

// The AI object at AI::Manager+0x84 lists CPU karts at +0xE8 (12 of 8 bytes) and human karts at
// +0x148 (4), followed by a pointer at +0x168 and the CPU count at +0x178: a 5th human would
// overwrite both. Each human entry is the human's AI object, then the human ranked i-th (the sort's
// output). Humans past the 4th go to extHumans, and the human count (+0x17C) stays 4; the readers that
// loop over the humans or use the last-ranked one are extended to them (PC3).
struct Human {
    u8 *ai;
    u8 *ranked;
};
static Human extHumans[kMaxLocal - kGameLocal];
static u32 extHumanCount;

typedef void (*RegisterKartFn)(u8 *ai, void *kartAI);
typedef bool (*IsCPUFn)(const void *kartAI);
typedef void (*HumansFn)(u8 *ai);
typedef void (*KartCallFn)(void *link, u32 arg);
typedef u8 *(*HumanOfFn)(u8 *owner);
static const RegisterKartFn registerKart = reinterpret_cast<RegisterKartFn>(0x807414b8);
static const IsCPUFn isCPU = reinterpret_cast<IsCPUFn>(0x8072624c);
static const HumansFn sortHumans = reinterpret_cast<HumansFn>(0x807426e4);
static const HumansFn rubberBand = reinterpret_cast<HumansFn>(0x80742944);
static const KartCallFn humanKartCall = reinterpret_cast<KartCallFn>(0x80591898);

static u32 HumanCount(const u8 *ai) {
    return *reinterpret_cast<const u32 *>(ai + 0x17c);
}
static u8 *&HumanAI(u8 *ai, u32 i) {
    return i < kGameLocal ? *reinterpret_cast<u8 **>(ai + 0x148 + i * 8) : extHumans[i - kGameLocal].ai;
}
static u8 *&HumanRanked(u8 *ai, u32 i) {
    return i < kGameLocal ? *reinterpret_cast<u8 **>(ai + 0x14c + i * 8) : extHumans[i - kGameLocal].ranked;
}

// The first kart registered in a race finds both counts 0, which resets extHumans. A human past the
// 4th gets the entry the registration (0x807414B8) would make: its AI object, from the kart AI's
// +0x10, +0x144, through that object's vtable (+0x34) at +0x3C.
static void RegisterKartWithAI(u8 *ai, void *kartAI) {
    if (*reinterpret_cast<const u32 *>(ai + 0x178) == 0 && HumanCount(ai) == 0) extHumanCount = 0;
    if (raceLocalCount > kGameLocal && HumanCount(ai) >= kGameLocal && !isCPU(kartAI)) {
        if (extHumanCount >= kMaxLocal - kGameLocal) return;
        u8 *owner = *reinterpret_cast<u8 **>(*reinterpret_cast<u8 **>(static_cast<u8 *>(kartAI) + 0x10) + 0x144);
        const HumanOfFn humanOf = *reinterpret_cast<HumanOfFn *>(*reinterpret_cast<u8 **>(owner + 0x34) + 0x3c);
        extHumans[extHumanCount].ai = humanOf(owner);
        extHumans[extHumanCount].ranked = nullptr;
        ++extHumanCount;
        return;
    }
    registerKart(ai, kartAI);
}
kmCall(0x807392bc, RegisterKartWithAI);  // AI::Manager::AddKartAIController+0x90

// The two update loops (0x80741660, 0x807418E0) call each human's vtable +0x10 after the CPUs'.
static void UpdateExtHumans() {
    for (u32 i = 0; i < extHumanCount; ++i) {
        u8 *human = extHumans[i].ai;
        reinterpret_cast<void (*)(u8 *)>((*reinterpret_cast<void ***>(human))[0x10 / 4])(human);
    }
}
// 0x80741660+0x9C, "mr r3, r31", right after its human loop; nothing volatile is live there.
asmFunc HumansUpdatedA() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        bl UpdateExtHumans;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        mr r3, r31;
        blr;)
}
kmCall(0x807416fc, HumansUpdatedA);
// 0x807418E0+0x84, "lwz r0, 0x24(r1)", the epilogue after its human loop.
asmFunc HumansUpdatedB() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        bl UpdateExtHumans;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        lwz r0, 0x24(r1);
        blr;)
}
kmCall(0x80741964, HumansUpdatedB);

// 0x807426E4 writes the humans in race order (each human's +0x14, 1-12) into the entries' +0x14C,
// one entry per ranked human: 8 humans write past the 4th entry. In a widened race with extHumans the
// same runs over all of them, entries 4+ in extHumans.
static void SortHumansWide(u8 *ai) {
    if (raceScreenCount == 0 || extHumanCount == 0) {
        sortHumans(ai);
        return;
    }
    u8 *byRank[12] = {};
    const u32 total = HumanCount(ai) + extHumanCount;
    for (u32 i = 0; i < total; ++i) {
        u8 *human = HumanAI(ai, i);
        const s32 rank = *reinterpret_cast<const s32 *>(human + 0x14);
        if (rank >= 1 && rank <= 12) byRank[rank - 1] = human;
    }
    u32 n = 0;
    for (u32 r = 0; r < 12; ++r) {
        if (byRank[r] != nullptr) HumanRanked(ai, n++) = byRank[r];
    }
}
kmCall(0x80741708, SortHumansWide);

// 0x80742944 rubber-bands every CPU against the last-ranked human, entry (+0x17C - 1)'s +0x14C. With
// extHumans that entry is past the 4th, so it is lent to entry 3 for the call.
static void RubberBandWide(u8 *ai) {
    if (raceScreenCount == 0 || extHumanCount == 0) {
        rubberBand(ai);
        return;
    }
    u8 *&last = HumanRanked(ai, kGameLocal - 1);
    u8 *const own = last;
    last = HumanRanked(ai, HumanCount(ai) + extHumanCount - 1);
    rubberBand(ai);
    last = own;
}
kmCall(0x80741710, RubberBandWide);

// 0x807431BC+0xD0 calls Kart::Link 0x80591898 with 1 for each human's kart, (+4)'s first word.
static void KartCallExtHumans() {
    for (u32 i = 0; i < extHumanCount; ++i) {
        humanKartCall(**reinterpret_cast<void ***>(extHumans[i].ai + 4), 1);
    }
}
// Its loop exit, "b +0x16C" (0x807432B4), goes to the epilogue at 0x80743328, which reloads LR from the
// function's frame; this returns there.
asmFunc HumansKartCalled() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        bl KartCallExtHumans;
        addi r1, r1, 0x10;
        lis r12, 0x8074;
        ori r12, r12, 0x3328;
        mtlr r12;
        blr;)
}
kmCall(0x807432b4, HumansKartCalled);

}  // namespace SplitScreen8
#endif
