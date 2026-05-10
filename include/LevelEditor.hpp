#pragma once
#include <raylib.h>
#include <raymath.h>
#include <array>
#include "Arena3D.hpp"
#include "DisplayManager.hpp"
#include "LevelData.hpp"
#include "LevelSlotPreview.hpp"
#include "MainMenu.hpp"
#include "Config3D.hpp"
#include "TypographyService.hpp"
#include "editor/LevelEditorDomain.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string_view>

class LevelEditor {
private:
    static constexpr float kEdgePanMarginPx = 36.0f;
    static constexpr float kZoomStepFovy = 3.0f;
    static constexpr float kMinCameraFovy = 24.0f;
    static constexpr float kMaxCameraFovy = 70.0f;

    LevelData::LevelDefinition currentLevel;
    int currentSlot;
    Camera3D camera;

    // Editing state
    int selectedObstacleIndex;
    int selectedAreaIndex;
    int selectedEnemyIndex;
    LevelData::Obstacle previewObstacle;
    LevelData::Area previewArea;
    LevelData::EnemySpawn previewEnemy;
    bool showPreview;
    editor::PaletteModel paletteModel_;
    std::string statusMessage_;
    float statusMessageTimerSeconds_{0.0f};

    // Drag state for click-and-drag obstacle creation
    bool draggingPlacement;
    bool draggingSelection;
    editor::GridCell dragStartCell;
    editor::SelectionRef draggedSelection_{};
    std::vector<LevelData::Obstacle> dragPreviewObstacles;
    std::vector<LevelData::Area> dragPreviewAreas;

    // Brush settings (snap to grid units)
    int brushSizeUnits;   // width/depth in grid squares
    int brushHeightUnits; // height in grid squares

    editor::ToolKind activeTool;

    // UI state
    bool showSlotSelector;
    enum class SlotSelectorAction {
        SAVE,
        LOAD
    };
    SlotSelectorAction slotSelectorAction;
    char levelNameBuffer[64];
    std::array<app_shell::level_slots::PreviewData, app_shell::level_slots::kTotalSlots> slotPreviewCards_{};
    bool slotPreviewCardsDirty_{true};
    bool editingLevelName_{false};

    // Grid snapping
    static constexpr float GRID_SNAP = 1.0f;
    static constexpr int MIN_SIZE_UNITS = 1;
    static constexpr int MIN_HEIGHT_UNITS = 1;

    int getMaxGridUnits() const {
        return static_cast<int>(Config::ARENA_SIZE * 2);
    }

    enum class ActiveField {
        NONE,
        WIDTH,
        DEPTH,
        HEIGHT
    };

    ActiveField activeField;
    bool editingField;
    char widthInput[16];
    char depthInput[16];
    char heightInput[16];

    bool checkObstacleCollision(const LevelData::Obstacle& a, const LevelData::Obstacle& b) const {
        // AABB collision check (2D, on XZ plane)
        float aMinX = a.x - a.width / 2.0f;
        float aMaxX = a.x + a.width / 2.0f;
        float aMinZ = a.z - a.depth / 2.0f;
        float aMaxZ = a.z + a.depth / 2.0f;

        float bMinX = b.x - b.width / 2.0f;
        float bMaxX = b.x + b.width / 2.0f;
        float bMinZ = b.z - b.depth / 2.0f;
        float bMaxZ = b.z + b.depth / 2.0f;

        return (aMinX < bMaxX && aMaxX > bMinX &&
                aMinZ < bMaxZ && aMaxZ > bMinZ);
    }

    void syncInputsToSelected() {
        if (selectedObstacleIndex >= 0 && selectedObstacleIndex < static_cast<int>(currentLevel.obstacles.size())) {
            const auto& sel = currentLevel.obstacles[selectedObstacleIndex];
            int wUnits = static_cast<int>(std::round(sel.width / GRID_SNAP));
            int dUnits = static_cast<int>(std::round(sel.depth / GRID_SNAP));
            int hUnits = static_cast<int>(std::round(sel.height / GRID_SNAP));
            std::snprintf(widthInput, sizeof(widthInput), "%d", wUnits);
            std::snprintf(depthInput, sizeof(depthInput), "%d", dUnits);
            std::snprintf(heightInput, sizeof(heightInput), "%d", hUnits);
        } else if (selectedAreaIndex >= 0 && selectedAreaIndex < static_cast<int>(currentLevel.areas.size())) {
            const auto& sel = currentLevel.areas[selectedAreaIndex];
            int wUnits = static_cast<int>(std::round(sel.width / GRID_SNAP));
            int dUnits = static_cast<int>(std::round(sel.depth / GRID_SNAP));
            std::snprintf(widthInput, sizeof(widthInput), "%d", wUnits);
            std::snprintf(depthInput, sizeof(depthInput), "%d", dUnits);
            std::snprintf(heightInput, sizeof(heightInput), "-");
        } else {
            std::snprintf(widthInput, sizeof(widthInput), "-");
            std::snprintf(depthInput, sizeof(depthInput), "-");
            std::snprintf(heightInput, sizeof(heightInput), "-");
        }
    }

    void applyFieldToSelected(ActiveField field) {
        bool editingObstacle = selectedObstacleIndex >= 0 && selectedObstacleIndex < static_cast<int>(currentLevel.obstacles.size());
        bool editingArea = selectedAreaIndex >= 0 && selectedAreaIndex < static_cast<int>(currentLevel.areas.size());
        if (!editingObstacle && !editingArea) return;

        auto parseUnits = [](const char* buf, int minVal, int maxVal) {
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(buf, &end, 10);
            int val = minVal;
            if (buf != end && errno != ERANGE) {
                val = static_cast<int>(parsed);
            }
            if (val < minVal) val = minVal;
            if (val > maxVal) val = maxVal;
            return val;
        };

        int maxSizeUnits = getMaxGridUnits();
        if (editingObstacle) {
            auto& sel = currentLevel.obstacles[selectedObstacleIndex];
            int newWidthUnits = static_cast<int>(std::round(sel.width / GRID_SNAP));
            int newDepthUnits = static_cast<int>(std::round(sel.depth / GRID_SNAP));
            int newHeightUnits = static_cast<int>(std::round(sel.height / GRID_SNAP));

            if (field == ActiveField::WIDTH) newWidthUnits = parseUnits(widthInput, MIN_SIZE_UNITS, maxSizeUnits);
            if (field == ActiveField::DEPTH) newDepthUnits = parseUnits(depthInput, MIN_SIZE_UNITS, maxSizeUnits);
            if (field == ActiveField::HEIGHT) newHeightUnits = parseUnits(heightInput, MIN_HEIGHT_UNITS, maxSizeUnits);

            float newWidth = newWidthUnits * GRID_SNAP;
            float newDepth = newDepthUnits * GRID_SNAP;
            float newHeight = newHeightUnits * GRID_SNAP;

            // Keep bottom-right corner anchored
            float rightEdge = sel.x + sel.width / 2.0f;
            float backEdge = sel.z + sel.depth / 2.0f;
            float cornerX = rightEdge - newWidth;
            float cornerZ = backEdge - newDepth;
            clampCornerToBounds(cornerX, cornerZ, newWidth, newDepth);
            sel.width = newWidth;
            sel.depth = newDepth;
            sel.height = newHeight;
            sel.x = cornerX + sel.width / 2.0f;
            sel.z = cornerZ + sel.depth / 2.0f;
        } else if (editingArea) {
            auto& sel = currentLevel.areas[selectedAreaIndex];
            int newWidthUnits = static_cast<int>(std::round(sel.width / GRID_SNAP));
            int newDepthUnits = static_cast<int>(std::round(sel.depth / GRID_SNAP));

            if (field == ActiveField::WIDTH) newWidthUnits = parseUnits(widthInput, MIN_SIZE_UNITS, maxSizeUnits);
            if (field == ActiveField::DEPTH) newDepthUnits = parseUnits(depthInput, MIN_SIZE_UNITS, maxSizeUnits);

            float newWidth = newWidthUnits * GRID_SNAP;
            float newDepth = newDepthUnits * GRID_SNAP;

            // Keep bottom-right corner anchored
            float rightEdge = sel.x + sel.width / 2.0f;
            float backEdge = sel.z + sel.depth / 2.0f;
            float cornerX = rightEdge - newWidth;
            float cornerZ = backEdge - newDepth;
            clampCornerToBounds(cornerX, cornerZ, newWidth, newDepth);
            sel.width = newWidth;
            sel.depth = newDepth;
            sel.x = cornerX + sel.width / 2.0f;
            sel.z = cornerZ + sel.depth / 2.0f;
        }

        syncInputsToSelected();
        editingField = false;
    }

    bool handleTextInput() {
        if (activeField == ActiveField::NONE) return false;

        char* buffer = nullptr;
        size_t bufferSize = 0;
        switch (activeField) {
            case ActiveField::WIDTH: buffer = widthInput; bufferSize = sizeof(widthInput); break;
            case ActiveField::DEPTH: buffer = depthInput; bufferSize = sizeof(depthInput); break;
            case ActiveField::HEIGHT: buffer = heightInput; bufferSize = sizeof(heightInput); break;
            default: break;
        }
        if (!buffer) return false;

        // Character input (digits only)
        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= '0' && ch <= '9') {
                size_t len = std::strlen(buffer);
                if (len + 1 < bufferSize) {
                    buffer[len] = static_cast<char>(ch);
                    buffer[len + 1] = '\0';
                }
            }
            ch = GetCharPressed();
        }

        // Control keys
        if (IsKeyPressed(KEY_BACKSPACE)) {
            size_t len = std::strlen(buffer);
            if (len > 0) buffer[len - 1] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            applyFieldToSelected(activeField);
            activeField = ActiveField::NONE;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            activeField = ActiveField::NONE;
            editingField = false;
            return true;
        }
        return false;
    }

    bool focusInputIfClicked(const Vector2& mp) {
        bool hasSelection = (selectedObstacleIndex >= 0 && selectedObstacleIndex < static_cast<int>(currentLevel.obstacles.size())) ||
                            (selectedAreaIndex >= 0 && selectedAreaIndex < static_cast<int>(currentLevel.areas.size()));
        if (!hasSelection) {
            return false;
        }

        const Rectangle infoCard = editor::ToolbarModel::infoCardRect();
        int infoX = static_cast<int>(infoCard.x) + 18;
        int infoY = static_cast<int>(infoCard.y) + 48;
        int boxW = 80;
        int boxH = 28;
        Rectangle widthBox{(float)infoX, (float)(infoY + 85), (float)boxW, (float)boxH};
        Rectangle depthBox{(float)(infoX + boxW + 10), (float)(infoY + 85), (float)boxW, (float)boxH};
        Rectangle heightBox{(float)(infoX + (boxW + 10) * 2), (float)(infoY + 85), (float)boxW, (float)boxH};

        if (CheckCollisionPointRec(mp, widthBox)) {
            activeField = ActiveField::WIDTH;
            editingField = true;
            return true;
        }
        if (CheckCollisionPointRec(mp, depthBox)) {
            activeField = ActiveField::DEPTH;
            editingField = true;
            return true;
        }
        if (selectedAreaIndex < 0 && CheckCollisionPointRec(mp, heightBox)) {
            activeField = ActiveField::HEIGHT;
            editingField = true;
            return true;
        }
        return false;
    }

    static TypographyStyleId typographyStyleForSize(int fontSize) {
        if (fontSize >= 40) {
            return TypographyStyleId::EditorTitle;
        }
        if (fontSize >= 32) {
            return TypographyStyleId::ScoreboardTitle;
        }
        if (fontSize >= 24) {
            return TypographyStyleId::AppSubtitle;
        }
        if (fontSize >= 22) {
            return TypographyStyleId::FieldLabel;
        }
        if (fontSize >= 20) {
            return TypographyStyleId::Body;
        }
        if (fontSize >= 18) {
            return TypographyStyleId::EditorBody;
        }
        return TypographyStyleId::Caption;
    }

    static void drawUiText(std::string_view text,
                           float x,
                           float y,
                           int fontSize,
                           Color color) {
        TypographyService::shared().draw(typographyStyleForSize(fontSize), text, Vector2{x, y}, color);
    }

    static int measureUiTextWidth(std::string_view text, int fontSize) {
        return TypographyService::shared().measureWidth(typographyStyleForSize(fontSize), text);
    }

    static void drawUiTextCentered(std::string_view text,
                                   float centerX,
                                   float y,
                                   int fontSize,
                                   Color color) {
        TypographyService::shared().drawCentered(typographyStyleForSize(fontSize), text, centerX, y, color);
    }

    static Rectangle centeredIconRect(const Rectangle& bounds, float size) {
        return Rectangle{
            bounds.x + (bounds.width - size) * 0.5f,
            bounds.y + (bounds.height - size) * 0.5f,
            size,
            size
        };
    }

    static void drawObstacleIcon(const Rectangle& icon, Color accent, Color ink) {
        const Rectangle front{icon.x + icon.width * 0.26f,
                              icon.y + icon.height * 0.36f,
                              icon.width * 0.48f,
                              icon.height * 0.42f};
        const Vector2 topA{front.x, front.y};
        const Vector2 topB{front.x + front.width, front.y};
        const Vector2 topC{front.x + front.width + icon.width * 0.16f, front.y - icon.height * 0.16f};
        const Vector2 topD{front.x + icon.width * 0.16f, front.y - icon.height * 0.16f};
        const Vector2 rightA{front.x + front.width, front.y};
        const Vector2 rightB{front.x + front.width, front.y + front.height};
        const Vector2 rightC{topC.x, front.y + front.height - icon.height * 0.16f};

        DrawTriangle(topA, topD, topC, Fade(accent, 0.95f));
        DrawTriangle(topA, topC, topB, Fade(accent, 0.95f));
        DrawTriangle(rightA, topC, rightC, Fade(accent, 0.7f));
        DrawTriangle(rightA, rightC, rightB, Fade(accent, 0.7f));
        DrawRectangleRounded(front, 0.12f, 6, Fade(accent, 0.82f));
        DrawRectangleRoundedLines(front, 0.12f, 6, ink);
        DrawLineEx(topA, topD, 2.0f, ink);
        DrawLineEx(topD, topC, 2.0f, ink);
        DrawLineEx(topC, rightC, 2.0f, ink);
    }

    static void drawAreaIcon(const Rectangle& icon, Color accent, Color ink) {
        const Rectangle board{icon.x + icon.width * 0.17f,
                              icon.y + icon.height * 0.17f,
                              icon.width * 0.66f,
                              icon.height * 0.66f};
        const float cellGap = icon.width * 0.035f;
        const float cellSize = (board.width - cellGap * 2.0f) / 3.0f;

        DrawRectangleRounded(board, 0.12f, 8, Fade(accent, 0.14f));
        DrawRectangleRoundedLines(board, 0.12f, 8, ink);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const Rectangle cell{
                    board.x + static_cast<float>(column) * (cellSize + cellGap),
                    board.y + static_cast<float>(row) * (cellSize + cellGap),
                    cellSize,
                    cellSize
                };
                const bool highlighted = row == 1 && column == 1;
                DrawRectangleRounded(cell,
                                     0.14f,
                                     4,
                                     highlighted ? Fade(accent, 0.88f) : Fade(accent, 0.42f));
                DrawRectangleRoundedLines(cell,
                                          0.14f,
                                          4,
                                          highlighted ? ink : Fade(ink, 0.65f));
            }
        }
    }

    static void drawCharacterIcon(const Rectangle& icon, Color accent, Color ink) {
        const Vector2 head{icon.x + icon.width * 0.5f, icon.y + icon.height * 0.28f};
        DrawCircleV(head, icon.width * 0.13f, accent);
        DrawCircleLines(static_cast<int>(head.x), static_cast<int>(head.y), icon.width * 0.13f, ink);
        DrawLineEx(Vector2{head.x, icon.y + icon.height * 0.42f},
                   Vector2{head.x, icon.y + icon.height * 0.66f},
                   4.0f,
                   ink);
        DrawLineEx(Vector2{head.x - icon.width * 0.18f, icon.y + icon.height * 0.5f},
                   Vector2{head.x + icon.width * 0.18f, icon.y + icon.height * 0.5f},
                   3.5f,
                   ink);
        DrawLineEx(Vector2{head.x, icon.y + icon.height * 0.66f},
                   Vector2{head.x - icon.width * 0.18f, icon.y + icon.height * 0.82f},
                   3.5f,
                   ink);
        DrawLineEx(Vector2{head.x, icon.y + icon.height * 0.66f},
                   Vector2{head.x + icon.width * 0.18f, icon.y + icon.height * 0.82f},
                   3.5f,
                   ink);
    }

    static void drawMoveIcon(const Rectangle& icon, Color accent, Color ink) {
        const Vector2 center{icon.x + icon.width * 0.5f, icon.y + icon.height * 0.5f};
        const float stem = icon.width * 0.22f;
        const float head = icon.width * 0.14f;
        const float shaft = 3.4f;

        DrawLineEx(Vector2{center.x - stem, center.y},
                   Vector2{center.x + stem, center.y},
                   shaft,
                   ink);
        DrawLineEx(Vector2{center.x, center.y - stem},
                   Vector2{center.x, center.y + stem},
                   shaft,
                   ink);

        const Vector2 leftTip{center.x - stem - head, center.y};
        const Vector2 rightTip{center.x + stem + head, center.y};
        const Vector2 upTip{center.x, center.y - stem - head};
        const Vector2 downTip{center.x, center.y + stem + head};

        DrawTriangle(leftTip,
                     Vector2{center.x - stem + 1.0f, center.y - head * 0.7f},
                     Vector2{center.x - stem + 1.0f, center.y + head * 0.7f},
                     Fade(accent, 0.92f));
        DrawTriangle(rightTip,
                     Vector2{center.x + stem - 1.0f, center.y - head * 0.7f},
                     Vector2{center.x + stem - 1.0f, center.y + head * 0.7f},
                     Fade(accent, 0.92f));
        DrawTriangle(upTip,
                     Vector2{center.x - head * 0.7f, center.y - stem + 1.0f},
                     Vector2{center.x + head * 0.7f, center.y - stem + 1.0f},
                     Fade(accent, 0.92f));
        DrawTriangle(downTip,
                     Vector2{center.x - head * 0.7f, center.y + stem - 1.0f},
                     Vector2{center.x + head * 0.7f, center.y + stem - 1.0f},
                     Fade(accent, 0.92f));

        DrawLineEx(leftTip, Vector2{center.x - stem + 1.0f, center.y - head * 0.7f}, 1.6f, ink);
        DrawLineEx(leftTip, Vector2{center.x - stem + 1.0f, center.y + head * 0.7f}, 1.6f, ink);
        DrawLineEx(rightTip, Vector2{center.x + stem - 1.0f, center.y - head * 0.7f}, 1.6f, ink);
        DrawLineEx(rightTip, Vector2{center.x + stem - 1.0f, center.y + head * 0.7f}, 1.6f, ink);
        DrawLineEx(upTip, Vector2{center.x - head * 0.7f, center.y - stem + 1.0f}, 1.6f, ink);
        DrawLineEx(upTip, Vector2{center.x + head * 0.7f, center.y - stem + 1.0f}, 1.6f, ink);
        DrawLineEx(downTip, Vector2{center.x - head * 0.7f, center.y + stem - 1.0f}, 1.6f, ink);
        DrawLineEx(downTip, Vector2{center.x + head * 0.7f, center.y + stem - 1.0f}, 1.6f, ink);

        DrawCircleV(center, icon.width * 0.11f, Fade(accent, 0.82f));
        DrawCircleLines(static_cast<int>(center.x),
                        static_cast<int>(center.y),
                        icon.width * 0.11f,
                        ink);
    }

    static void drawSaveIcon(const Rectangle& icon, Color accent, Color ink) {
        const Rectangle disk{icon.x + icon.width * 0.18f,
                             icon.y + icon.height * 0.14f,
                             icon.width * 0.64f,
                             icon.height * 0.72f};
        DrawRectangleRounded(disk, 0.08f, 6, Fade(accent, 0.78f));
        DrawRectangleRoundedLines(disk, 0.08f, 6, ink);
        DrawRectangleRounded(Rectangle{disk.x + disk.width * 0.18f,
                                       disk.y + disk.height * 0.12f,
                                       disk.width * 0.48f,
                                       disk.height * 0.24f},
                             0.06f,
                             4,
                             Color{16, 24, 38, 255});
        DrawRectangleRounded(Rectangle{disk.x + disk.width * 0.24f,
                                       disk.y + disk.height * 0.55f,
                                       disk.width * 0.52f,
                                       disk.height * 0.24f},
                             0.08f,
                             5,
                             Color{230, 238, 246, 255});
        DrawLineEx(Vector2{disk.x + disk.width * 0.3f, disk.y + disk.height * 0.62f},
                   Vector2{disk.x + disk.width * 0.7f, disk.y + disk.height * 0.62f},
                   1.6f,
                   Color{66, 82, 104, 255});
        DrawRectangle(static_cast<int>(disk.x + disk.width * 0.68f),
                      static_cast<int>(disk.y + disk.height * 0.15f),
                      static_cast<int>(disk.width * 0.12f),
                      static_cast<int>(disk.height * 0.18f),
                      ink);
    }

    static void drawToolIcon(editor::ToolKind tool, const Rectangle& bounds, Color accent, bool selected) {
        const Rectangle icon = centeredIconRect(bounds, 54.0f);
        const Color ink = selected ? WHITE : Color{210, 220, 236, 255};
        switch (tool) {
            case editor::ToolKind::Obstacle:
                drawObstacleIcon(icon, accent, ink);
                return;
            case editor::ToolKind::Area:
                drawAreaIcon(icon, accent, ink);
                return;
            case editor::ToolKind::Character:
                drawCharacterIcon(icon, accent, ink);
                return;
            case editor::ToolKind::Move:
                drawMoveIcon(icon, accent, ink);
                return;
        }
    }

    static void drawCard(const Rectangle& rect, Color fill, Color border) {
        DrawRectangleRounded(rect, 0.16f, 10, fill);
        DrawRectangleRoundedLines(rect, 0.16f, 10, border);
    }

    static const char* toolLabel(editor::ToolKind tool) {
        switch (tool) {
            case editor::ToolKind::Obstacle:
                return "Object";
            case editor::ToolKind::Area:
                return "Area";
            case editor::ToolKind::Character:
                return "Character";
            case editor::ToolKind::Move:
                return "Move";
        }
        return "Object";
    }

    static Color toolAccent(editor::ToolKind tool) {
        switch (tool) {
            case editor::ToolKind::Obstacle:
                return Color{90, 150, 255, 255};
            case editor::ToolKind::Area:
                return Color{100, 220, 170, 255};
            case editor::ToolKind::Character:
                return Color{255, 145, 115, 255};
            case editor::ToolKind::Move:
                return Color{245, 205, 90, 255};
        }
        return SKYBLUE;
    }

    bool wouldCollide(const LevelData::Obstacle& obs, int skipIndex = -1) const {
        for (size_t i = 0; i < currentLevel.obstacles.size(); ++i) {
            if (static_cast<int>(i) == skipIndex) continue;
            if (checkObstacleCollision(obs, currentLevel.obstacles[i])) {
                return true;
            }
        }
        return false;
    }

    void syncLevelNameBuffer() {
        const std::size_t copied =
            currentLevel.name.copy(levelNameBuffer, sizeof(levelNameBuffer) - 1u);
        levelNameBuffer[copied] = '\0';
    }

    void syncPaletteFromSelection() {
        const editor::SelectionRef selection = currentSelection();
        switch (selection.kind) {
            case editor::SelectionKind::Obstacle:
                paletteModel_.syncColor(editor::ToolKind::Obstacle,
                                        currentLevel.obstacles[selection.index].color);
                break;
            case editor::SelectionKind::Area:
                paletteModel_.syncColor(editor::ToolKind::Area,
                                        currentLevel.areas[selection.index].color);
                break;
            case editor::SelectionKind::Character:
                paletteModel_.syncColor(editor::ToolKind::Character,
                                        currentLevel.enemies[selection.index].color);
                break;
            case editor::SelectionKind::None:
                break;
        }
    }

    void resetSelectionState() {
        selectedObstacleIndex = -1;
        selectedAreaIndex = -1;
        selectedEnemyIndex = -1;
        activeField = ActiveField::NONE;
        editingField = false;
        draggingPlacement = false;
        draggingSelection = false;
        draggedSelection_ = editor::SelectionRef{};
        dragPreviewObstacles.clear();
        dragPreviewAreas.clear();
        syncInputsToSelected();
    }

    editor::SelectionRef currentSelection() const {
        return editor::selectionFromIndices(
            selectedObstacleIndex,
            selectedAreaIndex,
            selectedEnemyIndex);
    }

    void clearSelection() {
        selectedObstacleIndex = -1;
        selectedAreaIndex = -1;
        selectedEnemyIndex = -1;
        syncInputsToSelected();
    }

    void setStatusMessage(const std::string& message,
                          float durationSeconds = 2.5f) {
        statusMessage_ = message;
        statusMessageTimerSeconds_ = durationSeconds;
    }

    void tickStatusMessage(float dtSeconds) {
        if (statusMessageTimerSeconds_ <= 0.0f) {
            return;
        }
        statusMessageTimerSeconds_ =
            std::max(0.0f, statusMessageTimerSeconds_ - dtSeconds);
        if (statusMessageTimerSeconds_ <= 0.0f) {
            statusMessage_.clear();
        }
    }

    editor::ToolKind selectionToolKind() const {
        const editor::SelectionRef selection = currentSelection();
        switch (selection.kind) {
            case editor::SelectionKind::Area:
                return editor::ToolKind::Area;
            case editor::SelectionKind::Character:
                return editor::ToolKind::Character;
            case editor::SelectionKind::Obstacle:
            case editor::SelectionKind::None:
                return editor::ToolKind::Obstacle;
        }
        return editor::ToolKind::Obstacle;
    }

    editor::ToolKind effectivePaletteTool() const {
        return activeTool == editor::ToolKind::Move ? selectionToolKind() : activeTool;
    }

    void applyPaletteToPreviewAndSelection() {
        previewObstacle.color = paletteModel_.activeColor(editor::ToolKind::Obstacle);
        previewArea.color = paletteModel_.activeColor(editor::ToolKind::Area);
        previewEnemy.color = paletteModel_.activeColor(editor::ToolKind::Character);

        const editor::SelectionRef selection = currentSelection();
        if (!selection.valid()) {
            return;
        }

        switch (selection.kind) {
            case editor::SelectionKind::Obstacle:
                currentLevel.obstacles[selection.index].color =
                    paletteModel_.activeColor(editor::ToolKind::Obstacle);
                break;
            case editor::SelectionKind::Area:
                currentLevel.areas[selection.index].color =
                    paletteModel_.activeColor(editor::ToolKind::Area);
                break;
            case editor::SelectionKind::Character:
                currentLevel.enemies[selection.index].color =
                    paletteModel_.activeColor(editor::ToolKind::Character);
                break;
            case editor::SelectionKind::None:
                break;
        }
    }

    void setActiveTool(editor::ToolKind tool) {
        activeTool = tool;
        activeField = ActiveField::NONE;
        editingField = false;
        draggingPlacement = false;
        draggingSelection = false;
        draggedSelection_ = editor::SelectionRef{};
        dragPreviewObstacles.clear();
        dragPreviewAreas.clear();
        if (tool != editor::ToolKind::Move) {
            clearSelection();
        }
        applyPaletteToPreviewAndSelection();
    }

    static Rectangle slotSelectorCardRect(std::size_t index) {
        constexpr float kSlotWidth = 350.0f;
        constexpr float kSlotHeight = 200.0f;
        constexpr float kSpacingX = 40.0f;
        constexpr float kSpacingY = 36.0f;
        const float totalWidth =
            app_shell::level_slots::kGridCols * kSlotWidth +
            (app_shell::level_slots::kGridCols - 1) * kSpacingX;
        const float startX = (1920.0f - totalWidth) * 0.5f;
        const float startY = 236.0f;
        return app_shell::level_slots::gridCardRectForIndex(
            static_cast<int>(index),
            startX,
            startY,
            kSlotWidth,
            kSlotHeight,
            kSpacingX,
            kSpacingY);
    }

    static Rectangle levelNameInputRect() {
        return Rectangle{620.0f, 108.0f, 680.0f, 50.0f};
    }

    bool handleLevelNameTextInput() {
        if (!editingLevelName_) {
            return false;
        }

        bool handled = false;
        int ch = GetCharPressed();
        while (ch > 0) {
            handled =
                editor::LevelNameModel::appendCharacter(
                    levelNameBuffer,
                    sizeof(levelNameBuffer),
                    ch) ||
                handled;
            ch = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            handled =
                editor::LevelNameModel::eraseLast(levelNameBuffer, sizeof(levelNameBuffer)) ||
                handled;
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            editingLevelName_ = false;
            handled = true;
        }

        return handled;
    }

    void refreshSlotPreviewCards() {
        for (std::size_t index = 0; index < slotPreviewCards_.size(); ++index) {
            slotPreviewCards_[index] =
                app_shell::level_slots::loadPreviewData(static_cast<int>(index) + 1);
        }
        slotPreviewCardsDirty_ = false;
    }

    void openSlotSelector(SlotSelectorAction action) {
        slotSelectorAction = action;
        refreshSlotPreviewCards();
        if (slotSelectorAction == SlotSelectorAction::SAVE) {
            syncLevelNameBuffer();
            editingLevelName_ = true;
        } else {
            editingLevelName_ = false;
        }
        showSlotSelector = true;
    }

    GameMode commitSlotSelectorChoice(int slot) {
        if (slotSelectorAction == SlotSelectorAction::LOAD) {
            loadLevelFromSlot(slot);
        } else {
            saveLevelToSlot(slot);
        }
        slotPreviewCardsDirty_ = true;
        showSlotSelector = false;
        editingLevelName_ = false;
        return GameMode::LEVEL_EDITOR;
    }

    bool saveLevelToSlot(int slot) {
        currentLevel.name = editor::LevelNameModel::sanitizedForSave(levelNameBuffer);
        syncLevelNameBuffer();
        const bool saved =
            LevelData::saveLevelDefinition(currentLevel, LevelData::getLevelPath(slot));
        if (saved) {
            currentSlot = slot;
            slotPreviewCardsDirty_ = true;
            setStatusMessage("Saved level to slot " + std::to_string(slot));
        } else {
            setStatusMessage("Failed to save slot " + std::to_string(slot), 3.5f);
        }
        return saved;
    }

    bool loadLevelFromSlot(int slot) {
        if (!LevelData::levelExists(slot)) {
            return false;
        }

        LevelData::LevelDefinition loadedLevel;
        if (!LevelData::loadLevelDefinition(loadedLevel, LevelData::getLevelPath(slot))) {
            return false;
        }

        currentLevel = loadedLevel;
        currentSlot = slot;
        slotPreviewCardsDirty_ = true;
        resetSelectionState();
        syncLevelNameBuffer();
        paletteModel_.syncFloorColor(currentLevel.floorColor);
        applyPaletteToPreviewAndSelection();
        setStatusMessage("Loaded slot " + std::to_string(slot));
        return true;
    }

    Vector3 mouseGroundIntersection() const {
        const Ray ray = GetScreenToWorldRayEx(display::mousePosition(),
                                             camera,
                                             Config::SCREEN_WIDTH,
                                             Config::SCREEN_HEIGHT);
        if (std::fabs(ray.direction.y) <= 0.0001f) {
            return Vector3{};
        }

        const float t = -ray.position.y / ray.direction.y;
        if (t <= 0.0f) {
            return Vector3{};
        }

        Vector3 hitPoint = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
        hitPoint.y = 0.0f;
        return hitPoint;
    }

    editor::GridCell mouseGridCell() const {
        return editor::ViewportMapper::cellFromWorld(
            mouseGroundIntersection(),
            Config::ARENA_SIZE);
    }

    static Vector2 edgePanInput(Vector2 mouseScreen) {
        if (!IsCursorOnScreen()) {
            return Vector2{};
        }

        Vector2 input{};
        if (mouseScreen.x <= kEdgePanMarginPx) {
            input.x -= 1.0f;
        }
        if (mouseScreen.x >= static_cast<float>(Config::SCREEN_WIDTH) - kEdgePanMarginPx) {
            input.x += 1.0f;
        }
        if (mouseScreen.y <= kEdgePanMarginPx) {
            input.y -= 1.0f;
        }
        if (mouseScreen.y >= static_cast<float>(Config::SCREEN_HEIGHT) - kEdgePanMarginPx) {
            input.y += 1.0f;
        }
        return input;
    }

    static bool editorModifierDown(int leftKey, int rightKey) {
        return IsKeyDown(leftKey) || IsKeyDown(rightKey);
    }

    int getObstacleAt(Vector3 worldPos) const {
        for (std::size_t i = currentLevel.obstacles.size(); i > 0u; --i) {
            const std::size_t index = i - 1u;
            const auto& obs = currentLevel.obstacles[index];
            float halfW = obs.width / 2.0f;
            float halfD = obs.depth / 2.0f;

            if (worldPos.x >= obs.x - halfW && worldPos.x <= obs.x + halfW &&
                worldPos.z >= obs.z - halfD && worldPos.z <= obs.z + halfD) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    int getAreaAt(Vector3 worldPos) const {
        for (std::size_t i = currentLevel.areas.size(); i > 0u; --i) {
            const std::size_t index = i - 1u;
            const auto& area = currentLevel.areas[index];
            float halfW = area.width / 2.0f;
            float halfD = area.depth / 2.0f;

            if (worldPos.x >= area.x - halfW && worldPos.x <= area.x + halfW &&
                worldPos.z >= area.z - halfD && worldPos.z <= area.z + halfD) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    int getEnemyAt(Vector3 worldPos) const {
        float radius = 0.6f;
        for (std::size_t i = currentLevel.enemies.size(); i > 0u; --i) {
            const std::size_t index = i - 1u;
            const auto& e = currentLevel.enemies[index];
            if (fabs(worldPos.x - e.x) <= radius && fabs(worldPos.z - e.z) <= radius) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void clampCornerToBounds(float& cornerX, float& cornerZ, float width, float depth) const {
        float maxBound = Config::ARENA_SIZE;
        cornerX = std::clamp(cornerX, -maxBound, maxBound - width);
        cornerZ = std::clamp(cornerZ, -maxBound, maxBound - depth);
    }

    editor::SelectionRef selectionAt(const Vector3& worldPos) const {
        if (const int obstacleIndex = getObstacleAt(worldPos); obstacleIndex >= 0) {
            return editor::SelectionRef{editor::SelectionKind::Obstacle, obstacleIndex};
        }
        if (const int areaIndex = getAreaAt(worldPos); areaIndex >= 0) {
            return editor::SelectionRef{editor::SelectionKind::Area, areaIndex};
        }
        if (const int enemyIndex = getEnemyAt(worldPos); enemyIndex >= 0) {
            return editor::SelectionRef{editor::SelectionKind::Character, enemyIndex};
        }
        return editor::SelectionRef{};
    }

    void applySelection(const editor::SelectionRef& selection) {
        clearSelection();
        switch (selection.kind) {
            case editor::SelectionKind::Obstacle:
                selectedObstacleIndex = selection.index;
                break;
            case editor::SelectionKind::Area:
                selectedAreaIndex = selection.index;
                break;
            case editor::SelectionKind::Character:
                selectedEnemyIndex = selection.index;
                break;
            case editor::SelectionKind::None:
                break;
        }
        syncInputsToSelected();
        syncPaletteFromSelection();
    }

    void generateDragRectangle(const editor::GridCell& startCell,
                               const editor::GridCell& endCell) {
        dragPreviewObstacles.clear();
        dragPreviewAreas.clear();

        const editor::GridRect rect = editor::ViewportMapper::dragRect(
            startCell,
            endCell,
            brushSizeUnits,
            brushSizeUnits,
            Config::ARENA_SIZE);

        if (activeTool == editor::ToolKind::Area) {
            dragPreviewAreas.push_back(editor::ViewportMapper::areaFromRect(
                rect,
                paletteModel_.activeColor(editor::ToolKind::Area)));
            return;
        }

        if (activeTool == editor::ToolKind::Obstacle) {
            const LevelData::Obstacle obstacle = editor::ViewportMapper::obstacleFromRect(
                rect,
                brushHeightUnits,
                paletteModel_.activeColor(editor::ToolKind::Obstacle));
            if (!wouldCollide(obstacle)) {
                dragPreviewObstacles.push_back(obstacle);
            }
        }
    }

    void updatePlacementPreview(const editor::GridCell& cell) {
        dragPreviewObstacles.clear();
        dragPreviewAreas.clear();

        if (activeTool == editor::ToolKind::Move) {
            showPreview = false;
            return;
        }

        if (activeTool == editor::ToolKind::Character) {
            previewEnemy = editor::ViewportMapper::enemyFromCell(
                cell,
                paletteModel_.activeColor(editor::ToolKind::Character));
            showPreview = true;
            return;
        }

        const editor::GridRect rect = editor::ViewportMapper::centeredBrushRect(
            cell,
            brushSizeUnits,
            brushSizeUnits,
            Config::ARENA_SIZE);

        if (activeTool == editor::ToolKind::Area) {
            previewArea = editor::ViewportMapper::areaFromRect(
                rect,
                paletteModel_.activeColor(editor::ToolKind::Area));
            showPreview = true;
            return;
        }

        previewObstacle = editor::ViewportMapper::obstacleFromRect(
            rect,
            brushHeightUnits,
            paletteModel_.activeColor(editor::ToolKind::Obstacle));
        showPreview = !wouldCollide(previewObstacle);
    }

    void commitPlacementPreview() {
        if (activeTool == editor::ToolKind::Area && !dragPreviewAreas.empty()) {
            currentLevel.areas.push_back(dragPreviewAreas.back());
            selectedAreaIndex = static_cast<int>(currentLevel.areas.size()) - 1;
            selectedObstacleIndex = -1;
            selectedEnemyIndex = -1;
            syncInputsToSelected();
            return;
        }
        if (activeTool == editor::ToolKind::Obstacle && !dragPreviewObstacles.empty()) {
            currentLevel.obstacles.push_back(dragPreviewObstacles.back());
            selectedObstacleIndex = static_cast<int>(currentLevel.obstacles.size()) - 1;
            selectedAreaIndex = -1;
            selectedEnemyIndex = -1;
            syncInputsToSelected();
        }
    }

    bool moveSelectionToCell(const editor::SelectionRef& selection,
                             const editor::GridCell& cell) {
        if (!selection.valid()) {
            return false;
        }

        switch (selection.kind) {
            case editor::SelectionKind::Obstacle: {
                const LevelData::Obstacle moved =
                    editor::ViewportMapper::moveObstacleToCell(
                        currentLevel.obstacles[selection.index],
                        cell,
                        Config::ARENA_SIZE);
                if (wouldCollide(moved, selection.index)) {
                    return false;
                }
                currentLevel.obstacles[selection.index] = moved;
                syncInputsToSelected();
                return true;
            }
            case editor::SelectionKind::Area:
                currentLevel.areas[selection.index] =
                    editor::ViewportMapper::moveAreaToCell(
                        currentLevel.areas[selection.index],
                        cell,
                        Config::ARENA_SIZE);
                syncInputsToSelected();
                return true;
            case editor::SelectionKind::Character:
                currentLevel.enemies[selection.index] =
                    editor::ViewportMapper::moveEnemyToCell(
                        currentLevel.enemies[selection.index],
                        cell);
                syncInputsToSelected();
                return true;
            case editor::SelectionKind::None:
                break;
        }
        return false;
    }

public:
    const LevelData::LevelDefinition& portableLevelDefinition() const {
        return currentLevel;
    }

    void applyPortableLevelDefinition(const LevelData::LevelDefinition& level,
                                      int slot = -1) {
        currentLevel = level;
        if (slot > 0) {
            currentSlot = slot;
        }
        resetSelectionState();
        syncLevelNameBuffer();
        paletteModel_.syncFloorColor(currentLevel.floorColor);
        applyPaletteToPreviewAndSelection();
    }

    bool savePortableLevelDefinition(int slot) {
        return saveLevelToSlot(slot);
    }

    bool loadPortableLevelDefinition(int slot) {
        return loadLevelFromSlot(slot);
    }

    LevelEditor(int slot = 1)
        : currentSlot(slot),
          selectedObstacleIndex(-1),
          selectedAreaIndex(-1),
          selectedEnemyIndex(-1),
          showPreview(false),
          draggingPlacement(false),
          draggingSelection(false),
          dragStartCell({0, 0}),
          brushSizeUnits(4),
          brushHeightUnits(2),
          activeTool(editor::ToolKind::Obstacle),
          showSlotSelector(false),
          slotSelectorAction(SlotSelectorAction::SAVE),
          activeField(ActiveField::NONE) {

        SetWindowTitle("Netcode Simulator - Level Editor");
        display::enableCursorPreservingPosition();
        TypographyService::shared().initialize();

        // Initialize camera (top-down view like spectator)
        camera.position = Vector3{0.0f, 40.0f, 30.0f};
        camera.target = Vector3{0.0f, 0.0f, 0.0f};
        camera.up = Vector3{0.0f, 1.0f, 0.0f};
        camera.fovy = 45.0f;
        camera.projection = CAMERA_PERSPECTIVE;

        // Load level if exists
        currentLevel.name = "Level " + std::to_string(slot);
        if (!loadLevelFromSlot(slot)) {
            syncLevelNameBuffer();
        }

        // Initialize preview entities and floor palette.
        applyPaletteToPreviewAndSelection();
        previewObstacle = LevelData::Obstacle{
            0.0f,
            0.0f,
            static_cast<float>(brushSizeUnits),
            static_cast<float>(brushSizeUnits),
            static_cast<float>(brushHeightUnits),
            paletteModel_.activeColor(editor::ToolKind::Obstacle)};
        previewArea = LevelData::Area{
            0.0f,
            0.0f,
            static_cast<float>(brushSizeUnits),
            static_cast<float>(brushSizeUnits),
            paletteModel_.activeColor(editor::ToolKind::Area)};
        previewEnemy = LevelData::EnemySpawn{
            0.0f,
            0.0f,
            paletteModel_.activeColor(editor::ToolKind::Character)};

        std::snprintf(widthInput, sizeof(widthInput), "%d", brushSizeUnits);
        std::snprintf(depthInput, sizeof(depthInput), "%d", brushSizeUnits);
        std::snprintf(heightInput, sizeof(heightInput), "%d", brushHeightUnits);
        editingField = false;
    }

    ~LevelEditor() {
    }

    GameMode update(float dt, bool allowBackShortcut = true) {
        tickStatusMessage(dt);

        // Handle modal selectors before editor input.
        if (showSlotSelector) {
            return updateSlotSelector(allowBackShortcut);
        }

        const Vector2 mouseScreen = display::mousePosition();
        const float wheelMove = GetMouseWheelMove();
        const bool shiftWheelModifier =
            editorModifierDown(KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT);
        const bool heightWheelModifier =
            editorModifierDown(KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL);
        const bool zoomWheel =
            activeField == ActiveField::NONE &&
            !shiftWheelModifier &&
            !heightWheelModifier &&
            wheelMove != 0.0f;
        if (zoomWheel) {
            camera.fovy = std::clamp(camera.fovy - wheelMove * kZoomStepFovy,
                                     kMinCameraFovy,
                                     kMaxCameraFovy);
        }

        // Camera controls: WASD and edge-panning near the virtual render bounds.
        float camSpeed = 15.0f * dt;
        Vector2 panInput = edgePanInput(mouseScreen);
        if (IsKeyDown(KEY_W)) panInput.y -= 1.0f;
        if (IsKeyDown(KEY_S)) panInput.y += 1.0f;
        if (IsKeyDown(KEY_A)) panInput.x -= 1.0f;
        if (IsKeyDown(KEY_D)) panInput.x += 1.0f;
        camera.position.x += panInput.x * camSpeed;
        camera.position.z += panInput.y * camSpeed;

        // Keep camera position in bounds
        float maxCam = Config::ARENA_SIZE + 10.0f;
        camera.position.x = std::clamp(camera.position.x, -maxCam, maxCam);
        camera.position.z = std::clamp(camera.position.z, -maxCam, maxCam);
        camera.target.x = camera.position.x;
        camera.target.z = camera.position.z - 10.0f;

        const Vector3 mouseWorld = mouseGroundIntersection();
        const editor::GridCell mouseCell = mouseGridCell();
        bool clickConsumed = false;

        // Cycle visible toolbar tools with the historical keyboard shortcut.
        if (IsKeyPressed(KEY_P)) {
            const auto nextTool = [&]() {
                switch (activeTool) {
                    case editor::ToolKind::Obstacle:
                        return editor::ToolKind::Area;
                    case editor::ToolKind::Area:
                        return editor::ToolKind::Character;
                    case editor::ToolKind::Character:
                        return editor::ToolKind::Move;
                    case editor::ToolKind::Move:
                        return editor::ToolKind::Obstacle;
                }
                return editor::ToolKind::Obstacle;
            }();
            setActiveTool(nextTool);
        }

        if (IsKeyPressed(KEY_C)) {
            paletteModel_.cycleColor(effectivePaletteTool());
            applyPaletteToPreviewAndSelection();
        }

        if (IsKeyPressed(KEY_F)) {
            paletteModel_.cycleFloorColor();
            currentLevel.floorColor = paletteModel_.floorColor();
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (editor::ToolbarModel::saveButtonHit(mouseScreen)) {
                openSlotSelector(SlotSelectorAction::SAVE);
                clickConsumed = true;
            } else if (const auto toolHit = editor::ToolbarModel::toolAtPoint(mouseScreen)) {
                setActiveTool(*toolHit);
                clickConsumed = true;
            } else if (const auto paletteHit =
                           editor::ToolbarModel::paletteIndexAtPoint(
                               mouseScreen,
                               paletteModel_.paletteSize(effectivePaletteTool()))) {
                paletteModel_.setActiveIndex(effectivePaletteTool(), *paletteHit);
                applyPaletteToPreviewAndSelection();
                clickConsumed = true;
            } else if (focusInputIfClicked(mouseScreen)) {
                clickConsumed = true;
                draggingPlacement = false;
                draggingSelection = false;
            } else if (activeField != ActiveField::NONE) {
                applyFieldToSelected(activeField);
                activeField = ActiveField::NONE;
                editingField = false;
            }
        }

        const bool clearedFocusedField = handleTextInput();

        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            applySelection(selectionAt(mouseWorld));
            activeField = ActiveField::NONE;
            editingField = false;
        }

        if (!clickConsumed &&
            activeField == ActiveField::NONE &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (activeTool == editor::ToolKind::Character) {
                currentLevel.enemies.push_back(editor::ViewportMapper::enemyFromCell(
                    mouseCell,
                    paletteModel_.activeColor(editor::ToolKind::Character)));
                selectedEnemyIndex = static_cast<int>(currentLevel.enemies.size()) - 1;
                selectedObstacleIndex = -1;
                selectedAreaIndex = -1;
                syncInputsToSelected();
                clickConsumed = true;
            } else if (activeTool == editor::ToolKind::Move) {
                const editor::SelectionRef selection = selectionAt(mouseWorld);
                if (selection.valid()) {
                    applySelection(selection);
                    draggingSelection = true;
                    draggedSelection_ = selection;
                    moveSelectionToCell(selection, mouseCell);
                } else {
                    clearSelection();
                }
                clickConsumed = true;
            } else {
                draggingPlacement = true;
                dragStartCell = mouseCell;
                generateDragRectangle(dragStartCell, mouseCell);
                clickConsumed = true;
            }
        }

        if (draggingSelection) {
            moveSelectionToCell(draggedSelection_, mouseCell);
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                draggingSelection = false;
                draggedSelection_ = editor::SelectionRef{};
            }
            showPreview = false;
        } else if (draggingPlacement) {
            generateDragRectangle(dragStartCell, mouseCell);
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                commitPlacementPreview();
                draggingPlacement = false;
                dragPreviewObstacles.clear();
                dragPreviewAreas.clear();
            }
        } else {
            updatePlacementPreview(mouseCell);
        }

        if (activeField == ActiveField::NONE) {
            const int maxUnits = getMaxGridUnits();
            if (IsKeyPressed(KEY_ONE)) {
                brushSizeUnits = std::clamp(brushSizeUnits - 1, MIN_SIZE_UNITS, maxUnits);
            }
            if (IsKeyPressed(KEY_TWO)) {
                brushSizeUnits = std::clamp(brushSizeUnits + 1, MIN_SIZE_UNITS, maxUnits);
            }
            if (IsKeyPressed(KEY_THREE)) {
                brushHeightUnits = std::clamp(brushHeightUnits - 1, MIN_HEIGHT_UNITS, maxUnits);
            }
            if (IsKeyPressed(KEY_FOUR)) {
                brushHeightUnits = std::clamp(brushHeightUnits + 1, MIN_HEIGHT_UNITS, maxUnits);
            }
        }

        if (activeField == ActiveField::NONE && !editingField && (IsKeyPressed(KEY_X) || IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))) {
            if (selectedObstacleIndex >= 0) {
                currentLevel.obstacles.erase(currentLevel.obstacles.begin() + selectedObstacleIndex);
                selectedObstacleIndex = -1;
                activeField = ActiveField::NONE;
            } else if (selectedAreaIndex >= 0) {
                currentLevel.areas.erase(currentLevel.areas.begin() + selectedAreaIndex);
                selectedAreaIndex = -1;
            } else if (selectedEnemyIndex >= 0) {
                currentLevel.enemies.erase(currentLevel.enemies.begin() + selectedEnemyIndex);
                selectedEnemyIndex = -1;
            }
            syncInputsToSelected();
        }

        const bool sizeWheel =
            activeField == ActiveField::NONE && wheelMove != 0.0f && shiftWheelModifier;
        const bool heightWheel =
            activeField == ActiveField::NONE && wheelMove != 0.0f && heightWheelModifier;
        if (selectedObstacleIndex >= 0 &&
            selectedObstacleIndex < static_cast<int>(currentLevel.obstacles.size()) &&
            !draggingSelection) {
            auto& selected = currentLevel.obstacles[selectedObstacleIndex];
            bool modified = false;

            if (sizeWheel) {
                int sizeUnits = static_cast<int>(std::round(selected.width / GRID_SNAP));
                sizeUnits = std::clamp(sizeUnits + static_cast<int>(wheelMove),
                                       MIN_SIZE_UNITS,
                                       getMaxGridUnits());
                const editor::GridCell centerCell = editor::ViewportMapper::cellFromWorld(
                    Vector3{selected.x, 0.0f, selected.z},
                    Config::ARENA_SIZE);
                const LevelData::Obstacle resized = editor::ViewportMapper::obstacleFromRect(
                    editor::ViewportMapper::centeredBrushRect(
                        centerCell,
                        sizeUnits,
                        sizeUnits,
                        Config::ARENA_SIZE),
                    static_cast<int>(std::round(selected.height / GRID_SNAP)),
                    selected.color);
                if (!wouldCollide(resized, selectedObstacleIndex)) {
                    selected = resized;
                    modified = true;
                }
            } else if (heightWheel) {
                int heightUnits = static_cast<int>(std::round(selected.height / GRID_SNAP));
                heightUnits = std::clamp(heightUnits + static_cast<int>(wheelMove),
                                         MIN_HEIGHT_UNITS,
                                         getMaxGridUnits());
                selected.height = heightUnits * GRID_SNAP;
                modified = true;
            }

            if (activeField == ActiveField::NONE) {
                editor::GridCell centerCell = editor::ViewportMapper::cellFromWorld(
                    Vector3{selected.x, 0.0f, selected.z},
                    Config::ARENA_SIZE);
                if (IsKeyPressed(KEY_UP)) { centerCell.z -= 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_DOWN)) { centerCell.z += 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_LEFT)) { centerCell.x -= 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_RIGHT)) { centerCell.x += 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
            }
            if (modified) syncInputsToSelected();
        } else if (selectedAreaIndex >= 0 && selectedAreaIndex < static_cast<int>(currentLevel.areas.size())) {
            auto& selected = currentLevel.areas[selectedAreaIndex];
            bool modified = false;

            if (sizeWheel) {
                int sizeUnits = static_cast<int>(std::round(selected.width / GRID_SNAP));
                sizeUnits = std::clamp(sizeUnits + static_cast<int>(wheelMove),
                                       MIN_SIZE_UNITS,
                                       getMaxGridUnits());
                const editor::GridCell centerCell = editor::ViewportMapper::cellFromWorld(
                    Vector3{selected.x, 0.0f, selected.z},
                    Config::ARENA_SIZE);
                selected = editor::ViewportMapper::areaFromRect(
                    editor::ViewportMapper::centeredBrushRect(
                        centerCell,
                        sizeUnits,
                        sizeUnits,
                        Config::ARENA_SIZE),
                    selected.color);
                modified = true;
            }

            if (activeField == ActiveField::NONE) {
                editor::GridCell centerCell = editor::ViewportMapper::cellFromWorld(
                    Vector3{selected.x, 0.0f, selected.z},
                    Config::ARENA_SIZE);
                if (IsKeyPressed(KEY_UP)) { centerCell.z -= 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_DOWN)) { centerCell.z += 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_LEFT)) { centerCell.x -= 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_RIGHT)) { centerCell.x += 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
            }
            if (modified) syncInputsToSelected();
        } else if (selectedEnemyIndex >= 0 && selectedEnemyIndex < static_cast<int>(currentLevel.enemies.size())) {
            auto& selected = currentLevel.enemies[selectedEnemyIndex];
            bool modified = false;

            if (activeField == ActiveField::NONE) {
                editor::GridCell centerCell = editor::ViewportMapper::cellFromWorld(
                    Vector3{selected.x, 0.0f, selected.z},
                    Config::ARENA_SIZE);
                if (IsKeyPressed(KEY_UP)) { centerCell.z -= 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_DOWN)) { centerCell.z += 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_LEFT)) { centerCell.x -= 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
                if (IsKeyPressed(KEY_RIGHT)) { centerCell.x += 1; modified = moveSelectionToCell(currentSelection(), centerCell) || modified; }
            }
            if (modified) syncInputsToSelected();
        } else {
            if (sizeWheel) {
                brushSizeUnits = std::clamp(brushSizeUnits + static_cast<int>(wheelMove),
                                            MIN_SIZE_UNITS,
                                            getMaxGridUnits());
            } else if (heightWheel && activeTool == editor::ToolKind::Obstacle) {
                brushHeightUnits = std::clamp(brushHeightUnits + static_cast<int>(wheelMove),
                                              MIN_HEIGHT_UNITS,
                                              getMaxGridUnits());
            }
        }

        if (IsKeyPressed(KEY_F5)) {
            openSlotSelector(SlotSelectorAction::SAVE);
        }

        if (IsKeyPressed(KEY_L)) {
            openSlotSelector(SlotSelectorAction::LOAD);
        }

        if (clearedFocusedField) {
            return GameMode::LEVEL_EDITOR;
        }
        if (allowBackShortcut &&
            activeField == ActiveField::NONE &&
            (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q))) {
            return GameMode::MAIN_MENU;
        }

        return GameMode::LEVEL_EDITOR;
    }

    GameMode updateSlotSelector(bool allowBackShortcut = true) {
        if (slotPreviewCardsDirty_) {
            refreshSlotPreviewCards();
        }

        const bool saving = slotSelectorAction == SlotSelectorAction::SAVE;
        const Vector2 mousePos = display::mousePosition();
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (saving && CheckCollisionPointRec(mousePos, levelNameInputRect())) {
                editingLevelName_ = true;
                return GameMode::LEVEL_EDITOR;
            }

            for (std::size_t index = 0; index < slotPreviewCards_.size(); ++index) {
                if (CheckCollisionPointRec(mousePos, slotSelectorCardRect(index))) {
                    return commitSlotSelectorChoice(slotPreviewCards_[index].slotNumber);
                }
            }

            if (saving) {
                editingLevelName_ = false;
            }
        }

        if (saving) {
            handleLevelNameTextInput();
        }

        if (!saving || !editingLevelName_) {
            for (int i = 0; i < 9; ++i) {
                if (IsKeyPressed(KEY_ONE + i)) {
                    return commitSlotSelectorChoice(i + 1);
                }
            }
        }

        if (allowBackShortcut &&
            (IsKeyPressed(KEY_ESCAPE) || (!editingLevelName_ && IsKeyPressed(KEY_Q)))) {
            showSlotSelector = false;
            editingLevelName_ = false;
        }

        return GameMode::LEVEL_EDITOR;
    }

    void render() {
        ClearBackground(Color{10, 10, 15, 255});

        // 3D rendering
        BeginMode3D(camera);

        // Draw grid
        for (int x = -25; x <= 25; x++) {
            DrawLine3D(Vector3{(float)x, 0.0f, -25.0f}, Vector3{(float)x, 0.0f, 25.0f}, DARKGRAY);
        }
        for (int z = -25; z <= 25; z++) {
            DrawLine3D(Vector3{-25.0f, 0.0f, (float)z}, Vector3{25.0f, 0.0f, (float)z}, DARKGRAY);
        }

        // Draw arena bounds
        float arenaSize = Config::ARENA_SIZE;
        DrawCubeWires(Vector3{arenaSize, 1.0f, 0.0f}, 0.5f, 2.0f, arenaSize * 2, RED);
        DrawCubeWires(Vector3{-arenaSize, 1.0f, 0.0f}, 0.5f, 2.0f, arenaSize * 2, RED);
        DrawCubeWires(Vector3{0.0f, 1.0f, arenaSize}, arenaSize * 2, 2.0f, 0.5f, RED);
        DrawCubeWires(Vector3{0.0f, 1.0f, -arenaSize}, arenaSize * 2, 2.0f, 0.5f, RED);

        // Draw center marker (player/enemy spawn)
        DrawCube(Vector3{0.0f, 0.5f, 0.0f}, 0.5f, 1.0f, 0.5f, YELLOW);
        DrawCubeWires(Vector3{0.0f, 0.5f, 0.0f}, 0.6f, 1.1f, 0.6f, GOLD);

        // Draw areas (tints under the grid)
        for (size_t i = 0; i < currentLevel.areas.size(); ++i) {
            const auto& area = currentLevel.areas[i];
            bool isSelected = static_cast<int>(i) == selectedAreaIndex;
            float yOffset = (area.color.g > area.color.r + 20 && area.color.g > area.color.b + 20) ? -0.035f : -0.027f;
            Vector3 pos = {area.x, yOffset, area.z};
            Color fill = area.color;
            DrawCube(pos, area.width, 0.04f, area.depth, fill);
            if (isSelected) {
                DrawCubeWires(pos, area.width + 0.1f, 0.06f, area.depth + 0.1f, YELLOW);
            }
        }

        // Draw existing obstacles
        for (size_t i = 0; i < currentLevel.obstacles.size(); ++i) {
            const auto& obs = currentLevel.obstacles[i];
            bool isSelected = static_cast<int>(i) == selectedObstacleIndex;

            Vector3 pos = {obs.x, obs.height / 2.0f, obs.z};
            Arena3D::drawShadedObjectBox(pos, obs.width, obs.height, obs.depth, obs.color);

            Color wireColor = isSelected ? YELLOW : Fade(WHITE, 0.3f);
            DrawCubeWires(pos, obs.width, obs.height, obs.depth, wireColor);

            // Selection highlight
            if (isSelected) {
                DrawCubeWires(pos, obs.width + 0.2f, obs.height + 0.2f, obs.depth + 0.2f, GOLD);
            }
        }

        // Draw enemies (markers)
        for (size_t i = 0; i < currentLevel.enemies.size(); ++i) {
            const auto& e = currentLevel.enemies[i];
            bool isSelected = static_cast<int>(i) == selectedEnemyIndex;
            Vector3 pos = {e.x, 0.5f, e.z};
            DrawCube(pos, 0.6f, 1.0f, 0.6f, e.color);
            DrawCubeWires(pos, 0.8f, 1.2f, 0.8f, isSelected ? YELLOW : Fade(WHITE, 0.5f));
        }

        // Draw preview geometry from the shared mapper.
        if (draggingPlacement) {
            for (const auto& obs : dragPreviewObstacles) {
                Vector3 pos = {obs.x, obs.height / 2.0f, obs.z};
                Color previewColor = Fade(obs.color, 0.6f);
                Arena3D::drawShadedObjectBox(pos, obs.width, obs.height, obs.depth, previewColor);
                DrawCubeWires(pos, obs.width, obs.height, obs.depth, GREEN);
            }
            for (const auto& area : dragPreviewAreas) {
                Vector3 pos = {area.x, -0.025f, area.z};
                Color previewColor = Fade(area.color, 0.6f);
                DrawCube(pos, area.width, 0.05f, area.depth, previewColor);
                DrawCubeWires(pos, area.width, 0.05f, area.depth, GREEN);
            }
            const Vector3 startCellWorld = editor::ViewportMapper::worldFromCell(dragStartCell);
            const Vector3 mouseCellWorld = editor::ViewportMapper::worldFromCell(mouseGridCell());
            Vector3 start = {startCellWorld.x, 0.5f, startCellWorld.z};
            Vector3 end = {mouseCellWorld.x, 0.5f, mouseCellWorld.z};
            DrawLine3D(start, end, SKYBLUE);
        } else if (showPreview) {
            if (activeTool == editor::ToolKind::Area) {
                float yOffset = (previewArea.color.g > previewArea.color.r + 20 && previewArea.color.g > previewArea.color.b + 20) ? -0.035f : -0.027f;
                Vector3 previewPos = {previewArea.x, yOffset, previewArea.z};
                Color previewColor = Fade(previewArea.color, 0.5f);
                DrawCube(previewPos, previewArea.width, 0.04f, previewArea.depth, previewColor);
                DrawCubeWires(previewPos, previewArea.width, 0.04f, previewArea.depth, GREEN);
            } else if (activeTool == editor::ToolKind::Character) {
                Vector3 previewPos = {previewEnemy.x, 0.5f, previewEnemy.z};
                Color previewColor = Fade(previewEnemy.color, 0.5f);
                DrawCube(previewPos, 0.6f, 1.0f, 0.6f, previewColor);
                DrawCubeWires(previewPos, 0.8f, 1.2f, 0.8f, GREEN);
            } else {
                Vector3 previewPos = {previewObstacle.x, previewObstacle.height / 2.0f, previewObstacle.z};
                Color previewColor = Fade(previewObstacle.color, 0.5f);
                Arena3D::drawShadedObjectBox(previewPos,
                                             previewObstacle.width,
                                             previewObstacle.height,
                                             previewObstacle.depth,
                                             previewColor);
                DrawCubeWires(previewPos, previewObstacle.width, previewObstacle.height, previewObstacle.depth, GREEN);
            }
        }

        EndMode3D();

        // 2D UI
        renderUI();

        if (showSlotSelector) {
            renderSlotSelector();
        }
    }

    void renderUI() {
        const Rectangle footerCard{180.0f,
                                   static_cast<float>(Config::SCREEN_HEIGHT - 78),
                                   static_cast<float>(Config::SCREEN_WIDTH) - 560.0f,
                                   54.0f};
        const Rectangle infoCard = editor::ToolbarModel::infoCardRect();
        const Rectangle paletteCard = editor::ToolbarModel::paletteCardRect();
        const Rectangle toolbarRail = editor::ToolbarModel::railRect();
        const editor::ToolKind paletteTool = effectivePaletteTool();

        drawCard(toolbarRail, Color{16, 20, 28, 235}, Color{48, 66, 94, 255});
        for (std::size_t index = 0; index < editor::ToolbarModel::kToolButtons.size(); ++index) {
            const auto& spec = editor::ToolbarModel::kToolButtons[index];
            const Rectangle rect = editor::ToolbarModel::toolButtonRect(index);
            const bool selected = spec.tool == activeTool;
            const Color accent = toolAccent(spec.tool);
            const Color fill = selected
                ? Fade(accent, 0.32f)
                : Color{24, 30, 42, 255};
            const Color border = selected
                ? accent
                : Color{62, 74, 96, 255};

            DrawRectangleRounded(rect,
                                 0.24f,
                                 10,
                                 fill);
            DrawRectangleRoundedLines(rect, 0.24f, 10, border);
            DrawCircleV(Vector2{rect.x + rect.width - 12.0f, rect.y + 12.0f},
                        selected ? 4.0f : 2.2f,
                        selected ? accent : Fade(border, 0.55f));
            drawToolIcon(spec.tool, rect, accent, selected);
        }

        const Rectangle saveRect = editor::ToolbarModel::saveButtonRect();
        DrawRectangleRounded(saveRect, 0.24f, 10, Color{24, 42, 70, 255});
        DrawRectangleRoundedLines(saveRect, 0.24f, 10, SKYBLUE);
        drawSaveIcon(centeredIconRect(saveRect, 52.0f), SKYBLUE, WHITE);

        drawCard(paletteCard, Color{18, 24, 34, 235}, Color{60, 84, 118, 255});
        drawUiTextCentered(paletteModel_.title(paletteTool),
                           paletteCard.x + paletteCard.width * 0.5f,
                           paletteCard.y + 26.0f,
                           18,
                           toolAccent(paletteTool));
        for (int index = 0; index < paletteModel_.paletteSize(paletteTool); ++index) {
            const Rectangle swatch =
                editor::ToolbarModel::paletteSwatchRect(static_cast<std::size_t>(index));
            DrawRectangleRounded(swatch, 0.2f, 6, paletteModel_.paletteColor(paletteTool, index));
            DrawRectangleLinesEx(swatch,
                                 index == paletteModel_.activeIndex(paletteTool) ? 3.0f : 1.0f,
                                 index == paletteModel_.activeIndex(paletteTool)
                                     ? toolAccent(paletteTool)
                                     : Color{56, 64, 82, 255});
        }
        const Rectangle lastSwatch = editor::ToolbarModel::paletteSwatchRect(
            static_cast<std::size_t>(paletteModel_.paletteSize(paletteTool) - 1));
        const float floorSectionY = lastSwatch.y + lastSwatch.height + 18.0f;
        drawUiTextCentered("Current floor tint",
                           paletteCard.x + paletteCard.width * 0.5f,
                           floorSectionY,
                           15,
                           LIGHTGRAY);
        DrawRectangleRounded(Rectangle{paletteCard.x + (paletteCard.width - 52.0f) * 0.5f,
                                       floorSectionY + 22.0f,
                                       52.0f,
                                       20.0f},
                             0.25f,
                             6,
                             currentLevel.floorColor.a > 0 ? currentLevel.floorColor
                                                           : Color{25, 28, 35, 255});
        DrawRectangleRoundedLines(Rectangle{paletteCard.x + (paletteCard.width - 52.0f) * 0.5f,
                                            floorSectionY + 22.0f,
                                            52.0f,
                                            20.0f},
                                  0.25f,
                                  6,
                                  WHITE);

        drawCard(infoCard, Color{18, 24, 34, 235}, Color{70, 82, 104, 255});
        const int infoX = static_cast<int>(infoCard.x) + 18;
        const int infoY = static_cast<int>(infoCard.y) + 48;
        if (selectedObstacleIndex >= 0 && selectedObstacleIndex < static_cast<int>(currentLevel.obstacles.size())) {
            const auto& sel = currentLevel.obstacles[selectedObstacleIndex];
            drawUiText("SELECTED OBJECT", static_cast<float>(infoX), static_cast<float>(infoY), 22, YELLOW);
            drawUiText(TextFormat("Center: (%.1f, %.1f)", sel.x, sel.z),
                       static_cast<float>(infoX),
                       static_cast<float>(infoY + 34),
                       18,
                       WHITE);

            int boxW = 80;
            int boxH = 28;
            Rectangle widthBox{(float)infoX, (float)(infoY + 85), (float)boxW, (float)boxH};
            Rectangle depthBox{(float)(infoX + boxW + 10), (float)(infoY + 85), (float)boxW, (float)boxH};
            Rectangle heightBox{(float)(infoX + (boxW + 10) * 2), (float)(infoY + 85), (float)boxW, (float)boxH};

            auto drawBox = [&](Rectangle box, const char* label, const char* text, ActiveField field) {
                Color border = (activeField == field) ? YELLOW : LIGHTGRAY;
                DrawRectangleRec(box, Color{30, 30, 40, 255});
                DrawRectangleLinesEx(box, 2, border);
                drawUiText(label, box.x, box.y - 18.0f, 16, LIGHTGRAY);
                drawUiText(text, box.x + 6.0f, box.y + 6.0f, 18, WHITE);
            };

            drawBox(widthBox, "W", widthInput, ActiveField::WIDTH);
            drawBox(depthBox, "D", depthInput, ActiveField::DEPTH);
            drawBox(heightBox, "H", heightInput, ActiveField::HEIGHT);
            DrawRectangle(infoX, infoY + 184, 58, 58, sel.color);
            DrawRectangleLines(infoX, infoY + 184, 58, 58, WHITE);
        } else if (selectedAreaIndex >= 0 && selectedAreaIndex < static_cast<int>(currentLevel.areas.size())) {
            const auto& sel = currentLevel.areas[selectedAreaIndex];
            drawUiText("SELECTED AREA", static_cast<float>(infoX), static_cast<float>(infoY), 22, YELLOW);
            drawUiText(TextFormat("Center: (%.1f, %.1f)", sel.x, sel.z),
                       static_cast<float>(infoX),
                       static_cast<float>(infoY + 34),
                       18,
                       WHITE);

            int boxW = 80;
            int boxH = 28;
            Rectangle widthBox{(float)infoX, (float)(infoY + 85), (float)boxW, (float)boxH};
            Rectangle depthBox{(float)(infoX + boxW + 10), (float)(infoY + 85), (float)boxW, (float)boxH};
            Rectangle heightBox{(float)(infoX + (boxW + 10) * 2), (float)(infoY + 85), (float)boxW, (float)boxH};

            auto drawBox = [&](Rectangle box, const char* label, const char* text, ActiveField field, bool enabled) {
                Color border = (activeField == field) ? YELLOW : LIGHTGRAY;
                if (!enabled) border = GRAY;
                DrawRectangleRec(box, Color{30, 30, 40, 255});
                DrawRectangleLinesEx(box, 2, border);
                drawUiText(label, box.x, box.y - 18.0f, 16, enabled ? LIGHTGRAY : GRAY);
                drawUiText(text, box.x + 6.0f, box.y + 6.0f, 18, enabled ? WHITE : GRAY);
            };

            drawBox(widthBox, "W", widthInput, ActiveField::WIDTH, true);
            drawBox(depthBox, "D", depthInput, ActiveField::DEPTH, true);
            drawBox(heightBox, "H", "-", ActiveField::HEIGHT, false);
            DrawRectangle(infoX, infoY + 184, 58, 58, sel.color);
            DrawRectangleLines(infoX, infoY + 184, 58, 58, WHITE);
        } else if (selectedEnemyIndex >= 0 && selectedEnemyIndex < static_cast<int>(currentLevel.enemies.size())) {
            const auto& sel = currentLevel.enemies[selectedEnemyIndex];
            drawUiText("SELECTED CHARACTER", static_cast<float>(infoX), static_cast<float>(infoY), 22, YELLOW);
            drawUiText(TextFormat("Center: (%.1f, %.1f)", sel.x, sel.z),
                       static_cast<float>(infoX),
                       static_cast<float>(infoY + 34),
                       18,
                       WHITE);
            DrawRectangle(infoX, infoY + 98, 58, 58, sel.color);
            DrawRectangleLines(infoX, infoY + 98, 58, 58, WHITE);
        } else {
            drawUiText("PREVIEW", static_cast<float>(infoX), static_cast<float>(infoY), 22, SKYBLUE);
            drawUiText(toolLabel(activeTool),
                       static_cast<float>(infoX),
                       static_cast<float>(infoY + 34),
                       20,
                       toolAccent(activeTool));
            if (activeTool == editor::ToolKind::Obstacle) {
                drawUiText(TextFormat("Preview size: %.0f x %.0f",
                                      previewObstacle.width,
                                      previewObstacle.depth),
                           static_cast<float>(infoX),
                           static_cast<float>(infoY + 88),
                           18,
                           WHITE);
                drawUiText(TextFormat("Preview height: %.0f", previewObstacle.height),
                           static_cast<float>(infoX),
                           static_cast<float>(infoY + 116),
                           18,
                           WHITE);
                DrawRectangle(infoX, infoY + 154, 58, 58, previewObstacle.color);
                DrawRectangleLines(infoX, infoY + 154, 58, 58, WHITE);
            } else if (activeTool == editor::ToolKind::Area) {
                drawUiText(TextFormat("Preview size: %.0f x %.0f",
                                      previewArea.width,
                                      previewArea.depth),
                           static_cast<float>(infoX),
                           static_cast<float>(infoY + 88),
                           18,
                           WHITE);
                DrawRectangle(infoX, infoY + 134, 58, 58, previewArea.color);
                DrawRectangleLines(infoX, infoY + 134, 58, 58, WHITE);
            } else if (activeTool == editor::ToolKind::Character) {
                DrawRectangle(infoX, infoY + 98, 58, 58, previewEnemy.color);
                DrawRectangleLines(infoX, infoY + 98, 58, 58, WHITE);
            } else {
                DrawRectangle(infoX, infoY + 98, 58, 58, toolAccent(activeTool));
                DrawRectangleLines(infoX, infoY + 98, 58, 58, WHITE);
            }
        }

        drawCard(footerCard, Color{16, 20, 28, 230}, Color{48, 60, 80, 255});
        drawUiText(
            "WASD/edge pan  |  Wheel zoom  |  RMB select  |  Arrows nudge  |  Shift+wheel size  |  Ctrl+wheel height  |  F5 save  |  Q back",
            footerCard.x + 18.0f,
            footerCard.y + 18.0f,
            16,
            LIGHTGRAY);
        if (!statusMessage_.empty()) {
            drawUiTextCentered(statusMessage_,
                               Config::SCREEN_WIDTH * 0.5f,
                               footerCard.y - 36.0f,
                               18,
                               SKYBLUE);
        }
    }

    void renderSlotSelector() {
        DrawRectangle(0, 0, 1920, 1080, Fade(BLACK, 0.7f));

        const bool loading = slotSelectorAction == SlotSelectorAction::LOAD;
        drawUiTextCentered(loading ? "LOAD FROM SLOT" : "SAVE TO SLOT",
                           1920.0f * 0.5f,
                           loading ? 74.0f : 40.0f,
                           32,
                           loading ? SKYBLUE : GOLD);

        if (loading) {
            drawUiTextCentered("Click a slot or press 1-9",
                               1920.0f * 0.5f,
                               116.0f,
                               20,
                               LIGHTGRAY);
            drawUiTextCentered("Current editing slot highlighted   |   Q/ESC cancel",
                               1920.0f * 0.5f,
                               146.0f,
                               18,
                               GRAY);
        } else {
            const Rectangle inputRect = levelNameInputRect();
            const Color border = editingLevelName_ ? GOLD : SKYBLUE;
            const bool emptyName = std::strlen(levelNameBuffer) == 0u;
            const std::string_view displayName = emptyName ? std::string_view{"Untitled"} :
                                                             std::string_view{levelNameBuffer};
            const Color textColor = emptyName ? GRAY : WHITE;
            const int textWidth = measureUiTextWidth(displayName, 22);
            const float textPadding = 18.0f;
            const float visibleWidth = inputRect.width - textPadding * 2.0f;
            const float textX = textWidth > static_cast<int>(visibleWidth)
                ? inputRect.x + inputRect.width - textPadding - static_cast<float>(textWidth)
                : inputRect.x + textPadding;

            drawUiText("LEVEL NAME", inputRect.x, inputRect.y - 25.0f, 18, LIGHTGRAY);
            DrawRectangleRounded(inputRect, 0.12f, 10, Color{18, 22, 31, 245});
            DrawRectangleRoundedLines(inputRect, 0.12f, 10, border);

            BeginScissorMode(static_cast<int>(inputRect.x + textPadding),
                             static_cast<int>(inputRect.y + 2.0f),
                             static_cast<int>(visibleWidth),
                             static_cast<int>(inputRect.height - 4.0f));
            drawUiText(displayName, textX, inputRect.y + 13.0f, 22, textColor);
            EndScissorMode();

            if (editingLevelName_ && std::fmod(GetTime(), 1.0) < 0.5) {
                const float caretX = std::min(
                    inputRect.x + inputRect.width - textPadding,
                    textX + static_cast<float>(textWidth) + 2.0f);
                DrawRectangle(static_cast<int>(caretX),
                              static_cast<int>(inputRect.y + 12.0f),
                              2,
                              26,
                              GOLD);
            }

            drawUiTextCentered(
                editingLevelName_
                    ? "Type a name, then click a slot to save   |   Enter stops typing   |   Esc cancel"
                    : "Click a slot or press 1-9 to save   |   Click the name to edit   |   Q/ESC cancel",
                1920.0f * 0.5f,
                178.0f,
                18,
                LIGHTGRAY);
            drawUiTextCentered("Current editing slot highlighted",
                               1920.0f * 0.5f,
                               206.0f,
                               18,
                               GRAY);
        }

        const Vector2 mousePos = display::mousePosition();
        for (std::size_t index = 0; index < slotPreviewCards_.size(); ++index) {
            const auto& slot = slotPreviewCards_[index];
            const Rectangle rect = slotSelectorCardRect(index);
            const bool isCurrentSlot = slot.slotNumber == currentSlot;
            const bool hovered = CheckCollisionPointRec(mousePos, rect);
            const bool highlight = hovered || isCurrentSlot;

            Color bgColor;
            if (slot.exists) {
                bgColor = highlight ? Color{60, 70, 90, 255} : Color{30, 35, 50, 255};
            } else {
                bgColor = highlight ? Color{50, 55, 70, 255} : Color{20, 20, 25, 255};
            }

            const Color borderColor = isCurrentSlot
                ? Color{255, 215, 96, 255}
                : (hovered ? SKYBLUE : (slot.exists ? DARKGRAY : Color{50, 50, 55, 255}));

            DrawRectangleRounded(rect, 0.1f, 12, bgColor);
            DrawRectangleRoundedLines(rect, 0.1f, 12, borderColor);
            app_shell::level_slots::renderPreview(slot, rect);

            drawUiText(TextFormat("%d", slot.slotNumber),
                       rect.x + 20.0f,
                       rect.y + 20.0f,
                       28,
                       slot.exists ? LIGHTGRAY : DARKGRAY);
            drawUiTextCentered(slot.name,
                               rect.x + rect.width * 0.5f,
                               rect.y + rect.height - 44.0f,
                               18,
                               slot.exists ? WHITE : GRAY);

            if (isCurrentSlot) {
                const Rectangle badge{rect.x + rect.width - 120.0f, rect.y + 16.0f, 92.0f, 28.0f};
                DrawRectangleRounded(badge, 0.35f, 8, Fade(Color{255, 215, 96, 255}, 0.18f));
                DrawRectangleRoundedLines(badge, 0.35f, 8, Color{255, 215, 96, 255});
                drawUiTextCentered("CURRENT",
                                   badge.x + badge.width * 0.5f,
                                   badge.y + 6.0f,
                                   14,
                                   Color{255, 232, 170, 255});
            }

            if (!slot.exists) {
                drawUiTextCentered("Empty Slot",
                                   rect.x + rect.width * 0.5f,
                                   rect.y + rect.height - 20.0f,
                                   14,
                                   DARKGRAY);
            }
        }
    }
};
