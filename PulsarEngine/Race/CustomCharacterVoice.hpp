#ifndef _CUSTOMCHARACTERVOICE_
#define _CUSTOMCHARACTERVOICE_

#include <core/nw4r/snd/SoundArchiveLoader.hpp>

namespace Pulsar {
namespace Race {

void PatchLoadedCustomVoiceGroup(nw4r::snd::detail::SoundArchiveLoader *loader, u32 groupId, nw4r::snd::SoundMemoryAllocatable *allocater, void *groupData, void *waveData);
// SplitScreen8: not static, for its menu-script log of each racer's voice.
s8 GetPlayerVoiceAlias(u32 playerId);

}  // namespace Race
}  // namespace Pulsar

#endif
