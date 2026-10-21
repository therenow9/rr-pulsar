#ifndef _RACECUSTOMCHARACTERS_
#define _RACECUSTOMCHARACTERS_

#include <kamek.hpp>
#include <MarioKartWii/System/Identifiers.hpp>
#include <core/rvl/tpl.hpp>

class RacedataScenario;
namespace nw4r {
namespace lyt {
class Pane;
}
}  // namespace nw4r

namespace Pulsar {
namespace Race {

extern u8 racePlayerSlots[12];

void RandomizeCPUCharacterTables(const RacedataScenario &scenario);
u32 GetPlayerCustomCharacterSlot(u32 playerId, CharacterId character, bool isAward = false);
TPLPalettePtr LoadCustomCharacterIconFile(CharacterId character, u32 slot, bool minimap = false);
void LoadCustomCharacterIcon(CharacterId character, u32 slot, nw4r::lyt::Pane *pane, nw4r::lyt::Pane *shadow0, nw4r::lyt::Pane *shadow1 = nullptr, bool minimap = false);

}  // namespace Race
}  // namespace Pulsar

#endif
