#ifndef _CUSTOMCHARACTERVOICE_
#define _CUSTOMCHARACTERVOICE_

#include <core/nw4r/snd/SoundArchiveLoader.hpp>
#include <core/nw4r/snd/SoundHandle.hpp>

namespace Pulsar {
namespace Race {

void PatchLoadedCustomVoiceGroup(nw4r::snd::detail::SoundArchiveLoader *loader, u32 groupId, nw4r::snd::SoundMemoryAllocatable *allocater, void *groupData, void *waveData);
// SplitScreen8's menu-script log and P5-8's confirm voice (-1: nothing left to play).
s8 GetPlayerVoiceAlias(u32 playerId);
s32 GetMenuVoiceCharacter(s32 character, u32 hud, nw4r::snd::SoundHandle &handle);

}  // namespace Race
}  // namespace Pulsar

#endif
