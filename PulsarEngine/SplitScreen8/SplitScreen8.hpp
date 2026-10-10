#ifndef _SPLITSCREEN8_
#define _SPLITSCREEN8_
#include <MarioKartWii/System/Identifiers.hpp>

class Page;
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

// Local players of a widened race or of any race with more than 4, 0 otherwise; the scenario's own
// count stops at 4 (D25, Screens.cpp).
extern u8 raceLocalCount;
// Real controller holder of controller id 0..kMaxLocal-1, ids 4+ past the manager (D26, Input.cpp).
Input::RealControllerHolder &Holder(Input::Manager &input, u32 id);
// Local player 0..kMaxLocal-1's holder in the menus (Input.cpp), and whether an Action
// (Manipulator.hpp) is newly pressed on a holder this frame.
Input::RealControllerHolder *PlayerHolder(u32 player);
bool UIPressed(const Input::RealControllerHolder &holder, u32 action);
// Local players of the 5-8 player game the menus are setting up, 0 outside one; SectionParams' own
// count stays 4 in it (D60, Entry.cpp).
extern u8 menuLocalCount;
// P5-8's character and kart, read at InitRace in place of D62's default once picked; cleared at the
// main menu (Entry.cpp). SectionParams' own picks are 4 wide.
struct ExtPick {
    CharacterId character;
    KartId kart;
    bool picked;
};
extern ExtPick extPicks[kMaxLocal - kGameLocal];
// Player 5..kMaxLocal's character: their pick, else D62's default, character id = player (Entry.cpp).
CharacterId ExtCharacter(u32 player);
// The join page of a 5-8 player game (D64, Join.cpp), built by Pulsar's ExpSection in that game's section 0x54.
Page *NewJoinPage();
// P5-8's join state cleared and holders 4-7 put back on the dummy controller (Join.cpp).
void ClearExtPads();
#ifdef SS8_PAGES
// The character, kart or drift page of a 5-8 player game (D67, Select.cpp), by its PulPageId.
Page *NewSelectPage(u32 id);
// The character page (CharSelect.cpp).
Page *NewCharSelectPage();
// The kart if it is one of the character's 12, else the Standard Kart of the character's weight class (Select.cpp).
KartId KartForCharacter(CharacterId character, KartId kart);
#endif
// The colours of a local's hud slot 4..raceLocalCount-1 in a widened race; false for any other slot,
// which keeps RR's own (Colours.cpp, called from RR's UIColor.cpp).
bool HudSlotColour(u8 hud, RGBA16 *primary, RGBA16 *secondary);
// Player slot 4..kMaxLocal-1's colour pair, the owner's purple, white, brown and teal (Colours.cpp).
void SlotPalette(u32 slot, RGBA16 *primary, RGBA16 *secondary);

#ifdef SS8_DEBUG_BOOT
// Called from Pulsar's BootIntoSection with its section; returns the section the game boots into.
SectionId DebugBootPrepare(SectionId section);
#endif

}  // namespace SplitScreen8

#endif
