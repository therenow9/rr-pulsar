#include <kamek.hpp>

// SplitScreen8 M1: let local multiplayer players pick the same driver.
//
// Vanilla loads every non-Mii driver button as "player exclusive", so once one
// player hovers a driver the others can't reach it. Mii buttons are the exception:
// they use the CharacterSelect%d_%d_Mii controls (common_w117_mii_suit layout),
// which carry one OK marker per player (ok_null_1p..4p). We load every multiplayer
// button through that same path, pointed at CharacterSelect%d_%d_Multi controls
// (generated from the vanilla ones by tools/assets/gen_charselect_multi.py), and
// clear the exclusive flag. Port of mkw-sp PR #452, PAL addresses.
//
// Preview models are handled separately: Retro Rewind's MiiOutfitC already
// rewrites the CharacterModelManager slot layout, so mkw-sp's model patch can't be
// applied on top of it (see docs/m1-same-character.md).

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
        blr;
    )
}
kmCall(0x807e29c0, LoadMultiButtonCtrName);

// LoadButton+0xD4: r8 is PushButton::Load's "player exclusive" argument.
kmWrite32(0x807e29fc, 0x39000000);  // li r8, 0

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
        blr;
    )
}
kmCall(0x807e2a38, StoreIsMultiButton);

// CtrlMenuCharacterSelect::OnButtonClick+0xA8 writes the single-player "OK" text
// into the button. Multi buttons keep their per-player OK text, so skip it there.
kmRuntimeUse(0x8063ddb4);  // LayoutUIControl::SetMessage
asmFunc SkipOkMessageOnMultiButton() {
    ASM(
        nofralloc;
        lbz r0, 0x254(r3);
        cmpwi r0, 0;
        bnelr;
        lis r12, __kAutoMap_0x8063ddb4 @h;
        ori r12, r12, __kAutoMap_0x8063ddb4 @l;
        mtctr r12;
        bctr;
    )
}
kmCall(0x807e36a4, SkipOkMessageOnMultiButton);

// AwardsMgr::LoadPlayers+0x258: the awards scene attaches a Mii head pointer to
// every player slot. With duplicate drivers that pointer leaks onto normal
// characters, so only attach it for Mii character ids (0x18..0x2C).
asmFunc AwardsMiiHeadOnlyForMiis() {
    ASM(
        nofralloc;
        subi r0, r15, 0x18;
        li r7, 0;
        cmplwi r0, 0x14;
        bgtlr;
        addi r7, r10, 0x1c;
        blr;
    )
}
kmCall(0x80789598, AwardsMiiHeadOnlyForMiis);

}  // namespace SplitScreen8
