#include "net/LagCompensation.hpp"

#include "sim/SimulationRules.hpp"

#include <algorithm>

namespace net {
namespace {

sim::Vec3 lerpVec3(const sim::Vec3& from, const sim::Vec3& to, float alpha) {
    return sim::Vec3{
        from.x + (to.x - from.x) * alpha,
        from.y + (to.y - from.y) * alpha,
        from.z + (to.z - from.z) * alpha
    };
}

float lerpFloat(float from, float to, float alpha) {
    return from + (to - from) * alpha;
}

sim::HitscanTarget interpolateTarget(const RewindSample& from,
                                     const RewindSample& to,
                                     std::uint64_t targetServerTimeUs) {
    if (to.serverTimeUs <= from.serverTimeUs) {
        return to.target;
    }

    const double alpha = static_cast<double>(targetServerTimeUs - from.serverTimeUs) /
                         static_cast<double>(to.serverTimeUs - from.serverTimeUs);
    const float t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));

    sim::HitscanTarget target = to.target;
    target.center = lerpVec3(from.target.center, to.target.center, t);
    target.radius = lerpFloat(from.target.radius, to.target.radius, t);
    target.alive = to.target.alive;
    return target;
}

sim::HitscanTarget playerTargetFor(const sim::PlayerState& player, const sim::SimConfig& config) {
    return sim::buildPlayerHitscanTarget(player, config);
}

sim::HitscanTarget remoteActorTargetFor(const sim::RemoteActorState& actor) {
    sim::HitscanTarget target;
    target.entityId = actor.entityId;
    target.center = actor.position;
    target.center.y += sim::defaults::kEnemyBodyHeight * 0.5f;
    target.radius = actor.radius;
    target.alive = actor.alive && actor.health > 0.0f;
    return target;
}

void pruneExpiredSamples(std::deque<RewindSample>& history,
                         const LagCompensationConfig& config,
                         std::uint64_t latestServerTimeUs) {
    if (history.empty()) {
        return;
    }

    if (latestServerTimeUs > config.maxRewindUs) {
        const std::uint64_t earliestAllowedUs = latestServerTimeUs - config.maxRewindUs;
        while (history.size() > 1u && history[1].serverTimeUs <= earliestAllowedUs) {
            history.pop_front();
        }
    }

    while (history.size() > config.maxSamplesPerEntity) {
        history.pop_front();
    }
}

}  // namespace

namespace server {

LagCompensationService::LagCompensationService(LagCompensationConfig config)
    : config_(config) {}

void LagCompensationService::reset() {
    histories_.clear();
    latestEntityOrder_.clear();
}

void LagCompensationService::reconfigure(LagCompensationConfig config,
                                         std::uint64_t latestServerTimeUs) {
    config_ = config;
    for (auto& [entityId, history] : histories_) {
        (void)entityId;
        pruneExpiredSamples(history, config_, latestServerTimeUs);
    }
}

void LagCompensationService::recordWorldState(const sim::WorldState& world,
                                              const sim::SimConfig& config,
                                              std::uint64_t serverTimeUs) {
    latestEntityOrder_.clear();
    latestEntityOrder_.reserve(world.players.size() + world.enemies.size());

    for (const auto& player : world.players) {
        latestEntityOrder_.push_back(player.playerId);
        auto& history = histories_[player.playerId];
        if (!history.empty() && serverTimeUs <= history.back().serverTimeUs) {
            continue;
        }
        history.push_back(RewindSample{serverTimeUs, playerTargetFor(player, config)});
        pruneExpiredSamples(history, config_, serverTimeUs);
    }

    for (const auto& enemy : world.enemies) {
        latestEntityOrder_.push_back(enemy.entityId);
        auto& history = histories_[enemy.entityId];
        if (!history.empty() && serverTimeUs <= history.back().serverTimeUs) {
            continue;
        }
        history.push_back(RewindSample{serverTimeUs, remoteActorTargetFor(enemy)});
        pruneExpiredSamples(history, config_, serverTimeUs);
    }
}

std::uint64_t LagCompensationService::clampRewindTime(std::uint64_t requestedServerTimeUs,
                                                      std::uint64_t currentServerTimeUs) const {
    if (currentServerTimeUs <= config_.maxRewindUs) {
        return std::min(requestedServerTimeUs, currentServerTimeUs);
    }

    const std::uint64_t earliestAllowedUs = currentServerTimeUs - config_.maxRewindUs;
    if (requestedServerTimeUs < earliestAllowedUs) {
        return earliestAllowedUs;
    }
    return std::min(requestedServerTimeUs, currentServerTimeUs);
}

std::vector<sim::HitscanTarget> LagCompensationService::rewindTargets(
    std::uint64_t requestedServerTimeUs,
    std::uint64_t currentServerTimeUs) const {
    const std::uint64_t targetServerTimeUs =
        clampRewindTime(requestedServerTimeUs, currentServerTimeUs);

    std::vector<sim::HitscanTarget> targets;
    targets.reserve(latestEntityOrder_.size());

    for (int entityId : latestEntityOrder_) {
        const auto historyIt = histories_.find(entityId);
        if (historyIt == histories_.end() || historyIt->second.empty()) {
            continue;
        }

        const auto& history = historyIt->second;
        if (history.size() == 1u) {
            targets.push_back(history.back().target);
            continue;
        }

        if (targetServerTimeUs <= history.front().serverTimeUs) {
            targets.push_back(history.front().target);
            continue;
        }

        if (targetServerTimeUs >= history.back().serverTimeUs) {
            targets.push_back(history.back().target);
            continue;
        }

        bool foundInterpolatedTarget = false;
        for (std::size_t index = 1; index < history.size(); ++index) {
            const auto& previous = history[index - 1];
            const auto& current = history[index];
            if (targetServerTimeUs >= previous.serverTimeUs &&
                targetServerTimeUs <= current.serverTimeUs) {
                targets.push_back(interpolateTarget(previous, current, targetServerTimeUs));
                foundInterpolatedTarget = true;
                break;
            }
        }

        if (!foundInterpolatedTarget) {
            targets.push_back(history.back().target);
        }
    }

    return targets;
}

}  // namespace server

LagCompensationHistory::LagCompensationHistory(LagCompensationConfig config)
    : service_(config) {}

void LagCompensationHistory::reset() {
    service_.reset();
}

void LagCompensationHistory::reconfigure(LagCompensationConfig config,
                                         std::uint64_t latestServerTimeUs) {
    service_.reconfigure(config, latestServerTimeUs);
}

void LagCompensationHistory::recordWorldState(const sim::WorldState& world,
                                              const sim::SimConfig& config,
                                              std::uint64_t serverTimeUs) {
    service_.recordWorldState(world, config, serverTimeUs);
}

std::uint64_t LagCompensationHistory::clampRewindTime(std::uint64_t requestedServerTimeUs,
                                                      std::uint64_t currentServerTimeUs) const {
    return service_.clampRewindTime(requestedServerTimeUs, currentServerTimeUs);
}

std::vector<sim::HitscanTarget> LagCompensationHistory::rewindTargets(
    std::uint64_t requestedServerTimeUs,
    std::uint64_t currentServerTimeUs) const {
    return service_.rewindTargets(requestedServerTimeUs, currentServerTimeUs);
}

}  // namespace net
