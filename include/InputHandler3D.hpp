#pragma once
#include <algorithm>
#include <raylib.h>
#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "input/ControlBindings.hpp"
#include "sim/SimulationTypes.hpp"

class InputHandler3D {
public:
    struct InputState {
        // Bindings used for this input frame, so in-game help reflects remaps.
        input::ControlBindings controlBindings{input::ControlBindings::defaults()};
        Vector2 moveInput{0, 0};          // Bound movement axes
        Vector2 lookDelta{0, 0};          // Mouse look
        bool shoot{false};                 // Legacy fire field for existing local-only flows
        bool jumpPressed{false};           // Edge-triggered gameplay jump
        bool firePressed{false};           // Edge-triggered gameplay fire
        bool toggleInterp{false};          // Key I
        bool togglePrediction{false};      // Runtime setting request
        bool toggleKeyboardOverlay{false}; // Key K - Toggle keyboard help overlay
        bool toggleLagComp{false};         // Key L
        bool toggleUIMode{false};          // U - Toggle runtime settings menu
        bool toggleScoreboard{false};      // Tab - Hold scoreboard overlay
        bool switchTeam{false};            // Y - Open/close the explicit change-team menu
        bool menuLeft{false};              // Left arrow - move selection left in explicit menus
        bool menuRight{false};             // Right arrow - move selection right in explicit menus
        bool menuConfirm{false};           // Enter - confirm explicit menu selection or toggle mouse capture during gameplay
        bool toggleView{false};            // V - Historical switch binding for actor control or spectator target cycling
        bool toggleRealMode{false};        // R - Toggle realistic POV mode
        bool toggleEnemyAI{false};         // E - Toggle bot freeze / activation
        bool toggleSlowMotion{false};      // M - Toggle slow motion mode
        bool decreaseTimeScale{false};     // N - Slow down time scale
        bool toggleSplitMode{false};       // F - Toggle split/full screen mode
        bool toggleUIPanel{false};         // J - Compatibility shortcut for the runtime settings menu
        bool toggleSpectator{false};       // B - Toggle spectator mode
        bool toggleTickInfo{false};        // T - Toggle tick/time overlay
        bool toggleRecording{false};       // Key 5 - Toggle recording
        bool togglePlayback{false};        // Key 6 - Toggle playback play/pause
        bool stepForward{false};           // Key 8 - Step forward one frame
        bool stepBackward{false};          // Key 7 - Step backward one frame
        bool exportRecording{false};       // Key 9 - Save current replay recording
        bool resetPlayback{false};         // Key 0 - Reset replay playback to the first frame
        bool stopReplayPlayback{false};    // Backspace - Stop replay playback and return to live play
        bool addSpectatorCheckpoint{false}; // Key 1 - Save spectator checkpoint
        bool prevSpectatorCheckpoint{false}; // Key 2 - Previous checkpoint
        bool nextSpectatorCheckpoint{false}; // Key 3 - Next checkpoint
        bool deleteSpectatorCheckpoint{false}; // Key 4 - Delete current checkpoint
        bool decreaseSpectatorTransitionDuration{false}; // Key , - Decrease current checkpoint transition to next
        bool increaseSpectatorTransitionDuration{false}; // Key . - Increase current checkpoint transition to next
        bool toggleHelp{false};            // Key H - Toggle help overlay
        bool toggleRecordingOverlay{false}; // Key O - Toggle recording overlay
        bool spawnFrozenBotAhead{false};   // Key G - Spawn a frozen study bot ahead of the player
        bool toggleFovCones{false};         // Key C - Toggle player field-of-view cones
        bool toggleFocus{false};            // Key F1 - Toggle environment dimming
        bool toggleAreas{false};            // Key F2 - Toggle area visibility
        bool toggleAreaFilterGreen{false};  // Key F3 - Show only green areas
        bool toggleAreaFilterRed{false};    // Key F4 - Show only red areas
        bool moveUp{false};                // Bound spectator ascend input
        bool moveDown{false};              // Bound spectator descend input
        bool fastModifier{false};          // Bound spectator speed modifier
        bool pause{false};                 // P
        bool quit{false};                  // Q or ESC - contextual back/leave-session
    };

    struct InputFrame {
        float moveX{0.0f};
        float moveY{0.0f};
        bool jumpPressed{false};
        bool firePressed{false};
    };

    static void applyLookDelta(const InputState& input, float* yaw, float* pitch) {
        if (yaw == nullptr || pitch == nullptr) {
            return;
        }

        *yaw += input.lookDelta.x * Config::MOUSE_SENSITIVITY;
        *pitch -= input.lookDelta.y * Config::MOUSE_SENSITIVITY;
        *pitch = std::clamp(*pitch, sim::defaults::kMinPitch, sim::defaults::kMaxPitch);
    }

    static InputFrame toInputFrame(const InputState& input) {
        InputFrame frame;
        frame.moveX = input.moveInput.x;
        frame.moveY = input.moveInput.y;
        frame.jumpPressed = input.jumpPressed;
        frame.firePressed = input.firePressed;
        return frame;
    }

    static InputFrame toPredictionFrame(const InputState& input, bool predictionEnabled) {
        if (!predictionEnabled) {
            return InputFrame{};
        }
        return toInputFrame(input);
    }

    static sim::PlayerCommand toPlayerCommand(const InputFrame& input,
                                              float dtSeconds,
                                              std::uint32_t seq,
                                              float yaw,
                                              float pitch,
                                              std::uint64_t viewedServerTimeUs,
                                              std::uint32_t interpDelayMs = 0u) {
        sim::PlayerCommand command;
        command.seq = seq;
        command.dtSeconds = dtSeconds;
        command.moveX = input.moveX;
        command.moveY = input.moveY;
        command.yaw = yaw;
        command.pitch = pitch;
        command.viewedServerTimeUs = viewedServerTimeUs;
        command.interpDelayMs = interpDelayMs;

        if (input.jumpPressed) {
            command.buttons |= sim::commandButtonBit(sim::CommandButton::Jump);
        }
        if (input.firePressed) {
            command.buttons |= sim::commandButtonBit(sim::CommandButton::Fire);
        }
        return command;
    }

    static sim::PlayerCommand toPlayerCommand(const InputState& input,
                                              float dtSeconds,
                                              std::uint32_t seq,
                                              float yaw,
                                              float pitch,
                                              std::uint64_t viewedServerTimeUs,
                                              std::uint32_t interpDelayMs = 0u) {
        return toPlayerCommand(toInputFrame(input),
                               dtSeconds,
                               seq,
                               yaw,
                               pitch,
                               viewedServerTimeUs,
                               interpDelayMs);
    }

    static InputState poll(const input::ControlBindings& bindings) {
        InputState input;
        input.controlBindings = bindings;

        // Movement bindings stay separate from the semantic gameplay command fields.
        if (input::isActionTriggered(bindings, input::ActionId::MoveForward)) input.moveInput.y += 1.0f;
        if (input::isActionTriggered(bindings, input::ActionId::MoveBackward)) input.moveInput.y -= 1.0f;
        if (input::isActionTriggered(bindings, input::ActionId::MoveLeft)) input.moveInput.x -= 1.0f;
        if (input::isActionTriggered(bindings, input::ActionId::MoveRight)) input.moveInput.x += 1.0f;

        // Look (Mouse)
        input.lookDelta = display::mouseDelta();

        // Actions
        input.jumpPressed = input::isActionTriggered(bindings, input::ActionId::Jump);
        input.firePressed = input::isActionTriggered(bindings, input::ActionId::FirePrimary);
        input.shoot = input.firePressed;

        // Toggles
        input.toggleInterp = IsKeyPressed(KEY_I);
        input.toggleKeyboardOverlay = IsKeyPressed(KEY_K);
        input.toggleLagComp = IsKeyPressed(KEY_L);
        input.toggleUIMode = IsKeyPressed(KEY_U);
        input.toggleScoreboard = IsKeyDown(KEY_TAB);
        input.switchTeam = IsKeyPressed(KEY_Y);
        input.menuLeft = IsKeyPressed(KEY_LEFT);
        input.menuRight = IsKeyPressed(KEY_RIGHT);
        input.menuConfirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
        input.toggleView = IsKeyPressed(KEY_V);
        input.toggleRealMode = IsKeyPressed(KEY_R);
        input.toggleEnemyAI = IsKeyPressed(KEY_E);
        input.toggleSlowMotion = IsKeyPressed(KEY_M);
        input.decreaseTimeScale = IsKeyPressed(KEY_N);
        input.toggleSplitMode = IsKeyPressed(KEY_F);
        input.toggleUIPanel = IsKeyPressed(KEY_J);
        input.toggleSpectator = IsKeyPressed(KEY_B);
        input.toggleTickInfo = IsKeyPressed(KEY_T);
        input.toggleRecording = IsKeyPressed(KEY_FIVE);
        input.togglePlayback = IsKeyPressed(KEY_SIX);
        input.stepForward = IsKeyPressed(KEY_EIGHT);
        input.stepBackward = IsKeyPressed(KEY_SEVEN);
        input.exportRecording = IsKeyPressed(KEY_NINE);
        input.resetPlayback = IsKeyPressed(KEY_ZERO);
        input.stopReplayPlayback = IsKeyPressed(KEY_BACKSPACE);
        input.addSpectatorCheckpoint = IsKeyPressed(KEY_ONE);
        input.prevSpectatorCheckpoint = IsKeyPressed(KEY_TWO);
        input.nextSpectatorCheckpoint = IsKeyPressed(KEY_THREE);
        input.deleteSpectatorCheckpoint = IsKeyPressed(KEY_FOUR);
        input.decreaseSpectatorTransitionDuration = IsKeyPressed(KEY_COMMA);
        input.increaseSpectatorTransitionDuration = IsKeyPressed(KEY_PERIOD);
        input.toggleHelp = IsKeyPressed(KEY_H);
        input.toggleRecordingOverlay = IsKeyPressed(KEY_O);
        input.spawnFrozenBotAhead = IsKeyPressed(KEY_G);
        input.toggleFovCones = IsKeyPressed(KEY_C);
        input.toggleFocus = IsKeyPressed(KEY_F1);
        input.toggleAreas = IsKeyPressed(KEY_F2);
        input.toggleAreaFilterGreen = IsKeyPressed(KEY_F3);
        input.toggleAreaFilterRed = IsKeyPressed(KEY_F4);

        input.moveUp = input::isActionTriggered(bindings, input::ActionId::SpectatorAscend);
        input.moveDown = input::isActionTriggered(bindings, input::ActionId::SpectatorDescend);
        input.fastModifier = input::isActionTriggered(bindings, input::ActionId::SpectatorBoost);

        // System
        input.pause = IsKeyPressed(KEY_P);
        input.quit = IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_ESCAPE);

        return input;
    }

    static InputState poll() {
        static const input::ControlBindings kDefaultBindings = input::ControlBindings::defaults();
        return poll(kDefaultBindings);
    }
};
