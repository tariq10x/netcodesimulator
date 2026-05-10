#pragma once

#include <raylib.h>

#include <array>
#include <string>
#include <utility>

#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "LevelData.hpp"
#include "LevelSlotPreview.hpp"
#include "MainMenu.hpp"
#include "TypographyService.hpp"

class LevelSelectMenu {
private:
    static constexpr int GRID_COLS = app_shell::level_slots::kGridCols;
    static constexpr int GRID_ROWS = app_shell::level_slots::kGridRows;
    static constexpr int TOTAL_SLOTS = app_shell::level_slots::kTotalSlots;

    struct LevelSlot {
        Rectangle rect;
        bool hovered;
        app_shell::level_slots::PreviewData preview{};
    };

    std::array<LevelSlot, TOTAL_SLOTS> slots;
    int selectedSlot;
    bool clickReady;
    std::string titleText_;
    std::string instructionsText_;
    Rectangle backButton_{40.0f, 56.0f, 180.0f, 48.0f};

public:
    static net::SessionLaunchConfig buildLaunchConfigForSelection(
        const MainMenu::NavigationSelection& selection,
        int levelSlot,
        const std::string& playerName = "player",
        std::uint8_t localParticipantCount = net::kDefaultLocalParticipantCount,
        net::ShotEvaluationMode shotEvaluationMode = net::ShotEvaluationMode::SeenPosition) {
        switch (selection.entryPoint) {
            case net::SessionEntryPoint::Host:
                return net::makeHostSessionLaunchConfig(levelSlot,
                                                        playerName,
                                                        net::kDefaultServerPort,
                                                        net::kDefaultHostedBotCount,
                                                        net::kDefaultHostedBotCount,
                                                        0u,
                                                        net::kDefaultProxyServerPort,
                                                        net::kProtocolVersion,
                                                        sim::TeamId::Attacker,
                                                        localParticipantCount,
                                                        shotEvaluationMode);
            case net::SessionEntryPoint::LabStudy:
                return net::makeStudySessionLaunchConfig(levelSlot,
                                                         playerName,
                                                         net::kDefaultServerPort,
                                                         1u,
                                                         1u,
                                                         shotEvaluationMode,
                                                         localParticipantCount);
            case net::SessionEntryPoint::Replay:
                return net::makeReplaySessionLaunchConfig(levelSlot, localParticipantCount);
            case net::SessionEntryPoint::LevelEditor:
                return net::makeEditorSessionLaunchConfig(levelSlot);
            case net::SessionEntryPoint::Join:
            case net::SessionEntryPoint::None: {
                net::SessionLaunchConfig config;
                config.surface = selection.surface == MainMenu::AppShellSurface::LabStudy
                    ? net::SessionProductSurface::LabStudy
                    : net::SessionProductSurface::Multiplayer;
                config.entryPoint = selection.entryPoint;
                config.levelSlot = levelSlot;
                config.levelHash = net::makeLevelIdentityHash(levelSlot);
                config.playerName = playerName;
                config.localParticipantCount = localParticipantCount;
                config.shotEvaluationMode = shotEvaluationMode;
                net::normalizeSessionLaunchConfig(&config);
                return config;
            }
        }
        net::SessionLaunchConfig config;
        net::normalizeSessionLaunchConfig(&config);
        return config;
    }

    explicit LevelSelectMenu(std::string titleText = "SELECT LEVEL",
                             std::string instructionsText =
                                 "Click a level to continue | Number keys 1-9 to select | Q/ESC to return")
        : selectedSlot(-1),
          clickReady(false),
          titleText_(std::move(titleText)),
          instructionsText_(std::move(instructionsText)) {
        const float slotWidth = 350.0f;
        const float slotHeight = 200.0f;
        const float spacingX = 50.0f;
        const float spacingY = 50.0f;
        const float startX =
            (1920.0f - (GRID_COLS * slotWidth + (GRID_COLS - 1) * spacingX)) / 2.0f;
        const float startY = 250.0f;

        for (int row = 0; row < GRID_ROWS; ++row) {
            for (int col = 0; col < GRID_COLS; ++col) {
                const int index = row * GRID_COLS + col;
                const int slotNum = index + 1;

                slots[index].rect = Rectangle{
                    startX + col * (slotWidth + spacingX),
                    startY + row * (slotHeight + spacingY),
                    slotWidth,
                    slotHeight
                };
                slots[index].hovered = false;
                slots[index].preview = app_shell::level_slots::loadPreviewData(slotNum);
            }
        }
    }

    struct SelectResult {
        GameMode mode;
        int levelSlot;
    };

    const std::string& titleText() const {
        return titleText_;
    }

    const std::string& instructionsText() const {
        return instructionsText_;
    }

    Rectangle backButtonRectForTest() const {
        return backButton_;
    }

    SelectResult triggerBackForTest() const {
        return SelectResult{GameMode::MAIN_MENU, -1};
    }

    SelectResult update(bool allowBackShortcut = true) {
        Vector2 mousePos = display::mousePosition();

        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            clickReady = true;
        }

        if (CheckCollisionPointRec(mousePos, backButton_) &&
            clickReady &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            return SelectResult{GameMode::MAIN_MENU, -1};
        }

        for (int i = 0; i < TOTAL_SLOTS; ++i) {
            slots[i].hovered = CheckCollisionPointRec(mousePos, slots[i].rect);
            if (slots[i].hovered && clickReady && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                selectedSlot = slots[i].preview.slotNumber;
                return SelectResult{GameMode::GAMEPLAY, selectedSlot};
            }
        }

        for (int i = 0; i < 9; ++i) {
            const int key = KEY_ONE + i;
            if (IsKeyPressed(key)) {
                selectedSlot = slots[i].preview.slotNumber;
                return SelectResult{GameMode::GAMEPLAY, selectedSlot};
            }
        }

        if (allowBackShortcut && (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q))) {
            return SelectResult{GameMode::MAIN_MENU, -1};
        }

        return SelectResult{GameMode::LEVEL_SELECT, -1};
    }

    void render() {
        ClearBackground(Color{15, 15, 20, 255});
        TypographyService& typography = TypographyService::shared();

        typography.drawCentered(TypographyStyleId::ScreenTitle,
                                titleText_,
                                1920.0f * 0.5f,
                                100.0f,
                                SKYBLUE);

        const bool backHovered = CheckCollisionPointRec(display::mousePosition(), backButton_);
        DrawRectangleRounded(backButton_,
                             0.18f,
                             8,
                             backHovered ? Color{56, 72, 96, 255} : Color{34, 40, 56, 255});
        DrawRectangleRoundedLines(backButton_,
                                  0.18f,
                                  8,
                                  backHovered ? SKYBLUE : Color{88, 100, 126, 255});
        const TypographyStyle& backStyle = typography.style(TypographyStyleId::ButtonLabel);
        typography.drawCentered(TypographyStyleId::ButtonLabel,
                                "Back",
                                backButton_.x + backButton_.width * 0.5f,
                                backButton_.y + backButton_.height * 0.5f -
                                    backStyle.lineHeight * 0.5f,
                                LIGHTGRAY);

        for (int i = 0; i < TOTAL_SLOTS; ++i) {
            const auto& slot = slots[i];

            const bool isSelected = (selectedSlot == slot.preview.slotNumber);
            const bool highlight = slot.hovered || isSelected;

            Color bgColor;
            Color borderColor;
            if (slot.preview.exists) {
                bgColor = highlight ? Color{60, 70, 90, 255} : Color{30, 35, 50, 255};
                borderColor = highlight ? SKYBLUE : DARKGRAY;
            } else {
                bgColor = highlight ? Color{50, 55, 70, 255} : Color{20, 20, 25, 255};
                borderColor = highlight ? SKYBLUE : Color{50, 50, 55, 255};
            }

            DrawRectangleRounded(slot.rect, 0.1f, 12, bgColor);
            DrawRectangleRoundedLines(slot.rect, 0.1f, 12, borderColor);

            app_shell::level_slots::renderPreview(slot.preview, slot.rect);

            const char* slotText = TextFormat("%d", slot.preview.slotNumber);
            typography.draw(TypographyStyleId::EditorTitle,
                            slotText,
                            Vector2{slot.rect.x + 20.0f, slot.rect.y + 20.0f},
                            slot.preview.exists ? LIGHTGRAY : DARKGRAY);

            const Color nameColor = slot.preview.exists ? WHITE : GRAY;
            if (!slot.preview.exists) {
                typography.drawCentered(TypographyStyleId::SectionTitle,
                                        slot.preview.name,
                                        slot.rect.x + slot.rect.width * 0.5f,
                                        slot.rect.y + slot.rect.height - 76.0f,
                                        nameColor);
                const char* emptyText = "(Create in Level Editor)";
                typography.drawCentered(TypographyStyleId::Caption,
                                        emptyText,
                                        slot.rect.x + slot.rect.width * 0.5f,
                                        slot.rect.y + slot.rect.height - 36.0f,
                                        DARKGRAY);
            } else {
                typography.drawCentered(TypographyStyleId::SectionTitle,
                                        slot.preview.name,
                                        slot.rect.x + slot.rect.width * 0.5f,
                                        slot.rect.y + slot.rect.height - 48.0f,
                                        nameColor);
            }
        }

        if (!instructionsText_.empty()) {
            typography.drawCentered(TypographyStyleId::MenuInstruction,
                                    instructionsText_,
                                    1920.0f * 0.5f,
                                    1000.0f,
                                    LIGHTGRAY);
        }
    }
};
