#ifndef _SPLITSCREEN8_
#define _SPLITSCREEN8_
#include <MarioKartWii/System/Identifiers.hpp>

namespace SplitScreen8 {

// Local players, screens and hud slots SplitScreen8 supports.
const int kMaxLocal = 8;
// Width of the game's own per-player and per-screen arrays: indices below it stay on game storage.
const int kGameLocal = 4;

#ifdef SS8_DEBUG_BOOT
// Called from Pulsar's BootIntoSection with its section; returns the section the game boots into.
SectionId DebugBootPrepare(SectionId section);
#endif

}  // namespace SplitScreen8

#endif
