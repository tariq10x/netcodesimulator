#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "sim/SimulationTypes.hpp"

namespace net {

struct InterpolationConfig {
    std::size_t maxSamplesPerEntity{32u};
    float teleportDistance{25.0f};
};

struct InterpolationSample {
    std::uint64_t serverTimeUs{0u};
    sim::RemoteActorState state{};
};

class InterpolationBuffer {
public:
    explicit InterpolationBuffer(InterpolationConfig config = {});

    void reset();
    void pushSnapshot(std::uint64_t serverTimeUs, const std::vector<sim::RemoteActorState>& entities);
    std::vector<sim::RemoteActorState> sample(std::uint64_t targetServerTimeUs) const;

private:
    InterpolationConfig config_{};
    std::unordered_map<int, std::deque<InterpolationSample>> histories_{};
    std::vector<int> latestEntityOrder_{};
};

}  // namespace net
