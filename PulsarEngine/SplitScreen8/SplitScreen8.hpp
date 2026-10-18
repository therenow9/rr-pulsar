#ifndef _SPLITSCREEN8_
#define _SPLITSCREEN8_
#include <MarioKartWii/System/Identifiers.hpp>

class Page;
class GameScene;
class LayoutUIControl;
class CtrlMenuCourseSelectCourse;
namespace EGG {
class Heap;
class ExpHeap;
}  // namespace EGG
namespace GX {
struct TexObj;
}  // namespace GX
namespace Input {
class Manager;
class Controller;
class RealControllerHolder;
}  // namespace Input
namespace Pages {
class ModelRenderer;
}  // namespace Pages

namespace SplitScreen8 {

const int kMaxLocal = 8;
// Width of the game's own per-player and per-screen arrays: indices below it stay on game storage.
const int kGameLocal = 4;

// Screens of the current race: 0 while it uses the game's own count, else 6 or 8.
extern u8 raceScreenCount;
// The 2D layout's width: 832 x 456 units at 16:9, 608 x 456 at 4:3, centred on 0, y up.
float LayoutWidth();
// A hud 4-7 of a widened race, which the game's 4-wide per-hud state cannot hold. Compared in the caller's type: a signed
// hud of -1 is not wide, an unsigned one is.
template <typename Hud>
inline bool IsWideHud(Hud hud) {
    return raceScreenCount != 0 && hud >= kGameLocal;
}

// Game calls and statics used in more than one file, whose GameSource declarations are missing, private or mistyped.
typedef void (*UpdateStatesClassicFn)(u8 *controller, u8 *status, void *state, void *uiState);
typedef u32 (*CalculateIDFn)(const Input::RealControllerHolder *);
typedef void (*ControlFn)(LayoutUIControl *);
typedef void (*DummyBackLoadFn)(LayoutUIControl *, const char *, const char *, const char *, u32);
typedef void (*ShowModelFn)(Pages::ModelRenderer *, u32, bool, u32);
typedef GX::TexObj *(*ModelTexObjFn)(Pages::ModelRenderer *, u32);
static const UpdateStatesClassicFn updateStatesClassic = reinterpret_cast<UpdateStatesClassicFn>(0x8051f410);
static const CalculateIDFn calculateID = reinterpret_cast<CalculateIDFn>(0x8061be40);
static const ControlFn layoutCtor = reinterpret_cast<ControlFn>(0x8063d798);
static const DummyBackLoadFn dummyBackLoad = reinterpret_cast<DummyBackLoadFn>(0x807e9620);
// ModelRenderer's per-hud show and texture, as the model controls call them each frame; the show ends in
// MenuDriverModelMgr::TogglePlayerModel, which writes players[hud] (4 wide), so huds 0-3 only.
static const ShowModelFn showModel = reinterpret_cast<ShowModelFn>(0x805f59ac);
static const ModelTexObjFn modelTexObj = reinterpret_cast<ModelTexObjFn>(0x805f5a4c);
static u8 *const *const driverSoundManager = reinterpret_cast<u8 *const *>(0x809c4740);
static u8 *const *const miiManager = reinterpret_cast<u8 *const *>(0x809bd710);  // MiiManager::sInstance, private

// Local players of a widened race or of any race with more than 4, else 0; the scenario's own count stops at 4.
extern u8 raceLocalCount;
// A race's local players for a loop over them: the scenario's count, which stops at 4, unless the race has more on
// its own screens; a one-screen scene (Next Race's) gives locals 4-7 no hud, and their player id reads past hudPlayerIds.
inline u8 RaceLocals(u8 scenarioLocals) {
    return raceScreenCount != 0 && raceLocalCount != 0 ? raceLocalCount : scenarioLocals;
}
// A race hud's player id: huds 0-3 read hudPlayerIds as RR does, 4-7 go through GetPlayerIdOfLocalPlayer and its hooks.
u8 HudRacePlayer(u32 hud);
// Real controller holder of controller id 0..7; ids 4+ live past the manager's own holders.
Input::RealControllerHolder &Holder(Input::Manager &input, u32 id);
// A local player's holder in the menus, and whether an Action is newly pressed on a holder this frame.
Input::RealControllerHolder *PlayerHolder(u32 player);
bool UIPressed(const Input::RealControllerHolder &holder, u32 action);
// Local players of the 5-8 player game the menus are setting up, else 0; SectionParams' own count stays 4 in it.
extern u8 menuLocalCount;
// Whether a 5-8 player game leaves a track's variant out, by PulsarId; and, on the course or variant page, its button
// greyed and unselectable.
bool TrackOffForLocals(u32 track, u32 variant);
bool OffCourseButton(CtrlMenuCourseSelectCourse &course, u32 row, u32 track, s32 variant);
// The race scene's MEM2 heap in a 5-8 player race while it has room bytes free, else null: the race's MEM1 cannot also hold
// that race's layouts and items, unless a big track's archive, which sits on MEM2, leaves MEM2 less.
EGG::ExpHeap *RaceMem2(u32 room);
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
// P5-8's preview models out of the ScnMgr before RR's ResetScnMgr resets it.
void ResetExtPreviews();
// A menu skin model's load reserve: RR's 0xE1000 outside a 5-8 player game, else sized to what a model keeps.
u32 SkinLoadReserve(u32 reserve);
// The join page of a 5-8 player game, built by Pulsar's ExpSection in that game's section 0x54.
Page *NewJoinPage();
// P5-8's pads, as SectionPad's PadInfo holds P2-4's: the id bound at join (0 for none), whether the slot awaits an A, and
// the controller its holder gave up at the main menu, which the next 5-8 player join page gives back.
struct ExtPad {
    u32 id;
    bool waiting;
    Input::Controller *controller;
};
extern ExtPad extPads[kMaxLocal - kGameLocal];
// Puts holders 4-7 back on the dummy controller, each joined P5-8 keeping its pad for the next 5-8 player game.
void ClearExtPads();
// Binds a controller to a holder as TrySetController does (0x80523FCC).
void BindPad(Input::RealControllerHolder &holder, Input::Controller *controller);
// The character, kart or drift page of a 5-8 player game, by its PulPageId.
Page *NewSelectPage(u32 id);
Page *NewCharSelectPage();
// The kart if it is one of the character's 12, else the Standard Kart of the character's weight class.
KartId KartForCharacter(CharacterId character, KartId kart);
// RR's Randomize Combo roulette (ExpVR::randomDuration, ExpCharacterSelect::BeforeControlUpdate): 60 frames, a
// hop on each frame whose count left is 1 modulo 4 (the last onto the roll), every player confirming at 0.
const s32 kRollFrames = 60;
const s32 kRollHop = 4;
// Set by a roll on the character page, so the kart page opens on the karts' roulette.
extern bool kartRoulette;
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
// Shared by the character and kart pages.
void MenuPlayerColours(u32 player, RGBA16 *primary, RGBA16 *secondary);
void LoadNamePlate(LayoutUIControl &plate, u32 player, float x, float y, float scale);
void LoadModelPreview(LayoutUIControl &preview, float x, float y, float scale);
void SetPreviewTexture(LayoutUIControl &preview, const GX::TexObj *texObj);
void ShowSkinToggleIcons(LayoutUIControl &plate, ControllerType type);
// PushButton's animation names (0x808B7690, UpDownButton's the same), so a loaded control keeps its group ids.
extern const char *kButtonAnims[];
enum ButtonGroup {
    GROUP_LOOP,
    GROUP_SELECT,
    GROUP_SELECT_IN,
    GROUP_OK
};
enum SelectAnim {
    ANIM_FREE,
    ANIM_FREE_TO_SELECT,
    ANIM_SELECT,
    ANIM_SELECT_TO_FREE
};
const u32 kAnimStop = 1;  // SelectIn's SelectStop and OK's OKStop
void StartAnimations(LayoutUIControl &control, bool selected);
void SelectControl(LayoutUIControl &control, bool select);
// 0x80868ABC's voice types: the kart page's, and the team page's for Blue and Red (0x80868A3C).
const u32 kVoiceKart = 1;
const u32 kVoiceTeamBlue = 2;
const u32 kVoiceTeamRed = 3;
void ExtMenuVoice(u32 player, CharacterId character, u32 type);
void UpdateDriverPreviews(LayoutUIControl *const *previews);
bool MenuTeamVS();
// Calls a game fill that counts SectionParams' locals (4 in a 5-8 player game) with the true count.
void WithMenuLocalCount(void (*fill)());
// The holder menu input slot 4 answers, null for none; Pause.cpp's proxy, cleared at every section load.
void SetMenuSlotProxy(Input::RealControllerHolder *holder);
#ifdef SS8_MENU_SCRIPT
// A menu script's "open-mii-c" line: Mii C open on the 8-player page whatever the license's VR.
extern bool menuOpenMiiC;
#endif
// The colours of a local's hud slot 4..raceLocalCount-1 in a widened race; false for any other slot, which keeps RR's.
bool HudSlotColour(u8 hud, RGBA16 *primary, RGBA16 *secondary);
// Player slot 4-7's colour pair; slots 0-3 are out of range.
void SlotPalette(u32 slot, RGBA16 *primary, RGBA16 *secondary);

#if defined(SS8_DEBUG_BOOT) || defined(SS8_MENU_SCRIPT)
// Every local's kart to the AI, a second into a race: --boot-autodrive and a menu script's "autodrive" line.
void AutodriveLocals();
#endif
#ifdef SS8_DEBUG_BOOT
// Called from Pulsar's BootIntoSection with its section; returns the section the game boots into.
SectionId DebugBootPrepare(SectionId section);
#endif

}  // namespace SplitScreen8

#endif
