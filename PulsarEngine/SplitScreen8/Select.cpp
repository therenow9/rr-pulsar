#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/UI/Page/Page.hpp>
#include <MarioKartWii/UI/Page/Menu/Menu.hpp>
#include <MarioKartWii/UI/Ctrl/Menu/CtrlMenuText.hpp>
#include <MarioKartWii/UI/Ctrl/Manipulator.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <UI/UI.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// The kart and drift pages of a 5-8 player game (D67, D71), before C5 gives them their controls: each
// player readies with A and un-readies with B, P1's B with nobody ready goes back, and the last ready
// moves on. The character page is CharSelect.cpp's. Built only with --ss8-pages (SS8_PAGES) until the
// pages can be played; normal builds keep phase B's flow. docs/plans/m4-menu-flow.md, phase C, C3.

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

// Nobody picks a kart yet: P1-4 take SectionParams' last karts, written where the vanilla page's confirm
// writes them.
static void WriteKarts() {
    const SectionParams *params = SectionMgr::sInstance->sectionParams;
    for (u32 i = 0; i < kGameLocal; ++i) {
        RacedataPlayer &player = Racedata::sInstance->menusScenario.players[i];
        player.kartId = KartForCharacter(player.characterId, params->karts[i]);
    }
    fillCpuKarts();
}

class SelectPage : public Page {
public:
    explicit SelectPage(const SelectStep &step);
    ~SelectPage() override;
    PageId GetNextPage() const override {
        return this->nextPageId;
    }
    void OnInit() override;
    void OnActivate() override;
    void AfterControlUpdate() override;

private:
    void Leave(u32 next, u32 animDirection);

    const SelectStep &step;
    CtrlMenuPageTitleText *titleText;
    PageManipulatorManager manager;
    PageId nextPageId;
    bool ready[kMaxLocal];
    bool leaving;
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
}

void SelectPage::AfterControlUpdate() {
    if (this->currentState != STATE_ACTIVE || this->leaving)
        return;
    const u32 count = menuLocalCount;
    u32 readyCount = 0;
    for (u32 player = 0; player < count; ++player) {
        const Input::RealControllerHolder *holder = PlayerHolder(player);
        if (holder != nullptr && UIPressed(*holder, FORWARD_PRESS) && !this->ready[player]) {
            this->ready[player] = true;
            OS::Report("ss8 select: %s player %u ready\n", this->step.name, player + 1);
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
