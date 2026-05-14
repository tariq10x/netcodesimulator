#pragma once

#include <raylib.h>

#include <algorithm>

#include "character/CharacterProfile.hpp"

namespace ui {

inline void drawCharacterSilhouetteThumbnail(Rectangle bounds,
                                             character::CharacterAppearance appearance,
                                             Color fill,
                                             Color outline,
                                             Color background) {
    DrawRectangleRounded(bounds, 0.16f, 8, background);
    DrawRectangleRoundedLines(bounds, 0.16f, 8, Fade(outline, 0.42f));

    const character::CharacterGeometry geometry =
        character::buildCharacterGeometry(appearance);
    const float inset = std::min(bounds.width, bounds.height) * 0.12f;
    const float drawableWidth = std::max(1.0f, bounds.width - inset * 2.0f);
    const float drawableHeight = std::max(1.0f, bounds.height - inset * 2.0f);
    const float maxHeight = geometry.head.start.y + geometry.head.radius;
    const float metersToPixels =
        std::min(drawableWidth / character::kMaxShoulderWidth,
                 drawableHeight / std::max(0.01f, maxHeight));
    const Vector2 root{
        bounds.x + bounds.width * 0.5f,
        bounds.y + bounds.height - inset
    };

    auto project = [&](const sim::Vec3& point) {
        return Vector2{
            root.x + point.x * metersToPixels,
            root.y - point.y * metersToPixels
        };
    };

    const float torsoWidth = geometry.torso.radius * 2.0f * metersToPixels;
    const float torsoHeight =
        (geometry.torso.end.y - geometry.torso.start.y) * metersToPixels;
    const Rectangle torso{
        root.x - torsoWidth * 0.5f,
        root.y - torsoHeight,
        torsoWidth,
        torsoHeight
    };
    DrawRectangleRounded(torso, 0.45f, 12, fill);
    DrawRectangleRoundedLines(torso, 0.45f, 12, outline);

    auto drawShoulder = [&](const character::CharacterPrimitive& shoulder) {
        if (shoulder.radius <= 0.0f) {
            return;
        }
        const Vector2 start = project(shoulder.start);
        const Vector2 end = project(shoulder.end);
        const float thickness =
            std::max(2.0f, shoulder.radius * 2.0f * metersToPixels);
        DrawLineEx(start, end, thickness, fill);
        DrawCircleV(start, thickness * 0.5f, fill);
        DrawCircleV(end, thickness * 0.5f, fill);
        DrawLineEx(start, end, std::max(1.0f, thickness * 0.16f), outline);
    };
    drawShoulder(geometry.leftShoulder);
    drawShoulder(geometry.rightShoulder);

    const Vector2 head = project(geometry.head.start);
    DrawCircleV(head, geometry.head.radius * metersToPixels, fill);
    DrawCircleLines(static_cast<int>(head.x),
                    static_cast<int>(head.y),
                    geometry.head.radius * metersToPixels,
                    outline);
}

}  // namespace ui
