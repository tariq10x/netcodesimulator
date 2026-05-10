#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <raylib.h>

#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "MainMenu.hpp"
#include "TypographyService.hpp"
#include "app/UserSettings.hpp"
#include "input/ControlBindings.hpp"
#include "net/SessionLaunchConfig.hpp"

class SettingsMenu {
public:
    struct SessionLaunchDefaults {
        std::uint8_t localParticipantCount{net::kDefaultLocalParticipantCount};
        net::ShotEvaluationMode preferredShotEvaluationMode{
            net::ShotEvaluationMode::SeenPosition};
    };

    explicit SettingsMenu(const app::UserSettings& userSettings = {})
        : selectedPreset_(display::currentResolutionPreset()),
          workingSettings_(userSettings) {}

    static void applySessionLaunchDefaults(const SessionLaunchDefaults& defaults,
                                           net::SessionLaunchConfig* config) {
        if (config == nullptr) {
            return;
        }

        config->localParticipantCount =
            defaults.localParticipantCount == 0u
                ? net::kDefaultLocalParticipantCount
                : defaults.localParticipantCount;
        if (config->entryPoint == net::SessionEntryPoint::Host ||
            config->entryPoint == net::SessionEntryPoint::LabStudy) {
            config->shotEvaluationMode = defaults.preferredShotEvaluationMode;
        }
        net::normalizeSessionLaunchConfig(config);
    }

    GameMode update(bool allowBackShortcut = true) {
        if (captureMode_) {
            return updateCapture();
        }

        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            clickReady_ = true;
        }
        const bool leftClickPressed = clickReady_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

        const Vector2 mousePos = display::mousePosition();
        const bool resolutionHovered = CheckCollisionPointRec(mousePos, resolutionBox_);
        const bool applyHovered = CheckCollisionPointRec(mousePos, applyButton_);
        const bool resetHovered = CheckCollisionPointRec(mousePos, resetControlsButton_);
        const bool backHovered = CheckCollisionPointRec(mousePos, backButton_);

        if (resolutionHovered && leftClickPressed) {
            cycleSelectedPreset(1);
        }

        for (std::size_t actionIndex = 0; actionIndex < input::actionDescriptors().size(); ++actionIndex) {
            for (std::size_t slotIndex = 0; slotIndex < 2u; ++slotIndex) {
                if (CheckCollisionPointRec(mousePos, bindingSlotRect(actionIndex, slotIndex)) &&
                    leftClickPressed) {
                    beginCapture(actionIndex, slotIndex);
                    return GameMode::SETTINGS;
                }
            }
        }

        if (resetHovered && leftClickPressed) {
            workingSettings_.controls.resetToDefaults();
            statusMessage_.clear();
            return GameMode::SETTINGS;
        }

        if (applyHovered && leftClickPressed) {
            applyChanges();
            return GameMode::SETTINGS;
        }
        if (backHovered && leftClickPressed) {
            return GameMode::MAIN_MENU;
        }

        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) {
            cycleSelectedPreset(-1);
        }
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) {
            cycleSelectedPreset(1);
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
            applyChanges();
            return GameMode::SETTINGS;
        }
        if (allowBackShortcut && (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q))) {
            return GameMode::MAIN_MENU;
        }

        return GameMode::SETTINGS;
    }

    void render() const {
        ClearBackground(Color{15, 15, 20, 255});
        TypographyService& typography = TypographyService::shared();

        DrawRectangleGradientV(0, 0, Config::SCREEN_WIDTH, Config::SCREEN_HEIGHT,
                               Color{11, 13, 18, 255},
                               Color{18, 22, 30, 255});
        DrawCircleV(Vector2{250.0f, 170.0f}, 220.0f, Color{22, 72, 125, 60});
        DrawCircleV(Vector2{1650.0f, 150.0f}, 240.0f, Color{11, 92, 120, 40});

        typography.drawCentered(TypographyStyleId::ScreenTitle,
                                "SETTINGS",
                                Config::SCREEN_WIDTH * 0.5f,
                                96.0f,
                                SKYBLUE);

        renderDisplayPanel();
        renderControlsPanel();

        renderButton(applyButton_, "Apply", true);
        renderButton(resetControlsButton_, "Defaults", false);
        renderButton(backButton_, "Back", false);

        if (!statusMessage_.empty()) {
            typography.drawCentered(TypographyStyleId::AppSubtitle,
                                    statusMessage_,
                                    Config::SCREEN_WIDTH * 0.5f,
                                    930.0f,
                                    Color{255, 224, 64, 255});
        }
    }

    display::ResolutionPreset selectedPreset() const {
        return selectedPreset_;
    }

    SessionLaunchDefaults sessionLaunchDefaults() const {
        return sessionLaunchDefaults_;
    }

    const app::UserSettings& userSettings() const {
        return workingSettings_;
    }

    bool consumeApplyRequested() {
        const bool requested = applyRequested_;
        applyRequested_ = false;
        return requested;
    }

    void setStatusMessage(const std::string& statusMessage) {
        statusMessage_ = statusMessage;
    }

    void setSelectedPresetForTest(display::ResolutionPreset preset) {
        selectedPreset_ = preset;
    }

    void setSessionLocalParticipantCountForTest(std::uint8_t localParticipantCount) {
        sessionLaunchDefaults_.localParticipantCount =
            localParticipantCount == 0u
                ? net::kDefaultLocalParticipantCount
                : localParticipantCount;
    }

    void setSessionShotEvaluationModeForTest(net::ShotEvaluationMode mode) {
        sessionLaunchDefaults_.preferredShotEvaluationMode = mode;
    }

    void selectNextPresetForTest() {
        cycleSelectedPreset(1);
    }

    void applySelectedPresetForTest() {
        applySelectedPreset();
    }

    bool clickReadyForTest() const {
        return clickReady_;
    }

    void markClickReadyForTest() {
        clickReady_ = true;
    }

    const std::string& statusMessage() const {
        return statusMessage_;
    }

private:
    void cycleSelectedPreset(int direction) {
        constexpr std::array<display::ResolutionPreset, 2> presets{
            display::ResolutionPreset::HD1080,
            display::ResolutionPreset::HD540
        };

        std::size_t currentIndex = 0u;
        for (std::size_t index = 0; index < presets.size(); ++index) {
            if (presets[index] == selectedPreset_) {
                currentIndex = index;
                break;
            }
        }

        const int offset = direction >= 0 ? 1 : -1;
        const int wrapped = (static_cast<int>(currentIndex) + offset +
                             static_cast<int>(presets.size())) %
                            static_cast<int>(presets.size());
        selectedPreset_ = presets[static_cast<std::size_t>(wrapped)];
    }

    void applySelectedPreset() {
        display::applyResolutionPreset(selectedPreset_);
        statusMessage_.clear();
    }

    void applyChanges() {
        applySelectedPreset();
        applyRequested_ = true;
        statusMessage_.clear();
    }

    void beginCapture(std::size_t actionIndex, std::size_t slotIndex) {
        captureMode_ = true;
        captureActionIndex_ = actionIndex;
        captureSlotIndex_ = slotIndex;
        statusMessage_.clear();
    }

    GameMode updateCapture() {
        if (IsKeyPressed(KEY_ESCAPE)) {
            captureMode_ = false;
            statusMessage_.clear();
            return GameMode::SETTINGS;
        }

        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE)) {
            if (input::ActionBinding* binding =
                    workingSettings_.controls.mutableBinding(
                        input::actionDescriptors()[captureActionIndex_].id);
                binding != nullptr) {
                binding->slots[captureSlotIndex_] = input::unboundToken();
            }
            captureMode_ = false;
            statusMessage_.clear();
            return GameMode::SETTINGS;
        }

        input::InputToken token;
        if (!input::detectFirstPressedToken(&token)) {
            return GameMode::SETTINGS;
        }

        if (input::ActionBinding* binding =
                workingSettings_.controls.mutableBinding(
                    input::actionDescriptors()[captureActionIndex_].id);
            binding != nullptr) {
            binding->slots[captureSlotIndex_] = token;
        }

        captureMode_ = false;
        statusMessage_.clear();
        return GameMode::SETTINGS;
    }

    enum class ControlsSection {
        Gameplay = 0,
        Spectator = 1
    };

    Rectangle sectionPanelRect(ControlsSection section) const {
        return section == ControlsSection::Gameplay ? gameplayPanel_ : spectatorPanel_;
    }

    Color sectionAccent(ControlsSection section) const {
        return section == ControlsSection::Gameplay
            ? Color{93, 170, 255, 255}
            : Color{255, 196, 95, 255};
    }

    const char* sectionTitle(ControlsSection section) const {
        return section == ControlsSection::Gameplay ? "Gameplay" : "Spectator";
    }

    ControlsSection sectionForAction(input::ActionId action) const {
        switch (action) {
            case input::ActionId::SpectatorAscend:
            case input::ActionId::SpectatorDescend:
            case input::ActionId::SpectatorBoost:
                return ControlsSection::Spectator;
            case input::ActionId::MoveForward:
            case input::ActionId::MoveBackward:
            case input::ActionId::MoveLeft:
            case input::ActionId::MoveRight:
            case input::ActionId::Jump:
            case input::ActionId::FirePrimary:
            case input::ActionId::Count:
                return ControlsSection::Gameplay;
        }
        return ControlsSection::Gameplay;
    }

    std::size_t rowIndexForAction(input::ActionId action) const {
        switch (action) {
            case input::ActionId::MoveForward:
                return 0u;
            case input::ActionId::MoveBackward:
                return 0u;
            case input::ActionId::MoveLeft:
                return 1u;
            case input::ActionId::MoveRight:
                return 1u;
            case input::ActionId::Jump:
                return 2u;
            case input::ActionId::FirePrimary:
                return 2u;
            case input::ActionId::SpectatorAscend:
                return 0u;
            case input::ActionId::SpectatorDescend:
                return 1u;
            case input::ActionId::SpectatorBoost:
                return 2u;
            case input::ActionId::Count:
                break;
        }
        return 0u;
    }

    std::size_t columnIndexForAction(input::ActionId action) const {
        switch (action) {
            case input::ActionId::MoveBackward:
            case input::ActionId::MoveRight:
            case input::ActionId::FirePrimary:
                return 1u;
            case input::ActionId::MoveForward:
            case input::ActionId::MoveLeft:
            case input::ActionId::Jump:
            case input::ActionId::SpectatorAscend:
            case input::ActionId::SpectatorDescend:
            case input::ActionId::SpectatorBoost:
            case input::ActionId::Count:
                return 0u;
        }
        return 0u;
    }

    Rectangle actionRowRect(input::ActionId action) const {
        const ControlsSection section = sectionForAction(action);
        const Rectangle panel = sectionPanelRect(section);
        const float rowHeight = 68.0f;
        const float rowGap = 12.0f;
        const float startY = panel.y + 112.0f;
        const float panelPadding = 22.0f;
        const float columnGap = 18.0f;
        const float rowWidth = section == ControlsSection::Gameplay
            ? (panel.width - panelPadding * 2.0f - columnGap) * 0.5f
            : (panel.width - panelPadding * 2.0f);
        const float x = section == ControlsSection::Gameplay
            ? panel.x + panelPadding + static_cast<float>(columnIndexForAction(action)) * (rowWidth + columnGap)
            : panel.x + panelPadding;
        return Rectangle{
            x,
            startY + static_cast<float>(rowIndexForAction(action)) * (rowHeight + rowGap),
            rowWidth,
            rowHeight
        };
    }

    Rectangle bindingSlotRect(input::ActionId action, std::size_t slotIndex) const {
        const ControlsSection section = sectionForAction(action);
        const Rectangle row = actionRowRect(action);
        const float slotWidth = section == ControlsSection::Gameplay ? 124.0f : 164.0f;
        const float slotGap = 10.0f;
        const float slotHeight = 52.0f;
        const float x = row.x + row.width - (slotWidth * 2.0f + slotGap) +
                        static_cast<float>(slotIndex) * (slotWidth + slotGap);
        return Rectangle{x, row.y + (row.height - slotHeight) * 0.5f, slotWidth, slotHeight};
    }

    Rectangle bindingSlotRect(std::size_t actionIndex, std::size_t slotIndex) const {
        return bindingSlotRect(input::actionDescriptors()[actionIndex].id, slotIndex);
    }

    void renderDisplayPanel() const {
        TypographyService& typography = TypographyService::shared();
        DrawRectangleRounded(displayPanel_, 0.08f, 18, Color{24, 31, 43, 242});
        DrawRectangleRoundedLines(displayPanel_, 0.08f, 18, Color{98, 170, 255, 255});
        DrawRectangleRounded(Rectangle{displayPanel_.x, displayPanel_.y, displayPanel_.width, 10.0f},
                             0.18f,
                             10,
                             Color{98, 170, 255, 255});

        typography.draw(TypographyStyleId::ScoreboardTitle,
                        "Display",
                        Vector2{displayPanel_.x + 42.0f, displayPanel_.y + 26.0f},
                        WHITE);

        DrawRectangleRounded(resolutionBox_, 0.18f, 16, Color{44, 57, 81, 255});
        DrawRectangleRoundedLines(resolutionBox_, 0.18f, 16, Color{126, 191, 255, 255});

        const char* selectedLabel = display::resolutionOption(selectedPreset_).label;
        typography.drawCentered(TypographyStyleId::EditorTitle,
                                selectedLabel,
                                resolutionBox_.x + resolutionBox_.width * 0.5f,
                                resolutionBox_.y + 18.0f,
                                RAYWHITE);
        const std::string currentLine =
            std::string("Current: ") + display::currentWindowLabel();
        const Rectangle currentChip{displayPanel_.x + 42.0f, displayPanel_.y + 74.0f, 240.0f, 42.0f};
        DrawRectangleRounded(currentChip, 0.22f, 14, Color{34, 44, 61, 255});
        DrawRectangleRoundedLines(currentChip, 0.22f, 14, Color{72, 88, 112, 255});
        typography.draw(TypographyStyleId::Body,
                        currentLine,
                        Vector2{currentChip.x + 16.0f, currentChip.y + 8.0f},
                        LIGHTGRAY);
    }

    void renderControlsPanel() const {
        TypographyService& typography = TypographyService::shared();
        typography.draw(TypographyStyleId::OverlayTitle,
                        "Key Bindings",
                        Vector2{160.0f, 368.0f},
                        WHITE);

        renderControlsSection(ControlsSection::Gameplay);
        renderControlsSection(ControlsSection::Spectator);
    }

    void renderControlsSection(ControlsSection section) const {
        TypographyService& typography = TypographyService::shared();
        const Rectangle panel = sectionPanelRect(section);
        const Color accent = sectionAccent(section);

        DrawRectangleRounded(panel, 0.08f, 18, Color{23, 28, 39, 244});
        DrawRectangleRoundedLines(panel, 0.08f, 18, Color{74, 92, 120, 255});
        DrawRectangleRounded(Rectangle{panel.x, panel.y, panel.width, 10.0f},
                             0.18f,
                             10,
                             accent);

        typography.draw(TypographyStyleId::ScoreboardTitle,
                        sectionTitle(section),
                        Vector2{panel.x + 24.0f, panel.y + 22.0f},
                        WHITE);
        const float headerY = panel.y + 72.0f;
        const input::ActionId anchorAction =
            section == ControlsSection::Gameplay ? input::ActionId::MoveForward
                                                 : input::ActionId::SpectatorAscend;
        const Rectangle primaryHeader = bindingSlotRect(anchorAction, 0u);
        const Rectangle secondaryHeader = bindingSlotRect(anchorAction, 1u);
        typography.drawCentered(TypographyStyleId::Caption,
                                "Primary",
                                primaryHeader.x + primaryHeader.width * 0.5f,
                                headerY,
                                Color{144, 158, 182, 255});
        typography.drawCentered(TypographyStyleId::Caption,
                                "Alt",
                                secondaryHeader.x + secondaryHeader.width * 0.5f,
                                headerY,
                                Color{144, 158, 182, 255});

        if (section == ControlsSection::Gameplay) {
            for (input::ActionId action : {
                     input::ActionId::MoveForward,
                     input::ActionId::MoveBackward,
                     input::ActionId::MoveLeft,
                     input::ActionId::MoveRight,
                     input::ActionId::Jump,
                     input::ActionId::FirePrimary,
                 }) {
                renderActionRow(action);
            }
        } else {
            for (input::ActionId action : {
                     input::ActionId::SpectatorAscend,
                     input::ActionId::SpectatorDescend,
                     input::ActionId::SpectatorBoost,
                 }) {
                renderActionRow(action);
            }
        }
    }

    void renderActionRow(input::ActionId action) const {
        TypographyService& typography = TypographyService::shared();
        const Rectangle row = actionRowRect(action);
        const bool conflicted = input::bindingConflicts(workingSettings_.controls, action);
        const bool selectedRow = captureMode_ &&
                                 input::actionDescriptors()[captureActionIndex_].id == action;
        const Color rowFill = selectedRow
            ? Color{58, 51, 36, 255}
            : Color{32, 39, 53, 255};
        const Color rowBorder = selectedRow
            ? Color{255, 224, 64, 255}
            : (conflicted ? Color{214, 170, 85, 255} : Color{67, 84, 109, 255});

        DrawRectangleRounded(row, 0.16f, 12, rowFill);
        DrawRectangleRoundedLines(row, 0.16f, 12, rowBorder);

        const input::ActionDescriptor& descriptor = input::descriptor(action);
        typography.draw(TypographyStyleId::FieldLabel,
                        descriptor.label,
                        Vector2{row.x + 18.0f, row.y + 10.0f},
                        RAYWHITE);

        renderBindingSlot(action, 0u);
        renderBindingSlot(action, 1u);
    }

    void renderBindingSlot(input::ActionId action, std::size_t slotIndex) const {
        const Rectangle rect = bindingSlotRect(action, slotIndex);
        const bool hovered = CheckCollisionPointRec(display::mousePosition(), rect);
        const bool selected = captureMode_ &&
                              input::actionDescriptors()[captureActionIndex_].id == action &&
                              slotIndex == captureSlotIndex_;
        const input::ActionDescriptor& descriptor = input::descriptor(action);
        const input::ActionBinding& binding = workingSettings_.controls.binding(descriptor.id);
        const Color fill = selected
            ? Color{118, 86, 40, 255}
            : (hovered ? Color{76, 90, 118, 255} : Color{48, 58, 80, 255});
        const Color border = selected ? Color{255, 224, 64, 255}
                                      : (hovered ? SKYBLUE : Color{92, 106, 134, 255});

        DrawRectangleRounded(rect, 0.18f, 12, fill);
        DrawRectangleRoundedLines(rect, 0.18f, 12, border);

        const std::string label = selected
            ? "Press Any Key"
            : input::tokenDisplayName(binding.slots[slotIndex]);
        TypographyService& typography = TypographyService::shared();
        const TypographyStyle& style = typography.style(TypographyStyleId::Body);
        typography.drawCentered(TypographyStyleId::Body,
                                label,
                                rect.x + rect.width * 0.5f,
                                rect.y + rect.height * 0.5f - style.lineHeight * 0.5f,
                                WHITE);
    }

    void renderButton(const Rectangle& rect, const char* label, bool primary) const {
        const bool hovered = CheckCollisionPointRec(display::mousePosition(), rect);
        const Color fill = hovered
            ? (primary ? Color{86, 142, 234, 255} : Color{82, 92, 112, 255})
            : (primary ? Color{62, 110, 190, 255} : Color{50, 56, 72, 255});
        const Color border = hovered ? SKYBLUE : DARKGRAY;

        DrawRectangleRounded(rect, 0.18f, 12, fill);
        DrawRectangleRoundedLines(rect, 0.18f, 12, border);

        TypographyService& typography = TypographyService::shared();
        const TypographyStyle& style = typography.style(TypographyStyleId::ButtonLabel);
        typography.drawCentered(TypographyStyleId::ButtonLabel,
                                label,
                                rect.x + rect.width * 0.5f,
                                rect.y + rect.height * 0.5f - style.lineHeight * 0.5f,
                                WHITE);
    }

    display::ResolutionPreset selectedPreset_{display::ResolutionPreset::HD1080};
    app::UserSettings workingSettings_{};
    SessionLaunchDefaults sessionLaunchDefaults_{};
    std::string statusMessage_{};
    bool applyRequested_{false};
    bool captureMode_{false};
    bool clickReady_{false};
    std::size_t captureActionIndex_{0u};
    std::size_t captureSlotIndex_{0u};
    const Rectangle displayPanel_{160.0f, 156.0f, 1600.0f, 126.0f};
    const Rectangle gameplayPanel_{160.0f, 404.0f, 980.0f, 396.0f};
    const Rectangle spectatorPanel_{1160.0f, 404.0f, 600.0f, 396.0f};
    const Rectangle resolutionBox_{1180.0f, 178.0f, 522.0f, 82.0f};
    const Rectangle applyButton_{Config::SCREEN_WIDTH / 2.0f - 360.0f, 836.0f, 220.0f, 72.0f};
    const Rectangle resetControlsButton_{Config::SCREEN_WIDTH / 2.0f - 110.0f, 836.0f, 220.0f, 72.0f};
    const Rectangle backButton_{Config::SCREEN_WIDTH / 2.0f + 140.0f, 836.0f, 220.0f, 72.0f};
};
