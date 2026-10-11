#include <kamek.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/RKNet/RKNetController.hpp>
#include <MarioKartWii/UI/Ctrl/UIControl.hpp>
#include <MarioKartWii/UI/Page/Menu/CharacterSelect.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>

// Same-character select with 2-4 local players offline, ported from mkw-sp PR #452 (LICENSE_mkw-sp).
// Every multiplayer driver button loads the way the Mii button does: one OK marker per player and
// not exclusive to one player. The controls come from scripts/gen_charselect_multi.py. Online rooms
// and 1P keep the game's own buttons.

namespace Pulsar {
namespace UI {

// The generated controls' OK markers each take half the button with 2 players and a quarter with 3-4.
static const char multiButtonCtrName2[] = "CharacterSelect%d_%d_Multi2";
static const char multiButtonCtrName4[] = "CharacterSelect%d_%d_Multi4";

// Set by LoadButton's first multiplayer hook and read by the hooks after it and OnUpdate's, which run
// where a call would clobber live registers. An online character select is built before any room
// exists, so the section decides, as CharacterSelect::OnInit's own IsOnlineSection test does.
static bool sameCharacterOffline;

static bool UpdateSameCharacterOffline() {
    const SectionMgr *sectionMgr = SectionMgr::sInstance;
    const RKNet::Controller *controller = RKNet::Controller::sInstance;
    const bool onlineSection = sectionMgr != nullptr && sectionMgr->curSection != nullptr && IsOnlineSection(sectionMgr->curSection->sectionId);
    sameCharacterOffline = !onlineSection && (controller == nullptr || controller->roomType == RKNet::ROOMTYPE_NONE);
    return sameCharacterOffline;
}

// CtrlMenuCharacterSelect::LoadButton+0x5C, "cmpwi r28, 0" (isMii), reached only with 2+ players.
// Offline, CR0 is set from "r29 (the player count) != 1" instead, so the bne after takes the Mii
// button's path for every button. Volatile registers are dead here and LoadButton saved LR.
static asmFunc LoadAsMultiButtonIfOffline() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        bl UpdateSameCharacterOffline;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        cmpwi r3, 0;
        beq online;
        cmpwi r29, 1;
        blr;
    online:
        cmpwi r28, 0;
        blr;)
}
kmCall(0x807e2984, LoadAsMultiButtonIfOffline);

// LoadButton+0x98, "addi r5, r30, 0x80": the Mii path's "CharacterSelect%d_%d_Mii". Offline, use the
// _Multi controls, which exist for every column. r29 is the player count (0x807E2954); nothing reads
// CR0 before the snprintf.
static asmFunc LoadMultiButtonCtrName() {
    ASM(
        nofralloc;
        lis r5, sameCharacterOffline @ha;
        lbz r5, sameCharacterOffline @l(r5);
        cmpwi r5, 0;
        beq online;
        cmpwi r29, 2;
        lis r5, multiButtonCtrName2 @ha;
        addi r5, r5, multiButtonCtrName2 @l;
        beqlr;
        lis r5, multiButtonCtrName4 @ha;
        addi r5, r5, multiButtonCtrName4 @l;
        blr;
    online:
        addi r5, r30, 0x80;
        blr;)
}
kmCall(0x807e29c0, LoadMultiButtonCtrName);

// LoadButton+0xD4, "srwi r8, r0, 5": r8 is PushButton::Load's "player exclusive" argument
// (r0 = cntlzw isMii). Offline in multiplayer no button is exclusive; 1P (r29 == 1) keeps it.
// r9 is set after this (0x807E2A04).
static asmFunc ClearExclusiveIfMulti() {
    ASM(
        nofralloc;
        srwi r8, r0, 5;
        cmpwi r29, 1;
        beqlr;
        lis r9, sameCharacterOffline @ha;
        lbz r9, sameCharacterOffline @l(r9);
        cmpwi r9, 0;
        beqlr;
        li r8, 0;
        blr;)
}
kmCall(0x807e29fc, ClearExclusiveIfMulti);

// LoadButton+0x110, "stb r28, 0x254(r4)": the byte normally means "this is the Mii button", and the
// control uses it to pick the per-player OK markers, so offline multiplayer sets it on every button.
// 1P and online keep isMii. CR0 is not read before LoadButton's next compare; r12 is free.
static asmFunc StoreIsMultiButton() {
    ASM(
        nofralloc;
        stb r28, 0x254(r4);
        cmpwi r29, 1;
        beqlr;
        lis r12, sameCharacterOffline @ha;
        lbz r12, sameCharacterOffline @l(r12);
        cmpwi r12, 0;
        beqlr;
        li r0, 1;
        stb r0, 0x254(r4);
        blr;)
}
kmCall(0x807e2a38, StoreIsMultiButton);

// Whether a player has confirmed a driver: OnButtonDriverClick turns that player's holder off
// (ControlsManipulatorManager::InitHolders 0x805F2100 writes the byte at +0xA4 + player * 0x5C).
static u32 HasConfirmedDriver(u32 player) {
    Pages::CharacterSelect *page = SectionMgr::sInstance->curSection->Get<Pages::CharacterSelect>();
    if (page == nullptr)
        return 1;
    const u8 *manager = reinterpret_cast<const u8 *>(&page->GetManipulatorManager());
    return manager[(player & 0xff) * 0x5c + 0xa4] == 0;
}

// CtrlMenuCharacterSelect::OnUpdate+0x70, "cmpwi r0, 1": r0 is player r27's preview model state, 1
// once confirmed. Two players on one driver can share that model, so offline in multiplayer (r28
// players) the player's own holder must also be off, or one confirm would show every OK marker on
// the button. The beq after reads CR0; OnUpdate reloads the volatile registers after it and saved LR.
static asmFunc OwnConfirmForMarker() {
    ASM(
        nofralloc;
        cmpwi r0, 1;
        bnelr;
        cmpwi r28, 1;
        beqlr;
        lis r12, sameCharacterOffline @ha;
        lbz r12, sameCharacterOffline @l(r12);
        cmpwi r12, 0;
        beq online;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r27;
        bl HasConfirmedDriver;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        cmpwi r3, 1;
        blr;
    online:
        cmpwi r0, 1;
        blr;)
}
kmCall(0x807e311c, OwnConfirmForMarker);

// OnButtonClick+0xA8 and OnUpdate+0x4AC set the OK message (0x9FB + hud, coloured for that player) on
// the whole button. On a shared button that recolours every player's marker, so offline in multiplayer
// only the confirming player's marker text is set, as OnMultiplayerMiiSelected does for the Mii button.
static void SetOwnOkMessage(LayoutUIControl *button, u32 bmgId, const Text::Info *info) {
    const u32 hud = bmgId - 0x9fb;
    if (hud < 4 && reinterpret_cast<const u8 *>(button)[0x254] != 0 && UpdateSameCharacterOffline()) {
        char textBox[0x10];
        snprintf(textBox, sizeof(textBox), "ok_text_%dp", hud + 1);
        button->SetTextBoxMessage(textBox, bmgId, info);
        return;
    }
    button->SetMessage(bmgId, info);
}
kmCall(0x807e36a4, SetOwnOkMessage);
kmCall(0x807e3558, SetOwnOkMessage);

}  // namespace UI
}  // namespace Pulsar
