#ifdef SS8_DEBUG_SCREENS
#include <kamek.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Game objects that keep one entry per local player, 4 wide, reached by players 5-8 just by racing.
// docs/plans/m2-engine-widening.md, Phase C, has the site list.

namespace SplitScreen8 {

// The AI object at AI::Manager+0x84 lists CPU karts at +0xE8 (12 of 8 bytes) and human karts at
// +0x148 (4), followed by a pointer at +0x168 and the CPU count at +0x178: a 5th human would
// overwrite both. Its readers loop to the human count at +0x17C, so humans past the 4th are left out: the
// CPUs track players 1-4 only.
typedef void (*RegisterKartFn)(u8 *ai, void *kartAI);
typedef bool (*IsCPUFn)(const void *kartAI);
static const RegisterKartFn registerKart = reinterpret_cast<RegisterKartFn>(0x807414b8);
static const IsCPUFn isCPU = reinterpret_cast<IsCPUFn>(0x8072624c);

static void RegisterKartWithAI(u8 *ai, void *kartAI) {
    const u32 humans = *reinterpret_cast<const u32 *>(ai + 0x17c);
    if (raceLocalCount > kGameLocal && humans >= kGameLocal && !isCPU(kartAI)) return;
    registerKart(ai, kartAI);
}
kmCall(0x807392bc, RegisterKartWithAI);  // AI::Manager::AddKartAIController+0x90

}  // namespace SplitScreen8
#endif
