#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <raylib.h>

#include "Config3D.hpp"
#include "LevelData.hpp"

namespace editor {

enum class ToolKind : std::uint8_t {
    Obstacle = 0,
    Area = 1,
    Character = 2,
    Move = 3
};

enum class SelectionKind : std::uint8_t {
    None = 0,
    Obstacle = 1,
    Area = 2,
    Character = 3
};

struct SelectionRef {
    SelectionKind kind{SelectionKind::None};
    int index{-1};

    bool valid() const {
        return kind != SelectionKind::None && index >= 0;
    }
};

struct GridCell {
    int x{0};
    int z{0};
};

struct GridRect {
    float minX{0.0f};
    float minZ{0.0f};
    int widthUnits{1};
    int depthUnits{1};

    float maxX() const {
        return minX + static_cast<float>(widthUnits) - 1.0f;
    }

    float maxZ() const {
        return minZ + static_cast<float>(depthUnits) - 1.0f;
    }
};

class ViewportMapper {
public:
    static constexpr float kGridUnit = 1.0f;

    static GridCell cellFromWorld(const Vector3& worldPosition,
                                  float arenaHalfSize = Config::ARENA_SIZE) {
        GridCell cell{
            static_cast<int>(std::lround(worldPosition.x / kGridUnit)),
            static_cast<int>(std::lround(worldPosition.z / kGridUnit))
        };
        clampCell(&cell, arenaHalfSize);
        return cell;
    }

    static Vector3 worldFromCell(const GridCell& cell) {
        return Vector3{
            static_cast<float>(cell.x) * kGridUnit,
            0.0f,
            static_cast<float>(cell.z) * kGridUnit
        };
    }

    static GridRect centeredBrushRect(const GridCell& anchorCell,
                                      int widthUnits,
                                      int depthUnits,
                                      float arenaHalfSize = Config::ARENA_SIZE) {
        GridRect rect = centeredBrushRectUnclamped(anchorCell, widthUnits, depthUnits);
        clampRect(&rect, arenaHalfSize);
        return rect;
    }

    static GridRect dragRect(const GridCell& startCell,
                             const GridCell& endCell,
                             int minimumWidthUnits = 1,
                             int minimumDepthUnits = 1,
                             float arenaHalfSize = Config::ARENA_SIZE) {
        const GridRect startRect =
            centeredBrushRectUnclamped(startCell, minimumWidthUnits, minimumDepthUnits);
        const GridRect endRect =
            centeredBrushRectUnclamped(endCell, minimumWidthUnits, minimumDepthUnits);

        GridRect rect;
        rect.minX = std::min(startRect.minX, endRect.minX);
        rect.minZ = std::min(startRect.minZ, endRect.minZ);
        rect.widthUnits = std::max(
            1,
            static_cast<int>(std::lround(std::max(startRect.maxX(), endRect.maxX()) - rect.minX)) +
                1);
        rect.depthUnits = std::max(
            1,
            static_cast<int>(std::lround(std::max(startRect.maxZ(), endRect.maxZ()) - rect.minZ)) +
                1);
        clampRect(&rect, arenaHalfSize);
        return rect;
    }

    static GridRect rectForObstacle(const LevelData::Obstacle& obstacle) {
        return GridRect{
            obstacle.x - (obstacle.width - kGridUnit) * 0.5f,
            obstacle.z - (obstacle.depth - kGridUnit) * 0.5f,
            std::max(1, static_cast<int>(std::lround(obstacle.width / kGridUnit))),
            std::max(1, static_cast<int>(std::lround(obstacle.depth / kGridUnit)))
        };
    }

    static GridRect rectForArea(const LevelData::Area& area) {
        return GridRect{
            area.x - (area.width - kGridUnit) * 0.5f,
            area.z - (area.depth - kGridUnit) * 0.5f,
            std::max(1, static_cast<int>(std::lround(area.width / kGridUnit))),
            std::max(1, static_cast<int>(std::lround(area.depth / kGridUnit)))
        };
    }

    static LevelData::Obstacle obstacleFromRect(const GridRect& rect,
                                                int heightUnits,
                                                Color color) {
        LevelData::Obstacle obstacle;
        obstacle.x = rectCenterX(rect);
        obstacle.z = rectCenterZ(rect);
        obstacle.width = static_cast<float>(rect.widthUnits) * kGridUnit;
        obstacle.depth = static_cast<float>(rect.depthUnits) * kGridUnit;
        obstacle.height = static_cast<float>(std::max(heightUnits, 1)) * kGridUnit;
        obstacle.color = color;
        return obstacle;
    }

    static LevelData::Area areaFromRect(const GridRect& rect, Color color) {
        LevelData::Area area;
        area.x = rectCenterX(rect);
        area.z = rectCenterZ(rect);
        area.width = static_cast<float>(rect.widthUnits) * kGridUnit;
        area.depth = static_cast<float>(rect.depthUnits) * kGridUnit;
        area.color = color;
        return area;
    }

    static LevelData::EnemySpawn enemyFromCell(const GridCell& cell, Color color) {
        const Vector3 world = worldFromCell(cell);
        return LevelData::EnemySpawn{world.x, world.z, color};
    }

    static LevelData::Obstacle moveObstacleToCell(const LevelData::Obstacle& obstacle,
                                                  const GridCell& anchorCell,
                                                  float arenaHalfSize = Config::ARENA_SIZE) {
        const GridRect rect = centeredBrushRect(
            anchorCell,
            std::max(1, static_cast<int>(std::lround(obstacle.width / kGridUnit))),
            std::max(1, static_cast<int>(std::lround(obstacle.depth / kGridUnit))),
            arenaHalfSize);
        return obstacleFromRect(
            rect,
            std::max(1, static_cast<int>(std::lround(obstacle.height / kGridUnit))),
            obstacle.color);
    }

    static LevelData::Area moveAreaToCell(const LevelData::Area& area,
                                          const GridCell& anchorCell,
                                          float arenaHalfSize = Config::ARENA_SIZE) {
        const GridRect rect = centeredBrushRect(
            anchorCell,
            std::max(1, static_cast<int>(std::lround(area.width / kGridUnit))),
            std::max(1, static_cast<int>(std::lround(area.depth / kGridUnit))),
            arenaHalfSize);
        return areaFromRect(rect, area.color);
    }

    static LevelData::EnemySpawn moveEnemyToCell(const LevelData::EnemySpawn& enemy,
                                                 const GridCell& anchorCell) {
        LevelData::EnemySpawn moved = enemyFromCell(anchorCell, enemy.color);
        moved.color = enemy.color;
        return moved;
    }

private:
    static float rectCenterX(const GridRect& rect) {
        return rect.minX * kGridUnit +
               static_cast<float>(rect.widthUnits - 1) * kGridUnit * 0.5f;
    }

    static float rectCenterZ(const GridRect& rect) {
        return rect.minZ * kGridUnit +
               static_cast<float>(rect.depthUnits - 1) * kGridUnit * 0.5f;
    }

    static GridRect centeredBrushRectUnclamped(const GridCell& anchorCell,
                                               int widthUnits,
                                               int depthUnits) {
        GridRect rect;
        rect.widthUnits = std::max(widthUnits, 1);
        rect.depthUnits = std::max(depthUnits, 1);
        rect.minX =
            static_cast<float>(anchorCell.x) - static_cast<float>(rect.widthUnits - 1) * 0.5f;
        rect.minZ =
            static_cast<float>(anchorCell.z) - static_cast<float>(rect.depthUnits - 1) * 0.5f;
        return rect;
    }

    static int clampCellValue(int cell, float arenaHalfSize) {
        const int minCell = static_cast<int>(std::floor(-arenaHalfSize));
        const int maxCell = static_cast<int>(std::ceil(arenaHalfSize));
        return std::clamp(cell, minCell, maxCell);
    }

    static void clampCell(GridCell* cell, float arenaHalfSize) {
        if (cell == nullptr) {
            return;
        }
        cell->x = clampCellValue(cell->x, arenaHalfSize);
        cell->z = clampCellValue(cell->z, arenaHalfSize);
    }

    static void clampRect(GridRect* rect, float arenaHalfSize) {
        if (rect == nullptr) {
            return;
        }

        rect->widthUnits = std::max(rect->widthUnits, 1);
        rect->depthUnits = std::max(rect->depthUnits, 1);

        const float minCell = -arenaHalfSize + 0.5f;
        const float maxMinX = arenaHalfSize - static_cast<float>(rect->widthUnits) + 0.5f;
        const float maxMinZ = arenaHalfSize - static_cast<float>(rect->depthUnits) + 0.5f;
        rect->minX = std::clamp(rect->minX, minCell, maxMinX);
        rect->minZ = std::clamp(rect->minZ, minCell, maxMinZ);
    }
};

struct ToolbarButtonSpec {
    ToolKind tool{ToolKind::Obstacle};
    const char* label{""};
    const char* shortcut{""};
};

class ToolbarModel {
public:
    static constexpr float kRailX = 24.0f;
    static constexpr float kRailY = 24.0f;
    static constexpr float kRailWidth = 172.0f;
    static constexpr float kRailPaddingX = 14.0f;
    static constexpr float kRailPaddingY = 18.0f;
    static constexpr float kButtonHeight = 82.0f;
    static constexpr float kButtonGap = 16.0f;
    static constexpr float kCommandHeight = 70.0f;
    static constexpr float kPaletteCardGap = 22.0f;
    static constexpr float kPaletteCardHeight = 320.0f;
    static constexpr float kPaletteHeaderHeight = 82.0f;
    static constexpr std::size_t kPaletteSwatchesPerRow = 3u;
    static constexpr float kPaletteSwatchSize = 34.0f;
    static constexpr float kPaletteSwatchGap = 10.0f;

    static constexpr std::array<ToolbarButtonSpec, 4u> kToolButtons{{
        {ToolKind::Obstacle, "Object", ""},
        {ToolKind::Area, "Area", ""},
        {ToolKind::Character, "Character", ""},
        {ToolKind::Move, "Move", ""}
    }};

    static Rectangle railRect() {
        return Rectangle{
            kRailX,
            kRailY,
            kRailWidth,
            kRailPaddingY * 2.0f +
                static_cast<float>(kToolButtons.size()) * kButtonHeight +
                static_cast<float>(kToolButtons.size()) * kButtonGap +
                kCommandHeight
        };
    }

    static Rectangle toolButtonRect(std::size_t index) {
        return Rectangle{
            kRailX + kRailPaddingX,
            kRailY + kRailPaddingY + static_cast<float>(index) * (kButtonHeight + kButtonGap),
            kRailWidth - kRailPaddingX * 2.0f,
            kButtonHeight
        };
    }

    static Rectangle saveButtonRect() {
        return Rectangle{
            kRailX + kRailPaddingX,
            kRailY + kRailPaddingY +
                static_cast<float>(kToolButtons.size()) * (kButtonHeight + kButtonGap),
            kRailWidth - kRailPaddingX * 2.0f,
            kCommandHeight
        };
    }

    static Rectangle paletteCardRect() {
        const Rectangle rail = railRect();
        return Rectangle{kRailX, rail.y + rail.height + kPaletteCardGap, kRailWidth, kPaletteCardHeight};
    }

    static Rectangle infoCardRect() {
        return Rectangle{Config::SCREEN_WIDTH - 340.0f, 160.0f, 300.0f, 340.0f};
    }

    static Rectangle paletteSwatchRect(std::size_t index) {
        const float swatchRowWidth =
            static_cast<float>(kPaletteSwatchesPerRow) * kPaletteSwatchSize +
            static_cast<float>(kPaletteSwatchesPerRow - 1u) * kPaletteSwatchGap;
        const float startX = paletteCardRect().x + (paletteCardRect().width - swatchRowWidth) * 0.5f;
        const float startY = paletteCardRect().y + kPaletteHeaderHeight;
        return Rectangle{
            startX + std::fmod(static_cast<float>(index), static_cast<float>(kPaletteSwatchesPerRow)) *
                         (kPaletteSwatchSize + kPaletteSwatchGap),
            startY + std::floor(static_cast<float>(index) / static_cast<float>(kPaletteSwatchesPerRow)) *
                         (kPaletteSwatchSize + kPaletteSwatchGap),
            kPaletteSwatchSize,
            kPaletteSwatchSize
        };
    }

    static std::optional<ToolKind> toolAtPoint(const Vector2& point) {
        for (std::size_t index = 0; index < kToolButtons.size(); ++index) {
            if (CheckCollisionPointRec(point, toolButtonRect(index))) {
                return kToolButtons[index].tool;
            }
        }
        return std::nullopt;
    }

    static bool saveButtonHit(const Vector2& point) {
        return CheckCollisionPointRec(point, saveButtonRect());
    }

    static std::optional<int> paletteIndexAtPoint(const Vector2& point, int paletteSize) {
        for (int index = 0; index < paletteSize; ++index) {
            if (CheckCollisionPointRec(point, paletteSwatchRect(static_cast<std::size_t>(index)))) {
                return index;
            }
        }
        return std::nullopt;
    }
};

class LevelNameModel {
public:
    static constexpr std::size_t kMaxNameLength = 63u;

    static bool isAllowedCharacter(int codepoint) {
        return codepoint >= 32 &&
               codepoint <= 126 &&
               codepoint != '"' &&
               codepoint != '\\';
    }

    static bool appendCharacter(char* buffer, std::size_t bufferSize, int codepoint) {
        if (buffer == nullptr || bufferSize == 0u || !isAllowedCharacter(codepoint)) {
            return false;
        }

        const std::size_t length = boundedLength(buffer, bufferSize);
        if (length + 1u >= bufferSize || length >= kMaxNameLength) {
            return false;
        }

        buffer[length] = static_cast<char>(codepoint);
        buffer[length + 1u] = '\0';
        return true;
    }

    static bool eraseLast(char* buffer, std::size_t bufferSize) {
        if (buffer == nullptr || bufferSize == 0u) {
            return false;
        }

        const std::size_t length = boundedLength(buffer, bufferSize);
        if (length == 0u || length >= bufferSize) {
            return false;
        }

        buffer[length - 1u] = '\0';
        return true;
    }

    static std::string sanitizedForSave(std::string_view rawName) {
        std::string filtered;
        filtered.reserve(std::min(rawName.size(), kMaxNameLength));

        for (char character : rawName) {
            if (character == '\0') {
                break;
            }
            if (!isAllowedCharacter(static_cast<unsigned char>(character))) {
                continue;
            }
            if (filtered.size() >= kMaxNameLength) {
                break;
            }
            filtered.push_back(character);
        }

        const std::size_t first = filtered.find_first_not_of(' ');
        if (first == std::string::npos) {
            return "Untitled";
        }

        const std::size_t last = filtered.find_last_not_of(' ');
        return filtered.substr(first, last - first + 1u);
    }

private:
    static std::size_t boundedLength(const char* buffer, std::size_t bufferSize) {
        std::size_t length = 0u;
        while (length < bufferSize && buffer[length] != '\0') {
            ++length;
        }
        return length;
    }
};

class PaletteModel {
public:
    int activeIndex(ToolKind tool) const {
        switch (tool) {
            case ToolKind::Area:
                return areaIndex_;
            case ToolKind::Character:
                return characterIndex_;
            case ToolKind::Obstacle:
            case ToolKind::Move:
                return obstacleIndex_;
        }
        return obstacleIndex_;
    }

    int paletteSize(ToolKind tool) const {
        return tool == ToolKind::Area ? LevelData::AREA_PALETTE_SIZE : LevelData::PALETTE_SIZE;
    }

    Color paletteColor(ToolKind tool, int index) const {
        if (tool == ToolKind::Area) {
            const int clamped = std::clamp(index, 0, LevelData::AREA_PALETTE_SIZE - 1);
            return LevelData::AREA_PALETTE[clamped];
        }

        const int clamped = std::clamp(index, 0, LevelData::PALETTE_SIZE - 1);
        return LevelData::PALETTE[clamped];
    }

    Color activeColor(ToolKind tool) const {
        return paletteColor(tool, activeIndex(tool));
    }

    void setActiveIndex(ToolKind tool, int index) {
        if (tool == ToolKind::Area) {
            areaIndex_ = std::clamp(index, 0, LevelData::AREA_PALETTE_SIZE - 1);
            return;
        }
        if (tool == ToolKind::Character) {
            characterIndex_ = std::clamp(index, 0, LevelData::PALETTE_SIZE - 1);
            return;
        }
        obstacleIndex_ = std::clamp(index, 0, LevelData::PALETTE_SIZE - 1);
    }

    void cycleColor(ToolKind tool) {
        setActiveIndex(tool, activeIndex(tool) + 1);
    }

    void cycleFloorColor() {
        floorIndex_ = (floorIndex_ + 1) % LevelData::PALETTE_SIZE;
    }

    void syncColor(ToolKind tool, Color color) {
        setActiveIndex(tool, nearestPaletteIndex(tool, color));
    }

    void syncFloorColor(Color color) {
        floorIndex_ = nearestPaletteIndex(ToolKind::Obstacle, color);
    }

    Color floorColor() const {
        return LevelData::PALETTE[floorIndex_];
    }

    int floorIndex() const {
        return floorIndex_;
    }

    const char* title(ToolKind tool) const {
        switch (tool) {
            case ToolKind::Obstacle:
                return "Object Palette";
            case ToolKind::Area:
                return "Area Palette";
            case ToolKind::Character:
                return "Character Palette";
            case ToolKind::Move:
                return "Selection Palette";
        }
        return "Palette";
    }

private:
    int nearestPaletteIndex(ToolKind tool, Color color) const {
        const int size = paletteSize(tool);
        int bestIndex = 0;
        int bestDistance = std::numeric_limits<int>::max();
        for (int index = 0; index < size; ++index) {
            const Color candidate = paletteColor(tool, index);
            const int dr = static_cast<int>(candidate.r) - static_cast<int>(color.r);
            const int dg = static_cast<int>(candidate.g) - static_cast<int>(color.g);
            const int db = static_cast<int>(candidate.b) - static_cast<int>(color.b);
            const int da = static_cast<int>(candidate.a) - static_cast<int>(color.a);
            const int distance = dr * dr + dg * dg + db * db + da * da;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = index;
            }
        }
        return bestIndex;
    }

    int obstacleIndex_{0};
    int areaIndex_{0};
    int characterIndex_{0};
    int floorIndex_{0};
};

inline SelectionRef selectionFromIndices(int obstacleIndex,
                                         int areaIndex,
                                         int characterIndex) {
    if (obstacleIndex >= 0) {
        return SelectionRef{SelectionKind::Obstacle, obstacleIndex};
    }
    if (areaIndex >= 0) {
        return SelectionRef{SelectionKind::Area, areaIndex};
    }
    if (characterIndex >= 0) {
        return SelectionRef{SelectionKind::Character, characterIndex};
    }
    return SelectionRef{};
}

}  // namespace editor
