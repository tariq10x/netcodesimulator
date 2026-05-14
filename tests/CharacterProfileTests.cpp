#include "app/CharacterPresetStore.hpp"
#include "character/CharacterProfile.hpp"
#include "Config3D.hpp"
#include "TestDataRoot.hpp"

#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool nearlyEqual(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

void testAppearanceNormalizationClampsEditableShoulderValues() {
    character::CharacterAppearance appearance;
    appearance.shoulderWidth = -20.0f;
    appearance.shoulderHeight = 20.0f;
    appearance.shoulderAngleDeg = 100.0f;

    const character::CharacterAppearance normalized =
        character::normalizeAppearance(appearance);
    expect(nearlyEqual(normalized.shoulderWidth, character::kMinShoulderWidth),
           "normalization should clamp shoulder width to the supported minimum");
    appearance.shoulderWidth = 20.0f;
    expect(nearlyEqual(character::normalizeAppearance(appearance).shoulderWidth, 4.0f),
           "normalization should clamp shoulder width to the 4m editor maximum");
    expect(nearlyEqual(normalized.shoulderHeight, character::kMaxShoulderHeight),
           "normalization should clamp shoulder height to the supported maximum");
    expect(nearlyEqual(normalized.shoulderAngleDeg, character::kMaxShoulderAngleDeg),
           "normalization should clamp shoulder angle to the supported maximum");
}

void testGeometryKeepsBaseBodyStableAndBuildsIndependentShoulders() {
    character::CharacterAppearance noShoulders;
    noShoulders.shoulderWidth = 0.0f;
    const character::CharacterGeometry noShoulderGeometry =
        character::buildCharacterGeometry(noShoulders);
    expect(nearlyEqual(noShoulderGeometry.leftShoulder.radius, 0.0f) &&
               nearlyEqual(noShoulderGeometry.rightShoulder.radius, 0.0f) &&
               nearlyEqual(noShoulderGeometry.leftShoulder.start.x,
                           noShoulderGeometry.leftShoulder.end.x) &&
               nearlyEqual(noShoulderGeometry.rightShoulder.start.x,
                           noShoulderGeometry.rightShoulder.end.x),
           "zero shoulder width should remove the separate shoulder primitives");

    character::CharacterAppearance appearance;
    appearance.shoulderWidth = 1.60f;
    appearance.shoulderHeight = 0.24f;
    appearance.shoulderAngleDeg = 20.0f;

    const character::CharacterGeometry geometry =
        character::buildCharacterGeometry(appearance);

    expect(geometry.torso.kind == character::CharacterPrimitive::Kind::Cylinder &&
               nearlyEqual(geometry.torso.radius, Config::ENEMY_BODY_RADIUS),
           "character geometry should keep the baseline torso unchanged in visual phase one");
    expect(geometry.head.kind == character::CharacterPrimitive::Kind::Sphere &&
               nearlyEqual(geometry.head.radius, Config::ENEMY_HEAD_RADIUS),
           "character geometry should keep the baseline head unchanged in visual phase one");
    expect(geometry.leftShoulder.kind == character::CharacterPrimitive::Kind::Shoulder &&
               geometry.rightShoulder.kind == character::CharacterPrimitive::Kind::Shoulder,
           "shoulders should be explicit editable primitives, not a resized torso slice");
    expect(geometry.leftShoulder.end.x < geometry.leftShoulder.start.x &&
               geometry.rightShoulder.end.x > geometry.rightShoulder.start.x,
           "left and right shoulders should extend away from the torso independently");
    expect(nearlyEqual(geometry.leftShoulder.radius, 0.12f) &&
               nearlyEqual(geometry.rightShoulder.radius, 0.12f),
           "shoulder height should map to shoulder primitive thickness");
    expect(geometry.leftShoulder.end.y > geometry.leftShoulder.start.y &&
               geometry.rightShoulder.end.y > geometry.rightShoulder.start.y,
           "positive shoulder angle should raise both outer shoulder endpoints");
}

void testPresetStoreKeepsDefaultAndPersistsUserProfiles() {
    testsupport::ScopedTestDataRoot dataRoot("netcodesim-character-profile-tests");
    app::CharacterPresetStore store;

    const std::vector<character::CharacterProfile> initialProfiles = store.loadProfiles();
    expect(initialProfiles.size() == 1u && initialProfiles.front().builtIn,
           "preset store should always expose the built-in default profile");
    expect(!store.save(character::defaultProfile()),
           "preset store should not allow overwriting the built-in default profile");
    expect(!store.remove("default"),
           "preset store should not allow deleting the built-in default profile");

    character::CharacterProfile profile;
    profile.id = "wide-shoulders";
    profile.name = "Wide Shoulders";
    profile.appearance = character::CharacterAppearance{1.55f, 0.23f, -16.0f};
    profile.builtIn = false;

    expect(store.save(profile), "preset store should save user-created profiles");
    const std::vector<character::CharacterProfile> loadedProfiles = store.loadProfiles();
    expect(loadedProfiles.size() == 2u,
           "preset store should load the default profile plus saved user profiles");
    const character::CharacterProfile& loaded = loadedProfiles.back();
    expect(loaded.id == "wide-shoulders" &&
               loaded.name == "Wide Shoulders" &&
               nearlyEqual(loaded.appearance.shoulderWidth, 1.55f) &&
               nearlyEqual(loaded.appearance.shoulderHeight, 0.23f) &&
               nearlyEqual(loaded.appearance.shoulderAngleDeg, -16.0f),
           "preset store should round-trip editable shoulder appearance values");
    expect(store.nextAvailableProfileId("Wide Shoulders") == "wide-shoulders-2",
           "preset store should allocate stable collision-free ids for copied profiles");

    expect(store.remove("wide-shoulders"),
           "preset store should delete user-created profiles");
    expect(store.loadProfiles().size() == 1u,
           "preset store should return to only the default profile after deletion");
}

}  // namespace

int main() {
    try {
        testAppearanceNormalizationClampsEditableShoulderValues();
        testGeometryKeepsBaseBodyStableAndBuildsIndependentShoulders();
        testPresetStoreKeepsDefaultAndPersistsUserProfiles();
    } catch (const std::exception& ex) {
        std::cerr << "CharacterProfileTests failure: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "CharacterProfileTests passed\n";
    return 0;
}
