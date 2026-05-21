// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "switch_input.h"

#include "common/param_package.h"
#include "core/frontend/input.h"

#include <memory>
#include <mutex>

namespace Azahar::Switch {
namespace {

std::once_flag register_once;

class SwitchButtonDevice final : public Input::ButtonDevice {
public:
    explicit SwitchButtonDevice(SwitchButton button_) : button(button_) {}

    bool GetStatus() const override {
        return IsSwitchButtonPressed(button);
    }

private:
    SwitchButton button;
};

class SwitchButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<SwitchButtonDevice>(
            static_cast<SwitchButton>(params.Get("button", 0)));
    }
};

class SwitchAnalogDevice final : public Input::AnalogDevice {
public:
    explicit SwitchAnalogDevice(unsigned stick_index_) : stick_index(stick_index_) {}

    std::tuple<float, float> GetStatus() const override {
        return GetSwitchStick(stick_index);
    }

private:
    unsigned stick_index;
};

class SwitchAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<SwitchAnalogDevice>(static_cast<unsigned>(params.Get("stick", 0)));
    }
};

} // namespace

void RegisterSwitchInput() {
    std::call_once(register_once, [] {
        Input::RegisterFactory<Input::ButtonDevice>("switch",
                                                    std::make_shared<SwitchButtonFactory>());
        Input::RegisterFactory<Input::AnalogDevice>("switch",
                                                    std::make_shared<SwitchAnalogFactory>());
    });
}

std::string GetSwitchButtonParam(SwitchButton button) {
    return Common::ParamPackage{{"engine", "switch"},
                                {"button", std::to_string(static_cast<int>(button))}}
        .Serialize();
}

std::string GetSwitchAnalogParam(unsigned stick_index) {
    return Common::ParamPackage{{"engine", "switch"}, {"stick", std::to_string(stick_index)}}
        .Serialize();
}

} // namespace Azahar::Switch
