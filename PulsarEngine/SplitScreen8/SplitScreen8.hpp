#ifndef _SPLITSCREEN8_
#define _SPLITSCREEN8_
#include <MarioKartWii/System/Identifiers.hpp>

namespace Input {
class Manager;
class RealControllerHolder;
}  // namespace Input

namespace SplitScreen8 {

// Local players, screens and hud slots SplitScreen8 supports.
const int kMaxLocal = 8;
// Width of the game's own per-player and per-screen arrays: indices below it stay on game storage.
const int kGameLocal = 4;

// Screens of the current race: 0 while it uses the game's own count, else 6 or 8 (Screens.cpp).
extern u8 raceScreenCount;

#ifdef SS8_DEBUG_SCREENS
// Local players of a widened race, 0 otherwise; the scenario's own count stops at 4 (D25, Screens.cpp).
extern u8 raceLocalCount;
// Real controller holder of controller id 0..kMaxLocal-1, ids 4+ past the manager (D26, Input.cpp).
Input::RealControllerHolder &Holder(Input::Manager &input, u32 id);
#endif

#ifdef SS8_DEBUG_BOOT
// Called from Pulsar's BootIntoSection with its section; returns the section the game boots into.
SectionId DebugBootPrepare(SectionId section);
#endif

}  // namespace SplitScreen8

#endif
