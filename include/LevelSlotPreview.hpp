#pragma once

#include <raylib.h>

#include <string>

#include "Config3D.hpp"
#include "LevelData.hpp"
#include "TypographyService.hpp"

namespace app_shell {
namespace level_slots {

constexpr int kGridCols = 3;
constexpr int kGridRows = 3;
constexpr int kTotalSlots = 9;

struct PreviewData {
    int slotNumber{1};
    bool exists{false};
    std::string name{"Empty Slot"};
    LevelData::Level previewLevel{};
};

inline PreviewData loadPreviewData(int slotNumber) {
    PreviewData preview;
    preview.slotNumber = slotNumber;
    preview.exists = LevelData::levelExists(slotNumber);

    if (!preview.exists) {
        return preview;
    }

    LevelData::Level loadedLevel;
    if (LevelData::loadLevel(loadedLevel, slotNumber)) {
        preview.name = loadedLevel.name;
        preview.previewLevel = loadedLevel;
    } else {
        preview.name = "Level " + std::to_string(slotNumber);
    }
    return preview;
}

inline Rectangle gridCardRectForIndex(int index,
                                      float startX,
                                      float startY,
                                      float slotWidth,
                                      float slotHeight,
                                      float spacingX,
                                      float spacingY) {
    const int row = index / kGridCols;
    const int column = index % kGridCols;
    return Rectangle{
        startX + static_cast<float>(column) * (slotWidth + spacingX),
        startY + static_cast<float>(row) * (slotHeight + spacingY),
        slotWidth,
        slotHeight
    };
}

inline Rectangle previewRectFor(const Rectangle& slotRect) {
    return Rectangle{
        slotRect.x + 18.0f,
        slotRect.y + 54.0f,
        slotRect.width - 36.0f,
        slotRect.height - 104.0f
    };
}

inline Vector2 projectToPreview(const Rectangle& previewRect, float worldX, float worldZ) {
    const float arenaHalfSize = Config::ARENA_SIZE;
    const float normalizedX = (worldX + arenaHalfSize) / (arenaHalfSize * 2.0f);
    const float normalizedZ = (worldZ + arenaHalfSize) / (arenaHalfSize * 2.0f);
    return Vector2{
        previewRect.x + normalizedX * previewRect.width,
        previewRect.y + normalizedZ * previewRect.height
    };
}

inline void renderPreview(const PreviewData& slot, const Rectangle& slotRect) {
    const Rectangle previewRect = previewRectFor(slotRect);
    const Color background = slot.exists
        ? (slot.previewLevel.floorColor.a > 0
               ? Fade(slot.previewLevel.floorColor, 0.9f)
               : Color{24, 28, 38, 255})
        : Color{26, 26, 32, 255};
    const Color border = slot.exists ? Color{120, 150, 190, 255} : Color{65, 65, 72, 255};

    DrawRectangleRounded(previewRect, 0.08f, 8, background);
    DrawRectangleRoundedLines(previewRect, 0.08f, 8, border);

    if (!slot.exists) {
        TypographyService& typography = TypographyService::shared();
        const TypographyStyle& captionStyle = typography.style(TypographyStyleId::Caption);
        typography.drawCentered(TypographyStyleId::Caption,
                                "No Preview",
                                previewRect.x + previewRect.width * 0.5f,
                                previewRect.y + previewRect.height * 0.5f -
                                    captionStyle.lineHeight * 0.5f,
                                GRAY);
        return;
    }

    DrawRectangleLines(static_cast<int>(previewRect.x + 6.0f),
                       static_cast<int>(previewRect.y + 6.0f),
                       static_cast<int>(previewRect.width - 12.0f),
                       static_cast<int>(previewRect.height - 12.0f),
                       Fade(RAYWHITE, 0.35f));

    for (const auto& area : slot.previewLevel.areas) {
        const Vector2 topLeft = projectToPreview(previewRect,
                                                 area.x - area.width * 0.5f,
                                                 area.z - area.depth * 0.5f);
        const Vector2 bottomRight = projectToPreview(previewRect,
                                                     area.x + area.width * 0.5f,
                                                     area.z + area.depth * 0.5f);
        DrawRectangle(static_cast<int>(topLeft.x),
                      static_cast<int>(topLeft.y),
                      static_cast<int>(bottomRight.x - topLeft.x),
                      static_cast<int>(bottomRight.y - topLeft.y),
                      Fade(area.color, 0.8f));
    }

    for (const auto& obstacle : slot.previewLevel.obstacles) {
        const Vector2 topLeft = projectToPreview(previewRect,
                                                 obstacle.x - obstacle.width * 0.5f,
                                                 obstacle.z - obstacle.depth * 0.5f);
        const Vector2 bottomRight = projectToPreview(previewRect,
                                                     obstacle.x + obstacle.width * 0.5f,
                                                     obstacle.z + obstacle.depth * 0.5f);
        DrawRectangle(static_cast<int>(topLeft.x),
                      static_cast<int>(topLeft.y),
                      static_cast<int>(bottomRight.x - topLeft.x),
                      static_cast<int>(bottomRight.y - topLeft.y),
                      obstacle.color);
        DrawRectangleLines(static_cast<int>(topLeft.x),
                           static_cast<int>(topLeft.y),
                           static_cast<int>(bottomRight.x - topLeft.x),
                           static_cast<int>(bottomRight.y - topLeft.y),
                           Fade(BLACK, 0.55f));
    }

    for (const auto& enemy : slot.previewLevel.enemies) {
        const Vector2 enemyPos = projectToPreview(previewRect, enemy.x, enemy.z);
        DrawCircleV(enemyPos, 4.0f, enemy.color);
        DrawCircleLines(static_cast<int>(enemyPos.x), static_cast<int>(enemyPos.y), 4.0f, WHITE);
    }

}

}  // namespace level_slots
}  // namespace app_shell
