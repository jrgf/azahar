// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <string>
#include <tuple>

namespace Azahar::Switch {

enum class SwitchButton : std::uint8_t {
    A,
    B,
    X,
    Y,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    ZL,
    ZR,
    Plus,
    Minus,
};

void RegisterSwitchInput();
void UpdateSwitchInput();
std::string GetSwitchButtonParam(SwitchButton button);
std::string GetSwitchAnalogParam(unsigned stick_index);
std::tuple<float, float> GetSwitchStick(unsigned stick_index);
bool IsSwitchButtonPressed(SwitchButton button);
bool GetSwitchTouchPoint(std::uint32_t* x, std::uint32_t* y);

} // namespace Azahar::Switch
