#include "LevelData.hpp"

#include "app/UserDataPaths.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

bool isValidSlot(int slot) {
    return slot >= 1 && slot <= 9;
}

void hashCombine(std::uint32_t* hash, std::uint32_t value) {
    if (hash == nullptr) {
        return;
    }

    *hash ^= value;
    *hash *= 16777619u;
}

void hashFloat(std::uint32_t* hash, float value) {
    union {
        float asFloat;
        std::uint32_t asBits;
    } bits{value};
    hashCombine(hash, bits.asBits);
}

void hashColor(std::uint32_t* hash, Color color) {
    hashCombine(hash, color.r);
    hashCombine(hash, color.g);
    hashCombine(hash, color.b);
    hashCombine(hash, color.a);
}

bool parseFloatValue(const std::string& line, float* outValue) {
    if (outValue == nullptr) {
        return false;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    const char* begin = line.c_str() + colon + 1u;
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(begin, &end);
    if (begin == end || errno == ERANGE) {
        return false;
    }

    *outValue = value;
    return true;
}

}  // namespace

namespace LevelData {

std::filesystem::path getLevelsDirectory() {
    std::filesystem::path base = app::userDataDirectory() / "levels";

    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        std::cerr << "Warning: Failed to create levels directory: " << ec.message() << std::endl;
    }
    return base;
}

std::filesystem::path getLevelPath(int slot) {
    return getLevelsDirectory() / ("level_" + std::to_string(slot) + ".json");
}

std::uint32_t schemaFingerprint(const LevelDefinition& level) {
    std::uint32_t hash = 2166136261u;

    for (char character : level.name) {
        hashCombine(&hash, static_cast<std::uint8_t>(character));
    }
    hashColor(&hash, level.floorColor);
    hashCombine(&hash, static_cast<std::uint32_t>(level.obstacles.size()));
    hashCombine(&hash, static_cast<std::uint32_t>(level.areas.size()));
    hashCombine(&hash, static_cast<std::uint32_t>(level.enemies.size()));

    for (const auto& obstacle : level.obstacles) {
        hashFloat(&hash, obstacle.x);
        hashFloat(&hash, obstacle.z);
        hashFloat(&hash, obstacle.width);
        hashFloat(&hash, obstacle.depth);
        hashFloat(&hash, obstacle.height);
        hashColor(&hash, obstacle.color);
    }

    for (const auto& area : level.areas) {
        hashFloat(&hash, area.x);
        hashFloat(&hash, area.z);
        hashFloat(&hash, area.width);
        hashFloat(&hash, area.depth);
        hashColor(&hash, area.color);
    }

    for (const auto& enemy : level.enemies) {
        hashFloat(&hash, enemy.x);
        hashFloat(&hash, enemy.z);
        hashColor(&hash, enemy.color);
    }

    return hash == 0u ? 1u : hash;
}

bool saveLevelDefinition(const LevelDefinition& level, const std::filesystem::path& filepath) {
    std::ofstream file(filepath, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Failed to save level to " << filepath << std::endl;
        return false;
    }

    file << "{\n";
    file << "  \"name\": \"" << level.name << "\",\n";
    file << "  \"floorColor\": ["
         << static_cast<int>(level.floorColor.r) << ", "
         << static_cast<int>(level.floorColor.g) << ", "
         << static_cast<int>(level.floorColor.b) << ", "
         << static_cast<int>(level.floorColor.a) << "],\n";
    file << "  \"areas\": [\n";

    for (std::size_t i = 0; i < level.areas.size(); ++i) {
        const auto& area = level.areas[i];
        file << "    {\n";
        file << "      \"x\": " << area.x << ",\n";
        file << "      \"z\": " << area.z << ",\n";
        file << "      \"width\": " << area.width << ",\n";
        file << "      \"depth\": " << area.depth << ",\n";
        file << "      \"color\": ["
             << static_cast<int>(area.color.r) << ", "
             << static_cast<int>(area.color.g) << ", "
             << static_cast<int>(area.color.b) << ", "
             << static_cast<int>(area.color.a) << "]\n";
        file << "    }";
        if (i < level.areas.size() - 1) {
            file << ",";
        }
        file << "\n";
    }

    file << "  ],\n";
    file << "  \"enemies\": [\n";
    for (std::size_t i = 0; i < level.enemies.size(); ++i) {
        const auto& enemy = level.enemies[i];
        file << "    {\n";
        file << "      \"x\": " << enemy.x << ",\n";
        file << "      \"z\": " << enemy.z << ",\n";
        file << "      \"color\": ["
             << static_cast<int>(enemy.color.r) << ", "
             << static_cast<int>(enemy.color.g) << ", "
             << static_cast<int>(enemy.color.b) << ", "
             << static_cast<int>(enemy.color.a) << "]\n";
        file << "    }";
        if (i < level.enemies.size() - 1) {
            file << ",";
        }
        file << "\n";
    }
    file << "  ],\n";
    file << "  \"obstacles\": [\n";

    for (std::size_t i = 0; i < level.obstacles.size(); ++i) {
        const auto& obstacle = level.obstacles[i];
        file << "    {\n";
        file << "      \"x\": " << obstacle.x << ",\n";
        file << "      \"z\": " << obstacle.z << ",\n";
        file << "      \"width\": " << obstacle.width << ",\n";
        file << "      \"depth\": " << obstacle.depth << ",\n";
        file << "      \"height\": " << obstacle.height << ",\n";
        file << "      \"color\": ["
             << static_cast<int>(obstacle.color.r) << ", "
             << static_cast<int>(obstacle.color.g) << ", "
             << static_cast<int>(obstacle.color.b) << ", "
             << static_cast<int>(obstacle.color.a) << "]\n";
        file << "    }";
        if (i < level.obstacles.size() - 1) {
            file << ",";
        }
        file << "\n";
    }

    file << "  ]\n";
    file << "}\n";

    std::cout << "Level saved to " << filepath << std::endl;
    return true;
}

bool saveLevel(const LevelDefinition& level, int slot) {
    if (!isValidSlot(slot)) {
        std::cerr << "Invalid level slot: " << slot << std::endl;
        return false;
    }

    return saveLevelDefinition(level, getLevelPath(slot));
}

bool loadLevelDefinition(LevelDefinition& level, const std::filesystem::path& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    level.obstacles.clear();
    level.name = "Untitled";
    level.floorColor = Color{20, 20, 25, 0};
    level.areas.clear();
    level.enemies.clear();

    std::string line;
    Obstacle currentObstacle;
    Area currentArea;
    EnemySpawn currentEnemy;
    bool inObstacle = false;
    bool inArea = false;
    bool inEnemy = false;
    bool inFloorPatches = false;
    int colorIndex = 0;
    enum class Section { NONE, OBSTACLES, AREAS, ENEMIES };
    Section section = Section::NONE;

    auto parseColorArray = [&](std::string buffer) -> Color {
        while (buffer.find(']') == std::string::npos && std::getline(file, line)) {
            const std::size_t start = line.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                buffer += " " + line.substr(start);
            }
        }

        std::stringstream stream;
        std::vector<int> values;
        for (char character : buffer) {
            if ((character >= '0' && character <= '9') || character == '-') {
                stream << character;
            } else {
                stream << ' ';
            }
        }

        int value = 0;
        while (stream >> value) {
            values.push_back(value);
        }

        if (values.size() >= 4) {
            return Color{
                static_cast<unsigned char>(values[0]),
                static_cast<unsigned char>(values[1]),
                static_cast<unsigned char>(values[2]),
                static_cast<unsigned char>(values[3])
            };
        }

        const int paletteIndex = colorIndex++ % PALETTE_SIZE;
        return PALETTE[paletteIndex];
    };

    while (std::getline(file, line)) {
        const std::size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        line = line.substr(start);

        if (line.find("\"floorPatches\"") != std::string::npos) {
            inFloorPatches = true;
            continue;
        }
        if (inFloorPatches) {
            if (line.find(']') != std::string::npos) {
                inFloorPatches = false;
            }
            continue;
        }

        if (line.find("\"areas\"") != std::string::npos) {
            section = Section::AREAS;
            continue;
        }
        if (line.find("\"obstacles\"") != std::string::npos) {
            section = Section::OBSTACLES;
            continue;
        }
        if (line.find("\"enemies\"") != std::string::npos) {
            section = Section::ENEMIES;
            continue;
        }
        if (line.find(']') != std::string::npos && !inObstacle && !inArea && !inEnemy) {
            section = Section::NONE;
        }

        if (line.find("\"name\":") != std::string::npos) {
            const std::size_t firstQuote = line.find('"', line.find(':'));
            const std::size_t secondQuote = line.find('"', firstQuote + 1);
            if (firstQuote != std::string::npos && secondQuote != std::string::npos) {
                level.name = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            }
        } else if (line.find("\"floorColor\":") != std::string::npos) {
            level.floorColor = parseColorArray(line);
        } else if (section == Section::OBSTACLES &&
                   line.find('{') != std::string::npos &&
                   !inObstacle) {
            inObstacle = true;
            currentObstacle = Obstacle();
        } else if (section == Section::AREAS &&
                   line.find('{') != std::string::npos &&
                   !inArea) {
            inArea = true;
            currentArea = Area();
        } else if (section == Section::ENEMIES &&
                   line.find('{') != std::string::npos &&
                   !inEnemy) {
            inEnemy = true;
            currentEnemy = EnemySpawn();
        } else if (inObstacle) {
            if (line.find("\"x\":") != std::string::npos) {
                parseFloatValue(line, &currentObstacle.x);
            } else if (line.find("\"z\":") != std::string::npos) {
                parseFloatValue(line, &currentObstacle.z);
            } else if (line.find("\"width\":") != std::string::npos) {
                parseFloatValue(line, &currentObstacle.width);
            } else if (line.find("\"depth\":") != std::string::npos) {
                parseFloatValue(line, &currentObstacle.depth);
            } else if (line.find("\"height\":") != std::string::npos) {
                parseFloatValue(line, &currentObstacle.height);
            } else if (line.find("\"color\":") != std::string::npos) {
                currentObstacle.color = parseColorArray(line);
            } else if (line.find('}') != std::string::npos) {
                level.obstacles.push_back(currentObstacle);
                inObstacle = false;
            }
        } else if (inArea) {
            if (line.find("\"x\":") != std::string::npos) {
                parseFloatValue(line, &currentArea.x);
            } else if (line.find("\"z\":") != std::string::npos) {
                parseFloatValue(line, &currentArea.z);
            } else if (line.find("\"width\":") != std::string::npos) {
                parseFloatValue(line, &currentArea.width);
            } else if (line.find("\"depth\":") != std::string::npos) {
                parseFloatValue(line, &currentArea.depth);
            } else if (line.find("\"color\":") != std::string::npos) {
                currentArea.color = parseColorArray(line);
            } else if (line.find('}') != std::string::npos) {
                level.areas.push_back(currentArea);
                inArea = false;
            }
        } else if (inEnemy) {
            if (line.find("\"x\":") != std::string::npos) {
                parseFloatValue(line, &currentEnemy.x);
            } else if (line.find("\"z\":") != std::string::npos) {
                parseFloatValue(line, &currentEnemy.z);
            } else if (line.find("\"color\":") != std::string::npos) {
                currentEnemy.color = parseColorArray(line);
            } else if (line.find('}') != std::string::npos) {
                level.enemies.push_back(currentEnemy);
                inEnemy = false;
            }
        }
    }

    std::cout << "Level loaded from " << filepath << ": " << level.name
              << " (" << level.obstacles.size() << " obstacles, "
              << level.areas.size() << " areas, "
              << level.enemies.size() << " enemies)" << std::endl;
    return true;
}

bool loadLevel(LevelDefinition& level, int slot) {
    if (!isValidSlot(slot)) {
        std::cerr << "Invalid level slot: " << slot << std::endl;
        return false;
    }

    return loadLevelDefinition(level, getLevelPath(slot));
}

bool levelExists(int slot) {
    if (!isValidSlot(slot)) {
        return false;
    }
    return std::filesystem::exists(getLevelPath(slot));
}

bool deleteLevel(int slot) {
    if (!isValidSlot(slot)) {
        return false;
    }
    const std::filesystem::path filepath = getLevelPath(slot);
    std::error_code ec;
    return std::filesystem::remove(filepath, ec);
}

bool operator==(const Obstacle& lhs, const Obstacle& rhs) {
    return lhs.x == rhs.x &&
           lhs.z == rhs.z &&
           lhs.width == rhs.width &&
           lhs.depth == rhs.depth &&
           lhs.height == rhs.height &&
           lhs.color.r == rhs.color.r &&
           lhs.color.g == rhs.color.g &&
           lhs.color.b == rhs.color.b &&
           lhs.color.a == rhs.color.a;
}

bool operator==(const Area& lhs, const Area& rhs) {
    return lhs.x == rhs.x &&
           lhs.z == rhs.z &&
           lhs.width == rhs.width &&
           lhs.depth == rhs.depth &&
           lhs.color.r == rhs.color.r &&
           lhs.color.g == rhs.color.g &&
           lhs.color.b == rhs.color.b &&
           lhs.color.a == rhs.color.a;
}

bool operator==(const EnemySpawn& lhs, const EnemySpawn& rhs) {
    return lhs.x == rhs.x &&
           lhs.z == rhs.z &&
           lhs.color.r == rhs.color.r &&
           lhs.color.g == rhs.color.g &&
           lhs.color.b == rhs.color.b &&
           lhs.color.a == rhs.color.a;
}

bool operator==(const LevelDefinition& lhs, const LevelDefinition& rhs) {
    return lhs.name == rhs.name &&
           lhs.floorColor.r == rhs.floorColor.r &&
           lhs.floorColor.g == rhs.floorColor.g &&
           lhs.floorColor.b == rhs.floorColor.b &&
           lhs.floorColor.a == rhs.floorColor.a &&
           lhs.obstacles == rhs.obstacles &&
           lhs.areas == rhs.areas &&
           lhs.enemies == rhs.enemies;
}

}  // namespace LevelData
