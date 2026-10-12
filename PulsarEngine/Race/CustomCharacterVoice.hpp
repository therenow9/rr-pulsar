#ifndef _CUSTOMCHARACTERVOICE_
#define _CUSTOMCHARACTERVOICE_

#include <core/nw4r/snd/SoundArchiveLoader.hpp>
#include <core/nw4r/snd/SoundHandle.hpp>

namespace Pulsar {
namespace Race {

void PatchLoadedCustomVoiceGroup(nw4r::snd::detail::SoundArchiveLoader *loader, u32 groupId, nw4r::snd::SoundMemoryAllocatable *allocater, void *groupData, void *waveData);
// SplitScreen8: not static, for its menu-script log of each racer's voice.
s8 GetPlayerVoiceAlias(u32 playerId);
// SplitScreen8: not static, for P5-8's confirm voice; -1 is no line to play (silent, or the skin's own
// trick line already started into handle).
s32 GetMenuVoiceCharacter(s32 character, u32 hud, nw4r::snd::SoundHandle &handle);

}  // namespace Race
}  // namespace Pulsar

#endif
