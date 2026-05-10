#include "app/CheckpointStore.hpp"

#include <cstdlib>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
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

void expectNear(float actual, float expected, float tolerance, const std::string& message) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

void expectVectorNear(Vector3 actual, Vector3 expected, float tolerance, const std::string& context) {
    expectNear(actual.x, expected.x, tolerance, context + " (x)");
    expectNear(actual.y, expected.y, tolerance, context + " (y)");
    expectNear(actual.z, expected.z, tolerance, context + " (z)");
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
#ifdef _WIN32
        char* existing = nullptr;
        std::size_t existingLength = 0u;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
            std::free(existing);
        }
#else
        const char* existing = std::getenv(name_.c_str());
        if (existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
        }
#endif
        set(value);
    }

    ~ScopedEnvVar() {
        if (hadOriginal_) {
            set(originalValue_);
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    bool hadOriginal_{false};
    std::string originalValue_{};
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(original_);
    }

private:
    std::filesystem::path original_;
};

void writeLegacyCheckpointFile(const std::filesystem::path& path,
                               const std::vector<SpectatorCamera::Checkpoint>& checkpoints) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    expect(file.is_open(), "expected legacy checkpoint fixture file to open for writing");
    for (const auto& checkpoint : checkpoints) {
        file << checkpoint.position.x << ' '
             << checkpoint.position.y << ' '
             << checkpoint.position.z << ' '
             << checkpoint.yaw << ' '
             << checkpoint.pitch << '\n';
    }
}

void expectCheckpoint(const SpectatorCamera::Checkpoint& checkpoint,
                      float x,
                      float y,
                      float z,
                      float yaw,
                      float pitch,
                      float transitionDurationSeconds,
                      const std::string& context) {
    expect(checkpoint.position.x == x &&
               checkpoint.position.y == y &&
               checkpoint.position.z == z &&
               checkpoint.yaw == yaw &&
               checkpoint.pitch == pitch &&
               checkpoint.transitionDurationSeconds == transitionDurationSeconds,
           context);
}

void testModeSpecificPathsRemainStable() {
    const app::CheckpointStore latencyStore(app::CheckpointCollection::Latency);
    const app::CheckpointStore replayStore(app::CheckpointCollection::ReplayStudio);

    expect(latencyStore.persistentPath().filename() == "checkpoints.json",
           "latency mode should persist checkpoints to checkpoints.json");
    expect(replayStore.persistentPath().filename() == "checkpoints_replay.json",
           "replay studio should persist checkpoints to its own checkpoint file");
    expect(latencyStore.legacyPath().has_value(),
           "latency mode should keep the legacy assets/checkpoints.json migration source");
    expect(latencyStore.legacyPath()->generic_string() == "assets/checkpoints.json",
           "latency mode should keep the legacy assets/checkpoints.json path stable");
    expect(!replayStore.legacyPath().has_value(),
           "replay studio should not read checkpoints from the latency-mode legacy asset path");
}

void testDefaultCheckpointTransitionRemainsHalfSecond() {
    expectNear(SpectatorCamera::kDefaultCheckpointTransitionSeconds,
               0.5f,
               0.0001f,
               "new checkpoints should default to a 0.5 second transition");

    const SpectatorCamera::Checkpoint checkpoint{};
    expectNear(checkpoint.transitionDurationSeconds,
               0.5f,
               0.0001f,
               "fresh spectator checkpoints should initialize with a 0.5 second transition");
}

void testLatencyModeFallsBackToLegacyAssetAndMarksMigration() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-checkpoint-store-legacy";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedHome("HOME", (tempRoot / "home").string());
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    SpectatorCamera::Checkpoint checkpoint{};
    checkpoint.position = Vector3{1.0f, 2.0f, 3.0f};
    checkpoint.yaw = 0.4f;
    checkpoint.pitch = -0.5f;
    checkpoint.transitionDurationSeconds = 1.5f;
    writeLegacyCheckpointFile(tempRoot / "repo" / "assets" / "checkpoints.json", {checkpoint});

    const app::CheckpointStore latencyStore(app::CheckpointCollection::Latency);
    const app::CheckpointLoadResult loaded = latencyStore.load();

    expect(loaded.loaded, "latency mode should load checkpoints from the legacy asset when no persistent save exists");
    expect(loaded.migratedLegacy, "latency mode should flag legacy asset loads so callers can migrate them");
    expect(loaded.checkpoints.size() == 1u, "latency mode should load the full legacy checkpoint list");
    expectCheckpoint(loaded.checkpoints.front(),
                     1.0f,
                     2.0f,
                     3.0f,
                     0.4f,
                     -0.5f,
                     SpectatorCamera::kDefaultCheckpointTransitionSeconds,
                     "latency mode should preserve legacy checkpoint payload values and default transition duration");

    std::filesystem::current_path(tempRoot.parent_path());
    std::filesystem::remove_all(tempRoot);
}

void testPersistentRoundTripUsesModeSpecificFiles() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-checkpoint-store-roundtrip";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    SpectatorCamera::Checkpoint latencyCheckpoint{};
    latencyCheckpoint.position = Vector3{4.0f, 5.0f, 6.0f};
    latencyCheckpoint.yaw = 0.7f;
    latencyCheckpoint.pitch = 0.1f;
    latencyCheckpoint.transitionDurationSeconds = 1.85f;

    SpectatorCamera::Checkpoint replayCheckpoint{};
    replayCheckpoint.position = Vector3{-4.0f, 1.5f, 8.0f};
    replayCheckpoint.yaw = -0.3f;
    replayCheckpoint.pitch = 0.9f;
    replayCheckpoint.transitionDurationSeconds = 0.85f;

    const app::CheckpointStore latencyStore(app::CheckpointCollection::Latency);
    const app::CheckpointStore replayStore(app::CheckpointCollection::ReplayStudio);

    expect(latencyStore.save({latencyCheckpoint}),
           "latency mode should write checkpoints through the shared store");
    expect(replayStore.save({replayCheckpoint}),
           "replay studio should write checkpoints through the shared store");
    expect(std::filesystem::exists(latencyStore.persistentPath()),
           "latency mode should write its persistent checkpoint file");
    expect(std::filesystem::exists(replayStore.persistentPath()),
           "replay studio should write its persistent checkpoint file");

    const app::CheckpointLoadResult latencyLoaded = latencyStore.load();
    const app::CheckpointLoadResult replayLoaded = replayStore.load();

    expect(latencyLoaded.loaded && !latencyLoaded.migratedLegacy,
           "latency mode should load directly from the persistent checkpoint file after save");
    expect(replayLoaded.loaded && !replayLoaded.migratedLegacy,
           "replay studio should load directly from the persistent checkpoint file after save");
    expectCheckpoint(latencyLoaded.checkpoints.front(), 4.0f, 5.0f, 6.0f, 0.7f, 0.1f, 1.85f,
                     "latency mode should round-trip checkpoint values through the shared store");
    expectCheckpoint(replayLoaded.checkpoints.front(), -4.0f, 1.5f, 8.0f, -0.3f, 0.9f, 0.85f,
                     "replay studio should round-trip checkpoint values through the shared store");

    std::filesystem::current_path(tempRoot.parent_path());
    std::filesystem::remove_all(tempRoot);
}

void testCheckpointHelpersRemainDeterministicAcrossRestarts() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-checkpoint-store-helpers";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    app::CheckpointStore::Checkpoints checkpoints;
    expect(app::CheckpointStore::initialCheckpointIndex(checkpoints) == -1,
           "empty checkpoint collections should not expose an active index");

    SpectatorCamera::Checkpoint checkpointA{};
    checkpointA.position = Vector3{1.0f, 2.0f, 3.0f};
    checkpointA.yaw = 0.25f;
    checkpointA.pitch = -0.1f;
    checkpointA.transitionDurationSeconds = 0.5f;

    SpectatorCamera::Checkpoint checkpointB{};
    checkpointB.position = Vector3{4.0f, 5.0f, 6.0f};
    checkpointB.yaw = 0.5f;
    checkpointB.pitch = -0.2f;
    checkpointB.transitionDurationSeconds = 1.0f;

    SpectatorCamera::Checkpoint checkpointC{};
    checkpointC.position = Vector3{7.0f, 8.0f, 9.0f};
    checkpointC.yaw = 0.75f;
    checkpointC.pitch = -0.3f;
    checkpointC.transitionDurationSeconds = 1.5f;

    int activeIndex = app::CheckpointStore::createCheckpoint(checkpoints, checkpointA);
    expect(activeIndex == 0, "creating the first checkpoint should activate index 0");
    activeIndex = app::CheckpointStore::createCheckpoint(checkpoints, checkpointB);
    expect(activeIndex == 1, "creating a checkpoint should activate the new tail entry");
    activeIndex = app::CheckpointStore::createCheckpoint(checkpoints, checkpointC);
    expect(activeIndex == 2, "creating multiple checkpoints should keep the newest entry active");

    const app::CheckpointStore store(app::CheckpointCollection::Latency);
    expect(store.save(checkpoints),
           "checkpoint helper test should persist the shared checkpoint list before restart");

    const app::CheckpointLoadResult initialReload = store.load();
    expect(initialReload.loaded && initialReload.checkpoints.size() == 3u,
           "checkpoint helper test should reload the full persisted checkpoint list");

    activeIndex = app::CheckpointStore::initialCheckpointIndex(initialReload.checkpoints);
    expect(activeIndex == 0,
           "reloaded checkpoint collections should deterministically begin from the first entry");
    activeIndex = app::CheckpointStore::cyclePreviousCheckpoint(initialReload.checkpoints, activeIndex);
    expect(activeIndex == 2,
           "cycling backward from the first checkpoint should wrap to the last entry");
    activeIndex = app::CheckpointStore::cycleNextCheckpoint(initialReload.checkpoints, activeIndex);
    expect(activeIndex == 0,
           "cycling forward after a wrapped previous selection should return to the first entry");
    activeIndex = app::CheckpointStore::cycleNextCheckpoint(initialReload.checkpoints, activeIndex);
    expect(activeIndex == 1,
           "cycling forward from the first checkpoint should advance to the next entry");

    app::CheckpointStore::Checkpoints trimmedCheckpoints = initialReload.checkpoints;
    activeIndex = app::CheckpointStore::deleteCheckpoint(trimmedCheckpoints, activeIndex);
    expect(activeIndex == 1,
           "deleting the active middle checkpoint should keep the replacement entry selected");
    expect(trimmedCheckpoints.size() == 2u,
           "deleting a checkpoint should shrink the shared collection");
    expectCheckpoint(trimmedCheckpoints.front(), 1.0f, 2.0f, 3.0f, 0.25f, -0.1f, 0.5f,
                     "checkpoint helper deletion should preserve the first entry");
    expectCheckpoint(trimmedCheckpoints.back(), 7.0f, 8.0f, 9.0f, 0.75f, -0.3f, 1.5f,
                     "checkpoint helper deletion should preserve the surviving tail entry order");
    expect(store.save(trimmedCheckpoints),
           "checkpoint helper test should persist the edited checkpoint list after deletion");

    const app::CheckpointLoadResult secondReload = store.load();
    expect(secondReload.loaded && secondReload.checkpoints.size() == 2u,
           "checkpoint helper test should reload the edited checkpoint list after deletion");
    expectCheckpoint(secondReload.checkpoints.front(), 1.0f, 2.0f, 3.0f, 0.25f, -0.1f, 0.5f,
                     "checkpoint helper persistence should keep the first surviving checkpoint stable");
    expectCheckpoint(secondReload.checkpoints.back(), 7.0f, 8.0f, 9.0f, 0.75f, -0.3f, 1.5f,
                     "checkpoint helper persistence should keep the second surviving checkpoint stable");

    std::filesystem::current_path(tempRoot.parent_path());
    std::filesystem::remove_all(tempRoot);
}

void testSpectatorCameraHelpersPreserveReusableTransforms() {
    const SpectatorCamera::Checkpoint checkpoint{
        Vector3{3.0f, 4.0f, 5.0f},
        0.6f,
        -0.35f,
        1.25f
    };

    const Camera3D freeFly = SpectatorCamera::freeFlyCamera(checkpoint);
    const SpectatorCamera::Checkpoint reconstructed = SpectatorCamera::checkpointFromCamera(freeFly);
    expectVectorNear(reconstructed.position, checkpoint.position, 0.0001f,
                     "free-fly camera helpers should preserve checkpoint positions");
    expectNear(reconstructed.yaw, checkpoint.yaw, 0.0001f,
               "free-fly camera helpers should preserve yaw");
    expectNear(reconstructed.pitch, checkpoint.pitch, 0.0001f,
               "free-fly camera helpers should preserve pitch");
    const Vector3 eyePosition{10.0f, 2.5f, -3.0f};
    const Camera3D firstPerson = SpectatorCamera::followFirstPersonCamera(eyePosition, 0.4f, -0.2f);
    const Vector3 expectedLookDirection = Math3D::lookDirection(0.4f, -0.2f);
    expectVectorNear(firstPerson.position, eyePosition, 0.0001f,
                     "first-person follow helpers should anchor the camera at the target eye position");
    expectVectorNear(firstPerson.target,
                     Vector3Add(eyePosition, expectedLookDirection),
                     0.0001f,
                     "first-person follow helpers should preserve the target look direction");

    const Vector3 targetPosition{-2.0f, 0.5f, 8.0f};
    const float zoom = 6.0f;
    const Camera3D thirdPerson =
        SpectatorCamera::followThirdPersonCamera(targetPosition, 0.4f, -0.2f, zoom);
    const Vector3 expectedFocus = Vector3Add(targetPosition, Vector3{0.0f, Config::PLAYER_EYE_HEIGHT, 0.0f});
    expectVectorNear(thirdPerson.target, expectedFocus, 0.0001f,
                     "third-person follow helpers should look back at the target eye position");
    expectNear(Vector3Distance(thirdPerson.position, expectedFocus), zoom, 0.0001f,
               "third-person follow helpers should preserve the requested follow distance");
    expectVectorNear(thirdPerson.position,
                     Vector3Subtract(expectedFocus, Vector3Scale(expectedLookDirection, zoom)),
                     0.0001f,
                     "third-person follow helpers should preserve the reusable follow offset math");
}

}  // namespace

int main() {
    try {
        testModeSpecificPathsRemainStable();
        testDefaultCheckpointTransitionRemainsHalfSecond();
        testLatencyModeFallsBackToLegacyAssetAndMarksMigration();
        testPersistentRoundTripUsesModeSpecificFiles();
        testCheckpointHelpersRemainDeterministicAcrossRestarts();
        testSpectatorCameraHelpersPreserveReusableTransforms();
        std::cout << "CheckpointStoreTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "CheckpointStoreTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
