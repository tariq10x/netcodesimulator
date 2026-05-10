#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "sim/WorldState.hpp"

namespace net {

struct LagCompensationConfig {
    std::uint64_t maxRewindUs{500'000u};
    std::size_t maxSamplesPerEntity{256u};
};

struct RewindSample {
    std::uint64_t serverTimeUs{0u};
    sim::HitscanTarget target{};
};

namespace server {

class LagCompensationService {
public:
    explicit LagCompensationService(LagCompensationConfig config = {});

    void reset();
    void reconfigure(LagCompensationConfig config, std::uint64_t latestServerTimeUs);
    void recordWorldState(const sim::WorldState& world,
                          const sim::SimConfig& config,
                          std::uint64_t serverTimeUs);
    std::uint64_t clampRewindTime(std::uint64_t requestedServerTimeUs,
                                  std::uint64_t currentServerTimeUs) const;
    std::vector<sim::HitscanTarget> rewindTargets(std::uint64_t requestedServerTimeUs,
                                                  std::uint64_t currentServerTimeUs) const;

private:
    LagCompensationConfig config_{};
    std::unordered_map<int, std::deque<RewindSample>> histories_{};
    std::vector<int> latestEntityOrder_{};
};

}  // namespace server
}  // namespace net
