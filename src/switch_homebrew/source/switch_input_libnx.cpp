// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "switch_input.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <switch.h>

namespace Azahar::Switch {
namespace {

constexpr float StickScale = 32767.0f;

struct InputState {
    PadState pad{};
    bool initialized = false;
    u64 buttons = 0;
    HidAnalogStickState sticks[2]{};
    bool touch_pressed = false;
    std::uint32_t touch_x = 0;
    std::uint32_t touch_y = 0;
    u64 last_update_tick = 0;
    std::mutex mutex;
};

InputState input_state;

u64 GetButtonMask(SwitchButton button) {
    switch (button) {
    case SwitchButton::A:
        return HidNpadButton_A;
    case SwitchButton::B:
        return HidNpadButton_B;
    case SwitchButton::X:
        return HidNpadButton_X;
    case SwitchButton::Y:
        return HidNpadButton_Y;
    case SwitchButton::Up:
        return HidNpadButton_Up;
    case SwitchButton::Down:
        return HidNpadButton_Down;
    case SwitchButton::Left:
        return HidNpadButton_Left;
    case SwitchButton::Right:
        return HidNpadButton_Right;
    case SwitchButton::L:
        return HidNpadButton_L;
    case SwitchButton::R:
        return HidNpadButton_R;
    case SwitchButton::ZL:
        return HidNpadButton_ZL;
    case SwitchButton::ZR:
        return HidNpadButton_ZR;
    case SwitchButton::Plus:
        return HidNpadButton_Plus;
    case SwitchButton::Minus:
        return HidNpadButton_Minus;
    }
    return 0;
}

void InitializeSwitchInputLocked() {
    if (input_state.initialized) {
        return;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&input_state.pad);
    hidInitializeTouchScreen();
    input_state.initialized = true;
}

float NormalizeStickAxis(s32 value) {
    return std::clamp(static_cast<float>(value) / StickScale, -1.0f, 1.0f);
}

} // namespace

void UpdateSwitchInput() {
    std::scoped_lock lock(input_state.mutex);
    InitializeSwitchInputLocked();

    const u64 now = armGetSystemTick();
    if (now - input_state.last_update_tick < 1000) {
        return;
    }
    input_state.last_update_tick = now;

    padUpdate(&input_state.pad);
    input_state.buttons = padGetButtons(&input_state.pad);
    input_state.sticks[0] = padGetStickPos(&input_state.pad, 0);
    input_state.sticks[1] = padGetStickPos(&input_state.pad, 1);

    HidTouchScreenState touch_state{};
    if (hidGetTouchScreenStates(&touch_state, 1) > 0 && touch_state.count > 0) {
        input_state.touch_pressed = true;
        input_state.touch_x = touch_state.touches[0].x;
        input_state.touch_y = touch_state.touches[0].y;
    } else {
        input_state.touch_pressed = false;
    }
}

std::tuple<float, float> GetSwitchStick(unsigned stick_index) {
    UpdateSwitchInput();
    std::scoped_lock lock(input_state.mutex);
    const HidAnalogStickState& stick = input_state.sticks[std::min<unsigned>(stick_index, 1)];
    return {NormalizeStickAxis(stick.x), NormalizeStickAxis(stick.y)};
}

bool IsSwitchButtonPressed(SwitchButton button) {
    UpdateSwitchInput();
    std::scoped_lock lock(input_state.mutex);
    return (input_state.buttons & GetButtonMask(button)) != 0;
}

bool GetSwitchTouchPoint(std::uint32_t* x, std::uint32_t* y) {
    UpdateSwitchInput();
    std::scoped_lock lock(input_state.mutex);
    if (!input_state.touch_pressed) {
        return false;
    }
    if (x != nullptr) {
        *x = input_state.touch_x;
    }
    if (y != nullptr) {
        *y = input_state.touch_y;
    }
    return true;
}

} // namespace Azahar::Switch
