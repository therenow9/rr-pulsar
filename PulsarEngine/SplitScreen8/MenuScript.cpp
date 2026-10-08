#ifdef SS8_MENU_SCRIPT
#include <kamek.hpp>
#include <core/rvl/PAD.hpp>
#include <core/rvl/OS/OS.hpp>
#include <MarioKartWii/Input/InputManager.hpp>

#ifdef SS8_DEBUG_BOOT
#error "a menu script and the debug boot both hook Input::Manager::CopyPADStatus's call (0x80520220)"
#endif

// Debug build only: presses GC pad buttons on ports 1-4 at fixed frames from boot, so menu paths (the
// 3P character select, M4's join screen) run with no one at the pads. tools/build_code.py
// --menu-script turns a script file into SS8_MENU_STEPS; docs/toolchain.md has the format.

namespace SplitScreen8 {

struct MenuStep {
    u32 frame;  // channel 0's pad reads since boot
    u32 port;  // 0-3
    u32 buttons;  // PAD::PAD_BUTTON_*
    u32 hold;  // frames
};
static const MenuStep menuSteps[] = {SS8_MENU_STEPS};
static u32 menuFrames;

// GCNController::UpdateImpl+0x70 calls Input::Manager::CopyPADStatus(manager, channel, &padStatus) for
// each plugged-in port every frame, and reads padStatus after it, as DebugBoot.cpp's race script does.
typedef u32 (*CopyPADStatusFn)(Input::Manager *, u32, PAD::Status *);
static const CopyPADStatusFn copyPADStatus = reinterpret_cast<CopyPADStatusFn>(0x80524628);

static u32 CopyPADStatusMenuScript(Input::Manager *input, u32 channel, PAD::Status *status) {
    const u32 result = copyPADStatus(input, channel, status);
    if (status == nullptr) return result;
    for (u32 i = 0; i < sizeof(menuSteps) / sizeof(menuSteps[0]); ++i) {
        const MenuStep &step = menuSteps[i];
        if (step.port != channel || menuFrames < step.frame || menuFrames >= step.frame + step.hold) continue;
        if (menuFrames == step.frame) OS::Report("ss8 menu: step %u frame %u port %u press %04x\n", i, menuFrames, channel + 1, step.buttons);
        status->buttons |= step.buttons;
    }
    if (channel == 0) ++menuFrames;
    return result;
}
kmCall(0x80520220, CopyPADStatusMenuScript);

}  // namespace SplitScreen8
#endif
