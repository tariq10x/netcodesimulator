#include "InputHandler3D.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool commandHas(const sim::PlayerCommand& command, sim::CommandButton button) {
    return sim::hasButton(command.buttons, button);
}

sim::PlayerCommand makeCommand(const InputHandler3D::InputState& input, std::uint32_t seq) {
    return InputHandler3D::toPlayerCommand(input, 1.0f / 60.0f, seq, 0.0f, 0.0f, 0u, 0u);
}

void testJumpUsesEdgeTriggeredGameplayField() {
    std::array<InputHandler3D::InputState, 3> inputs{};
    for (auto& input : inputs) {
        input.moveUp = true;
    }
    inputs[0].jumpPressed = true;

    const sim::PlayerCommand first = makeCommand(inputs[0], 1u);
    const sim::PlayerCommand second = makeCommand(inputs[1], 2u);
    const sim::PlayerCommand third = makeCommand(inputs[2], 3u);

    expect(commandHas(first, sim::CommandButton::Jump),
           "first poll should emit a jump when jumpPressed is true");
    expect(!commandHas(second, sim::CommandButton::Jump),
           "holding the spectator ascent field alone should not emit repeat jumps");
    expect(!commandHas(third, sim::CommandButton::Jump),
           "subsequent held polls should stay jump-free until a new press arrives");
}

void testFireUsesExplicitGameplayField() {
    std::array<InputHandler3D::InputState, 2> inputs{};
    for (auto& input : inputs) {
        input.shoot = true;
    }
    inputs[0].firePressed = true;

    const sim::PlayerCommand first = makeCommand(inputs[0], 1u);
    const sim::PlayerCommand second = makeCommand(inputs[1], 2u);

    expect(commandHas(first, sim::CommandButton::Fire),
           "first click should emit fire when firePressed is true");
    expect(!commandHas(second, sim::CommandButton::Fire),
           "legacy held shoot state alone should not emit repeat fire commands");
}

void testGameplayFieldsMatchMultiplayerCommandBits() {
    std::array<InputHandler3D::InputState, 4> inputs{};
    inputs[0].jumpPressed = true;
    inputs[1].firePressed = true;
    inputs[2].jumpPressed = true;
    inputs[2].firePressed = true;

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const sim::PlayerCommand command = makeCommand(inputs[index], static_cast<std::uint32_t>(index + 1u));
        expect(commandHas(command, sim::CommandButton::Jump) == inputs[index].jumpPressed,
               "jump command bit should match the shared gameplay jump field");
        expect(commandHas(command, sim::CommandButton::Fire) == inputs[index].firePressed,
               "fire command bit should match the shared gameplay fire field");
    }
}

}  // namespace

int main() {
    try {
        testJumpUsesEdgeTriggeredGameplayField();
        testFireUsesExplicitGameplayField();
        testGameplayFieldsMatchMultiplayerCommandBits();
        std::cout << "PlayerActionParityTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "PlayerActionParityTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
