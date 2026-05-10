#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <raylib.h>

#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "TypographyService.hpp"
#include "net/PredictionBuffer.hpp"
#include "net/Protocol.hpp"

class RuntimeSettingsOverlay {
public:
    enum class ControlType : std::uint8_t {
        Toggle = 0,
        Choice = 1,
        Slider = 2
    };

    enum class ControlId : std::uint8_t {
        Interpolation = 0,
        Prediction = 1,
        ReconciliationStrategy = 2,
        SmoothWindowMs = 3,
        ShotEvaluationMode = 4,
        TickRate = 5,
        SnapshotRate = 6,
        TargetLatency = 7,
        TargetLoss = 8,
        UpDelay = 9,
        DownDelay = 10,
        UpLoss = 11,
        DownLoss = 12,
        UpReorder = 13,
        DownReorder = 14,
        StudyEventLogging = 15,
        ReplayGhosts = 16,
        ReplayTrack = 17
    };

    struct ChoiceState {
        std::string label{};
        int value{0};
        bool selected{false};
        bool enabled{true};
    };

    struct ControlState {
        ControlId id{ControlId::Interpolation};
        ControlType type{ControlType::Toggle};
        std::string label{};
        std::string description{};
        std::string valueLabel{};
        bool visible{true};
        bool enabled{true};
        bool toggleValue{false};
        float sliderValue{0.0f};
        float sliderMin{0.0f};
        float sliderMax{100.0f};
        std::vector<ChoiceState> choices{};
    };

    struct TargetRowState {
        std::uint16_t targetId{0u};
        std::string label{};
        std::string detailLine{};
        std::string metricsLine{};
        std::string statsLine{};
        bool selected{false};
        bool editable{false};
    };

    struct TargetEditorState {
        bool available{false};
        std::string title{};
        std::string subtitle{};
        std::vector<std::string> statusLines{};
        ControlState latency{};
        ControlState loss{};
        bool showAdvancedNetworkControls{false};
        ControlState upDelay{};
        ControlState downDelay{};
        ControlState upLoss{};
        ControlState downLoss{};
        ControlState upReorder{};
        ControlState downReorder{};
    };

    struct State {
        bool visible{false};
        std::string title{"Runtime Settings"};
        std::string subtitle{};
        bool showTargetSections{true};
        std::string leftSectionTitle{"Runtime"};
        std::string targetListTitle{"Participants"};
        std::string targetEditorTitle{"Selected"};
        std::vector<ControlState> leftControls{};
        std::vector<TargetRowState> targets{};
        TargetEditorState targetEditor{};
    };

    struct Action {
        enum class Kind : std::uint8_t {
            None = 0,
            Close = 1,
            ToggleChanged = 2,
            ChoiceSelected = 3,
            SliderChanged = 4,
            TargetSelected = 5
        };

        Kind kind{Kind::None};
        ControlId controlId{ControlId::Interpolation};
        bool toggleValue{false};
        int choiceValue{0};
        float sliderValue{0.0f};
        std::uint16_t targetId{0u};
    };

    void resetInteraction() {
        activeDrag_ = DragTarget::None;
        targetScrollOffset_ = std::max(0.0f, targetScrollOffset_);
    }

    std::vector<Action> handleMouse(const State& state) {
        std::vector<Action> actions;
        if (!state.visible) {
            resetInteraction();
            return actions;
        }

        const bool showLeftSection = hasVisibleLeftControls(state);
        const bool showTargetSections = state.showTargetSections;
        const Layout layout = buildLayout(showLeftSection, showTargetSections);
        const Vector2 mouse = display::mousePosition();

        if (showTargetSections && CheckCollisionPointRec(mouse, layout.targetListViewport)) {
            const float wheel = GetMouseWheelMove();
            if (std::fabs(wheel) > 0.001f) {
                targetScrollOffset_ = std::clamp(
                    targetScrollOffset_ - wheel * 40.0f,
                    0.0f,
                    maxTargetScroll(state, layout));
            }
        }

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (CheckCollisionPointRec(mouse, layout.closeButton)) {
                actions.push_back(Action{Action::Kind::Close});
            }

            if (showLeftSection) {
                for (const ControlState& control : state.leftControls) {
                    if (!control.visible) {
                        continue;
                    }
                    const ControlLayout controlLayout =
                        buildLeftControlLayout(layout.leftSection, state.leftControls, control.id);
                    if (control.type == ControlType::Slider) {
                        pressSlider(control,
                                    SliderLayout{controlLayout.card,
                                                 controlLayout.sliderTrack,
                                                 controlLayout.sliderBounds},
                                    dragTargetFor(control.id),
                                    mouse,
                                    &actions);
                    } else {
                        handleControlPress(control, controlLayout, mouse, &actions);
                    }
                }
            }

            if (showTargetSections) {
                handleTargetSelection(state, layout, mouse, &actions);
                handleEditorPress(state.targetEditor, layout, mouse, &actions);
            }
        }

        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            activeDrag_ = DragTarget::None;
        }

        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && activeDrag_ != DragTarget::None) {
            handleDrag(state, layout, mouse, &actions);
        }

        return actions;
    }

    void render(const State& state) const {
        if (!state.visible) {
            return;
        }

        const bool showLeftSection = hasVisibleLeftControls(state);
        const bool showTargetSections = state.showTargetSections;
        const Layout layout = buildLayout(showLeftSection, showTargetSections);
        const Vector2 mouse = display::mousePosition();

        DrawRectangle(0,
                      0,
                      Config::SCREEN_WIDTH,
                      Config::SCREEN_HEIGHT,
                      Fade(BLACK, 0.38f));

        DrawRectangleRounded(layout.menu, 0.045f, 16, Color{14, 18, 26, 245});
        DrawRectangleRoundedLines(layout.menu, 0.045f, 16, Color{92, 124, 182, 255});

        drawTextFit(state.title,
                    layout.menu.x + 28.0f,
                    layout.menu.y + 20.0f,
                    layout.menu.width - 112.0f,
                    36,
                    RAYWHITE);
        drawTextFit(state.subtitle,
                    layout.menu.x + 30.0f,
                    layout.menu.y + 72.0f,
                    layout.menu.width - 116.0f,
                    20,
                    Color{193, 205, 223, 255});

        const Color closeFill = CheckCollisionPointRec(mouse, layout.closeButton)
            ? Color{196, 84, 84, 255}
            : Color{72, 42, 42, 255};
        DrawRectangleRounded(layout.closeButton, 0.3f, 10, closeFill);
        drawText("X",
                 layout.closeButton.x + 15.0f,
                 layout.closeButton.y + 3.0f,
                 22,
                 RAYWHITE);

        if (showLeftSection) {
            DrawRectangleRounded(layout.leftSection, 0.035f, 12, Color{22, 28, 40, 238});
            drawSectionTitle(state.leftSectionTitle, "", layout.leftSection);
        }
        if (showTargetSections) {
            DrawRectangleRounded(layout.targetListSection, 0.035f, 12, Color{22, 28, 40, 238});
            DrawRectangleRounded(layout.targetEditorSection, 0.035f, 12, Color{22, 28, 40, 238});
            drawSectionTitle(state.targetListTitle, "", layout.targetListSection);
            drawSectionTitle(state.targetEditorTitle, "", layout.targetEditorSection);
        }

        if (showLeftSection) {
            for (const ControlState& control : state.leftControls) {
                if (!control.visible) {
                    continue;
                }
                const ControlLayout controlLayout =
                    buildLeftControlLayout(layout.leftSection, state.leftControls, control.id);
                renderControl(control, controlLayout, mouse);
            }
        }

        if (showTargetSections) {
            renderTargetList(state, layout, mouse);
            renderTargetEditor(state.targetEditor, layout, mouse);
        }
    }

private:
    enum class DragTarget : std::uint8_t {
        None = 0,
        SmoothWindow = 1,
        TargetLatency = 2,
        TargetLoss = 3,
        UpDelay = 4,
        DownDelay = 5,
        UpLoss = 6,
        DownLoss = 7,
        UpReorder = 8,
        DownReorder = 9
    };

    struct Layout {
        Rectangle menu{};
        Rectangle closeButton{};
        Rectangle leftSection{};
        Rectangle targetListSection{};
        Rectangle targetEditorSection{};
        Rectangle targetListViewport{};
        Rectangle targetEditorArea{};
    };

    struct ControlLayout {
        Rectangle card{};
        Rectangle actionArea{};
        Rectangle sliderTrack{};
        Rectangle sliderBounds{};
        std::vector<Rectangle> choiceButtons{};
    };

    float targetScrollOffset_{0.0f};
    DragTarget activeDrag_{DragTarget::None};

    static bool hasVisibleLeftControls(const State& state) {
        return std::any_of(state.leftControls.begin(),
                           state.leftControls.end(),
                           [](const ControlState& control) {
                               return control.visible;
                           });
    }

    static std::size_t visibleLeftControlCount(const std::vector<ControlState>& controls) {
        return static_cast<std::size_t>(
            std::count_if(controls.begin(),
                          controls.end(),
                          [](const ControlState& control) {
                              return control.visible;
                          }));
    }

    static Layout buildLayout(bool showLeftSection, bool showTargetSections) {
        Layout layout;
        const bool showLeftOnly = showLeftSection && !showTargetSections;
        const float desiredMenuWidth = showLeftOnly ? 820.0f : 1540.0f;
        const float desiredMenuHeight = showLeftOnly ? 560.0f : 880.0f;
        const float menuWidth = std::min(desiredMenuWidth, static_cast<float>(Config::SCREEN_WIDTH) - 96.0f);
        const float menuHeight = std::min(desiredMenuHeight, static_cast<float>(Config::SCREEN_HEIGHT) - 96.0f);
        layout.menu = Rectangle{
            (static_cast<float>(Config::SCREEN_WIDTH) - menuWidth) * 0.5f,
            (static_cast<float>(Config::SCREEN_HEIGHT) - menuHeight) * 0.5f,
            menuWidth,
            menuHeight
        };

        layout.closeButton = Rectangle{
            layout.menu.x + layout.menu.width - 58.0f,
            layout.menu.y + 18.0f,
            38.0f,
            38.0f
        };

        constexpr float outerPadding = 26.0f;
        constexpr float gutter = 18.0f;
        const float innerY = layout.menu.y + 118.0f;
        const float innerHeight = layout.menu.height - 144.0f;
        const float innerWidth = layout.menu.width - (outerPadding * 2.0f);
        if (showLeftSection && showTargetSections) {
            const float leftWidth = innerWidth * 0.36f;
            const float targetWidth = innerWidth * 0.24f;
            const float editorWidth = innerWidth - leftWidth - targetWidth - (gutter * 2.0f);

            layout.leftSection = Rectangle{
                layout.menu.x + outerPadding,
                innerY,
                leftWidth,
                innerHeight
            };
            layout.targetListSection = Rectangle{
                layout.leftSection.x + layout.leftSection.width + gutter,
                innerY,
                targetWidth,
                innerHeight
            };
            layout.targetEditorSection = Rectangle{
                layout.targetListSection.x + layout.targetListSection.width + gutter,
                innerY,
                editorWidth,
                innerHeight
            };
        } else if (showLeftSection) {
            const float leftWidth = std::min(760.0f, innerWidth);
            layout.leftSection = Rectangle{
                layout.menu.x + outerPadding + (innerWidth - leftWidth) * 0.5f,
                innerY,
                leftWidth,
                innerHeight
            };
            layout.targetListSection = Rectangle{};
            layout.targetEditorSection = Rectangle{};
        } else if (showTargetSections) {
            layout.leftSection = Rectangle{};
            const float targetWidth = std::min(430.0f, innerWidth * 0.36f);
            const float editorWidth = innerWidth - targetWidth - gutter;
            layout.targetListSection = Rectangle{
                layout.menu.x + outerPadding,
                innerY,
                targetWidth,
                innerHeight
            };
            layout.targetEditorSection = Rectangle{
                layout.targetListSection.x + layout.targetListSection.width + gutter,
                innerY,
                editorWidth,
                innerHeight
            };
        } else {
            layout.leftSection = Rectangle{};
            layout.targetListSection = Rectangle{};
            layout.targetEditorSection = Rectangle{};
        }
        if (showTargetSections) {
            layout.targetListViewport = Rectangle{
                layout.targetListSection.x + 16.0f,
                layout.targetListSection.y + 68.0f,
                layout.targetListSection.width - 32.0f,
                layout.targetListSection.height - 86.0f
            };
            layout.targetEditorArea = Rectangle{
                layout.targetEditorSection.x + 16.0f,
                layout.targetEditorSection.y + 68.0f,
                layout.targetEditorSection.width - 32.0f,
                layout.targetEditorSection.height - 86.0f
            };
        } else {
            layout.targetListViewport = Rectangle{};
            layout.targetEditorArea = Rectangle{};
        }
        return layout;
    }

    static bool useLeftControlGrid(const Rectangle& section,
                                   const std::vector<ControlState>& controls) {
        constexpr std::size_t gridControlThreshold = 5u;
        constexpr float gridWidthThreshold = 400.0f;
        return section.width >= gridWidthThreshold &&
               visibleLeftControlCount(controls) >= gridControlThreshold;
    }

    static float leftGridControlHeight(const Rectangle& section,
                                       std::size_t visibleControlCount) {
        constexpr float topInset = 58.0f;
        constexpr float bottomInset = 18.0f;
        constexpr float rowGap = 12.0f;
        constexpr float preferredHeight = 104.0f;
        constexpr float minimumHeight = 88.0f;
        const std::size_t rowCount = std::max<std::size_t>(1u, (visibleControlCount + 1u) / 2u);
        const float availableHeight = section.height -
            topInset -
            bottomInset -
            rowGap * static_cast<float>(rowCount - 1u);
        return std::clamp(availableHeight / static_cast<float>(rowCount),
                          minimumHeight,
                          preferredHeight);
    }

    static float controlHeight(const ControlState& control) {
        switch (control.type) {
            case ControlType::Toggle:
                return 74.0f;
            case ControlType::Choice:
                return 92.0f;
            case ControlType::Slider:
                return 94.0f;
        }
        return 74.0f;
    }

    static ControlLayout buildLeftControlLayout(const Rectangle& section,
                                                const std::vector<ControlState>& controls,
                                                ControlId controlId) {
        ControlLayout layout;
        constexpr float inset = 16.0f;
        constexpr float topInset = 58.0f;
        const float startX = section.x + inset;
        const float startY = section.y + topInset;
        ControlType targetType = ControlType::Toggle;
        const bool grid = useLeftControlGrid(section, controls);

        if (grid) {
            constexpr float columnGap = 12.0f;
            constexpr float rowGap = 12.0f;
            const std::size_t visibleCount = visibleLeftControlCount(controls);
            const float cardWidth = (section.width - (inset * 2.0f) - columnGap) * 0.5f;
            const float cardHeight = leftGridControlHeight(section, visibleCount);
            std::size_t visibleIndex = 0u;
            for (const ControlState& control : controls) {
                if (!control.visible) {
                    continue;
                }
                if (control.id == controlId) {
                    const std::size_t column = visibleIndex % 2u;
                    const std::size_t row = visibleIndex / 2u;
                    layout.card = Rectangle{
                        startX + static_cast<float>(column) * (cardWidth + columnGap),
                        startY + static_cast<float>(row) * (cardHeight + rowGap),
                        cardWidth,
                        cardHeight
                    };
                    targetType = control.type;
                    break;
                }
                ++visibleIndex;
            }
        } else {
            float cursorY = startY;
            for (const ControlState& control : controls) {
                if (!control.visible) {
                    continue;
                }
                const float height = controlHeight(control);
                if (control.id == controlId) {
                    layout.card = Rectangle{startX, cursorY, section.width - 32.0f, height};
                    targetType = control.type;
                    break;
                }
                cursorY += height + 10.0f;
            }
        }

        if (grid) {
            layout.actionArea = Rectangle{
                layout.card.x + 14.0f,
                layout.card.y + 50.0f,
                layout.card.width - 28.0f,
                std::min(38.0f, std::max(32.0f, layout.card.height - 64.0f))
            };
        } else if (targetType == ControlType::Choice) {
            layout.actionArea = Rectangle{
                layout.card.x + 16.0f,
                layout.card.y + 42.0f,
                layout.card.width - 32.0f,
                38.0f
            };
        } else {
            layout.actionArea = Rectangle{
                layout.card.x + layout.card.width - 158.0f,
                layout.card.y + 16.0f,
                140.0f,
                40.0f
            };
        }
        layout.sliderTrack = Rectangle{
            layout.card.x + 18.0f,
            layout.card.y + layout.card.height - 22.0f,
            layout.card.width - 36.0f,
            8.0f
        };
        layout.sliderBounds = Rectangle{
            layout.sliderTrack.x,
            layout.sliderTrack.y - 10.0f,
            layout.sliderTrack.width,
            28.0f
        };
        return layout;
    }

    static float maxTargetScroll(const State& state, const Layout& layout) {
        constexpr float rowHeight = 108.0f;
        constexpr float rowGap = 12.0f;
        const float contentHeight = state.targets.empty()
            ? 0.0f
            : static_cast<float>(state.targets.size()) * rowHeight +
                  static_cast<float>(state.targets.size() - 1u) * rowGap;
        return std::max(0.0f, contentHeight - layout.targetListViewport.height);
    }

    static TypographyStyleId styleForSize(int size) {
        if (size >= 34) {
            return TypographyStyleId::ScoreboardTitle;
        }
        if (size >= 28) {
            return TypographyStyleId::OverlayTitle;
        }
        if (size >= 22) {
            return TypographyStyleId::OverlayAccent;
        }
        if (size >= 20) {
            return TypographyStyleId::Body;
        }
        return TypographyStyleId::Caption;
    }

    static int measureTextWidth(const std::string& text, int size) {
        return TypographyService::shared().measureWidth(styleForSize(size), text);
    }

    static void drawText(const std::string& text, float x, float y, int size, Color color) {
        if (!text.empty()) {
            TypographyService::shared().draw(styleForSize(size), text, Vector2{x, y}, color);
        }
    }

    static std::string fitTextToWidth(std::string text, int size, float maxWidth) {
        if (text.empty()) {
            return text;
        }
        if (maxWidth <= 0.0f) {
            return "";
        }
        if (static_cast<float>(measureTextWidth(text, size)) <= maxWidth) {
            return text;
        }

        const std::string suffix = "...";
        const int suffixWidth = measureTextWidth(suffix, size);
        if (static_cast<float>(suffixWidth) > maxWidth) {
            return "";
        }

        while (!text.empty() &&
               static_cast<float>(measureTextWidth(text + suffix, size)) > maxWidth) {
            text.pop_back();
        }
        return text.empty() ? suffix : text + suffix;
    }

    static void drawTextFit(const std::string& text,
                            float x,
                            float y,
                            float maxWidth,
                            int size,
                            Color color) {
        drawText(fitTextToWidth(text, size, maxWidth), x, y, size, color);
    }

    static void drawSectionTitle(const std::string& title,
                                 const std::string& subtitle,
                                 const Rectangle& section) {
        drawTextFit(title, section.x + 18.0f, section.y + 18.0f, section.width - 36.0f, 28, RAYWHITE);
        if (!subtitle.empty()) {
            drawTextFit(subtitle,
                        section.x + 18.0f,
                        section.y + 50.0f,
                        section.width - 36.0f,
                        18,
                        Color{168, 181, 201, 255});
        }
    }

    static Color cardFill(const ControlState& control, bool hovered) {
        if (!control.enabled) {
            return Color{34, 38, 48, 228};
        }
        return hovered ? Color{38, 50, 72, 245} : Color{30, 38, 54, 240};
    }

    static Color cardBorder(const ControlState& control) {
        return control.enabled ? Color{84, 116, 172, 255} : Color{66, 72, 86, 255};
    }

    static float sliderPercent(const ControlState& control) {
        if (control.sliderMax <= control.sliderMin) {
            return 0.0f;
        }
        return std::clamp(
            (control.sliderValue - control.sliderMin) / (control.sliderMax - control.sliderMin),
            0.0f,
            1.0f);
    }

    void renderControl(const ControlState& control,
                       ControlLayout layout,
                       const Vector2& mouse) const {
        if (control.type == ControlType::Choice) {
            layout.choiceButtons = buildChoiceButtons(control, layout.actionArea);
        }

        const bool hovered = CheckCollisionPointRec(mouse, layout.card);
        DrawRectangleRounded(layout.card, 0.035f, 10, cardFill(control, hovered));
        DrawRectangleRoundedLines(layout.card, 0.035f, 10, cardBorder(control));

        const bool actionBelowLabel = layout.actionArea.y > layout.card.y + 36.0f;
        const float labelMaxWidth = control.type == ControlType::Toggle && !actionBelowLabel
            ? layout.actionArea.x - layout.card.x - 36.0f
            : layout.card.width - 36.0f;
        drawTextFit(control.label,
                    layout.card.x + 18.0f,
                    layout.card.y + 13.0f,
                    labelMaxWidth,
                    22,
                    RAYWHITE);

        switch (control.type) {
            case ControlType::Toggle:
                renderToggle(control, layout.actionArea, mouse);
                break;
            case ControlType::Choice:
                renderChoiceButtons(control, layout.choiceButtons, mouse);
                break;
            case ControlType::Slider:
                renderSlider(control, layout.sliderTrack, layout.sliderBounds, mouse);
                break;
        }
    }

    static void renderToggle(const ControlState& control,
                             const Rectangle& button,
                             const Vector2& mouse) {
        const bool hovered = CheckCollisionPointRec(mouse, button);
        const Color fill = !control.enabled
            ? Color{54, 58, 66, 220}
            : control.toggleValue
                ? (hovered ? Color{76, 192, 132, 255} : Color{58, 168, 112, 255})
                : (hovered ? Color{190, 110, 110, 255} : Color{146, 84, 84, 255});
        DrawRectangleRounded(button, 0.25f, 8, fill);
        DrawRectangleRoundedLines(button, 0.25f, 8, Color{22, 24, 28, 255});

        const std::string label = control.toggleValue ? "Enabled" : "Disabled";
        const int labelWidth = measureTextWidth(label, 20);
        drawText(label,
                 button.x + (button.width - labelWidth) * 0.5f,
                 button.y + 10.0f,
                 20,
                 RAYWHITE);
    }

    static std::vector<Rectangle> buildChoiceButtons(const ControlState& control,
                                                     const Rectangle& actionArea) {
        std::vector<Rectangle> buttons;
        if (control.choices.empty()) {
            return buttons;
        }

        const float gap = 8.0f;
        const float totalGap = gap * static_cast<float>(control.choices.size() - 1u);
        const float width =
            (actionArea.width - totalGap) / static_cast<float>(control.choices.size());
        for (std::size_t index = 0; index < control.choices.size(); ++index) {
            buttons.push_back(Rectangle{
                actionArea.x + index * (width + gap),
                actionArea.y,
                width,
                actionArea.height
            });
        }
        return buttons;
    }

    static void renderChoiceButtons(const ControlState& control,
                                    const std::vector<Rectangle>& buttons,
                                    const Vector2& mouse) {
        for (std::size_t index = 0; index < buttons.size() && index < control.choices.size(); ++index) {
            const ChoiceState& choice = control.choices[index];
            const bool hovered = CheckCollisionPointRec(mouse, buttons[index]);
            const Color fill = !choice.enabled
                ? Color{58, 62, 72, 220}
                : choice.selected
                    ? (hovered ? Color{92, 132, 218, 255} : Color{74, 112, 196, 255})
                    : (hovered ? Color{70, 82, 104, 255} : Color{54, 66, 86, 255});
            DrawRectangleRounded(buttons[index], 0.22f, 8, fill);
            DrawRectangleRoundedLines(buttons[index], 0.22f, 8, Color{16, 18, 24, 255});

            const std::string label = fitTextToWidth(choice.label, 18, buttons[index].width - 12.0f);
            const int textWidth = measureTextWidth(label, 18);
            drawText(label,
                     buttons[index].x + (buttons[index].width - textWidth) * 0.5f,
                     buttons[index].y + 8.0f,
                     18,
                     RAYWHITE);
        }
    }

    static void renderSlider(const ControlState& control,
                             const Rectangle& track,
                             const Rectangle& bounds,
                             const Vector2& mouse) {
        const bool hovered = CheckCollisionPointRec(mouse, bounds);
        DrawRectangleRounded(track, 0.5f, 10, Color{52, 58, 68, 255});

        const float percent = sliderPercent(control);
        Rectangle fill = track;
        fill.width *= percent;
        DrawRectangleRounded(fill, 0.5f, 10, control.enabled ? Color{86, 136, 226, 255}
                                                             : Color{86, 96, 120, 255});

        const float handleX = track.x + track.width * percent;
        const Rectangle handle{
            handleX - 10.0f,
            track.y - 8.0f,
            20.0f,
            24.0f
        };
        DrawRectangleRounded(handle, 0.4f, 8, hovered ? Color{240, 242, 247, 255}
                                                      : Color{220, 224, 232, 255});
        DrawRectangleRoundedLines(handle, 0.4f, 8, Color{22, 24, 28, 255});

        const int valueWidth = measureTextWidth(control.valueLabel, 20);
        drawText(control.valueLabel,
                 track.x + track.width - valueWidth,
                 track.y - 42.0f,
                 20,
                 RAYWHITE);
    }

    static void handleControlPress(const ControlState& control,
                                   ControlLayout layout,
                                   const Vector2& mouse,
                                   std::vector<Action>* actions) {
        if (actions == nullptr || !control.enabled) {
            return;
        }

        if (control.type == ControlType::Choice) {
            layout.choiceButtons = buildChoiceButtons(control, layout.actionArea);
        }

        switch (control.type) {
            case ControlType::Toggle:
                if (CheckCollisionPointRec(mouse, layout.actionArea)) {
                    actions->push_back(Action{
                        Action::Kind::ToggleChanged,
                        control.id,
                        !control.toggleValue
                    });
                }
                break;
            case ControlType::Choice:
                for (std::size_t index = 0; index < control.choices.size() && index < layout.choiceButtons.size();
                     ++index) {
                    if (control.choices[index].enabled &&
                        CheckCollisionPointRec(mouse, layout.choiceButtons[index])) {
                        actions->push_back(Action{
                            Action::Kind::ChoiceSelected,
                            control.id,
                            false,
                            control.choices[index].value
                        });
                    }
                }
                break;
            case ControlType::Slider:
                break;
        }
    }

    void handleTargetSelection(const State& state,
                               const Layout& layout,
                               const Vector2& mouse,
                               std::vector<Action>* actions) const {
        if (actions == nullptr || state.targets.empty() ||
            !CheckCollisionPointRec(mouse, layout.targetListViewport)) {
            return;
        }

        constexpr float rowHeight = 108.0f;
        constexpr float rowGap = 12.0f;
        const float contentStartY = layout.targetListViewport.y - targetScrollOffset_;
        for (std::size_t index = 0; index < state.targets.size(); ++index) {
            const Rectangle row{
                layout.targetListViewport.x,
                contentStartY + index * (rowHeight + rowGap),
                layout.targetListViewport.width - 10.0f,
                rowHeight
            };
            if (CheckCollisionPointRec(mouse, row)) {
                actions->push_back(Action{
                    Action::Kind::TargetSelected,
                    ControlId::TargetLatency,
                    false,
                    0,
                    0.0f,
                    state.targets[index].targetId
                });
                return;
            }
        }
    }

    void handleEditorPress(const TargetEditorState& editor,
                           const Layout& layout,
                           const Vector2& mouse,
                           std::vector<Action>* actions) {
        if (actions == nullptr || !editor.available) {
            return;
        }

        const EditorLayout editorLayout = buildEditorLayout(layout, editor);
        pressSlider(editor.latency, editorLayout.latency, DragTarget::TargetLatency, mouse, actions);
        pressSlider(editor.loss, editorLayout.loss, DragTarget::TargetLoss, mouse, actions);

        if (editor.showAdvancedNetworkControls) {
            pressSlider(editor.upDelay, editorLayout.advanced[0], DragTarget::UpDelay, mouse, actions);
            pressSlider(editor.downDelay, editorLayout.advanced[1], DragTarget::DownDelay, mouse, actions);
            pressSlider(editor.upLoss, editorLayout.advanced[2], DragTarget::UpLoss, mouse, actions);
            pressSlider(editor.downLoss, editorLayout.advanced[3], DragTarget::DownLoss, mouse, actions);
            pressSlider(editor.upReorder, editorLayout.advanced[4], DragTarget::UpReorder, mouse, actions);
            pressSlider(editor.downReorder, editorLayout.advanced[5], DragTarget::DownReorder, mouse, actions);
        }

    }

    struct SliderLayout {
        Rectangle card{};
        Rectangle track{};
        Rectangle bounds{};
    };

    struct EditorLayout {
        Rectangle card{};
        Rectangle header{};
        SliderLayout latency{};
        SliderLayout loss{};
        SliderLayout advanced[6]{};
    };

    static SliderLayout buildSliderLayout(const Rectangle& card) {
        SliderLayout layout;
        layout.card = card;
        layout.track = Rectangle{
            card.x + 18.0f,
            card.y + card.height - 20.0f,
            card.width - 36.0f,
            8.0f
        };
        layout.bounds = Rectangle{
            layout.track.x,
            layout.track.y - 16.0f,
            layout.track.width,
            44.0f
        };
        return layout;
    }

    static EditorLayout buildEditorLayout(const Layout& layout,
                                          const TargetEditorState& editor) {
        EditorLayout editorLayout;
        editorLayout.card = layout.targetEditorArea;
        editorLayout.header = Rectangle{
            layout.targetEditorArea.x,
            layout.targetEditorArea.y,
            layout.targetEditorArea.width,
            78.0f
        };

        const float fullWidth = layout.targetEditorArea.width;
        const float cellGap = 14.0f;
        const bool splitPrimary = fullWidth >= 520.0f;
        const float primaryTop = layout.targetEditorArea.y + 88.0f;
        const float primaryHeight = 104.0f;
        const float primaryWidth = splitPrimary
            ? (fullWidth - cellGap) * 0.5f
            : fullWidth;
        editorLayout.latency = buildSliderLayout(Rectangle{
            layout.targetEditorArea.x,
            primaryTop,
            primaryWidth,
            primaryHeight
        });
        editorLayout.loss = buildSliderLayout(Rectangle{
            splitPrimary
                ? layout.targetEditorArea.x + primaryWidth + cellGap
                : layout.targetEditorArea.x,
            splitPrimary ? primaryTop : primaryTop + primaryHeight + cellGap,
            primaryWidth,
            primaryHeight
        });

        const float sectionTop = splitPrimary
            ? primaryTop + primaryHeight + 48.0f
            : primaryTop + (primaryHeight * 2.0f) + cellGap + 48.0f;
        const float cellWidth = (fullWidth - cellGap) * 0.5f;
        const float cellHeight = editor.showAdvancedNetworkControls ? 84.0f : 80.0f;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 2; ++column) {
                const int index = row * 2 + column;
                editorLayout.advanced[index] = buildSliderLayout(Rectangle{
                    layout.targetEditorArea.x + column * (cellWidth + cellGap),
                    sectionTop + row * (cellHeight + 12.0f),
                    cellWidth,
                    cellHeight
                });
            }
        }
        return editorLayout;
    }

    void pressSlider(const ControlState& control,
                     const SliderLayout& layout,
                     DragTarget dragTarget,
                     const Vector2& mouse,
                     std::vector<Action>* actions) {
        if (actions == nullptr || !control.visible || !control.enabled ||
            !CheckCollisionPointRec(mouse, layout.bounds)) {
            return;
        }

        activeDrag_ = dragTarget;
        actions->push_back(buildSliderAction(control, valueFromSlider(control, layout.track, mouse)));
    }

    static float valueFromSlider(const ControlState& control,
                                 const Rectangle& track,
                                 const Vector2& mouse) {
        if (control.sliderMax <= control.sliderMin || track.width <= 0.0f) {
            return control.sliderMin;
        }

        const float percent = std::clamp((mouse.x - track.x) / track.width, 0.0f, 1.0f);
        return control.sliderMin + percent * (control.sliderMax - control.sliderMin);
    }

    static Action buildSliderAction(const ControlState& control, float value) {
        return Action{
            Action::Kind::SliderChanged,
            control.id,
            false,
            0,
            value
        };
    }

    void handleDrag(const State& state,
                    const Layout& layout,
                    const Vector2& mouse,
                    std::vector<Action>* actions) const {
        if (actions == nullptr) {
            return;
        }

        if (hasVisibleLeftControls(state)) {
            for (const ControlState& control : state.leftControls) {
                if (!control.visible) {
                    continue;
                }
                const ControlLayout controlLayout =
                    buildLeftControlLayout(layout.leftSection, state.leftControls, control.id);
                if (control.type == ControlType::Slider &&
                    dragTargetFor(control.id) == activeDrag_ &&
                    control.enabled) {
                    actions->push_back(buildSliderAction(
                        control,
                        valueFromSlider(control, controlLayout.sliderTrack, mouse)));
                    return;
                }
            }
        }

        if (!state.showTargetSections || !state.targetEditor.available) {
            return;
        }

        const EditorLayout editorLayout = buildEditorLayout(layout, state.targetEditor);
        const SliderBinding bindings[] = {
            {state.targetEditor.latency, editorLayout.latency, DragTarget::TargetLatency},
            {state.targetEditor.loss, editorLayout.loss, DragTarget::TargetLoss},
            {state.targetEditor.upDelay, editorLayout.advanced[0], DragTarget::UpDelay},
            {state.targetEditor.downDelay, editorLayout.advanced[1], DragTarget::DownDelay},
            {state.targetEditor.upLoss, editorLayout.advanced[2], DragTarget::UpLoss},
            {state.targetEditor.downLoss, editorLayout.advanced[3], DragTarget::DownLoss},
            {state.targetEditor.upReorder, editorLayout.advanced[4], DragTarget::UpReorder},
            {state.targetEditor.downReorder, editorLayout.advanced[5], DragTarget::DownReorder}
        };

        for (const SliderBinding& binding : bindings) {
            if (binding.dragTarget == activeDrag_ &&
                binding.control.visible &&
                binding.control.enabled) {
                actions->push_back(buildSliderAction(
                    binding.control,
                    valueFromSlider(binding.control, binding.layout.track, mouse)));
                return;
            }
        }
    }

    struct SliderBinding {
        ControlState control{};
        SliderLayout layout{};
        DragTarget dragTarget{DragTarget::None};
    };

    static DragTarget dragTargetFor(ControlId controlId) {
        switch (controlId) {
            case ControlId::SmoothWindowMs:
                return DragTarget::SmoothWindow;
            case ControlId::TargetLatency:
                return DragTarget::TargetLatency;
            case ControlId::TargetLoss:
                return DragTarget::TargetLoss;
            case ControlId::UpDelay:
                return DragTarget::UpDelay;
            case ControlId::DownDelay:
                return DragTarget::DownDelay;
            case ControlId::UpLoss:
                return DragTarget::UpLoss;
            case ControlId::DownLoss:
                return DragTarget::DownLoss;
            case ControlId::UpReorder:
                return DragTarget::UpReorder;
            case ControlId::DownReorder:
                return DragTarget::DownReorder;
            default:
                break;
        }
        return DragTarget::None;
    }

    void renderTargetList(const State& state,
                          const Layout& layout,
                          const Vector2& mouse) const {
        if (state.targets.empty()) {
            drawTextFit("No participants available",
                        layout.targetListViewport.x,
                        layout.targetListViewport.y + 8.0f,
                        layout.targetListViewport.width,
                        20,
                        Color{175, 184, 200, 255});
            return;
        }

        BeginScissorMode(static_cast<int>(layout.targetListViewport.x),
                         static_cast<int>(layout.targetListViewport.y),
                         static_cast<int>(layout.targetListViewport.width),
                         static_cast<int>(layout.targetListViewport.height));
        constexpr float rowHeight = 108.0f;
        constexpr float rowGap = 12.0f;
        const float contentStartY = layout.targetListViewport.y - targetScrollOffset_;

        for (std::size_t index = 0; index < state.targets.size(); ++index) {
            const TargetRowState& row = state.targets[index];
            const Rectangle rowRect{
                layout.targetListViewport.x,
                contentStartY + index * (rowHeight + rowGap),
                layout.targetListViewport.width - 10.0f,
                rowHeight
            };

            if ((rowRect.y + rowRect.height) < layout.targetListViewport.y ||
                rowRect.y > (layout.targetListViewport.y + layout.targetListViewport.height)) {
                continue;
            }

            const bool hovered = CheckCollisionPointRec(mouse, rowRect);
            const Color fill = row.selected
                ? Color{48, 74, 116, 240}
                : hovered
                    ? Color{36, 48, 70, 236}
                    : Color{28, 34, 48, 232};
            DrawRectangleRounded(rowRect, 0.03f, 8, fill);
            DrawRectangleRoundedLines(rowRect, 0.03f, 8, Color{82, 108, 156, 255});

            drawTextFit(row.label,
                        rowRect.x + 14.0f,
                        rowRect.y + 10.0f,
                        rowRect.width - 28.0f,
                        22,
                        RAYWHITE);
            drawTextFit(row.detailLine,
                        rowRect.x + 14.0f,
                        rowRect.y + 39.0f,
                        rowRect.width - 28.0f,
                        16,
                        Color{175, 186, 202, 255});
            drawTextFit(row.metricsLine,
                        rowRect.x + 14.0f,
                        rowRect.y + 64.0f,
                        rowRect.width - 28.0f,
                        16,
                        Color{148, 205, 218, 255});
            if (!row.statsLine.empty()) {
                drawTextFit(row.statsLine,
                            rowRect.x + 14.0f,
                            rowRect.y + 84.0f,
                            rowRect.width - 28.0f,
                            15,
                            Color{214, 191, 122, 255});
            }
        }
        EndScissorMode();
    }

    void renderTargetEditor(const TargetEditorState& editor,
                            const Layout& layout,
                            const Vector2& mouse) const {
        if (!editor.available) {
            drawTextFit("Select a participant to edit live settings",
                        layout.targetEditorArea.x,
                        layout.targetEditorArea.y + 8.0f,
                        layout.targetEditorArea.width,
                        22,
                        Color{175, 184, 200, 255});
            return;
        }

        const EditorLayout editorLayout = buildEditorLayout(layout, editor);
        drawTextFit(editor.title,
                    editorLayout.header.x,
                    editorLayout.header.y + 4.0f,
                    editorLayout.header.width,
                    24,
                    RAYWHITE);
        drawTextFit(editor.subtitle,
                    editorLayout.header.x,
                    editorLayout.header.y + 42.0f,
                    editorLayout.header.width,
                    16,
                    Color{174, 184, 201, 255});

        renderEditorSlider(editor.latency, editorLayout.latency, mouse);
        renderEditorSlider(editor.loss, editorLayout.loss, mouse);

        if (editor.showAdvancedNetworkControls) {
            drawTextFit("Host Proxy Controls",
                        layout.targetEditorArea.x,
                        editorLayout.advanced[0].card.y - 30.0f,
                        layout.targetEditorArea.width,
                        20,
                        Color{244, 210, 130, 255});
            renderEditorSlider(editor.upDelay, editorLayout.advanced[0], mouse);
            renderEditorSlider(editor.downDelay, editorLayout.advanced[1], mouse);
            renderEditorSlider(editor.upLoss, editorLayout.advanced[2], mouse);
            renderEditorSlider(editor.downLoss, editorLayout.advanced[3], mouse);
            renderEditorSlider(editor.upReorder, editorLayout.advanced[4], mouse);
            renderEditorSlider(editor.downReorder, editorLayout.advanced[5], mouse);
        }

        float statusY = layout.targetEditorArea.y + layout.targetEditorArea.height - 76.0f;
        for (std::size_t index = 0; index < editor.statusLines.size() && index < 3u; ++index) {
            drawTextFit(editor.statusLines[index],
                        layout.targetEditorArea.x,
                        statusY + index * 20.0f,
                        layout.targetEditorArea.width,
                        16,
                        Color{168, 181, 202, 255});
        }
    }

    static void renderEditorSlider(const ControlState& control,
                                   const SliderLayout& layout,
                                   const Vector2& mouse) {
        if (!control.visible) {
            return;
        }
        const bool hovered = CheckCollisionPointRec(mouse, layout.card);
        DrawRectangleRounded(layout.card,
                             0.03f,
                             8,
                             cardFill(control, hovered));
        DrawRectangleRoundedLines(layout.card, 0.03f, 8, cardBorder(control));
        drawTextFit(control.label,
                    layout.card.x + 14.0f,
                    layout.card.y + 8.0f,
                    layout.card.width - 28.0f,
                    20,
                    RAYWHITE);
        renderSlider(control, layout.track, layout.bounds, mouse);
    }
};
