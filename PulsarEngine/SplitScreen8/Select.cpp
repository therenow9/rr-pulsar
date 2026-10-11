#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Menu/Menu.hpp>
#include <MarioKartWii/UI/Page/Other/ModelRenderer.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <Network/Settings/SelectionRestrictions.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The kart and drift pages of a 5-8 player game (D67, D71): each player readies with A and un-readies with B,
// P1's B with nobody ready goes back, and the last ready moves on; on the kart page left and right change the
// player's kart, and the previews put each driver on it. The character page is CharSelect.cpp's. Built only
// with --ss8-pages (SS8_PAGES); normal builds keep phase B's flow. docs/plans/m4-menu-flow.md, C3 and C5a.

namespace SplitScreen8 {

struct SelectStep {
    u32 id;
    u32 prev;
    u32 next;
    const wchar_t *title;  // English only until C4's pages take the game's own titles
    const char *name;
};
static const SelectStep kSteps[] = {
    {Pulsar::UI::PULPAGE_SS8KARTSELECT, Pulsar::UI::PULPAGE_SS8CHARSELECT, Pulsar::UI::PULPAGE_SS8DRIFTSELECT, L"Vehicles", "kart"},
    {Pulsar::UI::PULPAGE_SS8DRIFTSELECT, Pulsar::UI::PULPAGE_SS8KARTSELECT, PAGE_CUP_SELECT, L"Drift", "drift"},
};

// The vanilla kart page's last confirm gives every CPU from the game's count (4, D60) a random kart
// (KartSelect, 0x8084745C); it reads no argument. P5-8 are among them until InitRace makes them local
// (MenuLocalsLocal, Entry.cpp).
typedef void (*FillCpusFn)();
static const FillCpusFn fillCpuKarts = reinterpret_cast<FillCpusFn>(0x8084745c);
typedef KartId (*KartByIdxFn)(CharacterId, u8);
static const KartByIdxFn characterIdToKartIdByIdx = reinterpret_cast<KartByIdxFn>(0x8081cef4);  // not in Kamek's externals

KartId KartForCharacter(CharacterId character, KartId kart) {
    for (u8 k = 0; k < 12; ++k)
        if (characterIdToKartIdByIdx(character, k) == kart)
            return kart;
    return static_cast<KartId>(GetCharacterWeightClass(character));
}

// P1-4's karts, from SectionParams as the kart page leaves them, written where the vanilla page's confirm
// writes them.
static void WriteKarts() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 i = 0; i < kGameLocal; ++i) {
        RacedataPlayer &player = Racedata::sInstance->menusScenario.players[i];
        player.kartId = KartForCharacter(player.characterId, params->karts[i]);
    }
    fillCpuKarts();
}

// A character's karts as MultiKartSelect::FillPossibleKarts lists them (0x8084A3F8): its weight class in KartId
// order, unlocked as that page asks (IsKartUnlocked with its 2, 0x8084A45C), and RR's vehicle restriction.
static u32 KartChoices(CharacterId character, KartId *choices) {
    u32 count = 0;
    for (u8 k = 0; k < 12; ++k) {
        const KartId kart = characterIdToKartIdByIdx(character, k);
        if (IsKartUnlocked(kart, static_cast<u32>(2)) && Pulsar::Restrictions::IsVehicleEnabled(kart))
            choices[count++] = kart;
    }
    return count;
}

static CharacterId PlayerCharacter(u32 player) {
    return player < kGameLocal ? SectionMgr::sInstance->sectionParams->characters[player] : ExtCharacter(player);
}

typedef void (*ShowModelFn)(Pages::ModelRenderer *, u32, bool, u32);
// VehicleModelControl::OnUpdate's per-frame calls (0x805F3DAC, 0x805F3E30); they end in
// MenuDriverModelMgr::TogglePlayerModel, which writes players[hud] (4 wide), so P1-4 only.
static const ShowModelFn showModel = reinterpret_cast<ShowModelFn>(0x805f59ac);
const u32 kDriverKart = 0x14;  // MenuDriverModel's kart (0x805F3F30)

class SelectPage : public Page {
public:
    explicit SelectPage(const SelectStep &step);
    ~SelectPage() override;
    PageId GetNextPage() const override {
        return this->nextPageId;
    }
    void OnInit() override;
    void OnActivate() override;
    void OnDeactivate() override;
    void AfterControlUpdate() override;

private:
    void Leave(u32 next, u32 animDirection);
    void ActivateKarts();
    void StepKart(u32 player, s32 step);
    void RequestKart(u32 player);
    void UpdateKartPreviews();

    const SelectStep &step;
    CtrlMenuPageTitleText *titleText;
    PageManipulatorManager manager;
    PageId nextPageId;
    bool ready[kMaxLocal];
    bool leaving;
    KartId karts[kMaxLocal];  // on the kart page
};

Page *NewSelectPage(u32 id) {
    if (id == Pulsar::UI::PULPAGE_SS8CHARSELECT)
        return NewCharSelectPage();
    for (u32 i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i)
        if (kSteps[i].id == id)
            return new SelectPage(kSteps[i]);
    return nullptr;
}

SelectPage::SelectPage(const SelectStep &step) : step(step) {
    this->nextPageId = PAGE_NONE;
    this->titleText = new CtrlMenuPageTitleText;
}

SelectPage::~SelectPage() {
    delete this->titleText;
}

void SelectPage::OnInit() {
    this->InitControlGroup(1);
    this->AddControl(0, *this->titleText, 0);
    this->titleText->Load(false);
    Text::Info info;
    info.strings[0] = const_cast<wchar_t *>(this->step.title);
    this->titleText->SetMessage(Pulsar::UI::BMG_TEXT, &info);
    // A page with no control to select takes a PageManipulatorManager; every player's A and B are read
    // from the holders. SectionPad's reconnect overlay watches P1-4, as the vanilla pages' mask does with
    // D60's count of 4; P5-8's box is D69's (C6).
    this->manager.Init(1, false);
    this->SetManipulatorManager(this->manager);
    this->manager.UpdatePlayerBitfield2((1 << kGameLocal) - 1);
}

void SelectPage::OnActivate() {
    this->nextPageId = PAGE_NONE;
    this->leaving = false;
    for (u32 i = 0; i < kMaxLocal; ++i) this->ready[i] = false;
    OS::Report("ss8 select: %s page, %u players\n", this->step.name, menuLocalCount);
    if (this->step.id == Pulsar::UI::PULPAGE_SS8KARTSELECT)
        this->ActivateKarts();
}

void SelectPage::OnDeactivate() {
    if (this->step.id != Pulsar::UI::PULPAGE_SS8KARTSELECT)
        return;
    ExtKartsHide();
    ExtPreviewsHide();
}

// Each player starts on their last kart if their character has it, else the class's first choice. P1-4's karts
// load as a character click loads them (LoadKartModelsByCharacter, 0x805F570C), each time the page opens: their
// once-per-character flag (+0x9) is cleared first, since a skin, which the flag does not see, can change the archive.
void SelectPage::ActivateKarts() {
    const u32 count = menuLocalCount;
    if (!ExtPreviewsShow(count) || !ExtKartsShow(count))
        OS::Report("ss8 select: P5-8 kart previews failed\n");
    Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 player = 0; player < count; ++player) {
        const CharacterId character = PlayerCharacter(player);
        const KartId last = player < kGameLocal ? params->karts[player] : extPicks[player - kGameLocal].picked ? extPicks[player - kGameLocal].kart : static_cast<KartId>(-1);
        this->karts[player] = KartForCharacter(character, last);
        this->StepKart(player, 0);
        if (player < kGameLocal && renderer != nullptr) {
            renderer->params[player].unknown_0x9 = 0;
            renderer->LoadKartModelsByCharacter(player, character);
        }
    }
}

// A step of 0 keeps the kart if it is a choice, else takes the first.
void SelectPage::StepKart(u32 player, s32 step) {
    KartId choices[12];
    const CharacterId character = PlayerCharacter(player);
    const s32 count = static_cast<s32>(KartChoices(character, choices));
    if (count == 0)
        return;
    s32 at = 0;
    for (s32 k = 0; k < count; ++k)
        if (choices[k] == this->karts[player])
            at = k;
    at = ((at + step) % count + count) % count;
    this->karts[player] = choices[at];
    if (player < kGameLocal) {
        SectionMgr::sInstance->sectionParams->karts[player] = choices[at];
    } else {
        ExtPick &pick = extPicks[player - kGameLocal];
        pick.character = character;
        pick.kart = choices[at];
        pick.picked = true;
    }
    this->RequestKart(player);
    OS::Report("ss8 select: kart player %u on %#x\n", player + 1, choices[at]);
}

void SelectPage::RequestKart(u32 player) {
    if (player < kGameLocal)
        return;
    const CharacterId character = PlayerCharacter(player);
    ExtKartRequest(player, character, Pulsar::Driver::GetLocalPlayerSlot(player, character), this->karts[player]);
}

// What VehicleModelControl::OnUpdate does each frame for P1-4 (0x805F3CF8): the model shown, and once its karts
// have loaded (params' isVisible, 0x805F3DBC) the kart asked for and the driver put on it, unless it is in the
// confirmed pose (state 3, 0x805F3F0C).
void SelectPage::UpdateKartPreviews() {
    Pages::ModelRenderer *renderer = SectionMgr::sInstance->curSection->Get<Pages::ModelRenderer>();
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    if (renderer == nullptr || modelMgr == nullptr || modelMgr->driverModels == nullptr)
        return;
    for (u32 player = 0; player < kGameLocal && player < menuLocalCount; ++player) {
        showModel(renderer, player, false, 0);
        if (!renderer->params[player].isVisible)
            continue;
        showModel(renderer, player, true, 1);
        renderer->RequestKartModel(player, this->karts[player]);
        MenuDriverModel *model = modelMgr->driverModels->players[player].playerModel;
        if (model == nullptr || model->state == 3)
            continue;
        *reinterpret_cast<KartId *>(reinterpret_cast<u8 *>(model) + kDriverKart) = this->karts[player];
        model->SwitchState(player, MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT);
    }
}

void SelectPage::AfterControlUpdate() {
    const bool kartStep = this->step.id == Pulsar::UI::PULPAGE_SS8KARTSELECT;
    if (kartStep)
        this->UpdateKartPreviews();
    if (this->currentState != STATE_ACTIVE || this->leaving)
        return;
    const u32 count = menuLocalCount;
    u32 readyCount = 0;
    for (u32 player = 0; player < count; ++player) {
        const Input::RealControllerHolder *holder = PlayerHolder(player);
        if (holder != nullptr && UIPressed(*holder, FORWARD_PRESS) && !this->ready[player]) {
            this->ready[player] = true;
            OS::Report("ss8 select: %s player %u ready\n", this->step.name, player + 1);
        } else if (holder != nullptr && kartStep && !this->ready[player] && (UIPressed(*holder, LEFT_PRESS) || UIPressed(*holder, RIGHT_PRESS))) {
            this->StepKart(player, UIPressed(*holder, LEFT_PRESS) ? -1 : 1);
        } else if (holder != nullptr && UIPressed(*holder, BACK_PRESS)) {
            if (this->ready[player]) {
                this->ready[player] = false;
                OS::Report("ss8 select: %s player %u unready\n", this->step.name, player + 1);
            } else if (player == 0) {
                this->Leave(this->step.prev, 1);
                return;
            }
        }
        if (this->ready[player])
            ++readyCount;
    }
    if (readyCount < count)
        return;
    if (this->step.id == Pulsar::UI::PULPAGE_SS8KARTSELECT)
        WriteKarts();
    this->Leave(this->step.next, 0);
}

// Menu::LoadNextPageById (0x80837720) stores a Menu caller's id as the next Menu's prevPageId (0x808377DC),
// where that Menu's Back goes (LoadPrevPage). These pages are not Menus, so cup select gets it here.
void SelectPage::Leave(u32 next, u32 animDirection) {
    this->leaving = true;
    this->nextPageId = static_cast<PageId>(next);
    if (next == PAGE_CUP_SELECT && animDirection == 0) {
        Page *cupSelect = SectionMgr::sInstance->curSection->pages[PAGE_CUP_SELECT];
        if (cupSelect != nullptr)
            static_cast<Pages::Menu *>(cupSelect)->prevPageId = static_cast<PageId>(this->step.id);
    }
    OS::Report("ss8 select: %s page -> %#x\n", this->step.name, next);
    this->EndStateAnimated(animDirection, 0.0f);
}

// VSModeSelect::OnButtonClick's two "bl Menu::LoadNextPageById" with r4 = 0x6B: Team VS (0x80852A6C,
// the team bit set) and VS (0x80852A90, cleared). r3-r5 are the call's own arguments, and the code after
// it sets r0 and r3 before reading them. A 5-8 player game opens the character page in both; the team bit
// is not read until phase D.
static void VSModeNextPage(Pages::Menu &page, PageId id, PushButton &button) {
    page.LoadNextPageById(menuLocalCount > kGameLocal ? static_cast<PageId>(Pulsar::UI::PULPAGE_SS8CHARSELECT) : id, button);
}
kmCall(0x80852a6c, VSModeNextPage);
kmCall(0x80852a90, VSModeNextPage);

}  // namespace SplitScreen8
#endif
