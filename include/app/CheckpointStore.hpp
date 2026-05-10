#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "SpectatorCamera.hpp"

namespace app {

enum class CheckpointCollection {
    Latency,
    ReplayStudio,
};

struct CheckpointLoadResult {
    bool loaded{false};
    bool migratedLegacy{false};
    std::vector<SpectatorCamera::Checkpoint> checkpoints;
};

class CheckpointStore {
public:
    using Checkpoint = SpectatorCamera::Checkpoint;
    using Checkpoints = std::vector<Checkpoint>;

    explicit CheckpointStore(CheckpointCollection collection);

    std::filesystem::path persistentPath() const;
    std::optional<std::filesystem::path> legacyPath() const;

    CheckpointLoadResult load() const;
    bool save(const Checkpoints& checkpoints) const;

    static int initialCheckpointIndex(const Checkpoints& checkpoints);
    static int createCheckpoint(Checkpoints& checkpoints, const Checkpoint& checkpoint);
    static int cyclePreviousCheckpoint(const Checkpoints& checkpoints, int currentIndex);
    static int cycleNextCheckpoint(const Checkpoints& checkpoints, int currentIndex);
    static int deleteCheckpoint(Checkpoints& checkpoints, int currentIndex);

private:
    static bool loadFromFile(const std::filesystem::path& path, Checkpoints& checkpoints);
    static int cycleCheckpoint(const Checkpoints& checkpoints, int currentIndex, int direction);

    std::filesystem::path persistentDirectory() const;

    CheckpointCollection collection_;
};

}  // namespace app
