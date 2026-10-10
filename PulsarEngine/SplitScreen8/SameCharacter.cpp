#include <kamek.hpp>

// Same-character select for 2+ local players: every multiplayer driver button loads like a Mii
// button (per-player OK markers, not player-exclusive) from the generated _Multi controls.
// Port of mkw-sp PR #452; docs/m1-same-character.md has the design and the preview-model problem.

namespace SplitScreen8 {

static const char multiButtonCtrName[] = "CharacterSelect%d_%d_Multi";

// CtrlMenuCharacterSelect::LoadButton+0x5C: drop "cmpwi r28, 0" (isMii). The bne
// that follows then reuses the "player count != 1" result, so every multiplayer
// button takes the Mii control path.
kmWrite32(0x807e2984, 0x60000000);

// LoadButton+0x98: the Mii path formats "CharacterSelect%d_%d_Mii"; use the
// _Multi controls instead, which exist for every column.
asmFunc LoadMultiButtonCtrName() {
    ASM(
        nofralloc;
        lis r5, multiButtonCtrName @ha;
        addi r5, r5, multiButtonCtrName @l;
        blr;)
}
kmCall(0x807e29c0, LoadMultiButtonCtrName);

// LoadButton+0xD4 replaces "srwi r8, r0, 5": r8 is PushButton::Load's "player exclusive"
// argument (r0 = cntlzw isMii). This is past the 1P/MP join, so 1P (r29 == 1) keeps it.
asmFunc ClearExclusiveIfMulti() {
    ASM(
        nofralloc;
        srwi r8, r0, 5;
        cmpwi r29, 1;
        beqlr;
        li r8, 0;
        blr;)
}
kmCall(0x807e29fc, ClearExclusiveIfMulti);

// LoadButton+0x110: the byte at button+0x254 normally means "this is the Mii
// button". The rest of the control code uses it to pick the per-player OK panes,
// so set it for every button whenever more than one local player (r29) is present.
asmFunc StoreIsMultiButton() {
    ASM(
        nofralloc;
        xori r5, r29, 1;
        subic r0, r5, 1;
        subfe r0, r0, r5;
        stb r0, 0x254(r4);
        blr;)
}
kmCall(0x807e2a38, StoreIsMultiButton);

// AwardsMgr::LoadPlayers+0x258 replaces "addi r7, r10, 0x1c" (the Mii head pointer),
// which leaks onto normal characters with duplicate drivers; keep it for Mii ids
// 0x18..0x2C only. r0 holds the slot-search count (mtctr r0 at 0x80789584): leave it.
asmFunc AwardsMiiHeadOnlyForMiis() {
    ASM(
        nofralloc;
        li r7, 0;
        cmplwi r15, 0x18;
        bltlr;
        cmplwi r15, 0x2c;
        bgtlr;
        addi r7, r10, 0x1c;
        blr;)
}
kmCall(0x80789598, AwardsMiiHeadOnlyForMiis);

// Whether player p has confirmed a driver: OnButtonDriverClick turns its controls holder off
// (ControlsManipulatorManager::InitHolders(page vtable +0x70, p, 0), the byte +0xA4 of 0x5C each).
typedef void *(*GetPageFn)(u32 pageId);
static const GetPageFn getPage = reinterpret_cast<GetPageFn>(0x8083d44c);

static u32 PlayerConfirmed(u32 player) {
    u8 *page = static_cast<u8 *>(getPage(0x6b));
    if (page == nullptr)
        return 1;
    u8 *holders = reinterpret_cast<u8 *(*)(u8 *)>((*reinterpret_cast<void ***>(page))[0x70 / 4])(page);
    if (holders == nullptr)
        return 1;
    return holders[(player & 0xff) * 0x5c + 0xa4] == 0;
}

// CtrlMenuCharacterSelect::OnUpdate+0x70, "cmpwi r0, 1": r0 is player r27's preview model state, 1 once
// it is confirmed. Two players can still share a driver's model (online 2P, or a failed load of a
// player's own preview in RR's Driver/LocalPlayerSkins.cpp), so in multiplayer (r28 players) the player's own holder must also be
// off, or one player's confirm would show every OK marker on that driver. The beq after reads CR0;
// volatile registers are reloaded after it and OnUpdate saved LR.
asmFunc OwnConfirmForMarker() {
    ASM(
        nofralloc;
        cmpwi r0, 1;
        bnelr;
        cmpwi r28, 1;
        beqlr;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r27;
        bl PlayerConfirmed;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        cmpwi r3, 1;
        blr;)
}
kmCall(0x807e311c, OwnConfirmForMarker);

}  // namespace SplitScreen8
