#include "editor/LevelEditorDomain.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool sameColor(Color lhs, Color rhs) {
    return lhs.r == rhs.r &&
           lhs.g == rhs.g &&
           lhs.b == rhs.b &&
           lhs.a == rhs.a;
}

bool nearlyEqual(float lhs, float rhs, float epsilon = 0.001f) {
    const float delta = lhs - rhs;
    return delta <= epsilon && delta >= -epsilon;
}

void testCenteredBrushPreviewMatchesAnchorCellForOddAndEvenSizes() {
    const editor::GridRect single =
        editor::ViewportMapper::centeredBrushRect(editor::GridCell{0, 0}, 1, 1);
    const LevelData::Obstacle singleObstacle =
        editor::ViewportMapper::obstacleFromRect(single, 2, LevelData::PALETTE[0]);

    expect(nearlyEqual(singleObstacle.x, 0.0f) && nearlyEqual(singleObstacle.z, 0.0f),
           "1x1 centered brush previews should stay anchored to the selected grid cell");
    expect(nearlyEqual(singleObstacle.width, 1.0f) && nearlyEqual(singleObstacle.depth, 1.0f),
           "1x1 centered brush previews should keep the requested footprint");

    const editor::GridRect even =
        editor::ViewportMapper::centeredBrushRect(editor::GridCell{7, -3}, 4, 4);
    const LevelData::Obstacle evenObstacle =
        editor::ViewportMapper::obstacleFromRect(even, 2, LevelData::PALETTE[1]);

    expect(nearlyEqual(evenObstacle.x, 7.0f) && nearlyEqual(evenObstacle.z, -3.0f),
           "even-sized centered brush previews should stay anchored to the selected grid cell");
    expect(nearlyEqual(evenObstacle.width, 4.0f) && nearlyEqual(evenObstacle.depth, 4.0f),
           "even-sized centered brush previews should keep the requested footprint");
}

void testDragRectStartsFromTheSamePlacementAsSingleClickBrush() {
    const editor::GridRect dragRect =
        editor::ViewportMapper::dragRect(editor::GridCell{2, -2},
                                         editor::GridCell{2, -2},
                                         4,
                                         4);
    const LevelData::Obstacle dragObstacle =
        editor::ViewportMapper::obstacleFromRect(dragRect, 3, LevelData::PALETTE[2]);

    expect(nearlyEqual(dragObstacle.x, 2.0f) && nearlyEqual(dragObstacle.z, -2.0f),
           "a zero-length drag should place the same footprint as a single-click centered brush");
    expect(nearlyEqual(dragObstacle.width, 4.0f) && nearlyEqual(dragObstacle.depth, 4.0f),
           "a zero-length drag should preserve the configured brush footprint");
}

void testDragRectExpandsAcrossBrushSweepDeterministically() {
    const editor::GridRect dragRect =
        editor::ViewportMapper::dragRect(editor::GridCell{0, 0},
                                         editor::GridCell{3, 0},
                                         2,
                                         2);
    const LevelData::Obstacle obstacle =
        editor::ViewportMapper::obstacleFromRect(dragRect, 2, LevelData::PALETTE[3]);

    expect(nearlyEqual(obstacle.x, 1.5f) && nearlyEqual(obstacle.z, 0.0f),
           "drag placement should sweep between the start and end brush extents without extra offsets");
    expect(nearlyEqual(obstacle.width, 5.0f) && nearlyEqual(obstacle.depth, 2.0f),
           "drag placement should cover the full swept footprint of the configured brush");
}

void testMoveSelectionKeepsObstacleAnchoredToTargetCell() {
    LevelData::Obstacle obstacle;
    obstacle.x = -4.0f;
    obstacle.z = 6.0f;
    obstacle.width = 1.0f;
    obstacle.depth = 1.0f;
    obstacle.height = 3.0f;
    obstacle.color = LevelData::PALETTE[4];

    const LevelData::Obstacle moved =
        editor::ViewportMapper::moveObstacleToCell(obstacle, editor::GridCell{5, -2});

    expect(nearlyEqual(moved.x, 5.0f) && nearlyEqual(moved.z, -2.0f),
           "moving an obstacle should not introduce grid drift");
    expect(nearlyEqual(moved.width, 1.0f) &&
               nearlyEqual(moved.depth, 1.0f) &&
               nearlyEqual(moved.height, 3.0f),
           "moving an obstacle should preserve its dimensions");
    expect(sameColor(moved.color, obstacle.color),
           "moving an obstacle should preserve its color");
}

void testObstacleRectRoundTripsThroughSharedViewportMapper() {
    LevelData::Obstacle obstacle;
    obstacle.x = 7.0f;
    obstacle.z = -9.0f;
    obstacle.width = 4.0f;
    obstacle.depth = 6.0f;
    obstacle.height = 2.0f;
    obstacle.color = LevelData::PALETTE[5];

    const editor::GridRect rect = editor::ViewportMapper::rectForObstacle(obstacle);
    const LevelData::Obstacle roundTrip =
        editor::ViewportMapper::obstacleFromRect(rect, 2, obstacle.color);

    expect(nearlyEqual(roundTrip.x, obstacle.x) &&
               nearlyEqual(roundTrip.z, obstacle.z) &&
               nearlyEqual(roundTrip.width, obstacle.width) &&
               nearlyEqual(roundTrip.depth, obstacle.depth),
           "shared obstacle rect conversions should round-trip exactly for editor placement and move flows");
}

void testToolbarHitTargetsStayDeterministic() {
    const Rectangle toolRect = editor::ToolbarModel::toolButtonRect(0u);
    const Vector2 toolPoint{toolRect.x + toolRect.width * 0.5f,
                            toolRect.y + toolRect.height * 0.5f};
    const auto tool = editor::ToolbarModel::toolAtPoint(toolPoint);
    expect(tool.has_value() && *tool == editor::ToolKind::Obstacle,
           "toolbar hit testing should resolve the obstacle tool button deterministically");

    const Rectangle saveRect = editor::ToolbarModel::saveButtonRect();
    const Vector2 savePoint{saveRect.x + saveRect.width * 0.5f,
                            saveRect.y + saveRect.height * 0.5f};
    expect(editor::ToolbarModel::saveButtonHit(savePoint),
           "toolbar hit testing should expose a clickable save button");

    const Rectangle swatchRect = editor::ToolbarModel::paletteSwatchRect(3u);
    const Vector2 swatchPoint{swatchRect.x + swatchRect.width * 0.5f,
                              swatchRect.y + swatchRect.height * 0.5f};
    const auto swatchIndex = editor::ToolbarModel::paletteIndexAtPoint(swatchPoint, 8);
    expect(swatchIndex.has_value() && *swatchIndex == 3,
           "palette hit testing should map click positions back to the correct swatch");
}

void testToolbarAndPaletteShareACompactSingleColumnLayout() {
    const Rectangle railRect = editor::ToolbarModel::railRect();
    const Rectangle saveRect = editor::ToolbarModel::saveButtonRect();
    const Rectangle paletteRect = editor::ToolbarModel::paletteCardRect();
    const Rectangle swatch0 = editor::ToolbarModel::paletteSwatchRect(0u);
    const Rectangle swatch2 = editor::ToolbarModel::paletteSwatchRect(2u);
    const Rectangle swatch3 = editor::ToolbarModel::paletteSwatchRect(3u);
    const float trailingGap = railRect.y + railRect.height - (saveRect.y + saveRect.height);

    expect(nearlyEqual(railRect.x, paletteRect.x) && nearlyEqual(railRect.width, paletteRect.width),
           "palette card should share the same left-column width as the toolbar rail");
    expect(nearlyEqual(railRect.x, railRect.y) && railRect.y <= 24.0f,
           "toolbar rail should anchor near the top-left corner with matching top and left margins");
    expect(trailingGap <= 24.0f,
           "toolbar rail should end shortly after the save button instead of leaving a large dead zone below it");
    expect(nearlyEqual(swatch0.y, swatch2.y) && swatch3.y > swatch0.y,
           "palette swatches should wrap onto a new row after three colors to keep the palette column compact");
}

void testPaletteModelTracksIndependentToolSelections() {
    editor::PaletteModel palette;
    palette.setActiveIndex(editor::ToolKind::Obstacle, 2);
    palette.setActiveIndex(editor::ToolKind::Area, 4);
    palette.setActiveIndex(editor::ToolKind::Character, 6);
    palette.syncFloorColor(LevelData::PALETTE[3]);

    expect(palette.activeIndex(editor::ToolKind::Obstacle) == 2,
           "obstacle palette selection should not drift when other tool palettes change");
    expect(palette.activeIndex(editor::ToolKind::Area) == 4,
           "area palette selection should stay independent from obstacle colors");
    expect(palette.activeIndex(editor::ToolKind::Character) == 6,
           "character palette selection should stay independent from other tools");
    expect(sameColor(palette.floorColor(), LevelData::PALETTE[3]),
           "floor tint selection should track the synced palette color deterministically");
}

void testLevelNameModelEditsSaveSafeNames() {
    char buffer[8] = "";

    expect(editor::LevelNameModel::appendCharacter(buffer, sizeof(buffer), 'A'),
           "level name input should accept printable letters");
    expect(editor::LevelNameModel::appendCharacter(buffer, sizeof(buffer), '1'),
           "level name input should accept printable digits");
    expect(editor::LevelNameModel::appendCharacter(buffer, sizeof(buffer), ' '),
           "level name input should accept spaces");
    expect(std::string(buffer) == "A1 ",
           "level name input should append accepted characters in order");

    expect(!editor::LevelNameModel::appendCharacter(buffer, sizeof(buffer), '"'),
           "level name input should reject quotes that the level file cannot currently round-trip");
    expect(!editor::LevelNameModel::appendCharacter(buffer, sizeof(buffer), '\\'),
           "level name input should reject backslashes that the level file cannot currently round-trip");
    expect(editor::LevelNameModel::eraseLast(buffer, sizeof(buffer)),
           "level name input should support backspace edits");
    expect(std::string(buffer) == "A1",
           "level name input should erase only the last accepted character");
}

void testLevelNameModelSanitizesNamesForSaving() {
    expect(editor::LevelNameModel::sanitizedForSave("  Arena One  ") == "Arena One",
           "saved level names should trim surrounding spaces");
    expect(editor::LevelNameModel::sanitizedForSave("   ") == "Untitled",
           "saved level names should fall back when the edited name is blank");
    expect(editor::LevelNameModel::sanitizedForSave("A\"B\\C") == "ABC",
           "saved level names should strip characters that would break the current level file format");
}

}  // namespace

int main() {
    try {
        testCenteredBrushPreviewMatchesAnchorCellForOddAndEvenSizes();
        testDragRectStartsFromTheSamePlacementAsSingleClickBrush();
        testDragRectExpandsAcrossBrushSweepDeterministically();
        testMoveSelectionKeepsObstacleAnchoredToTargetCell();
        testObstacleRectRoundTripsThroughSharedViewportMapper();
        testToolbarHitTargetsStayDeterministic();
        testToolbarAndPaletteShareACompactSingleColumnLayout();
        testPaletteModelTracksIndependentToolSelections();
        testLevelNameModelEditsSaveSafeNames();
        testLevelNameModelSanitizesNamesForSaving();
        std::cout << "LevelEditorTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "LevelEditorTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
