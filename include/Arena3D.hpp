#pragma once
#include <raylib.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include "Config3D.hpp"
#include "LevelData.hpp"
#include "client/RenderFrame.hpp"
#include "sim/SimulationTypes.hpp"

class Arena3D {
public:
    struct BoxObstacle {
        Vector3 center;
        float sizeX;
        float sizeY;
        float sizeZ;
        Color color;
        Color outline;
    };

    enum class AreaFilter {
        ALL,
        RED_ONLY,
        GREEN_ONLY
    };

private:
    float size;
    float wallHeight;
    float wallThickness;
    std::vector<BoxObstacle> extraBlocks;
    bool useCustomLevel;
    Color floorColor;
    bool floorTintEnabled;
    std::vector<LevelData::Area> areas;
    bool areasVisible;
    float areasFade;
    AreaFilter areaFilter;
    AreaFilter areaFilterPrev;
    float areaFilterBlend;
    float areaFilterBlendTarget;

public:
    Arena3D(float arenaSize = Config::ARENA_SIZE)
        : size(arenaSize),
          wallHeight(Config::ARENA_WALL_HEIGHT),
          wallThickness(Config::ARENA_WALL_THICKNESS),
          useCustomLevel(false),
          floorColor{20, 20, 25, 0},
          floorTintEnabled(false),
          areasVisible(true),
          areasFade(1.0f),
          areaFilter(AreaFilter::ALL),
          areaFilterPrev(AreaFilter::ALL),
          areaFilterBlend(1.0f),
          areaFilterBlendTarget(1.0f) {
        // Default level with some obstacles
        extraBlocks = {
            {{10.0f, 1.4f, -12.0f}, 8.0f, 2.8f, 2.0f, Color{100, 200, 120, 255}, GREEN},
            {{13.0f, 1.4f, -8.0f}, 2.0f, 2.8f, 8.0f, Color{100, 200, 120, 255}, GREEN},
            {{-8.0f, 1.4f, 12.0f}, 12.0f, 2.8f, 1.5f, Color{180, 90, 200, 255}, MAGENTA},
            {{15.0f, 1.4f, 8.0f}, 6.0f, 2.8f, 10.0f, Color{255, 150, 50, 255}, ORANGE}
        };
    }

    // Load custom level
    void loadLevel(const LevelData::Level& level);

    // Clear all obstacles
    void clearObstacles() {
        extraBlocks.clear();
    }

    static void drawShadedObjectBox(Vector3 center,
                                    float sizeX,
                                    float sizeY,
                                    float sizeZ,
                                    Color color) {
        BoxObstacle box{center, sizeX, sizeY, sizeZ, color, color};
        drawBoxObstacle(box);
    }

    void setFloorColor(Color c) { floorColor = c; }
    Color getFloorColor() const { return floorColor; }
    void setFloorTintEnabled(bool enabled) { floorTintEnabled = enabled; }
    bool isFloorTintEnabled() const { return floorTintEnabled; }
    void setAreasVisible(bool enabled) { areasVisible = enabled; }
    bool getAreasVisible() const { return areasVisible; }
    void setAreaFilter(AreaFilter filter) {
        areaFilterPrev = areaFilter;
        areaFilter = filter;
        areaFilterBlend = 0.0f;
        areaFilterBlendTarget = 1.0f;
    }
    AreaFilter getAreaFilter() const { return areaFilter; }
    void updateAreasFade(float dt) {
        float target = areasVisible ? 1.0f : 0.0f;
        float step = dt * 1.0f; // ~1s
        if (areasFade < target) areasFade = std::min(target, areasFade + step);
        else if (areasFade > target) areasFade = std::max(target, areasFade - step);

        if (areaFilterBlend < areaFilterBlendTarget) areaFilterBlend = std::min(areaFilterBlendTarget, areaFilterBlend + step);
        else if (areaFilterBlend > areaFilterBlendTarget) areaFilterBlend = std::max(areaFilterBlendTarget, areaFilterBlend - step);
    }

    void render(float dimFactor = 0.0f) const {
        renderAreas();
        renderFloorGrid(dimFactor);
        renderWalls(dimFactor);
        renderAdditionalObstacles();
    }

    void render(const client::ArenaRenderLayer& layer) const {
        if (!layer.visible) {
            return;
        }
        render(layer.dimFactor);
    }

    bool isOutOfBounds(Vector3 pos) const {
        return fabs(pos.x) > size || fabs(pos.z) > size;
    }

    Vector3 clampToBounds(Vector3 pos) const {
        pos.x = std::clamp(pos.x, -size, size);
        pos.z = std::clamp(pos.z, -size, size);
        return pos;
    }

    float getSize() const { return size; }
    const std::vector<BoxObstacle>& getExtraBlocks() const { return extraBlocks; }

    std::vector<sim::CollisionBox> getCollisionBoxes() const {
        std::vector<sim::CollisionBox> boxes;
        boxes.reserve(extraBlocks.size());
        for (const auto& block : extraBlocks) {
            boxes.push_back(sim::CollisionBox{
                sim::Vec3{block.center.x, block.center.y, block.center.z},
                sim::Vec3{block.sizeX * 0.5f, block.sizeY * 0.5f, block.sizeZ * 0.5f}
            });
        }
        return boxes;
    }

    sim::MovementEnvironment buildMovementEnvironment() const {
        sim::MovementEnvironment environment;
        environment.arenaHalfSize = size;
        environment.collisionBoxes = getCollisionBoxes();
        return environment;
    }

    Vector3 resolveCollisions(Vector3 pos, Vector3 prevPos, float radius = 0.5f, float entityHeight = Config::ENEMY_BODY_HEIGHT) const {
        Vector3 resolved = pos;
        for (const auto& block : extraBlocks) {
            resolved = resolveAABBCollision(resolved, prevPos, radius, block, entityHeight);
        }
        return resolved;
    }

    float getGroundHeightAt(Vector3 pos) const {
        float ground = 0.0f;
        for (const auto& block : extraBlocks) {
            float halfX = block.sizeX * 0.5f;
            float halfZ = block.sizeZ * 0.5f;
            if (pos.x >= block.center.x - halfX && pos.x <= block.center.x + halfX &&
                pos.z >= block.center.z - halfZ && pos.z <= block.center.z + halfZ) {
                float top = block.center.y + block.sizeY * 0.5f;
                if (top > ground) ground = top;
            }
        }
        return ground;
    }

    Vector3 playerRenderRootFromEyePosition(const sim::Vec3& eyePosition,
                                            float eyeHeight = Config::PLAYER_EYE_HEIGHT) const {
        const Vector3 eyePos{eyePosition.x, eyePosition.y, eyePosition.z};
        const float surfaceHeight = getGroundHeightAt(eyePos);
        const float impliedRootHeight = eyePos.y - eyeHeight;
        return Vector3{eyePos.x, std::max(surfaceHeight, impliedRootHeight), eyePos.z};
    }

    Vector3 playerRenderRootFromState(const sim::PlayerState& state,
                                      float eyeHeight = Config::PLAYER_EYE_HEIGHT) const {
        return playerRenderRootFromEyePosition(state.position, eyeHeight);
    }

private:
    void renderFloorGrid(float dimFactor) const {
        dimFactor = std::clamp(dimFactor, 0.0f, 1.0f);
        unsigned char alpha = static_cast<unsigned char>(255.0f - dimFactor * (255.0f - 60.0f));
        Color c = Color{180, 180, 180, alpha};
        // Slightly overshoot to ensure the grid fully covers the arena floor
        int halfSize = static_cast<int>(std::ceil(size)) + 1;
        // Base floor tint (only if explicitly enabled and visible)
        if (floorTintEnabled && floorColor.a > 0) {
            DrawPlane(Vector3{0, -0.01f, 0}, Vector2{size * 2.0f, size * 2.0f}, floorColor);
        }

        for (int i = -halfSize; i <= halfSize; i++) {
            DrawLine3D(Vector3{(float)i, 0.0f, (float)-halfSize}, Vector3{(float)i, 0.0f, (float)halfSize}, c);
            DrawLine3D(Vector3{(float)-halfSize, 0.0f, (float)i}, Vector3{(float)halfSize, 0.0f, (float)i}, c);
        }
    }

    void renderAreas() const {
        if (areasFade <= 0.001f) return;
        float alphaScaleBase = areasFade; // unaffected by focus dimming
        const float AREA_HEIGHT = 0.04f;
        for (const auto& area : areas) {
            bool isRed = (area.color.r > area.color.g + 20) && (area.color.r > area.color.b + 20);
            bool isGreen = (area.color.g > area.color.r + 20) && (area.color.g > area.color.b + 20);
            auto match = [&](AreaFilter f) {
                if (f == AreaFilter::ALL) return true;
                if (f == AreaFilter::RED_ONLY) return isRed;
                if (f == AreaFilter::GREEN_ONLY) return isGreen;
                return true;
            };
            float prevMatch = match(areaFilterPrev) ? 1.0f : 0.0f;
            float currMatch = match(areaFilter) ? 1.0f : 0.0f;
            float weight = prevMatch + (currMatch - prevMatch) * areaFilterBlend;
            if (weight <= 0.001f) continue;
            float alphaScale = alphaScaleBase * weight;
            Color c = area.color;
            c.a = static_cast<unsigned char>(static_cast<float>(area.color.a) * alphaScale);
            float yOffset = isGreen ? -0.035f : -0.027f; // green slightly lower than red
            Vector3 pos{area.x, yOffset, area.z};
            DrawCube(pos, area.width, AREA_HEIGHT, area.depth, c);
        }
    }

    void renderWalls(float dimFactor) const {
        dimFactor = std::clamp(dimFactor, 0.0f, 1.0f);
        unsigned char alpha = static_cast<unsigned char>(255.0f - dimFactor * (255.0f - 60.0f));
        Color c = Color{128, 128, 128, alpha};
        // North wall
        DrawCubeWires(Vector3{0, wallHeight / 2, size},
                     size * 2, wallHeight, wallThickness, c);

        // South wall
        DrawCubeWires(Vector3{0, wallHeight / 2, -size},
                     size * 2, wallHeight, wallThickness, c);

        // East wall
        DrawCubeWires(Vector3{size, wallHeight / 2, 0},
                     wallThickness, wallHeight, size * 2, c);

        // West wall
        DrawCubeWires(Vector3{-size, wallHeight / 2, 0},
                     wallThickness, wallHeight, size * 2, c);
    }

    void renderAdditionalObstacles() const {
        for (const auto& block : extraBlocks) {
            BoxObstacle dimmed = block;
            // Brighten base colors slightly
            auto brighten = [](unsigned char channel) {
                int val = channel + 40;
                return static_cast<unsigned char>(std::min(val, 255));
            };
            dimmed.color.r = brighten(dimmed.color.r);
            dimmed.color.g = brighten(dimmed.color.g);
            dimmed.color.b = brighten(dimmed.color.b);

            dimmed.color.a = block.color.a;
            dimmed.outline.a = block.outline.a;
            drawBoxObstacle(dimmed);
        }
    }

    static bool overlapsAABB(Vector3 pos, float radius, const BoxObstacle& box) {
        float halfX = box.sizeX / 2.0f;
        float halfZ = box.sizeZ / 2.0f;

        return (pos.x + radius > box.center.x - halfX && pos.x - radius < box.center.x + halfX &&
                pos.z + radius > box.center.z - halfZ && pos.z - radius < box.center.z + halfZ);
    }

    Vector3 resolveAABBCollision(Vector3 pos, Vector3 prevPos, float radius, const BoxObstacle& box, float entityHeight) const {
        // Vertical separation: allow passing over/under without horizontal collision
        float halfEntityH = entityHeight * 0.5f;
        float entityBottom = pos.y - halfEntityH;
        float entityTop = pos.y + halfEntityH;
        float boxTop = box.center.y + box.sizeY * 0.5f;
        float boxBottom = box.center.y - box.sizeY * 0.5f;
        if (entityBottom >= boxTop - 0.05f || entityTop <= boxBottom + 0.05f) {
            return pos; // Above or below the box, no collision
        }
        if (!overlapsAABB(pos, radius, box)) {
            return pos;
        }

        float halfX = box.sizeX / 2.0f;
        float halfZ = box.sizeZ / 2.0f;
        Vector3 resolved = pos;

        Vector3 testX = pos;
        testX.x = prevPos.x;
        if (!overlapsAABB(testX, radius, box)) {
            resolved.x = prevPos.x;
        }

        Vector3 testZ = pos;
        testZ.z = prevPos.z;
        if (!overlapsAABB(testZ, radius, box)) {
            resolved.z = prevPos.z;
        }

        if (overlapsAABB(resolved, radius, box)) {
            float distToLeft = fabs(pos.x - (box.center.x - halfX - radius));
            float distToRight = fabs(pos.x - (box.center.x + halfX + radius));
            float distToFront = fabs(pos.z - (box.center.z - halfZ - radius));
            float distToBack = fabs(pos.z - (box.center.z + halfZ + radius));

            float minDist = fmin(fmin(distToLeft, distToRight), fmin(distToFront, distToBack));

            if (minDist == distToLeft) resolved.x = box.center.x - halfX - radius;
            else if (minDist == distToRight) resolved.x = box.center.x + halfX + radius;
            else if (minDist == distToFront) resolved.z = box.center.z - halfZ - radius;
            else resolved.z = box.center.z + halfZ + radius;
        }

        return resolved;
    }

    static Color shadeObjectFaceColor(Color color, float factor) {
        auto shade = [factor](unsigned char channel) {
            const int value = static_cast<int>(std::round(static_cast<float>(channel) * factor));
            return static_cast<unsigned char>(std::clamp(value, 0, 255));
        };
        return Color{shade(color.r), shade(color.g), shade(color.b), color.a};
    }

    static void drawBoxFace(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color color) {
        DrawTriangle3D(a, b, c, color);
        DrawTriangle3D(a, c, d, color);
    }

    static void drawBoxObstacle(const BoxObstacle& box) {
        const float halfX = box.sizeX * 0.5f;
        const float halfY = box.sizeY * 0.5f;
        const float halfZ = box.sizeZ * 0.5f;

        const float minX = box.center.x - halfX;
        const float maxX = box.center.x + halfX;
        const float minY = box.center.y - halfY;
        const float maxY = box.center.y + halfY;
        const float minZ = box.center.z - halfZ;
        const float maxZ = box.center.z + halfZ;

        const Vector3 v000{minX, minY, minZ};
        const Vector3 v001{minX, minY, maxZ};
        const Vector3 v010{minX, maxY, minZ};
        const Vector3 v011{minX, maxY, maxZ};
        const Vector3 v100{maxX, minY, minZ};
        const Vector3 v101{maxX, minY, maxZ};
        const Vector3 v110{maxX, maxY, minZ};
        const Vector3 v111{maxX, maxY, maxZ};

        const Color top = shadeObjectFaceColor(box.color, 1.18f);
        const Color north = shadeObjectFaceColor(box.color, 1.00f);
        const Color west = shadeObjectFaceColor(box.color, 0.90f);
        const Color east = shadeObjectFaceColor(box.color, 0.78f);
        const Color south = shadeObjectFaceColor(box.color, 0.68f);
        drawBoxFace(v010, v011, v111, v110, top);
        drawBoxFace(v001, v101, v111, v011, north);
        drawBoxFace(v000, v010, v110, v100, south);
        drawBoxFace(v000, v001, v011, v010, west);
        drawBoxFace(v100, v110, v111, v101, east);
    }
};
