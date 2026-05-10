#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <raylib.h>

#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "TypographyService.hpp"
#include "input/ControlBindings.hpp"

namespace client {

class KeyboardHelpOverlay {
public:
    static Rectangle iconBounds() {
        return Rectangle{
            24.0f,
            static_cast<float>(Config::SCREEN_HEIGHT) - 78.0f,
            118.0f,
            54.0f
        };
    }

    static void renderIcon(bool active) {
        const Rectangle bounds = iconBounds();
        const Color fill = active ? Color{42, 68, 104, 238} : Color{16, 21, 30, 224};
        const Color border = active ? Color{117, 184, 255, 255} : Color{96, 112, 138, 220};

        DrawRectangleRounded(Rectangle{bounds.x + 3.0f,
                                       bounds.y + 4.0f,
                                       bounds.width,
                                       bounds.height},
                             0.18f,
                             10,
                             Fade(BLACK, 0.28f));
        DrawRectangleRounded(bounds, 0.18f, 10, fill);
        DrawRectangleRoundedLines(bounds, 0.18f, 10, border);

        const Rectangle keyboard{bounds.x + 12.0f, bounds.y + 13.0f, 58.0f, 29.0f};
        DrawRectangleRounded(keyboard, 0.12f, 8, Fade(RAYWHITE, active ? 0.22f : 0.14f));
        DrawRectangleRoundedLines(keyboard, 0.12f, 8, Fade(RAYWHITE, 0.55f));
        for (int row = 0; row < 3; ++row) {
            const float keyY = keyboard.y + 5.0f + static_cast<float>(row) * 7.0f;
            const int keyCount = row == 2 ? 5 : 6;
            for (int key = 0; key < keyCount; ++key) {
                const float keyX = keyboard.x + 5.0f + static_cast<float>(key) * 8.0f +
                                   (row == 1 ? 3.0f : 0.0f);
                DrawRectangleRounded(Rectangle{keyX,
                                               keyY,
                                               row == 2 && key == 2 ? 14.0f : 5.0f,
                                               4.0f},
                                     0.4f,
                                     4,
                                     Fade(RAYWHITE, 0.72f));
            }
        }

        const Rectangle badge{bounds.x + 78.0f, bounds.y + 11.0f, 27.0f, 31.0f};
        DrawRectangleRounded(badge, 0.22f, 8, active ? Color{117, 184, 255, 255}
                                                     : Color{50, 62, 82, 255});
        DrawRectangleRoundedLines(badge, 0.22f, 8, Fade(RAYWHITE, 0.35f));
        TypographyService& typography = TypographyService::shared();
        const TypographyStyle& style = typography.style(TypographyStyleId::OverlayAccent);
        typography.drawCentered(TypographyStyleId::OverlayAccent,
                                "K",
                                badge.x + badge.width * 0.5f,
                                badge.y + badge.height * 0.5f - style.lineHeight * 0.5f,
                                RAYWHITE);
    }

    static void render(const input::ControlBindings& bindings) {
        const Rectangle panel = panelBounds();
        const Rectangle keyboardArea = keyboardBounds(panel);
        const Rectangle mouseArea = mouseBounds(keyboardArea);
        const std::vector<KeyView> keys = buildKeyboard(keyboardArea);
        const std::vector<MouseButtonView> mouseButtons = buildMouse(mouseArea);
        const std::vector<Callout> callouts = buildCallouts(bindings);
        const HoveredAnchor hover = hoveredAnchor(keys, mouseButtons, display::mousePosition());
        const std::vector<Callout> focusedCallouts = calloutsForHover(callouts, hover);
        const std::vector<CalloutLayout> layouts = layoutFocusedCallouts(focusedCallouts, panel);

        DrawRectangle(0, 0, Config::SCREEN_WIDTH, Config::SCREEN_HEIGHT, Fade(BLACK, 0.52f));
        DrawRectangleRounded(Rectangle{panel.x + 6.0f, panel.y + 8.0f, panel.width, panel.height},
                             0.035f,
                             18,
                             Fade(BLACK, 0.30f));
        DrawRectangleRounded(panel, 0.035f, 18, Color{12, 16, 24, 246});
        DrawRectangleRoundedLines(panel, 0.035f, 18, Color{88, 132, 184, 255});

        drawText(TypographyStyleId::ScoreboardTitle,
                 "Keyboard Map",
                 Vector2{panel.x + 34.0f, panel.y + 24.0f},
                 RAYWHITE);
        drawText(TypographyStyleId::Body,
                 "K closes | hover colored keys or mouse buttons for bindings",
                 Vector2{panel.x + 36.0f, panel.y + 70.0f},
                 Color{178, 194, 218, 255});

        drawColumnTitle("Colored keys have actions", panel.x + 36.0f, panel.y + 118.0f);
        drawColumnTitle("Focused binding", panel.x + panel.width - 426.0f, panel.y + 118.0f);

        for (const KeyView& key : keys) {
            renderKey(key, callouts, focusedCallouts, hover);
        }
        renderMouse(mouseButtons, callouts, focusedCallouts, hover);

        Vector2 anchor{};
        const bool hasAnchor = hoverAnchorPosition(hover, keys, mouseButtons, &anchor);
        if (hasAnchor && !layouts.empty()) {
            for (const CalloutLayout& layout : layouts) {
                const Vector2 target = layout.callout.side == Side::Left
                    ? Vector2{layout.box.x + layout.box.width,
                              layout.box.y + layout.box.height * 0.5f}
                    : Vector2{layout.box.x, layout.box.y + layout.box.height * 0.5f};
                DrawLineEx(anchor, target, 4.0f, Fade(layout.callout.color, 0.74f));
            }
        }

        if (layouts.empty()) {
            renderHoverHint(panel, hover.active);
        }

        for (const CalloutLayout& layout : layouts) {
            renderCallout(layout);
        }
    }

private:
    enum class Side {
        Left,
        Right
    };

    enum class AnchorKind {
        Keyboard,
        Mouse
    };

    struct HoveredAnchor {
        bool active{false};
        AnchorKind kind{AnchorKind::Keyboard};
        int code{KEY_NULL};
        int mouseButton{MOUSE_BUTTON_LEFT};
    };

    struct KeySpec {
        int code;
        const char* label;
        float width;
    };

    struct KeyView {
        int code;
        const char* label;
        Rectangle rect;
    };

    struct MouseButtonView {
        int button;
        const char* label;
        Rectangle rect;
    };

    struct Callout {
        AnchorKind anchorKind{AnchorKind::Keyboard};
        std::vector<int> codes{};
        int mouseButton{MOUSE_BUTTON_LEFT};
        std::string label{};
        Color color{RAYWHITE};
        Side side{Side::Left};
    };

    struct CalloutLayout {
        Callout callout{};
        Rectangle box{};
    };

    static Rectangle panelBounds() {
        return Rectangle{130.0f, 86.0f, 1660.0f, 908.0f};
    }

    static Rectangle keyboardBounds(Rectangle panel) {
        return Rectangle{
            panel.x + (panel.width - 900.0f) * 0.5f,
            panel.y + 246.0f,
            900.0f,
            330.0f
        };
    }

    static Rectangle mouseBounds(Rectangle keyboardArea) {
        return Rectangle{
            keyboardArea.x + keyboardArea.width * 0.5f - 100.0f,
            keyboardArea.y + 346.0f,
            200.0f,
            154.0f
        };
    }

    static void renderHoverHint(Rectangle panel, bool hoveringUnboundControl) {
        const Rectangle hint{
            panel.x + panel.width - 426.0f,
            panel.y + 156.0f,
            390.0f,
            112.0f
        };
        DrawRectangleRounded(hint, 0.08f, 12, Color{20, 26, 38, 232});
        DrawRectangleRoundedLines(hint, 0.08f, 12, Color{74, 92, 120, 255});
        drawText(TypographyStyleId::OverlayAccent,
                 hoveringUnboundControl ? "No binding here" : "Hover a colored key",
                 Vector2{hint.x + 18.0f, hint.y + 18.0f},
                 RAYWHITE);
        drawText(TypographyStyleId::Caption,
                 hoveringUnboundControl
                     ? "This control has no mapped action in the current context."
                     : "Connectors appear only for the key or mouse button under the cursor.",
                 Vector2{hint.x + 18.0f, hint.y + 62.0f},
                 Color{178, 194, 218, 255});
    }

    static void drawText(TypographyStyleId styleId,
                         std::string_view text,
                         Vector2 position,
                         Color color) {
        TypographyService::shared().draw(styleId, text, position, color);
    }

    static void drawColumnTitle(std::string_view text, float x, float y) {
        drawText(TypographyStyleId::OverlayAccent, text, Vector2{x, y}, Color{118, 184, 255, 255});
    }

    static Color categoryColor(std::string_view category) {
        if (category == "Movement") {
            return Color{93, 170, 255, 255};
        }
        if (category == "Combat") {
            return Color{255, 119, 72, 255};
        }
        if (category == "Spectator") {
            return Color{255, 196, 95, 255};
        }
        return Color{196, 210, 232, 255};
    }

    static Side sideForCategory(std::string_view category) {
        return category == "Spectator" ? Side::Right : Side::Left;
    }

    static Color runtimeColor() {
        return Color{144, 204, 255, 255};
    }

    static Color replayColor() {
        return Color{216, 174, 255, 255};
    }

    static Color studyColor() {
        return Color{118, 218, 172, 255};
    }

    static bool sameCodes(const std::vector<int>& lhs, const std::vector<int>& rhs) {
        return lhs == rhs;
    }

    static HoveredAnchor hoveredAnchor(const std::vector<KeyView>& keys,
                                       const std::vector<MouseButtonView>& mouseButtons,
                                       Vector2 mouse) {
        for (const KeyView& key : keys) {
            if (CheckCollisionPointRec(mouse, key.rect)) {
                HoveredAnchor hover;
                hover.active = true;
                hover.kind = AnchorKind::Keyboard;
                hover.code = key.code;
                return hover;
            }
        }
        for (const MouseButtonView& button : mouseButtons) {
            if (CheckCollisionPointRec(mouse, button.rect)) {
                HoveredAnchor hover;
                hover.active = true;
                hover.kind = AnchorKind::Mouse;
                hover.mouseButton = button.button;
                return hover;
            }
        }
        return HoveredAnchor{};
    }

    static bool calloutMatchesHover(const Callout& callout, HoveredAnchor hover) {
        if (!hover.active || callout.anchorKind != hover.kind) {
            return false;
        }
        if (hover.kind == AnchorKind::Mouse) {
            return callout.mouseButton == hover.mouseButton;
        }
        return std::find(callout.codes.begin(), callout.codes.end(), hover.code) !=
               callout.codes.end();
    }

    static std::vector<Callout> calloutsForHover(const std::vector<Callout>& callouts,
                                                 HoveredAnchor hover) {
        std::vector<Callout> focused;
        if (!hover.active) {
            return focused;
        }
        for (const Callout& callout : callouts) {
            if (calloutMatchesHover(callout, hover)) {
                focused.push_back(callout);
            }
        }
        return focused;
    }

    static bool hoverReferencesKey(HoveredAnchor hover, int code) {
        return hover.active && hover.kind == AnchorKind::Keyboard && hover.code == code;
    }

    static bool hoverReferencesMouse(HoveredAnchor hover, int button) {
        return hover.active && hover.kind == AnchorKind::Mouse && hover.mouseButton == button;
    }

    static bool focusedCalloutsReferenceKey(const std::vector<Callout>& callouts, int code) {
        for (const Callout& callout : callouts) {
            if (calloutReferencesKey(callout, code)) {
                return true;
            }
        }
        return false;
    }

    static bool focusedCalloutsReferenceMouse(const std::vector<Callout>& callouts, int button) {
        for (const Callout& callout : callouts) {
            if (calloutReferencesMouse(callout, button)) {
                return true;
            }
        }
        return false;
    }

    static void appendLabel(std::string* target, std::string_view label) {
        if (target == nullptr || label.empty()) {
            return;
        }
        if (target->find(label) != std::string::npos) {
            return;
        }
        if (!target->empty()) {
            *target += " / ";
        }
        *target += label;
    }

    static void appendKeyboardCallout(std::vector<Callout>* callouts,
                                      std::vector<int> codes,
                                      std::string_view label,
                                      Color color,
                                      Side side) {
        if (callouts == nullptr || codes.empty()) {
            return;
        }
        for (Callout& callout : *callouts) {
            if (callout.anchorKind == AnchorKind::Keyboard && sameCodes(callout.codes, codes)) {
                appendLabel(&callout.label, label);
                return;
            }
        }
        Callout callout;
        callout.anchorKind = AnchorKind::Keyboard;
        callout.codes = std::move(codes);
        callout.label = std::string(label);
        callout.color = color;
        callout.side = side;
        callouts->push_back(std::move(callout));
    }

    static void appendMouseCallout(std::vector<Callout>* callouts,
                                   int button,
                                   std::string_view label,
                                   Color color,
                                   Side side) {
        if (callouts == nullptr) {
            return;
        }
        for (Callout& callout : *callouts) {
            if (callout.anchorKind == AnchorKind::Mouse && callout.mouseButton == button) {
                appendLabel(&callout.label, label);
                return;
            }
        }
        Callout callout;
        callout.anchorKind = AnchorKind::Mouse;
        callout.mouseButton = button;
        callout.label = std::string(label);
        callout.color = color;
        callout.side = side;
        callouts->push_back(std::move(callout));
    }

    static std::string_view actionDescription(input::ActionId action) {
        switch (action) {
            case input::ActionId::MoveForward:
                return "Move forward";
            case input::ActionId::MoveBackward:
                return "Move backward";
            case input::ActionId::MoveLeft:
                return "Strafe left";
            case input::ActionId::MoveRight:
                return "Strafe right";
            case input::ActionId::Jump:
                return "Jump / multi-jump while playing";
            case input::ActionId::FirePrimary:
                return "Fire primary weapon";
            case input::ActionId::SpectatorAscend:
                return "Raise free-fly camera";
            case input::ActionId::SpectatorDescend:
                return "Lower free-fly camera";
            case input::ActionId::SpectatorBoost:
                return "Hold for faster free-fly camera";
            case input::ActionId::Count:
                break;
        }
        return "Unmapped action";
    }

    static std::vector<Callout> buildCallouts(const input::ControlBindings& bindings) {
        std::vector<Callout> callouts;
        for (const input::ActionDescriptor& descriptor : input::actionDescriptors()) {
            const input::ActionBinding& binding = bindings.binding(descriptor.id);
            for (const input::InputToken token : binding.slots) {
                if (!token.isBound()) {
                    continue;
                }
                const Color color = categoryColor(descriptor.category);
                const Side side = sideForCategory(descriptor.category);
                if (token.device == input::InputDevice::Keyboard) {
                    appendKeyboardCallout(&callouts,
                                          {token.code},
                                          actionDescription(descriptor.id),
                                          color,
                                          side);
                } else if (token.device == input::InputDevice::MouseButton) {
                    appendMouseCallout(&callouts,
                                       token.code,
                                       actionDescription(descriptor.id),
                                       color,
                                       Side::Right);
                }
            }
        }

        appendKeyboardCallout(&callouts,
                              {KEY_K},
                              "Open or close this keyboard map",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_U},
                              "Open settings: prediction and network",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_J},
                              "Open settings: prediction and network",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_TAB},
                              "Hold to show the scoreboard",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_ENTER},
                              "Confirm menus or release cursor",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_Q},
                              "Back out or leave current session",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_ESCAPE},
                              "Back out or leave current session",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts, {KEY_Y}, "Change team", runtimeColor(), Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_V},
                              "Switch controlled actor or target",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_B},
                              "Toggle spectator or free camera",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_I},
                              "Toggle interpolation for your player",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_E},
                              "Host toggles bot freeze or activation",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_G},
                              "Spawn frozen study bot ahead",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_C},
                              "Show or hide player FOV cones",
                              runtimeColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_O},
                              "Show replay and recording controls",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_ONE},
                              "Save current spectator checkpoint",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_TWO},
                              "Move to previous spectator checkpoint",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_THREE},
                              "Move to next spectator checkpoint",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_FOUR},
                              "Delete selected spectator checkpoint",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_FIVE},
                              "Start or stop gameplay recording",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_SIX},
                              "Play or pause the current replay",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_SEVEN},
                              "Rewind replay by a large step",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_EIGHT},
                              "Fast-forward replay by a large step",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_NINE},
                              "Export command replay recording",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_ZERO},
                              "Reset replay playback to the first frame",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_BACKSPACE},
                              "Exit replay playback and return to live play",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_COMMA},
                              "Shorten replay checkpoint transition",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_PERIOD},
                              "Lengthen replay checkpoint transition",
                              replayColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_F1},
                              "Dim or restore environment clutter",
                              studyColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_F2},
                              "Show or hide marked study areas",
                              studyColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_F3},
                              "Filter to green areas or show all",
                              studyColor(),
                              Side::Right);
        appendKeyboardCallout(&callouts,
                              {KEY_F4},
                              "Filter to red areas or show all",
                              studyColor(),
                              Side::Right);
        return callouts;
    }

    static void addRow(std::vector<KeyView>* keys,
                       Rectangle keyboardArea,
                       float y,
                       const std::vector<KeySpec>& specs) {
        if (keys == nullptr || specs.empty()) {
            return;
        }
        constexpr float gap = 7.0f;
        float rowWidth = 0.0f;
        for (const KeySpec& spec : specs) {
            rowWidth += spec.width;
        }
        rowWidth += gap * static_cast<float>(specs.size() - 1u);

        float x = keyboardArea.x + (keyboardArea.width - rowWidth) * 0.5f;
        for (const KeySpec& spec : specs) {
            keys->push_back(KeyView{spec.code, spec.label, Rectangle{x, y, spec.width, 44.0f}});
            x += spec.width + gap;
        }
    }

    static std::vector<KeyView> buildKeyboard(Rectangle keyboardArea) {
        std::vector<KeyView> keys;
        const float rowY = keyboardArea.y;
        addRow(&keys,
               keyboardArea,
               rowY,
               {{KEY_ESCAPE, "Esc", 58.0f},
                {KEY_F1, "F1", 50.0f},
                {KEY_F2, "F2", 50.0f},
                {KEY_F3, "F3", 50.0f},
                {KEY_F4, "F4", 50.0f}});
        addRow(&keys,
               keyboardArea,
               rowY + 56.0f,
               {{KEY_ONE, "1", 54.0f},
                {KEY_TWO, "2", 54.0f},
                {KEY_THREE, "3", 54.0f},
                {KEY_FOUR, "4", 54.0f},
                {KEY_FIVE, "5", 54.0f},
                {KEY_SIX, "6", 54.0f},
                {KEY_SEVEN, "7", 54.0f},
                {KEY_EIGHT, "8", 54.0f},
                {KEY_NINE, "9", 54.0f},
                {KEY_ZERO, "0", 54.0f},
                {KEY_BACKSPACE, "Backspace", 116.0f}});
        addRow(&keys,
               keyboardArea,
               rowY + 112.0f,
               {{KEY_TAB, "Tab", 72.0f},
                {KEY_Q, "Q", 54.0f},
                {KEY_W, "W", 54.0f},
                {KEY_E, "E", 54.0f},
                {KEY_R, "R", 54.0f},
                {KEY_T, "T", 54.0f},
                {KEY_Y, "Y", 54.0f},
                {KEY_U, "U", 54.0f},
                {KEY_I, "I", 54.0f},
                {KEY_O, "O", 54.0f},
                {KEY_P, "P", 54.0f}});
        addRow(&keys,
               keyboardArea,
               rowY + 168.0f,
               {{KEY_A, "A", 54.0f},
                {KEY_S, "S", 54.0f},
                {KEY_D, "D", 54.0f},
                {KEY_F, "F", 54.0f},
                {KEY_G, "G", 54.0f},
                {KEY_H, "H", 54.0f},
                {KEY_J, "J", 54.0f},
                {KEY_K, "K", 54.0f},
                {KEY_L, "L", 54.0f},
                {KEY_ENTER, "Enter", 86.0f}});
        addRow(&keys,
               keyboardArea,
               rowY + 224.0f,
               {{KEY_LEFT_SHIFT, "Shift", 88.0f},
                {KEY_Z, "Z", 54.0f},
                {KEY_X, "X", 54.0f},
                {KEY_C, "C", 54.0f},
                {KEY_V, "V", 54.0f},
                {KEY_B, "B", 54.0f},
                {KEY_N, "N", 54.0f},
                {KEY_M, "M", 54.0f},
                {KEY_COMMA, ",", 54.0f},
                {KEY_PERIOD, ".", 54.0f},
                {KEY_RIGHT_SHIFT, "Shift", 88.0f}});
        addRow(&keys,
               keyboardArea,
               rowY + 280.0f,
               {{KEY_LEFT_CONTROL, "Ctrl", 68.0f},
                {KEY_LEFT_ALT, "Alt", 58.0f},
                {KEY_SPACE, "Space", 260.0f},
                {KEY_RIGHT_ALT, "Alt", 58.0f},
                {KEY_RIGHT_CONTROL, "Ctrl", 68.0f},
                {KEY_LEFT, "Left", 60.0f},
                {KEY_UP, "Up", 54.0f},
                {KEY_DOWN, "Down", 64.0f},
                {KEY_RIGHT, "Right", 68.0f}});
        return keys;
    }

    static std::vector<MouseButtonView> buildMouse(Rectangle mouseArea) {
        return {
            MouseButtonView{
                MOUSE_BUTTON_LEFT,
                "M1",
                Rectangle{mouseArea.x + 46.0f, mouseArea.y + 18.0f, 44.0f, 58.0f}},
            MouseButtonView{
                MOUSE_BUTTON_MIDDLE,
                "M3",
                Rectangle{mouseArea.x + 92.0f, mouseArea.y + 20.0f, 16.0f, 50.0f}},
            MouseButtonView{
                MOUSE_BUTTON_RIGHT,
                "M2",
                Rectangle{mouseArea.x + 110.0f, mouseArea.y + 18.0f, 44.0f, 58.0f}},
            MouseButtonView{
                MOUSE_BUTTON_SIDE,
                "M4",
                Rectangle{mouseArea.x + 16.0f, mouseArea.y + 62.0f, 28.0f, 26.0f}},
            MouseButtonView{
                MOUSE_BUTTON_EXTRA,
                "M5",
                Rectangle{mouseArea.x + 16.0f, mouseArea.y + 94.0f, 28.0f, 26.0f}},
            MouseButtonView{
                MOUSE_BUTTON_FORWARD,
                "M+",
                Rectangle{mouseArea.x + 156.0f, mouseArea.y + 62.0f, 28.0f, 26.0f}},
            MouseButtonView{
                MOUSE_BUTTON_BACK,
                "M-",
                Rectangle{mouseArea.x + 156.0f, mouseArea.y + 94.0f, 28.0f, 26.0f}},
        };
    }

    static bool calloutReferencesKey(const Callout& callout, int code) {
        return callout.anchorKind == AnchorKind::Keyboard &&
               std::find(callout.codes.begin(), callout.codes.end(), code) != callout.codes.end();
    }

    static bool calloutReferencesMouse(const Callout& callout, int button) {
        return callout.anchorKind == AnchorKind::Mouse && callout.mouseButton == button;
    }

    static bool keyHighlight(const std::vector<Callout>& callouts, int code, Color* colorOut) {
        for (const Callout& callout : callouts) {
            if (calloutReferencesKey(callout, code)) {
                if (colorOut != nullptr) {
                    *colorOut = callout.color;
                }
                return true;
            }
        }
        return false;
    }

    static bool mouseHighlight(const std::vector<Callout>& callouts, int button, Color* colorOut) {
        for (const Callout& callout : callouts) {
            if (calloutReferencesMouse(callout, button)) {
                if (colorOut != nullptr) {
                    *colorOut = callout.color;
                }
                return true;
            }
        }
        return false;
    }

    static const MouseButtonView* mouseButtonView(const std::vector<MouseButtonView>& buttons,
                                                  int button) {
        for (const MouseButtonView& candidate : buttons) {
            if (candidate.button == button) {
                return &candidate;
            }
        }
        return nullptr;
    }

    static void renderMouseButtonRegion(const MouseButtonView& button,
                                        const std::vector<Callout>& callouts,
                                        const std::vector<Callout>& focusedCallouts,
                                        HoveredAnchor hover,
                                        float roundness) {
        Color highlight{};
        const bool active = mouseHighlight(callouts, button.button, &highlight);
        const bool focused = focusedCalloutsReferenceMouse(focusedCallouts, button.button);
        const bool hovered = hoverReferencesMouse(hover, button.button);
        const Color fill = focused
            ? Fade(highlight, 0.64f)
            : (active ? Fade(highlight, 0.24f)
                      : (hovered ? Color{50, 58, 74, 255} : Color{32, 40, 56, 255}));
        const Color border = focused
            ? RAYWHITE
            : (active ? Fade(highlight, 0.82f) : Color{86, 102, 130, 255});

        DrawRectangleRounded(button.rect, roundness, 12, fill);
        DrawRectangleRoundedLines(button.rect, roundness, 12, border);
        const TypographyStyle& style =
            TypographyService::shared().style(TypographyStyleId::Caption);
        TypographyService::shared().drawCentered(TypographyStyleId::Caption,
                                                 button.label,
                                                 button.rect.x + button.rect.width * 0.5f,
                                                 button.rect.y + button.rect.height * 0.5f -
                                                     style.lineHeight * 0.5f,
                                                 active ? RAYWHITE : Color{176, 190, 210, 255});
    }

    static void renderKey(const KeyView& key,
                          const std::vector<Callout>& callouts,
                          const std::vector<Callout>& focusedCallouts,
                          HoveredAnchor hover) {
        Color highlight{};
        const bool active = keyHighlight(callouts, key.code, &highlight);
        const bool focused = focusedCalloutsReferenceKey(focusedCallouts, key.code);
        const bool hovered = hoverReferencesKey(hover, key.code);
        const Color fill = focused
            ? Fade(highlight, 0.64f)
            : (active ? Fade(highlight, 0.24f)
                      : (hovered ? Color{44, 50, 66, 255} : Color{28, 34, 46, 255}));
        const Color border = focused
            ? RAYWHITE
            : (active ? Fade(highlight, 0.82f) : Color{76, 88, 110, 255});

        DrawRectangleRounded(key.rect, 0.13f, 8, fill);
        DrawRectangleRoundedLines(key.rect, 0.13f, 8, border);
        if (focused) {
            DrawRectangleRoundedLines(Rectangle{key.rect.x - 3.0f,
                                                key.rect.y - 3.0f,
                                                key.rect.width + 6.0f,
                                                key.rect.height + 6.0f},
                                      0.13f,
                                      8,
                                      Fade(highlight, 0.92f));
        }
        const TypographyStyle& style =
            TypographyService::shared().style(TypographyStyleId::Caption);
        TypographyService::shared().drawCentered(TypographyStyleId::Caption,
                                                 key.label,
                                                 key.rect.x + key.rect.width * 0.5f,
                                                 key.rect.y + key.rect.height * 0.5f -
                                                     style.lineHeight * 0.5f,
                                                 active ? RAYWHITE : Color{176, 190, 210, 255});
    }

    static void renderMouse(const std::vector<MouseButtonView>& buttons,
                            const std::vector<Callout>& callouts,
                            const std::vector<Callout>& focusedCallouts,
                            HoveredAnchor hover) {
        if (buttons.empty()) {
            return;
        }

        const MouseButtonView* left = mouseButtonView(buttons, MOUSE_BUTTON_LEFT);
        const MouseButtonView* middle = mouseButtonView(buttons, MOUSE_BUTTON_MIDDLE);
        const MouseButtonView* right = mouseButtonView(buttons, MOUSE_BUTTON_RIGHT);
        const MouseButtonView* side = mouseButtonView(buttons, MOUSE_BUTTON_SIDE);
        const MouseButtonView* extra = mouseButtonView(buttons, MOUSE_BUTTON_EXTRA);
        const MouseButtonView* forward = mouseButtonView(buttons, MOUSE_BUTTON_FORWARD);
        const MouseButtonView* back = mouseButtonView(buttons, MOUSE_BUTTON_BACK);

        const Rectangle body{
            buttons.front().rect.x - 8.0f,
            buttons.front().rect.y - 16.0f,
            124.0f,
            150.0f
        };
        DrawRectangleRounded(Rectangle{body.x + 3.0f, body.y + 5.0f, body.width, body.height},
                             0.46f,
                             24,
                             Fade(BLACK, 0.26f));
        DrawRectangleRounded(body, 0.46f, 24, Color{22, 28, 40, 246});
        DrawRectangleRoundedLines(body, 0.46f, 24, Color{78, 96, 124, 255});

        const float seamTop = body.y + 16.0f;
        const float seamBottom = body.y + 70.0f;
        DrawLineEx(Vector2{body.x + body.width * 0.5f, seamTop},
                   Vector2{body.x + body.width * 0.5f, seamBottom},
                   2.0f,
                   Fade(Color{120, 140, 168, 255}, 0.55f));
        DrawLineEx(Vector2{body.x + 18.0f, body.y + 72.0f},
                   Vector2{body.x + body.width - 18.0f, body.y + 72.0f},
                   2.0f,
                   Fade(Color{120, 140, 168, 255}, 0.38f));

        if (side != nullptr) {
            DrawRectangleRounded(Rectangle{side->rect.x - 6.0f,
                                           side->rect.y - 4.0f,
                                           side->rect.width + 12.0f,
                                           side->rect.height * 2.0f + 14.0f},
                                 0.35f,
                                 10,
                                 Color{20, 26, 38, 232});
        }
        if (forward != nullptr) {
            DrawRectangleRounded(Rectangle{forward->rect.x - 6.0f,
                                           forward->rect.y - 4.0f,
                                           forward->rect.width + 12.0f,
                                           forward->rect.height * 2.0f + 14.0f},
                                 0.35f,
                                 10,
                                 Color{20, 26, 38, 232});
        }

        if (left != nullptr) {
            renderMouseButtonRegion(*left, callouts, focusedCallouts, hover, 0.34f);
        }
        if (right != nullptr) {
            renderMouseButtonRegion(*right, callouts, focusedCallouts, hover, 0.34f);
        }
        if (middle != nullptr) {
            renderMouseButtonRegion(*middle, callouts, focusedCallouts, hover, 0.45f);
            DrawLineEx(Vector2{middle->rect.x + middle->rect.width * 0.5f,
                               middle->rect.y + 7.0f},
                       Vector2{middle->rect.x + middle->rect.width * 0.5f,
                               middle->rect.y + middle->rect.height - 7.0f},
                       2.0f,
                       Fade(RAYWHITE, 0.34f));
        }
        for (const MouseButtonView* button : {side, extra, forward, back}) {
            if (button != nullptr) {
                renderMouseButtonRegion(*button, callouts, focusedCallouts, hover, 0.32f);
            }
        }
    }

    static const KeyView* findKey(const std::vector<KeyView>& keys, int code) {
        const auto it = std::find_if(keys.begin(), keys.end(), [code](const KeyView& key) {
            return key.code == code;
        });
        return it == keys.end() ? nullptr : &(*it);
    }

    static const MouseButtonView* findMouseButton(const std::vector<MouseButtonView>& buttons,
                                                  int button) {
        const auto it =
            std::find_if(buttons.begin(), buttons.end(), [button](const MouseButtonView& view) {
                return view.button == button;
            });
        return it == buttons.end() ? nullptr : &(*it);
    }

    static bool hoverAnchorPosition(HoveredAnchor hover,
                                    const std::vector<KeyView>& keys,
                                    const std::vector<MouseButtonView>& mouseButtons,
                                    Vector2* anchorOut) {
        if (anchorOut == nullptr || !hover.active) {
            return false;
        }

        if (hover.kind == AnchorKind::Mouse) {
            const MouseButtonView* button = findMouseButton(mouseButtons, hover.mouseButton);
            if (button == nullptr) {
                return false;
            }
            *anchorOut = Vector2{button->rect.x + button->rect.width * 0.5f,
                                 button->rect.y + button->rect.height * 0.5f};
            return true;
        }

        const KeyView* key = findKey(keys, hover.code);
        if (key == nullptr) {
            return false;
        }
        *anchorOut = Vector2{key->rect.x + key->rect.width * 0.5f,
                             key->rect.y + key->rect.height * 0.5f};
        return true;
    }

    static std::vector<CalloutLayout> layoutFocusedCallouts(const std::vector<Callout>& callouts,
                                                            Rectangle panel) {
        std::vector<CalloutLayout> layouts;
        constexpr float boxWidth = 390.0f;
        constexpr float boxHeight = 58.0f;
        constexpr float gap = 12.0f;
        float leftY = panel.y + 156.0f;
        float rightY = panel.y + 156.0f;
        const float leftX = panel.x + 36.0f;
        const float rightX = panel.x + panel.width - 36.0f - boxWidth;

        for (const Callout& callout : callouts) {
            Rectangle box{};
            if (callout.side == Side::Left) {
                box = Rectangle{leftX, leftY, boxWidth, boxHeight};
                leftY += boxHeight + gap;
            } else {
                box = Rectangle{rightX, rightY, boxWidth, boxHeight};
                rightY += boxHeight + gap;
            }
            layouts.push_back(CalloutLayout{callout, box});
        }
        return layouts;
    }

    static std::string fitText(std::string text, TypographyStyleId styleId, float maxWidth) {
        TypographyService& typography = TypographyService::shared();
        if (typography.measureWidth(styleId, text) <= static_cast<int>(maxWidth)) {
            return text;
        }
        constexpr std::string_view ellipsis = "...";
        while (!text.empty() &&
               typography.measureWidth(styleId, text + std::string(ellipsis)) >
                   static_cast<int>(maxWidth)) {
            text.pop_back();
        }
        return text.empty() ? std::string(ellipsis) : text + std::string(ellipsis);
    }

    static void renderCallout(const CalloutLayout& layout) {
        DrawRectangleRounded(layout.box, 0.22f, 8, Fade(layout.callout.color, 0.16f));
        DrawRectangleRoundedLines(layout.box, 0.22f, 8, Fade(layout.callout.color, 0.82f));
        const std::string label =
            fitText(layout.callout.label, TypographyStyleId::Body, layout.box.width - 24.0f);
        drawText(TypographyStyleId::Body,
                 label,
                 Vector2{layout.box.x + 12.0f, layout.box.y + 15.0f},
                 RAYWHITE);
    }
};

}  // namespace client
