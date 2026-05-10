#pragma once

#include "server/LagCompensationService.hpp"

namespace net {

class LagCompensationHistory {
public:
    explicit LagCompensationHistory(LagCompensationConfig config = {});

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
    server::LagCompensationService service_{};
};

}  // namespace net
