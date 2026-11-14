#include <kamek.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/UI/Ctrl/CountDown.hpp>
#include <MarioKartWii/UI/Page/Menu/Menu.hpp>
#include <MarioKartWii/UI/Page/Other/Title.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#ifndef SS8_VANILLA
#include <Settings/Settings.hpp>
#endif
#include <SplitScreen8/SplitScreen8.hpp>

namespace SplitScreen8 {

static u32 driftChosenLocals;

bool DriftChosenBy(u32 localCount) {
    return localCount <= driftChosenLocals;
}

// A drift page leaves forward only once every local has confirmed, so those who have chosen are always P1 to Pn.
void DriftChosen(u32 localCount) {
    if (localCount <= driftChosenLocals)
        return;
    driftChosenLocals = localCount;
    OS::Report("ss8 select: drift chosen by %u players\n", localCount);
}

// The title screen, where P1's pad is assigned (Title::OnActivate, vtable +0x30): Keep Last and Same as Last Time ask
// everyone again.
static void TitleActivate(Pages::Title &title) {
    driftChosenLocals = 0;
    EndSamePass();
    OS::Report("ss8 select: drift picks cleared at the title\n");
    title.Pages::Title::OnActivate();
}
kmWritePointer(0x808BEED8, TitleActivate);

#ifndef SS8_VANILLA
using namespace Pulsar::Settings;

struct RowText {
    SettingId id;
    const wchar_t *title;
    const wchar_t *options[4];
    const wchar_t *descriptions[4];
};

// English only. RR's page names open with this escape (its Common.bmg's 0x62F01 to 0x62F0E).
static const wchar_t kPageName[] =
  L"\x001a\x0800\x0000\x0055"
  L"8 Players";
static const RowText kRows[] = {
    {SETTING_SS8TRANSMISSION, L"Transmission Select", {L"Choose", L"Vanilla", L"Outside", L"Inside"},
        {L"Each player picks inside or outside drift.", L"Skips the page: every vehicle keeps its own transmission.", L"Skips the page: everyone's transmission will be on outside drift.",
            L"Skips the page: everyone's transmission will be on inside drift."}},
    {SETTING_SS8DRIFT, L"Drift Select", {L"Choose", L"Keep Last"},
        {L"Each player picks manual or automatic drift.", L"Each player picks once; after that the page is skipped until the title screen."}},
    {SETTING_SS8SELECTTIMER, L"Select Timer", {L"Off", L"15 s", L"30 s", L"60 s"},
        {L"The character, vehicle and drift pages wait for everyone.", L"15 seconds a page, then each player gets their last pick, or what they are on.",
            L"30 seconds a page, then each player gets their last pick, or what they are on.", L"60 seconds a page, then each player gets their last pick, or what they are on."}},
    {SETTING_SS8SCREENLABELS, L"Screen Labels", {L"Off", L"On"}, {L"No screen names under the controllers.", L"Controller Registration names each player's screen under their controller."}},
    {SETTING_SS8SAMELAST, L"Same as Last Time", {L"On", L"Off"},
        {L"Player 1 can hold a button on the character page to repeat everyone's last choices.", L"No Same as Last Time on the character page."}},
};
const u8 kSettingsTextFont[4] = {1, 0, 0, 0};

const wchar_t *SettingsText(s32 bmgId) {
    const u32 page = Params::GetPageDef(SETTINGS_PAGE_SS8).nameBmg;
    const u32 id = static_cast<u32>(bmgId);
    if ((id & ~0xFFF) != (page & ~0xFFF))
        return nullptr;
    if (id == page)
        return kPageName;
    if (id == page + 0x10)
        return L"Access your 8 Players settings.";
    if (id == page + 0x1F)
        return L"8 Players Settings";
    for (u32 row = 0; row < sizeof(kRows) / sizeof(kRows[0]); ++row) {
        const RowText &text = kRows[row];
        if (id == static_cast<u32>(text.id))
            return text.title;
        for (u32 option = 0; option < Params::GetSettingDef(text.id).optionCount; ++option) {
            if (id == Params::GetOptionBmg(text.id, option))
                return text.options[option];
            if (id == Params::GetDescriptionBmg(text.id, option))
                return text.descriptions[option];
        }
    }
    return nullptr;
}

s32 ForcedTransmission() {
    switch (Mgr::Get().GetSettingValue(SETTING_SS8TRANSMISSION)) {
        case Pulsar::SS8TRANSMISSION_VANILLA:
            return Pulsar::TRANSMISSION_DEFAULT;
        case Pulsar::SS8TRANSMISSION_OUTSIDE:
            return Pulsar::TRANSMISSION_OUTSIDE;
        case Pulsar::SS8TRANSMISSION_INSIDE:
            return Pulsar::TRANSMISSION_INSIDE;
        default:
            return -1;
    }
}

bool SkipDrift(u32 localCount) {
    return Mgr::Get().GetSettingValue(SETTING_SS8DRIFT) == Pulsar::SS8DRIFT_KEEPLAST && DriftChosenBy(localCount);
}

bool SameAsLastTime() {
    return Mgr::Get().GetSettingValue(SETTING_SS8SAMELAST) == Pulsar::SS8SAMELAST_ON;
}

bool ScreenLabels() {
    return Mgr::Get().GetSettingValue(SETTING_SS8SCREENLABELS) == Pulsar::SS8SCREENLABELS_ON;
}

u32 SelectTimerSeconds() {
    static const u8 kSeconds[] = {0, 15, 30, 60};
    return kSeconds[Mgr::Get().GetSettingValue(SETTING_SS8SELECTTIMER)];
}

static CountDown *pageTimer;

// Both pages confirm each enabled player's highlighted button once their timer reads 0 (DriftSelect::AfterControlUpdate
// 0x8084E280, MultiDriftSelect's 0x8084B3AC), which Menu::LoadNextPageById honours only while the page is active.
void PassDriftPage(Pages::Menu &page, CountDown *&timer) {
    static CountDown passTimer;  // here, not at file scope: Kamek runs those constructors before StaticR.rel, which has CountDown's
    if (timer == &passTimer)
        timer = pageTimer;
    if (page.currentState != STATE_ACTIVE || page.curStateDuration != 0 || !SkipDrift(SectionMgr::sInstance->sectionParams->localPlayerCount))
        return;
    if (page.animationDirection == 1) {
        OS::Report("ss8 select: game drift page passed back\n");
        page.LoadPrevPage(page.backButton);
        return;
    }
    OS::Report("ss8 select: game drift page passed\n");
    passTimer.countdown = 0.0f;
    pageTimer = timer;
    timer = &passTimer;
}
#endif

}  // namespace SplitScreen8
