#include <kamek.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Other/RegisterPad.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Ctrl/ModelControl.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The join page of a 5-8 player game (D64), in place of RegisteredPads (0x61) and RegisterPad (0x62),
// whose PadControl[4] and per-hud prompts stop at 4. It asks for players 2..N in turn, one waiting
// slot at a time: P2-4 through SectionPad (RequestPad; SectionPad::Update then calls TrySetController),
// P5-8 through extPads and TrySetController on holders 4-7. Once all are in it opens vanilla's
// OK/Change page (ConfirmPads, 0x64) as RegisteredPads does (D66). docs/plans/m4-menu-flow.md, phase B.

namespace SplitScreen8 {

// PadControl (RecognizePad.hpp) keeps its members private; its functions by address. Load's mode 0
// leaves its pad id to SetPadID; hud 4+ (P5-8) plays the "Menu" colour, which ColourSlot replaces.
const u32 kPadControlSize = 0x184;
const u32 kDummyBackSize = 0x184;
typedef void (*PadCtorFn)(LayoutUIControl *);
typedef void (*PadLoadFn)(LayoutUIControl *, const char *, const char *, const char *, u32, u8, u32, u32);
typedef void (*PadSetIdFn)(LayoutUIControl *, u32);
typedef void (*PadFn)(LayoutUIControl *);
static const PadCtorFn padCtor = reinterpret_cast<PadCtorFn>(0x806012bc);
static const PadLoadFn padLoad = reinterpret_cast<PadLoadFn>(0x80601350);
static const PadSetIdFn padSetId = reinterpret_cast<PadSetIdFn>(0x80601564);
static const PadFn padRegister = reinterpret_cast<PadFn>(0x8060156c);

typedef void (*TrySetControllerFn)(Input::Manager *, u32, u32);
typedef u32 (*CalculateIDFn)(const Input::RealControllerHolder *);
typedef void (*SectionPadSlotFn)(SectionPad *, u32);
typedef bool (*HasAControllerFn)(const SectionPad *, u32);
typedef void (*SectionPadFn)(SectionPad *);
typedef void (*PositionFromBaseFn)(UIControl *);
typedef void (*ControlFn)(LayoutUIControl *);
typedef void (*DummyBackLoadFn)(LayoutUIControl *, const char *, const char *, const char *, u32);
typedef void (*RequestBackModelFn)(BackGroundModelControl *, s32);
typedef bool (*IsPageTopLayerFn)(const Section *, const Page *);
static const TrySetControllerFn trySetController = reinterpret_cast<TrySetControllerFn>(0x80523ebc);
static const CalculateIDFn calculateID = reinterpret_cast<CalculateIDFn>(0x8061be40);
static const SectionPadSlotFn requestPad = reinterpret_cast<SectionPadSlotFn>(0x8061b6ec);
static const SectionPadSlotFn resetPad = reinterpret_cast<SectionPadSlotFn>(0x8061b7c4);
static const HasAControllerFn hasAController = reinterpret_cast<HasAControllerFn>(0x8061b3cc);
static const SectionPadFn resetPadsStatus = reinterpret_cast<SectionPadFn>(0x8061b9a0);
static const SectionPadFn resetAllPads = reinterpret_cast<SectionPadFn>(0x8061bcc8);
static const PositionFromBaseFn positionFromBase = reinterpret_cast<PositionFromBaseFn>(0x8063d3cc);
static const ControlFn resetMsg = reinterpret_cast<ControlFn>(0x8063dfc8);
static const ControlFn layoutCtor = reinterpret_cast<ControlFn>(0x8063d798);
static const DummyBackLoadFn dummyBackLoad = reinterpret_cast<DummyBackLoadFn>(0x807e9620);
static const RequestBackModelFn requestBackModel = reinterpret_cast<RequestBackModelFn>(0x805f2e84);
static const IsPageTopLayerFn isPageTopLayer = reinterpret_cast<IsPageTopLayerFn>(0x80622e6c);

const u32 kTitleBmg = 0x961;  // RegisterPad's title, "Controller Registration"
const u32 kPromptBmg = 0x9c6;  // "Press the A Button on Player 1's controller.", then 2-4
const SoundIDs kJoinSound = static_cast<SoundIDs>(0x20);  // RegisterPad's

// D64's two rows, P1-4 above P5-8, at 0.80 of EntryPlayerPad's 114 x 156 layout units.
const float kSlotScale = 0.8f;
const float kSlotY = 8.0f;

// D70: while ConfirmPads is up the slots shrink and move left and its OK and Revise buttons shrink at
// the right; at their vanilla place and size the buttons cover the fourth column of slots.
const float kConfirmSlotScale = 0.72f;
const float kConfirmSlotX = -100.0f;
const float kConfirmButtonScale = 0.8f;
const float kConfirmButtonX = 210.0f;
const u32 kConfirmFrames = 10;

// P5-8's pads, as SectionPad's PadInfo holds P2-4's (D61): the id bound when the slot joined, 0 for
// none, and whether the slot is the one waiting for an A.
struct ExtPad {
    u32 id;
    bool waiting;
};
static ExtPad extPads[kMaxLocal - kGameLocal];

static Input::RealControllerHolder &SlotHolder(u32 slot) { return Holder(*Input::Manager::sInstance, slot); }

static bool BackPressed(const Input::RealControllerHolder &holder) {
    return (holder.uiinputStates[0].buttonActions & 2) != 0 && (holder.uiinputStates[1].buttonActions & 2) == 0;
}

static bool IsColourTrack(const nw4r::lyt::AnimationLink &link) {
    const nw4r::lyt::res::AnimationBlock *res = link.animTrans->resource;
    const u32 animOffsets = nw4r::ut::ConvertOffsToPtr<u32>(res, res->animOffsetToAnimOffsetsArray)[link.idx];
    const nw4r::lyt::res::AnimationContent *content = nw4r::ut::ConvertOffsToPtr<nw4r::lyt::res::AnimationContent>(res, animOffsets);
    const u32 *infoOffsets = nw4r::ut::ConvertOffsToPtr<u32>(content, sizeof(*content));
    for (int i = 0; i < content->infoCount; ++i) {
        const nw4r::lyt::res::AnimationInfo *info = nw4r::ut::ConvertOffsToPtr<nw4r::lyt::res::AnimationInfo>(content, infoOffsets[i]);
        if (info->kind == nw4r::lyt::res::ANIMATIONTYPE_RLMC) return true;
    }
    return false;
}

// Pulsar's UnbindRLMC steps on from the link it has just unbound, which ends in an endless walk of a
// null link once a material has more than one colour track; each slot material has one per player
// colour. This scan starts again after every unbind.
static void UnbindColourTracks(nw4r::lyt::Material *material) {
    typedef nw4r::ut::LinkList<nw4r::lyt::AnimationLink, offsetof(nw4r::lyt::AnimationLink, link)> Links;
    bool unbound = true;
    while (unbound) {
        unbound = false;
        for (Links::Iterator it = material->animLinkList.GetBeginIter(); it != material->animLinkList.GetEndIter(); ++it) {
            if (it->disable || !IsColourTrack(*it)) continue;
            material->UnbindAnimation(it->animTrans);
            unbound = true;
            break;
        }
    }
}

// The slot's frame, its stripes and its controller's glow take tev colour 1 from the "Player"
// animation group; for P5-8 those tracks are unbound and the slot palette written once.
static void ColourPane(LayoutUIControl &control, const char *name, u8 r, u8 g, u8 b) {
    nw4r::lyt::Pane *pane = control.layout.GetPaneByName(name);
    if (pane == nullptr) return;
    nw4r::lyt::Material *material = pane->GetMaterial();
    if (material == nullptr) return;
    UnbindColourTracks(material);
    material->tevColours[1].r = r;
    material->tevColours[1].g = g;
    material->tevColours[1].b = b;
}

static void ColourSlot(LayoutUIControl &control, u32 slot) {
    RGBA16 primary, secondary;
    SlotPalette(slot, &primary, &secondary);
    ColourPane(control, "waku", primary.red, primary.green, primary.blue);
    ColourPane(control, "borderline", primary.red * 2 / 5, primary.green * 2 / 5, primary.blue * 2 / 5);
    ColourPane(control, "con_light", secondary.red, secondary.green, secondary.blue);
}

class JoinPage : public Page {
public:
    JoinPage();
    ~JoinPage() override;
    PageId GetNextPage() const override { return this->nextPageId; }
    void OnInit() override;
    void OnActivate() override;
    void AfterControlUpdate() override;

private:
    bool Joined(u32 slot) const;
    u32 SlotId(u32 slot) const;
    void Request(u32 slot);
    void Release(u32 slot);
    void Prompt(s32 slot);
    void PlaceSlots();
    void OpenConfirm();
    void Leave(u32 animDirection, float animLength);
    void Confirming();
    void OnBackPress(u32 hudSlotId);

    CtrlMenuPageTitleText *titleText;
    CtrlMenuInstructionText *bottomText;
    LayoutUIControl *backButton;  // a CtrlMenuDummyBack: the visual only, as RegisterPad has
    LayoutUIControl *pads[kMaxLocal];
    PageManipulatorManager manager;
    PtmfHolder_1A<JoinPage, void, u32> onBackPressHandler;
    PageId nextPageId;
    s32 requested;  // the slot waiting for its A, -1 for none
    s32 prompted;  // the slot the instruction text asks for, -1 for none, -2 before the first
    u32 shownIds[kMaxLocal];  // the pad id each slot shows
    bool leaving;
    bool confirming;  // ConfirmPads is open over the page
    u32 confirmFrame;  // 0 at D64's layout, kConfirmFrames at D70's
};

Page *NewJoinPage() { return new JoinPage; }

JoinPage::JoinPage() {
    this->nextPageId = PAGE_NONE;
    this->onBackPressHandler.subject = this;
    this->onBackPressHandler.ptmf = &JoinPage::OnBackPress;
    this->titleText = new CtrlMenuPageTitleText;
    this->bottomText = new CtrlMenuInstructionText;
    // Built as RegisterPad::__ct builds its own (0x80603860): a LayoutUIControl with the dummy's vtable.
    this->backButton = static_cast<LayoutUIControl *>(::operator new(kDummyBackSize));
    layoutCtor(this->backButton);
    *reinterpret_cast<u32 *>(this->backButton) = 0x808d3710;
    for (int i = 0; i < kMaxLocal; ++i) {
        this->pads[i] = static_cast<LayoutUIControl *>(::operator new(kPadControlSize));
        padCtor(this->pads[i]);
    }
}

JoinPage::~JoinPage() {
    delete this->titleText;
    delete this->bottomText;
    delete this->backButton;
    for (int i = 0; i < kMaxLocal; ++i) delete this->pads[i];
}

void JoinPage::OnInit() {
    this->InitControlGroup(3 + kMaxLocal);
    this->AddControl(0, *this->titleText, 0);
    this->titleText->Load(false);
    this->titleText->SetMessage(kTitleBmg);
    this->AddControl(1, *this->bottomText, 0);
    this->bottomText->Load();
    this->AddControl(2, *this->backButton, 0);
    dummyBackLoad(this->backButton, Pulsar::UI::buttonFolder, "Back", "ButtonBack", 1);
    for (u32 slot = 0; slot < kMaxLocal; ++slot) {
        LayoutUIControl &pad = *this->pads[slot];
        this->AddControl(3 + slot, pad, 0);
        const bool game = slot < kGameLocal;
        padLoad(&pad, "pad_recognize", "EntryPlayerPad", "Pad1P_Div4", game ? slot : kGameLocal, game, 0, 0);
        if (!game) ColourSlot(pad, slot);
    }
    this->confirmFrame = 0;
    this->PlaceSlots();
    // P1 drives the page as RegisterPad's PageManipulatorManager does, with B only and no control to
    // select; P2-8's B is read from their holders. As RegisterPad's (0x80603A2C), no player is in the mask
    // SectionPad's reconnect overlay scans: after Change every pad is reset, P1's included.
    this->manager.Init(1, false);
    this->SetManipulatorManager(this->manager);
    this->manager.UpdatePlayerBitfield2(0);
    this->manager.SetGlobalHandler(BACK_PRESS, this->onBackPressHandler, false);
}

void JoinPage::OnActivate() {
    this->nextPageId = PAGE_NONE;
    this->leaving = false;
    this->confirming = false;
    this->confirmFrame = 0;
    this->PlaceSlots();
    this->requested = -1;
    this->prompted = -2;
    // As RegisteredPads::OnActivate does (0x80603170): no background model behind the slots.
    Page *background = SectionMgr::sInstance->curSection->pages[0x5c];
    if (background != nullptr) requestBackModel(reinterpret_cast<BackGroundModelControl *>(reinterpret_cast<u8 *>(background) + 0x1c8), -1);
    for (u32 slot = 0; slot < kMaxLocal; ++slot) {
        this->shownIds[slot] = ~0u;
        this->pads[slot]->isHidden = slot >= menuLocalCount;
    }
}

bool JoinPage::Joined(u32 slot) const {
    if (slot < kGameLocal) return hasAController(&SectionMgr::sInstance->pad, slot);
    const ExtPad &pad = extPads[slot - kGameLocal];
    return !pad.waiting && pad.id != 0 && pad.id == calculateID(&SlotHolder(slot));
}

u32 JoinPage::SlotId(u32 slot) const {
    if (!this->Joined(slot)) return 0;
    if (slot < kGameLocal) return SectionMgr::sInstance->pad.GetCurrentID(slot);
    return extPads[slot - kGameLocal].id;
}

// The slot waits for an A on any controller no holder has; its holder drops to the dummy controller.
void JoinPage::Request(u32 slot) {
    if (slot < kGameLocal) {
        requestPad(&SectionMgr::sInstance->pad, slot);
        return;
    }
    Input::Manager *input = Input::Manager::sInstance;
    SlotHolder(slot).SetController(&input->dummyController, nullptr);
    extPads[slot - kGameLocal].id = 0;
    extPads[slot - kGameLocal].waiting = true;
}

// The slot empties and stops waiting; its holder drops to the dummy controller.
void JoinPage::Release(u32 slot) {
    if (slot < kGameLocal) {
        resetPad(&SectionMgr::sInstance->pad, slot);
        return;
    }
    Input::Manager *input = Input::Manager::sInstance;
    SlotHolder(slot).SetController(&input->dummyController, nullptr);
    extPads[slot - kGameLocal].id = 0;
    extPads[slot - kGameLocal].waiting = false;
}

void JoinPage::Prompt(s32 slot) {
    if (slot == this->prompted) return;
    this->prompted = slot;
    if (slot >= 0 && slot < kGameLocal) {
        this->bottomText->SetMessage(kPromptBmg + slot);
        return;
    }
    if (slot < 0) {
        this->bottomText->SetMessage(0);  // as RegisterPad clears it after each pad (0x80603D98)
        return;
    }
    // English only: Retro Rewind's text has no prompt past Player 4.
    static const wchar_t *const extPrompts[kMaxLocal - kGameLocal] = {
        L"Press the A Button on Player 5's controller.",
        L"Press the A Button on Player 6's controller.",
        L"Press the A Button on Player 7's controller.",
        L"Press the A Button on Player 8's controller.",
    };
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(extPrompts[slot - kGameLocal]);
    this->bottomText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
}

void JoinPage::PlaceSlots() {
    const float t = static_cast<float>(this->confirmFrame) / kConfirmFrames;
    const float scale = kSlotScale + (kConfirmSlotScale - kSlotScale) * t;
    const float dx = (114.0f + 24.0f) * scale;
    const float dy = 156.0f * scale + 12.0f;
    for (u32 slot = 0; slot < kMaxLocal; ++slot) {
        LayoutUIControl &pad = *this->pads[slot];
        PositionAndScale &base = pad.positionAndscale[0];
        base.position.x = kConfirmSlotX * t + (-1.5f + (slot % 4)) * dx;
        base.position.y = kSlotY + (slot < kGameLocal ? dy / 2 : -dy / 2);
        base.scale.x = scale;
        base.scale.z = scale;  // Vec2 names its second component z
        positionFromBase(&pad);
    }
}

// Section 0x54 is built again for every game, so this ConfirmPads serves only this 5-8 player one.
void JoinPage::OpenConfirm() {
    this->confirming = true;
    Pages::ConfirmPads *confirm = SectionMgr::sInstance->curSection->Get<Pages::ConfirmPads>();
    if (confirm != nullptr) {
        PushButton *buttons[2] = {&confirm->okButton, &confirm->changeButton};
        for (int i = 0; i < 2; ++i) {
            PositionAndScale &base = buttons[i]->positionAndscale[0];
            base.position.x = kConfirmButtonX;
            base.scale.x = kConfirmButtonScale;
            base.scale.z = kConfirmButtonScale;
            positionFromBase(buttons[i]);
        }
    }
    this->AddPageLayer(PAGE_CONFIRM_PAD, 0);
}

void JoinPage::AfterControlUpdate() {
    if (this->currentState != STATE_ACTIVE || this->leaving) return;
    const u32 frame = this->confirming ? kConfirmFrames : 0;
    if (this->confirmFrame != frame) {
        this->confirmFrame += frame > this->confirmFrame ? 1 : -1;
        this->PlaceSlots();
    }
    if (this->confirming) {
        this->Confirming();
        return;
    }
    const u32 count = menuLocalCount;
    for (u32 slot = kGameLocal; slot < count; ++slot) {
        ExtPad &pad = extPads[slot - kGameLocal];
        if (!pad.waiting) continue;
        trySetController(Input::Manager::sInstance, slot, 0);
        const u32 id = calculateID(&SlotHolder(slot));
        if (id == 0) continue;
        pad.id = id;
        pad.waiting = false;
    }
    for (u32 slot = 1; slot < count; ++slot) {
        if (!this->Joined(slot)) continue;
        const Input::RealControllerHolder *holder =
            slot < kGameLocal ? SectionMgr::sInstance->pad.padInfos[slot].controllerHolder : &SlotHolder(slot);
        if (holder != nullptr && BackPressed(*holder)) {
            this->Release(slot);
            if (static_cast<s32>(slot) == this->requested) this->requested = -1;
        }
    }
    s32 next = -1;
    for (u32 slot = 0; slot < count && next < 0; ++slot) {
        if (!this->Joined(slot)) next = slot;
    }
    if (next != this->requested) {
        if (this->requested >= 0 && !this->Joined(this->requested)) this->Release(this->requested);
        if (next >= 0) this->Request(next);
        this->requested = next;
    }
    for (u32 slot = 0; slot < count; ++slot) {
        const u32 id = this->SlotId(slot);
        if (id != this->shownIds[slot]) {
            padSetId(this->pads[slot], id);
            // The lamp row, "{arg border|0}", draws hud 0-3 only; P5-8's slots (hud 4) show "???".
            if (slot >= kGameLocal) resetMsg(this->pads[slot]);
            if (id != 0 && this->shownIds[slot] != ~0u) {
                padRegister(this->pads[slot]);
                this->PlaySound(kJoinSound, slot);
            }
            this->shownIds[slot] = id;
        }
        // PadControl::OnUpdate asks SectionPad about its hud, 4 for every P5-8 slot.
        if (slot >= kGameLocal) this->pads[slot]->animator.GetAnimationGroupById(3).PlayAnimationAtFrame(id != 0 ? 0 : 1, 0.0f);
    }
    this->Prompt(next);
    if (next < 0) this->OpenConfirm();
}

// ConfirmPads' OK and Back end RegisteredPads through its canEnd, which RegisteredPads::AfterControlUpdate
// reads (0x806032C8); section 0x54 builds that page in every game, unshown in a 5-8 player one. Change
// only closes ConfirmPads here (ConfirmChangeNextPage); every pad is reset as its OnDeactivate does before
// RegisterPad (0x806051FC), and the page asks again from Player 1.
void JoinPage::Confirming() {
    Pages::RegisteredPads *registered = SectionMgr::sInstance->curSection->Get<Pages::RegisteredPads>();
    if (registered != nullptr && registered->canEnd) {
        registered->canEnd = false;
        this->nextPageId = registered->endAnimDirection == 0 ? PAGE_MULTIPLAYER_MENU : PAGE_NONE;
        this->Leave(registered->endAnimDirection, registered->endAnimLength);
        return;
    }
    if (!isPageTopLayer(SectionMgr::sInstance->curSection, this)) return;
    this->confirming = false;
    resetAllPads(&SectionMgr::sInstance->pad);
    for (u32 slot = kGameLocal; slot < menuLocalCount; ++slot) this->Release(slot);
    this->requested = -1;
}

void JoinPage::Leave(u32 animDirection, float animLength) {
    this->leaving = true;
    this->EndStateAnimated(animDirection, animLength);
}

// Back to the main menu, as RegisterPad's back does from section 0x54 (0x80603C94).
void JoinPage::OnBackPress(u32) {
    if (this->leaving || this->confirming) return;
    this->Leave(1, 0.0f);
    SectionMgr *sectionMgr = SectionMgr::sInstance;
    resetPadsStatus(&sectionMgr->pad);
    sectionMgr->SetNextSection(SECTION_MAIN_MENU_FROM_MENU, 1);
    sectionMgr->RequestSceneChange(0, 0xff);
}

// ConfirmPads' Change ("li r0, 0x62") opens RegisterPad, which stops at 4, and ConfirmPads' own
// OnDeactivate (0x806051E4) resets every pad only when its next page is 0x62. In a 5-8 player game
// -1 only closes the page; Confirming resets the pads.
asmFunc ConfirmChangeNextPage() {
    ASM(
        nofralloc;
        lis r12, menuLocalCount @ha;
        lbz r12, menuLocalCount @l(r12);
        li r0, 0x62;
        cmplwi r12, 4;
        blelr;
        li r0, -1;
        blr;)
}
kmCall(0x80605234, ConfirmChangeNextPage);

// Every 5-8 player game starts at the main menu; its pads go when the main menu comes back (Entry.cpp).
void ClearExtPads() {
    Input::Manager *input = Input::Manager::sInstance;
    for (u32 slot = kGameLocal; slot < kMaxLocal; ++slot) {
        extPads[slot - kGameLocal].id = 0;
        extPads[slot - kGameLocal].waiting = false;
        if (input != nullptr) SlotHolder(slot).SetController(&input->dummyController, nullptr);
    }
}

}  // namespace SplitScreen8
