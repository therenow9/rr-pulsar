#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <Settings/Settings.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

namespace Pulsar {
namespace Race {

static bool autoAccelerating[SplitScreen8::kMaxLocal];
static bool accelerateHeld[SplitScreen8::kMaxLocal];
static void UpdateAutoAcceleration(Input::RealControllerHolder &holder, bool isPaused) {
    holder.Input::ControllerHolder::Update(isPaused);

    u32 slot = 0;  // holders 4-7 sit past the manager's own four
    while (&SplitScreen8::Holder(*Input::Manager::sInstance, slot) != &holder) ++slot;
    const bool pressed = (holder.curController->inputState.buttonActions & 0x1) != 0;
    if (Raceinfo::sInstance == nullptr || Raceinfo::sInstance->stage != RACESTAGE_RACE || holder.blockInputs || Racedata::sInstance->racesScenario.settings.gamemode == MODE_TIME_TRIAL
      || System::sInstance->IsContext(PULSAR_MODE_OTT) || Settings::Mgr::Get().GetSettingValue(Settings::SETTING_AUTOACCELERATION) == 0) {
        autoAccelerating[slot] = false;
    } else if (!isPaused) {
        if (pressed && !accelerateHeld[slot])
            autoAccelerating[slot] = !autoAccelerating[slot];
        if (autoAccelerating[slot]) {
            holder.inputStates[0].buttonActions |= 0x1;
            // Controller mappings require physical acceleration before they emit hop/drift.
            if ((holder.inputStates[0].buttonActions & 0x2) != 0 && ((holder.inputStates[1].buttonActions & 0x2) == 0 || (holder.inputStates[1].buttonActions & 0x8) != 0))
                holder.inputStates[0].buttonActions |= 0x8;
        }
    }
    accelerateHeld[slot] = pressed;
}
kmCall(0x80521784, UpdateAutoAcceleration);

}  // namespace Race
}  // namespace Pulsar
