#pragma once

#include <string>
#include <vector>

#include "sim/SimulationTypes.hpp"

namespace character {

struct CharacterAppearance {
    float shoulderWidth{1.05f};
    float shoulderHeight{0.16f};
    float shoulderAngleDeg{0.0f};
};

struct CharacterProfile {
    std::string id{"default"};
    std::string name{"Default"};
    CharacterAppearance appearance{};
    bool builtIn{false};
};

struct CharacterPrimitive {
    enum class Kind {
        Cylinder = 0,
        Sphere = 1,
        Shoulder = 2
    };

    Kind kind{Kind::Cylinder};
    sim::Vec3 start{};
    sim::Vec3 end{};
    float radius{0.0f};
};

struct CharacterGeometry {
    CharacterPrimitive torso{};
    CharacterPrimitive head{};
    CharacterPrimitive leftShoulder{};
    CharacterPrimitive rightShoulder{};
};

constexpr float kMinShoulderWidth = 0.00f;
constexpr float kMaxShoulderWidth = 4.00f;
constexpr float kMinShoulderHeight = 0.06f;
constexpr float kMaxShoulderHeight = 0.34f;
constexpr float kMinShoulderAngleDeg = -35.0f;
constexpr float kMaxShoulderAngleDeg = 35.0f;

CharacterAppearance defaultAppearance();
CharacterProfile defaultProfile();
CharacterAppearance normalizeAppearance(CharacterAppearance appearance);
CharacterProfile normalizeProfile(CharacterProfile profile);
CharacterGeometry buildCharacterGeometry(CharacterAppearance appearance);
std::string sanitizeProfileId(const std::string& text);
bool isBuiltInProfileId(const std::string& id);

}  // namespace character
