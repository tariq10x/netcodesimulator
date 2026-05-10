#pragma once

#include <cstdint>
#include <vector>

#include "replay/ReplayDemo.hpp"

namespace replay {

enum class ReplayPlaybackTrack : std::uint8_t {
    ServerTruth = 0,
    Control = 1
};

class ReplayPlaybackRuntime {
public:
    bool load(const ReplayDemo& demo, ReplayPlaybackTrack track);
    bool seekToTick(std::uint32_t serverTick);

    const sim::WorldState& worldState() const;
    const std::vector<sim::PlayerState>& controlPlayers() const;
    std::uint32_t serverTick() const;
    std::uint64_t serverTimeUs() const;

private:
    const ReplayDemo* demo_{nullptr};
    ReplayPlaybackTrack track_{ReplayPlaybackTrack::ServerTruth};
    sim::WorldState worldState_{};
    sim::SimConfig simConfig_{};
    std::vector<sim::PlayerState> controlPlayers_{};
    std::uint32_t serverTick_{0u};
    std::uint64_t serverTimeUs_{0u};
};

}  // namespace replay
