#pragma once
#include <raylib.h>
#include <filesystem>
#include <vector>
#include <string>

namespace LevelData {
    // Obstacle definition
    struct Obstacle {
        float x{0.0f};           // Position X (center)
        float z{0.0f};           // Position Z (center)
        float width{4.0f};       // Width (X dimension)
        float depth{4.0f};       // Depth (Z dimension)
        float height{2.0f};      // Height (Y dimension)
        Color color{120, 150, 200, 255};  // RGBA color

        Obstacle() = default;
        Obstacle(float px, float pz, float w, float d, float h, Color c)
            : x(px), z(pz), width(w), depth(d), height(h), color(c) {}
    };

    // Area definition (thin floor tint patches)
    struct Area {
        float x{0.0f};      // Position X (center)
        float z{0.0f};      // Position Z (center)
        float width{4.0f};  // Width (X dimension)
        float depth{4.0f};  // Depth (Z dimension)
        Color color{120, 120, 120, 120}; // Semi-transparent color
    };

    // Enemy spawn definition
    struct EnemySpawn {
        float x{0.0f};
        float z{0.0f};
        Color color{255, 0, 0, 255};
    };

    // Portable level schema shared by runtime and editor code.
    struct LevelDefinition {
        std::string name{"Untitled"};
        Color floorColor{20, 20, 25, 0};
        std::vector<Obstacle> obstacles;
        std::vector<Area> areas;
        std::vector<EnemySpawn> enemies;

        LevelDefinition() = default;
        explicit LevelDefinition(const std::string& levelName) : name(levelName) {}
    };

    using Level = LevelDefinition;

    // Color palette - 10 visually appealing complementary colors
    inline const Color PALETTE[] = {
        Color{100, 149, 237, 255},  // Cornflower Blue
        Color{255, 127, 80, 255},   // Coral (complementary to blue)
        Color{147, 112, 219, 255},  // Medium Purple
        Color{218, 165, 32, 255},   // Golden Rod (complementary to purple)
        Color{72, 209, 204, 255},   // Medium Turquoise
        Color{220, 20, 60, 255},    // Crimson (complementary to turquoise)
        Color{50, 205, 50, 255},    // Lime Green
        Color{255, 99, 132, 255},   // Pink (complementary to green)
        Color{255, 215, 0, 255},    // Gold
        Color{138, 43, 226, 255}    // Blue Violet (complementary to gold)
    };
    constexpr int PALETTE_SIZE = 10;

    // Area palette (semi-transparent tints)
    inline const Color AREA_PALETTE[] = {
        Color{140, 255, 140, 140},  // Neon-ish light green
        Color{255, 150, 150, 140},  // Neon-ish light red
        Color{255, 255, 255, 120},  // White
        Color{20, 20, 20, 120},     // Black (softened)
        Color{210, 210, 210, 130}   // Lighter gray
    };
    constexpr int AREA_PALETTE_SIZE = 5;

    bool saveLevelDefinition(const LevelDefinition& level, const std::filesystem::path& path);
    bool loadLevelDefinition(LevelDefinition& level, const std::filesystem::path& path);
    std::filesystem::path getLevelsDirectory();
    std::filesystem::path getLevelPath(int slot);
    std::uint32_t schemaFingerprint(const LevelDefinition& level);
    bool saveLevel(const LevelDefinition& level, int slot);
    bool loadLevel(LevelDefinition& level, int slot);
    bool levelExists(int slot);
    bool deleteLevel(int slot);

    bool operator==(const Obstacle& lhs, const Obstacle& rhs);
    bool operator==(const Area& lhs, const Area& rhs);
    bool operator==(const EnemySpawn& lhs, const EnemySpawn& rhs);
    bool operator==(const LevelDefinition& lhs, const LevelDefinition& rhs);
}
