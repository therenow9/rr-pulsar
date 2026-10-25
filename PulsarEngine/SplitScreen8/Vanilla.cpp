#ifdef SS8_VANILLA
#include <kamek.hpp>
#include <Dolphin/DolphinIOS.hpp>
#include <core/System/SystemManager.hpp>
#include <core/rvl/gx/GX.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/rvl/VI/VI.hpp>
#include <core/RK/RKSystem.hpp>
#include <MarioKartWii/3D/GameScreenEffects/GameScreenEffects.hpp>
#include <MarioKartWii/3D/Scn/ScnMgr.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/System/NdevArgsExtractor.hpp>
#include <MarioKartWii/File/BMG.hpp>
#include <MarioKartWii/Scene/GameScene.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Layout/Layout.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Mario Kart Wii without Retro Rewind: the Pulsar and RR hooks SplitScreen8 relies on, from the RR files this build leaves out
// (UI/UI.cpp, UI/UIColor.cpp, Driver/CustomCharacters.cpp, Race/CustomCharacters.cpp, Race/ResourceFixes.cpp), and this build's own changes.

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

// GameScene::reinit+0x1C, "bl GameScene::Reset", before it frees the structs heaps: the outgoing scene's light textures
// still have EFB copies queued into them, so drain the GPU first, as EGG::SceneManager::destroyScene does.
static void ResetAndDrainGpu(GameScene &scene) {
    scene.Reset();
    GX::Flush();
    GX::DrawDone();
}
kmCall(0x8051b7cc, ResetAndDrainGpu);

// ScnMgrRace::LoadPostEffects' "ori r4, r4, 0x10" (2+ screens, then 1P): no bloom path is created, as on the courses the
// game leaves it off. RR's Bloom setting blends the pass to nothing instead, so the GPU still draws it.
kmWrite32(0x805b1560, 0x60000000);
kmWrite32(0x805b15bc, 0x60000000);

// RaceCameraMgr::Update's "bl GameScreenEffectsMgr::Update" (lens flare, course filters): none in a 5-8 game, as RR's. WiiCompiled
// writes a flare's depth copy back a frame late, into the one-screen pass's freed heap, over the race's effect creators.
static void UpdateScreenEffects(GameScreenEffectsMgr &mgr) {
    if (menuLocalCount <= kGameLocal)
        mgr.Update();
}
kmCall(0x805a9050, UpdateScreenEffects);

static bool fullRateRace;
static u32 raceUpdates;
static u32 lastRaceUpdates;
static bool raceUpdating;
static u32 raceStartRetrace;
static u32 skippedDraws;

// RaceScene::CreateAndInitInstances+0x68, "bl GameScene::SetFramerate", on every race load: the game halves the rate at 3+
// screens. WiiCompiled (RR's runtime marker, written at boot) and Dolphin keep the full rate, skipping draws when behind.
static void SetRaceFramerate(GameScene &scene, u8 halfRate) {
    fullRateRace = (*reinterpret_cast<volatile u32 *>(0x800017d8) == 1 || Dolphin::IsEmulator()) && Racedata::sInstance->racesScenario.screenCount > 2;
    scene.SetFramerate(fullRateRace ? 0 : halfRate);
}
kmCall(0x80554270, SetRaceFramerate);

static void CountRaceUpdate() {
    ++raceUpdates;
}
static RaceFrameHook countRaceUpdate(CountRaceUpdate);

// A full-rate race keeps 60 updates a second as RR's frameskip does: while the updates trail the retraces it skips the draw, at
// most three in a row. A tick with no race update since the last (a load, a pause, the HOME menu, the menus) keeps the game's pacing.
extern "C" u32 DrawThisTick(u32 draw) {
    raceUpdating = fullRateRace && raceUpdates != lastRaceUpdates;
    lastRaceUpdates = raceUpdates;
    if (!raceUpdating) {
        raceStartRetrace = VI::GetRetraceCount() - raceUpdates;
        skippedDraws = 0;
        return draw;
    }
    const s32 behind = static_cast<s32>(VI::GetRetraceCount() - raceStartRetrace - raceUpdates);
    if (behind > 8)
        raceStartRetrace += behind;
    else if (behind > 1 && skippedDraws < 3) {
        ++skippedDraws;
        return 0;
    }
    skippedDraws = 0;
    return draw;
}

// RKSystem::Run+0xA8, "cmpwi r25, 0" (r25 draws this tick), as RR's Frameskip.cpp; the prologue saved LR, and no volatile
// register is live across it.
asmFunc DrawThisTickHook() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        mr r3, r25;
        bl DrawThisTick;
        mr r25, r3;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        cmpwi r25, 0;
        blr;)
}
kmCall(0x800095c4, DrawThisTickHook);

// AsyncDisplay::beginFrame+0x28, "bl OS::SleepThread": a racing full-rate frame past its retrace starts the next at once.
// One frame per tick: the scene change after a race draws its own frames with no tick, and yielding there starves its loader.
static void WaitForRetrace(OS::ThreadQueue *queue) {
    const bool racing = raceUpdating;
    raceUpdating = false;
    if (racing && RKSystem::mInstance.asyncDisplay->retraceCount != RKSystem::mInstance.asyncDisplay->frameStartRetraceCount)
        OS::YieldThread();
    else
        OS::SleepThread(queue);
}
kmCall(0x8020fe4c, WaitForRetrace);

#if defined(SS8_DEBUG_BOOT) || defined(SS8_MENU_SCRIPT)
// SectionMgr::Init's "bl NdevArgsExtractor::ExtractAllArgs", as RR's BootIntoSection. A slot on a USA or Japanese disc runs slot
// 0's PAL save, which the game would offer to recreate (the country changed), after the USA disc's ESRB notice: boot as on PAL.
static SectionId BootIntoSection(const NdevArgsExtractor &extractor) {
    SectionMgr &mgr = *SectionMgr::sInstance;
    if (mgr.bootSectionId == SECTION_COUNTRY_CHANGED || mgr.bootSectionId == SECTION_ESRB_NOTICE)
        mgr.bootSectionId = SECTION_NONE;
#ifdef SS8_DEBUG_BOOT
    // With no ndevArg it returns before registering pad 0, so RR's own string sets license 0 and GC port 1 (0x124).
    SystemManager::sInstance->ndevArg = "-s132 -l0 -p292";
    extractor.ExtractAllArgs();
    return DebugBootPrepare(SECTION_NONE);
#else
    return extractor.ExtractAllArgs();
#endif
}
kmCall(0x80634f20, BootIntoSection);
#endif

}  // namespace SplitScreen8
#endif
