#include <kamek.hpp>
#include <runtimeWrite.hpp>

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
        blr;
    )
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
        blr;
    )
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
        blr;
    )
}
kmCall(0x80789598, AwardsMiiHeadOnlyForMiis);

}  // namespace SplitScreen8
