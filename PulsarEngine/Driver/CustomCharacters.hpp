#ifndef _CUSTOMCHARACTERS_
#define _CUSTOMCHARACTERS_

#include <kamek.hpp>
#include <MarioKartWii/System/Identifiers.hpp>

namespace Pulsar {
namespace Driver {

enum {
    CHARACTER_COUNT = 0x18,
    MAX_CUSTOM_CHARACTER_SLOTS = 100,
    LOCAL_PLAYER_SKIN_COUNT = 4
};

extern bool characterTables[CHARACTER_COUNT][MAX_CUSTOM_CHARACTER_SLOTS + 1];
extern u8 selectedSlots[CHARACTER_COUNT];

void CreateCharacterTable();
bool LoadDriverBRRES(CharacterId character, u32 slot);

// LocalPlayerSkins.cpp: each local player's own skin in offline multiplayer, and the preview models
// that show two players on one character with different skins.
u32 GetLocalPlayerSlot(u32 hud, CharacterId character);
void SetLocalPlayerSlot(u32 hud, CharacterId character, u32 slot);
bool HasOwnLocalPlayerSlots();
u32 GetSharedModelSlot(CharacterId character, bool focused, u32 &standingHud);
u32 GetRequestedModelSlot(u32 hud, CharacterId character);
bool LoadLocalPlayerModel(u32 hud, CharacterId character, u32 slot);
void DestroyLocalPlayerPreviews();

}  // namespace Driver
}  // namespace Pulsar

#endif
