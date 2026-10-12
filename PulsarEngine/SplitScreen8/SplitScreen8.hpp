#ifndef _SPLITSCREEN8_
#define _SPLITSCREEN8_
#include <MarioKartWii/System/Identifiers.hpp>

class Page;
class GameScene;
class LayoutUIControl;
namespace EGG {
class Heap;
class ExpHeap;
}  // namespace EGG
namespace GX {
struct TexObj;
}  // namespace GX
namespace Input {
class Manager;
class RealControllerHolder;
}  // namespace Input

namespace SplitScreen8 {

const int kMaxLocal = 8;
// Width of the game's own per-player and per-screen arrays: indices below it stay on game storage.
const int kGameLocal = 4;

// Screens of the current race: 0 while it uses the game's own count, else 6 or 8.
extern u8 raceScreenCount;

// Local players of a widened race or of any race with more than 4, else 0; the scenario's own count stops at 4.
extern u8 raceLocalCount;
// Real controller holder of controller id 0..7; ids 4+ live past the manager's own holders.
Input::RealControllerHolder &Holder(Input::Manager &input, u32 id);
// A local player's holder in the menus, and whether an Action is newly pressed on a holder this frame.
Input::RealControllerHolder *PlayerHolder(u32 player);
bool UIPressed(const Input::RealControllerHolder &holder, u32 action);
// Local players of the 5-8 player game the menus are setting up, else 0; SectionParams' own count stays 4 in it.
extern u8 menuLocalCount;
// P5-8's character and kart, read at InitRace once picked and cleared at the main menu; SectionParams' picks are 4 wide.
struct ExtPick {
    CharacterId character;
    KartId kart;
    bool picked;
};
extern ExtPick extPicks[kMaxLocal - kGameLocal];
// Player 5..8's character: their pick, else character id = player.
CharacterId ExtCharacter(u32 player);
// Hud 0-7's skin slot for a character in RR's selectedSlots; 0 for a Mii.
u32 SkinSlot(u32 hud, u32 character);
void SetSkinSlot(u32 hud, u32 character, u32 slot);
// P5-8's preview models out of the ScnMgr before RR's ResetScnMgr resets it; empty outside --ss8-pages builds.
void ResetExtPreviews();
// A menu skin model's load reserve: RR's 0xE1000 outside a 5-8 player game, else sized to what a model keeps.
u32 SkinLoadReserve(u32 reserve);
// The join page of a 5-8 player game, built by Pulsar's ExpSection in that game's section 0x54.
Page *NewJoinPage();
// Clears P5-8's join state and puts holders 4-7 back on the dummy controller.
void ClearExtPads();
#ifdef SS8_PAGES
// The character, kart or drift page of a 5-8 player game, by its PulPageId.
Page *NewSelectPage(u32 id);
Page *NewCharSelectPage();
// The kart if it is one of the character's 12, else the Standard Kart of the character's weight class.
KartId KartForCharacter(CharacterId character, KartId kart);
// P5-8's previews: Show builds the textures (null until then) and gives the model pass loop `count` passes until
// Hide; Stock says the player's model fell back to the character's stock one.
bool ExtPreviewsShow(u32 count);
void ExtPreviewsHide();
bool ExtPreviewLoad(u32 player, CharacterId character, u32 slot);
void ExtPreviewDrop(u32 player);
void ExtPreviewPose(u32 player, bool confirmed);
bool ExtPreviewIdle(u32 player);
bool ExtPreviewStock(u32 player, CharacterId character);
GX::TexObj *ExtPreviewTexObj(u32 player);
// P5-8's kart previews on the kart page, each player's driver on the kart wanted for them.
bool ExtKartsShow(u32 count);
void ExtKartsHide();
void ExtKartRequest(u32 player, CharacterId character, u32 slot, KartId kart);
// MenuDriverModel's confirmed-kart pose, unnamed in the header (set at 0x8084A288): MenuKartModel::Update
// stops turning the driver's kart in it (0x80832814).
const u32 kStateKartChosen = 3;
// A P5-8 driver's kart pose: kStateKartChosen once confirmed, else the turning one.
void ExtKartPose(u32 player, bool confirmed);
// Shared by the character and kart pages; SkinToggleStep is this frame's toggle press, -1, 0 or 1.
void MenuPlayerColours(u32 player, RGBA16 *primary, RGBA16 *secondary);
void LoadNamePlate(LayoutUIControl &plate, u32 player, float x, float y, float scale);
void LoadModelPreview(LayoutUIControl &preview, float x, float y, float scale);
void SetPreviewTexture(LayoutUIControl &preview, const GX::TexObj *texObj);
void ShowSkinToggleIcons(LayoutUIControl &plate, ControllerType type);
s32 SkinToggleStep(Input::RealControllerHolder &holder);
void ExtKartVoice(u32 player, CharacterId character);
// The holder menu input slot 4 answers, null for none; Pause.cpp's proxy, cleared at every section load.
void SetMenuSlotProxy(Input::RealControllerHolder *holder);
#endif
#ifdef SS8_MENU_SCRIPT
// A menu script's "open-mii-c" line: Mii C open on the 8-player page whatever the license's VR.
extern bool menuOpenMiiC;
#endif
// The colours of a local's hud slot 4..raceLocalCount-1 in a widened race; false for any other slot, which keeps RR's.
bool HudSlotColour(u8 hud, RGBA16 *primary, RGBA16 *secondary);
// Player slot 4-7's colour pair; slots 0-3 are out of range.
void SlotPalette(u32 slot, RGBA16 *primary, RGBA16 *secondary);

#ifdef SS8_DEBUG_BOOT
// Called from Pulsar's BootIntoSection with its section; returns the section the game boots into.
SectionId DebugBootPrepare(SectionId section);
#endif

}  // namespace SplitScreen8

#endif
