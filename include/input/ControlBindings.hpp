#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace input {

enum class TriggerMode : std::uint8_t {
    Pressed = 0,
    Down = 1
};

enum class InputDevice : std::uint8_t {
    None = 0,
    Keyboard = 1,
    MouseButton = 2
};

enum class ActionId : std::uint8_t {
    MoveForward = 0,
    MoveBackward,
    MoveLeft,
    MoveRight,
    Jump,
    FirePrimary,
    SpectatorAscend,
    SpectatorDescend,
    SpectatorBoost,
    Count
};

constexpr std::size_t kActionCount = static_cast<std::size_t>(ActionId::Count);

struct InputToken {
    InputDevice device{InputDevice::None};
    int code{-1};

    constexpr bool isBound() const {
        return device != InputDevice::None && code >= 0;
    }
};

constexpr bool operator==(InputToken lhs, InputToken rhs) {
    return lhs.device == rhs.device && lhs.code == rhs.code;
}

constexpr bool operator!=(InputToken lhs, InputToken rhs) {
    return !(lhs == rhs);
}

constexpr InputToken unboundToken() {
    return InputToken{};
}

constexpr InputToken keyboardToken(int code) {
    return InputToken{InputDevice::Keyboard, code};
}

constexpr InputToken mouseButtonToken(int code) {
    return InputToken{InputDevice::MouseButton, code};
}

struct ActionBinding {
    std::array<InputToken, 2> slots{};
};

struct ActionDescriptor {
    ActionId id;
    const char* persistentId;
    const char* label;
    const char* category;
    TriggerMode triggerMode;
};

struct ControlBindings {
    std::array<ActionBinding, kActionCount> actions{};

    ControlBindings();

    static ControlBindings defaults();
    void resetToDefaults();

    const ActionBinding& binding(ActionId id) const;
    ActionBinding* mutableBinding(ActionId id);
};

constexpr std::size_t actionIndex(ActionId id) {
    return static_cast<std::size_t>(id);
}

const std::array<ActionDescriptor, kActionCount>& actionDescriptors();
const ActionDescriptor& descriptor(ActionId id);
const ActionBinding& defaultBinding(ActionId id);

bool isActionTriggered(const ControlBindings& bindings, ActionId id);
bool bindingConflicts(const ControlBindings& bindings, ActionId id);
bool detectFirstPressedToken(InputToken* tokenOut);

std::string tokenDisplayName(InputToken token);
std::string serializeToken(InputToken token);
bool tryParseToken(std::string_view value, InputToken* tokenOut);
std::string bindingSummary(const ActionBinding& binding);

}  // namespace input
