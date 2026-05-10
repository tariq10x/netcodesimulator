#include "app/CheckpointStore.hpp"

#include "app/UserDataPaths.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace app {

CheckpointStore::CheckpointStore(CheckpointCollection collection)
    : collection_(collection) {}

int CheckpointStore::initialCheckpointIndex(const Checkpoints& checkpoints) {
    return checkpoints.empty() ? -1 : 0;
}

int CheckpointStore::createCheckpoint(Checkpoints& checkpoints, const Checkpoint& checkpoint) {
    checkpoints.push_back(checkpoint);
    return static_cast<int>(checkpoints.size()) - 1;
}

int CheckpointStore::cyclePreviousCheckpoint(const Checkpoints& checkpoints, int currentIndex) {
    return cycleCheckpoint(checkpoints, currentIndex, -1);
}

int CheckpointStore::cycleNextCheckpoint(const Checkpoints& checkpoints, int currentIndex) {
    return cycleCheckpoint(checkpoints, currentIndex, 1);
}

int CheckpointStore::deleteCheckpoint(Checkpoints& checkpoints, int currentIndex) {
    if (checkpoints.empty()) {
        return -1;
    }
    if (currentIndex < 0 || currentIndex >= static_cast<int>(checkpoints.size())) {
        return currentIndex;
    }

    checkpoints.erase(checkpoints.begin() + currentIndex);
    if (checkpoints.empty()) {
        return -1;
    }
    return currentIndex % static_cast<int>(checkpoints.size());
}

std::filesystem::path CheckpointStore::persistentDirectory() const {
    return userDataDirectory();
}

std::filesystem::path CheckpointStore::persistentPath() const {
    const char* fileName = nullptr;
    switch (collection_) {
        case CheckpointCollection::Latency:
            fileName = "checkpoints.json";
            break;
        case CheckpointCollection::ReplayStudio:
            fileName = "checkpoints_replay.json";
            break;
    }
    return persistentDirectory() / fileName;
}

std::optional<std::filesystem::path> CheckpointStore::legacyPath() const {
    if (collection_ == CheckpointCollection::Latency) {
        return std::filesystem::path("assets") / "checkpoints.json";
    }
    return std::nullopt;
}

CheckpointLoadResult CheckpointStore::load() const {
    CheckpointLoadResult result;
    const std::filesystem::path currentPath = persistentPath();
    result.loaded = loadFromFile(currentPath, result.checkpoints);
    if (result.loaded) {
        return result;
    }

    const std::optional<std::filesystem::path> fallback = legacyPath();
    if (fallback.has_value() && *fallback != currentPath) {
        result.loaded = loadFromFile(*fallback, result.checkpoints);
        result.migratedLegacy = result.loaded;
    }

    return result;
}

bool CheckpointStore::save(const Checkpoints& checkpoints) const {
    const std::filesystem::path path = persistentPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    for (const auto& checkpoint : checkpoints) {
        file << checkpoint.position.x << ' '
             << checkpoint.position.y << ' '
             << checkpoint.position.z << ' '
             << checkpoint.yaw << ' '
             << checkpoint.pitch << ' '
             << checkpoint.transitionDurationSeconds << '\n';
    }
    return true;
}

int CheckpointStore::cycleCheckpoint(const Checkpoints& checkpoints, int currentIndex, int direction) {
    if (checkpoints.empty()) {
        return -1;
    }

    int baseIndex = currentIndex;
    if (baseIndex < 0 || baseIndex >= static_cast<int>(checkpoints.size())) {
        baseIndex = 0;
    }

    const int size = static_cast<int>(checkpoints.size());
    return (baseIndex + direction + size) % size;
}

bool CheckpointStore::loadFromFile(const std::filesystem::path& path,
                                   Checkpoints& checkpoints) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream lineStream(line);
        SpectatorCamera::Checkpoint checkpoint{};
        if (!(lineStream >> checkpoint.position.x
                         >> checkpoint.position.y
                         >> checkpoint.position.z
                         >> checkpoint.yaw
                         >> checkpoint.pitch)) {
            continue;
        }
        if (!(lineStream >> checkpoint.transitionDurationSeconds)) {
            checkpoint.transitionDurationSeconds =
                SpectatorCamera::kDefaultCheckpointTransitionSeconds;
        }
        checkpoints.push_back(checkpoint);
    }
    return !checkpoints.empty();
}

}  // namespace app
