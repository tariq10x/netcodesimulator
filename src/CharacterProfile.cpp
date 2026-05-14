#include "character/CharacterProfile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "Config3D.hpp"

namespace character {
namespace {

constexpr float kTorsoRadius = Config::ENEMY_BODY_RADIUS;
constexpr float kTorsoHeight = Config::ENEMY_BODY_HEIGHT;
constexpr float kHeadRadius = Config::ENEMY_HEAD_RADIUS;
constexpr float kHeadCenterY = Config::ENEMY_HEAD_OFFSET;
constexpr float kShoulderAnchorY = 1.34f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kShoulderVisibilityEpsilon = 0.001f;

float clamp(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

sim::Vec3 vec(float x, float y, float z) {
    return sim::Vec3{x, y, z};
}

}  // namespace

CharacterAppearance defaultAppearance() {
    return CharacterAppearance{};
}

CharacterProfile defaultProfile() {
    CharacterProfile profile;
    profile.id = "default";
    profile.name = "Default";
    profile.appearance = defaultAppearance();
    profile.builtIn = true;
    return profile;
}

CharacterAppearance normalizeAppearance(CharacterAppearance appearance) {
    appearance.shoulderWidth =
        clamp(appearance.shoulderWidth, kMinShoulderWidth, kMaxShoulderWidth);
    appearance.shoulderHeight =
        clamp(appearance.shoulderHeight, kMinShoulderHeight, kMaxShoulderHeight);
    appearance.shoulderAngleDeg =
        clamp(appearance.shoulderAngleDeg, kMinShoulderAngleDeg, kMaxShoulderAngleDeg);
    return appearance;
}

CharacterProfile normalizeProfile(CharacterProfile profile) {
    profile.id = sanitizeProfileId(profile.id);
    if (profile.id.empty()) {
        profile.id = "character";
    }
    if (isBuiltInProfileId(profile.id)) {
        profile.id = "default";
        profile.builtIn = true;
    }
    if (profile.name.empty()) {
        profile.name = profile.builtIn ? "Default" : "Character";
    }
    profile.appearance = normalizeAppearance(profile.appearance);
    return profile;
}

CharacterGeometry buildCharacterGeometry(CharacterAppearance appearance) {
    appearance = normalizeAppearance(appearance);

    CharacterGeometry geometry;
    geometry.torso.kind = CharacterPrimitive::Kind::Cylinder;
    geometry.torso.start = vec(0.0f, 0.0f, 0.0f);
    geometry.torso.end = vec(0.0f, kTorsoHeight, 0.0f);
    geometry.torso.radius = kTorsoRadius;

    geometry.head.kind = CharacterPrimitive::Kind::Sphere;
    geometry.head.start = vec(0.0f, kHeadCenterY, 0.0f);
    geometry.head.end = geometry.head.start;
    geometry.head.radius = kHeadRadius;

    const float halfTorso = kTorsoRadius * 0.70f;
    const float halfShoulders = std::max(halfTorso, appearance.shoulderWidth * 0.5f);
    const float shoulderRun = halfShoulders - halfTorso;
    const bool shouldersVisible = shoulderRun > kShoulderVisibilityEpsilon;
    const float shoulderRise =
        std::tan(appearance.shoulderAngleDeg * kPi / 180.0f) *
        shoulderRun;

    geometry.leftShoulder.kind = CharacterPrimitive::Kind::Shoulder;
    geometry.leftShoulder.start = vec(-halfTorso, kShoulderAnchorY, 0.0f);
    geometry.leftShoulder.end = vec(-halfShoulders, kShoulderAnchorY + shoulderRise, 0.0f);
    geometry.leftShoulder.radius = shouldersVisible ? appearance.shoulderHeight * 0.5f : 0.0f;

    geometry.rightShoulder.kind = CharacterPrimitive::Kind::Shoulder;
    geometry.rightShoulder.start = vec(halfTorso, kShoulderAnchorY, 0.0f);
    geometry.rightShoulder.end = vec(halfShoulders, kShoulderAnchorY + shoulderRise, 0.0f);
    geometry.rightShoulder.radius = shouldersVisible ? appearance.shoulderHeight * 0.5f : 0.0f;

    return geometry;
}

std::string sanitizeProfileId(const std::string& text) {
    std::string sanitized;
    bool lastWasSeparator = false;
    for (unsigned char rawCh : text) {
        const char ch = static_cast<char>(std::tolower(rawCh));
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            sanitized.push_back(ch);
            lastWasSeparator = false;
        } else if (!sanitized.empty() && !lastWasSeparator) {
            sanitized.push_back('-');
            lastWasSeparator = true;
        }
    }
    while (!sanitized.empty() && sanitized.back() == '-') {
        sanitized.pop_back();
    }
    return sanitized;
}

bool isBuiltInProfileId(const std::string& id) {
    return id == "default";
}

}  // namespace character
