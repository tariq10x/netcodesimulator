#include "Arena3D.hpp"
#include "LevelData.hpp"

void Arena3D::loadLevel(const LevelData::Level& level) {
    // Clear existing obstacles
    extraBlocks.clear();
    useCustomLevel = true;
    floorColor = level.floorColor;
    areas = level.areas;

    // Convert LevelData::Obstacle to BoxObstacle
    for (const auto& obs : level.obstacles) {
        BoxObstacle boxObs;
        boxObs.center = Vector3{obs.x, obs.height / 2.0f, obs.z};
        boxObs.sizeX = obs.width;
        boxObs.sizeY = obs.height;
        boxObs.sizeZ = obs.depth;
        boxObs.color = obs.color;
        boxObs.outline = obs.color;

        extraBlocks.push_back(boxObs);
    }
}
