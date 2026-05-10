#include "input/ControlBindings.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <vector>

#include <raylib.h>

namespace input {
namespace {

struct NamedToken {
    InputToken token;
    const char* serialized;
    const char* display;
};

using NamedTokenList = std::array<NamedToken, 62>;

constexpr NamedTokenList kNamedTokens{{
    {keyboardToken(KEY_A), "key:a", "A"},
    {keyboardToken(KEY_B), "key:b", "B"},
    {keyboardToken(KEY_C), "key:c", "C"},
    {keyboardToken(KEY_D), "key:d", "D"},
    {keyboardToken(KEY_E), "key:e", "E"},
    {keyboardToken(KEY_F), "key:f", "F"},
    {keyboardToken(KEY_G), "key:g", "G"},
    {keyboardToken(KEY_H), "key:h", "H"},
    {keyboardToken(KEY_I), "key:i", "I"},
    {keyboardToken(KEY_J), "key:j", "J"},
    {keyboardToken(KEY_K), "key:k", "K"},
    {keyboardToken(KEY_L), "key:l", "L"},
    {keyboardToken(KEY_M), "key:m", "M"},
    {keyboardToken(KEY_N), "key:n", "N"},
    {keyboardToken(KEY_O), "key:o", "O"},
    {keyboardToken(KEY_P), "key:p", "P"},
    {keyboardToken(KEY_Q), "key:q", "Q"},
    {keyboardToken(KEY_R), "key:r", "R"},
    {keyboardToken(KEY_S), "key:s", "S"},
    {keyboardToken(KEY_T), "key:t", "T"},
    {keyboardToken(KEY_U), "key:u", "U"},
    {keyboardToken(KEY_V), "key:v", "V"},
    {keyboardToken(KEY_W), "key:w", "W"},
    {keyboardToken(KEY_X), "key:x", "X"},
    {keyboardToken(KEY_Y), "key:y", "Y"},
    {keyboardToken(KEY_Z), "key:z", "Z"},
    {keyboardToken(KEY_ZERO), "key:0", "0"},
    {keyboardToken(KEY_ONE), "key:1", "1"},
    {keyboardToken(KEY_TWO), "key:2", "2"},
    {keyboardToken(KEY_THREE), "key:3", "3"},
    {keyboardToken(KEY_FOUR), "key:4", "4"},
    {keyboardToken(KEY_FIVE), "key:5", "5"},
    {keyboardToken(KEY_SIX), "key:6", "6"},
    {keyboardToken(KEY_SEVEN), "key:7", "7"},
    {keyboardToken(KEY_EIGHT), "key:8", "8"},
    {keyboardToken(KEY_NINE), "key:9", "9"},
    {keyboardToken(KEY_SPACE), "key:space", "Space"},
    {keyboardToken(KEY_TAB), "key:tab", "Tab"},
    {keyboardToken(KEY_ENTER), "key:enter", "Enter"},
    {keyboardToken(KEY_KP_ENTER), "key:keypad_enter", "Keypad Enter"},
    {keyboardToken(KEY_ESCAPE), "key:escape", "Esc"},
    {keyboardToken(KEY_BACKSPACE), "key:backspace", "Backspace"},
    {keyboardToken(KEY_DELETE), "key:delete", "Delete"},
    {keyboardToken(KEY_LEFT_SHIFT), "key:left_shift", "Left Shift"},
    {keyboardToken(KEY_RIGHT_SHIFT), "key:right_shift", "Right Shift"},
    {keyboardToken(KEY_LEFT_CONTROL), "key:left_ctrl", "Left Ctrl"},
    {keyboardToken(KEY_RIGHT_CONTROL), "key:right_ctrl", "Right Ctrl"},
    {keyboardToken(KEY_LEFT_ALT), "key:left_alt", "Left Alt"},
    {keyboardToken(KEY_RIGHT_ALT), "key:right_alt", "Right Alt"},
    {keyboardToken(KEY_UP), "key:up", "Up"},
    {keyboardToken(KEY_DOWN), "key:down", "Down"},
    {keyboardToken(KEY_LEFT), "key:left", "Left"},
    {keyboardToken(KEY_RIGHT), "key:right", "Right"},
    {keyboardToken(KEY_COMMA), "key:comma", ","},
    {keyboardToken(KEY_PERIOD), "key:period", "."},
    {mouseButtonToken(MOUSE_BUTTON_LEFT), "mouse:left", "Mouse Left"},
    {mouseButtonToken(MOUSE_BUTTON_RIGHT), "mouse:right", "Mouse Right"},
    {mouseButtonToken(MOUSE_BUTTON_MIDDLE), "mouse:middle", "Mouse Middle"},
    {mouseButtonToken(MOUSE_BUTTON_SIDE), "mouse:side", "Mouse Side"},
    {mouseButtonToken(MOUSE_BUTTON_EXTRA), "mouse:extra", "Mouse Extra"},
    {mouseButtonToken(MOUSE_BUTTON_FORWARD), "mouse:forward", "Mouse Forward"},
    {mouseButtonToken(MOUSE_BUTTON_BACK), "mouse:back", "Mouse Back"},
}};

constexpr std::array<ActionDescriptor, kActionCount> kActionDescriptors{{
    {ActionId::MoveForward, "move_forward", "Move Forward", "Movement", TriggerMode::Down},
    {ActionId::MoveBackward, "move_backward", "Move Backward", "Movement", TriggerMode::Down},
    {ActionId::MoveLeft, "move_left", "Move Left", "Movement", TriggerMode::Down},
    {ActionId::MoveRight, "move_right", "Move Right", "Movement", TriggerMode::Down},
    {ActionId::Jump, "jump", "Jump", "Movement", TriggerMode::Pressed},
    {ActionId::FirePrimary, "fire_primary", "Fire", "Combat", TriggerMode::Pressed},
    {ActionId::SpectatorAscend, "spectator_ascend", "Spectator Up", "Spectator", TriggerMode::Down},
    {ActionId::SpectatorDescend, "spectator_descend", "Spectator Down", "Spectator", TriggerMode::Down},
    {ActionId::SpectatorBoost, "spectator_boost", "Spectator Boost", "Spectator", TriggerMode::Down},
}};

constexpr std::array<ActionBinding, kActionCount> kDefaultBindings{{
    ActionBinding{{keyboardToken(KEY_W), unboundToken()}},
    ActionBinding{{keyboardToken(KEY_S), unboundToken()}},
    ActionBinding{{keyboardToken(KEY_A), unboundToken()}},
    ActionBinding{{keyboardToken(KEY_D), unboundToken()}},
    ActionBinding{{keyboardToken(KEY_SPACE), unboundToken()}},
    ActionBinding{{mouseButtonToken(MOUSE_BUTTON_LEFT), unboundToken()}},
    ActionBinding{{keyboardToken(KEY_SPACE), unboundToken()}},
    ActionBinding{{keyboardToken(KEY_LEFT_CONTROL), keyboardToken(KEY_RIGHT_CONTROL)}},
    ActionBinding{{keyboardToken(KEY_LEFT_SHIFT), keyboardToken(KEY_RIGHT_SHIFT)}},
}};

const NamedToken* findNamedToken(InputToken token) {
    for (const auto& candidate : kNamedTokens) {
        if (candidate.token == token) {
            return &candidate;
        }
    }
    return nullptr;
}

const NamedToken* findNamedToken(std::string_view serialized) {
    for (const auto& candidate : kNamedTokens) {
        if (serialized == candidate.serialized) {
            return &candidate;
        }
    }
    return nullptr;
}

bool tokenTriggered(InputToken token, TriggerMode mode) {
    if (!token.isBound()) {
        return false;
    }

    switch (token.device) {
        case InputDevice::Keyboard:
            return mode == TriggerMode::Pressed ? IsKeyPressed(token.code) : IsKeyDown(token.code);
        case InputDevice::MouseButton:
            return mode == TriggerMode::Pressed ? IsMouseButtonPressed(token.code)
                                                : IsMouseButtonDown(token.code);
        case InputDevice::None:
            return false;
    }
    return false;
}

std::string fallbackTokenString(InputToken token, const char* prefix) {
    std::ostringstream builder;
    builder << prefix << ' ' << token.code;
    return builder.str();
}

}  // namespace

ControlBindings::ControlBindings() {
    resetToDefaults();
}

ControlBindings ControlBindings::defaults() {
    ControlBindings bindings;
    bindings.actions = kDefaultBindings;
    return bindings;
}

void ControlBindings::resetToDefaults() {
    actions = kDefaultBindings;
}

const ActionBinding& ControlBindings::binding(ActionId id) const {
    return actions[actionIndex(id)];
}

ActionBinding* ControlBindings::mutableBinding(ActionId id) {
    return &actions[actionIndex(id)];
}

const std::array<ActionDescriptor, kActionCount>& actionDescriptors() {
    return kActionDescriptors;
}

const ActionDescriptor& descriptor(ActionId id) {
    return kActionDescriptors[actionIndex(id)];
}

const ActionBinding& defaultBinding(ActionId id) {
    return kDefaultBindings[actionIndex(id)];
}

bool isActionTriggered(const ControlBindings& bindings, ActionId id) {
    const ActionBinding& binding = bindings.binding(id);
    const TriggerMode mode = descriptor(id).triggerMode;
    for (const InputToken token : binding.slots) {
        if (tokenTriggered(token, mode)) {
            return true;
        }
    }
    return false;
}

bool bindingConflicts(const ControlBindings& bindings, ActionId id) {
    const ActionBinding& targetBinding = bindings.binding(id);
    for (const InputToken token : targetBinding.slots) {
        if (!token.isBound()) {
            continue;
        }
        for (const ActionDescriptor& candidate : kActionDescriptors) {
            if (candidate.id == id) {
                continue;
            }
            for (const InputToken other : bindings.binding(candidate.id).slots) {
                if (token == other) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool detectFirstPressedToken(InputToken* tokenOut) {
    if (tokenOut == nullptr) {
        return false;
    }

    const int keyCode = GetKeyPressed();
    if (keyCode != 0) {
        *tokenOut = keyboardToken(keyCode);
        return true;
    }

    for (int button : {MOUSE_BUTTON_LEFT,
                       MOUSE_BUTTON_RIGHT,
                       MOUSE_BUTTON_MIDDLE,
                       MOUSE_BUTTON_SIDE,
                       MOUSE_BUTTON_EXTRA,
                       MOUSE_BUTTON_FORWARD,
                       MOUSE_BUTTON_BACK}) {
        if (IsMouseButtonPressed(button)) {
            *tokenOut = mouseButtonToken(button);
            return true;
        }
    }

    return false;
}

std::string tokenDisplayName(InputToken token) {
    if (!token.isBound()) {
        return "Unbound";
    }

    if (const NamedToken* named = findNamedToken(token)) {
        return named->display;
    }

    switch (token.device) {
        case InputDevice::Keyboard:
            return fallbackTokenString(token, "Key");
        case InputDevice::MouseButton:
            return fallbackTokenString(token, "Mouse");
        case InputDevice::None:
            break;
    }
    return "Unbound";
}

std::string serializeToken(InputToken token) {
    if (!token.isBound()) {
        return "none";
    }

    if (const NamedToken* named = findNamedToken(token)) {
        return named->serialized;
    }

    std::ostringstream builder;
    if (token.device == InputDevice::Keyboard) {
        builder << "keycode:" << token.code;
    } else if (token.device == InputDevice::MouseButton) {
        builder << "mousecode:" << token.code;
    } else {
        builder << "none";
    }
    return builder.str();
}

bool tryParseToken(std::string_view value, InputToken* tokenOut) {
    if (tokenOut == nullptr) {
        return false;
    }

    if (value == "none" || value.empty()) {
        *tokenOut = unboundToken();
        return true;
    }

    if (const NamedToken* named = findNamedToken(value)) {
        *tokenOut = named->token;
        return true;
    }

    const auto parseNumericSuffix = [&](std::string_view prefix, InputDevice device) {
        if (value.rfind(prefix, 0) != 0) {
            return false;
        }

        const std::string numberText = std::string(value.substr(prefix.size()));
        try {
            const int code = std::stoi(numberText);
            *tokenOut = InputToken{device, code};
            return true;
        } catch (...) {
            return false;
        }
    };

    return parseNumericSuffix("keycode:", InputDevice::Keyboard) ||
           parseNumericSuffix("mousecode:", InputDevice::MouseButton);
}

std::string bindingSummary(const ActionBinding& binding) {
    std::ostringstream builder;
    bool wroteAny = false;
    for (const InputToken token : binding.slots) {
        if (!token.isBound()) {
            continue;
        }
        if (wroteAny) {
            builder << " / ";
        }
        builder << tokenDisplayName(token);
        wroteAny = true;
    }

    if (!wroteAny) {
        return "Unbound";
    }
    return builder.str();
}

}  // namespace input
