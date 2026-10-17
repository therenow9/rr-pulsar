#ifdef SS8_VANILLA
#include <kamek.hpp>
#include <MarioKartWii/3D/Scn/ScnMgr.hpp>
#include <MarioKartWii/File/BMG.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/Layout.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Mario Kart Wii without Retro Rewind: the Pulsar and RR hooks SplitScreen8 relies on, from the RR files this build leaves out
// (UI/UI.cpp, UI/UIColor.cpp, Driver/CustomCharacters.cpp, Race/CustomCharacters.cpp).

namespace Pulsar {
namespace UI {

// Pulsar's ExpSection, for SplitScreen8's pages only.
static ExpSection *CreateSection() {
    ExpSection *section = new ExpSection;
    memset(section->pulPages, 0, sizeof(section->pulPages));
    section->hasAutoVote = false;
    return section;
}
kmCall(0x8063504c, CreateSection);
kmWrite32(0x80635058, 0x60000000);

void ExpSection::CreatePages(ExpSection &self, SectionId id) {
    self.CreateSectionPages(id);
    self.CreatePulPages();
}
kmCall(0x80622088, ExpSection::CreatePages);

// No transmission page: transmissions are RR's.
void ExpSection::CreatePulPages() {
    if (this->sectionId != SECTION_LOCAL_MULTIPLAYER || SplitScreen8::menuLocalCount <= SplitScreen8::kGameLocal)
        return;
    this->CreateAndInitPage(*this, PULPAGE_SS8JOIN);
    this->CreateAndInitPage(*this, PULPAGE_SS8CHARSELECT);
    this->CreateAndInitPage(*this, PULPAGE_SS8KARTSELECT);
    this->CreateAndInitPage(*this, PULPAGE_SS8DRIFTSELECT);
    this->CreateAndInitPage(*this, PULPAGE_SS8TEAMSELECT);
}

void ExpSection::CreateAndInitPage(ExpSection &self, u32 id) {
    Page *page;
    if (id == PULPAGE_SS8JOIN)
        page = SplitScreen8::NewJoinPage();
    else if (id >= PULPAGE_SS8CHARSELECT && id <= PULPAGE_SS8TEAMSELECT)
        page = SplitScreen8::NewSelectPage(id);
    else
        page = self.CreatePageById(static_cast<PageId>(id));
    if (id < PULPAGE_INITIAL)
        self.Set(page, static_cast<PageId>(id));
    else
        self.SetPulPage(page, static_cast<PulPageId>(id));
    page->Init(static_cast<PageId>(id));
}
kmBranch(0x80622d08, ExpSection::CreateAndInitPage);

void ExpSection::DisposePulPages(SectionPad &pad, bool enablePointer) {
    pad.EnablePointer(enablePointer);  // default
    register ExpSection *section;
    asm(mr section, r31;);
    for (int pulPageId = 0; pulPageId < PULPAGE_MAX; ++pulPageId) {
        Page *page = section->pulPages[pulPageId];
        if (page != nullptr) {
            page->Dispose();
            delete page;
        }
    }
}
kmCall(0x80622268, ExpSection::DisposePulPages);

void ExpSection::AddPageLayer(ExpSection &self, u32 id) {
    AddPageLayerAnimatedReturnTopLayer(self, id, 0xff);
}
kmBranch(0x80622da0, ExpSection::AddPageLayer);

Page *ExpSection::AddPageLayerAnimatedReturnTopLayer(ExpSection &self, u32 id, u32 animDirection) {
    if (animDirection == 0xff)
        animDirection = self.animDirection;
    Page *page;
    if (id < PULPAGE_INITIAL) {
        page = self.pages[id];
    } else
        page = self.pulPages[id - PULPAGE_INITIAL];

    self.activePages[++self.layerCount] = page;
    if (animDirection != 0xffffffff)
        page->animationDirection = animDirection;  // inlined Page::SetAnimDirection
    page->Activate();
    return page;
}
kmBranch(0x80622e00, ExpSection::AddPageLayerAnimatedReturnTopLayer);

kmWrite32(0x80623128, 0x48000020);  // skip the usual activate
kmWrite32(0x80623140, 0x60000000);  // nop layerCount increase, as AddPageLayer will do it for us
kmWrite32(0x80623144, 0x60000000);  // nop setanimdirection as r29 is faulty

void ExpSection::SetNextPage(u32 id, u32 animDirection) {
    register ExpSection *self;
    asm(mr self, r28;);
    AddPageLayerAnimatedReturnTopLayer(*self, id, animDirection);
}
kmCall(0x8062314c, ExpSection::SetNextPage);

void UnbindRLMC(lyt::Material *mat) {
    for (ut::LinkList<lyt::AnimationLink, offsetof(lyt::AnimationLink, link)>::Iterator it = mat->animLinkList.GetBeginIter(); it != mat->animLinkList.GetEndIter(); ++it) {
        if (!it->disable) {
            lyt::AnimTransform *anim = it->animTrans;
            u32 idx = it->idx;
            const lyt::res::AnimationBlock *res = anim->resource;
            u32 animOffsets = ut::ConvertOffsToPtr<u32>(res, res->animOffsetToAnimOffsetsArray)[idx];
            const lyt::res::AnimationContent *animContent = ut::ConvertOffsToPtr<lyt::res::AnimationContent>(res, animOffsets);

            const u32 *animInfoOffsets = ut::ConvertOffsToPtr<u32>(animContent, sizeof(*animContent));
            for (int i = 0; i < animContent->infoCount; ++i) {
                const lyt::res::AnimationInfo *animInfo = ut::ConvertOffsToPtr<lyt::res::AnimationInfo>(animContent, animInfoOffsets[i]);
                if (animInfo->kind == lyt::res::ANIMATIONTYPE_RLMC)
                    mat->UnbindAnimation(anim);
            }
        }
        if (mat->animLinkList.GetSize() == 0)
            break;
    }
}

}  // namespace UI
}  // namespace Pulsar

namespace SplitScreen8 {

// Pulsar's BMG_TEXT, "\z{a02,2000000000}": the text passed in with the message. No game BMG holds it, so the three
// lookups (BMGHolder 0x805F8C88, 0x805F8CF0, 0x805F8D2C) answer it past every message count and are the game's otherwise.
static const wchar_t kTextMsg[] = {0x001a, 0x0a02, 0x0020, 0x0000, 0x0000, 0x0000};
static const u8 kTextAttributes[4] = {1, 0, 0, 0};
static const s32 kTextIdx = 0x7fff;

static s32 GetMsgId(const BMGHolder &bmg, s32 bmgId) {
    if (bmg.messageIds != nullptr) {
        const BMGMessageIds &msgIds = *bmg.messageIds;
        for (int i = 0; i < msgIds.msgCount; ++i) {
            if (msgIds.messageIds[i] == bmgId)
                return i;
            if (msgIds.messageIds[i] > bmgId)
                break;
        }
    }
    return bmgId == Pulsar::UI::BMG_TEXT ? kTextIdx : -1;
}
kmBranch(0x805f8c88, GetMsgId);

static wchar_t *GetMsgByMsgId(const BMGHolder &bmg, s32 msgIdx) {
    if (msgIdx == kTextIdx)
        return const_cast<wchar_t *>(kTextMsg);
    if (msgIdx < 0 || msgIdx >= bmg.info->msgCount)
        return nullptr;
    return reinterpret_cast<wchar_t *>((u8 *)bmg.data + (bmg.info->entries[msgIdx].dat1Offset & 0xFFFFFFFE));
}
kmBranch(0x805f8cf0, GetMsgByMsgId);

static const u8 *GetFontIndex(const BMGHolder &bmg, s32 msgIdx) {
    if (msgIdx == kTextIdx)
        return kTextAttributes;
    if (msgIdx < 0 || msgIdx >= bmg.info->msgCount)
        return nullptr;
    return &bmg.info->entries[msgIdx].font;
}
kmBranch(0x805f8d2c, GetFontIndex);

// RKSYS::LicenseCompletion::IsCompleted+0x28 (srwi r3, r0, 31): every unlock reads as earned, so every character, kart
// and track is open from the start, as RR writes it; nothing is saved.
kmWrite32(0x80549974, 0x38600001);  // li r3, 1

// Pages::MainMenu::__ct+0x7C (li r4, 4): three controls, no Mario Kart Channel button (owner).
kmWrite32(0x80625e1c, 0x38800003);  // li r4, 3

// CtrlMenuCharacterSelect::LoadButton+0xD4 (srwi r8, r0, 5): no character button is exclusive, as RR writes it.
kmWrite32(0x807e29fc, 0x39000000);  // li r8, 0

// AwardsMgr::LoadPlayers+0x304, "li r27, 1" (a Mii head): only Mii ids (from 0x18) get the head, as RR's awards hook.
static asmFunc AwardsMiiHeadOnlyForMiis() {
    ASM(
        nofralloc;
        lwz r0, 0(r23);
        cmplwi r0, 0x18;
        bltlr;
        li r27, 1;
        blr;)
}
kmCall(0x80789644, AwardsMiiHeadOnlyForMiis);

// ControlManipulator::GetHudSlotIdColor, a leaf: huds 4-7 take their own colours, others the game's 0x18-byte entries.
extern const RGBA16 hudSlotIdColours[];
static void HudSlotIdColor(u8 hud, RGBA16 *primary, RGBA16 *secondary) {
    if (HudSlotColour(hud, primary, secondary))
        return;
    const RGBA16 *entry = hudSlotIdColours + 3 * (hud < 4 ? hud + 1 : 0);
    *primary = entry[0];
    *secondary = entry[1];
}
kmBranch(0x805f0440, HudSlotIdColor);

// GameScene::Reset's ScnMgr::Reset: P5-8's preview models leave the ScnMgr first.
static void ResetScnMgr() {
    ResetExtPreviews();
    ScnMgr::Reset();
}
kmCall(0x8051b118, ResetScnMgr);

}  // namespace SplitScreen8
#endif
