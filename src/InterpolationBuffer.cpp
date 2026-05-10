#include "net/InterpolationBuffer.hpp"

#include <algorithm>
#include <cmath>

namespace net {
namespace {

float distance(const sim::Vec3& lhs, const sim::Vec3& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    const float dz = lhs.z - rhs.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

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

sim::RemoteActorState interpolateState(const InterpolationSample& from,
                                       const InterpolationSample& to,
                                       std::uint64_t targetServerTimeUs) {
    if (to.serverTimeUs <= from.serverTimeUs) {
        return to.state;
    }

    const double alpha = static_cast<double>(targetServerTimeUs - from.serverTimeUs) /
                         static_cast<double>(to.serverTimeUs - from.serverTimeUs);
    const float t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));

    sim::RemoteActorState state = to.state;
    state.position = lerpVec3(from.state.position, to.state.position, t);
    state.velocity = lerpVec3(from.state.velocity, to.state.velocity, t);
    state.yaw = lerpFloat(from.state.yaw, to.state.yaw, t);
    state.pitch = lerpFloat(from.state.pitch, to.state.pitch, t);
    state.health = lerpFloat(from.state.health, to.state.health, t);
    state.radius = lerpFloat(from.state.radius, to.state.radius, t);
    state.alive = to.state.alive;
    return state;
}

}  // namespace

InterpolationBuffer::InterpolationBuffer(InterpolationConfig config)
    : config_(config) {}

void InterpolationBuffer::reset() {
    histories_.clear();
    latestEntityOrder_.clear();
}

void InterpolationBuffer::pushSnapshot(std::uint64_t serverTimeUs,
                                       const std::vector<sim::RemoteActorState>& entities) {
    latestEntityOrder_.clear();
    latestEntityOrder_.reserve(entities.size());

    for (const auto& entity : entities) {
        latestEntityOrder_.push_back(entity.entityId);
        auto& history = histories_[entity.entityId];

        if (!history.empty()) {
            const auto& latest = history.back();
            if (serverTimeUs <= latest.serverTimeUs) {
                continue;
            }
            if (distance(latest.state.position, entity.position) > config_.teleportDistance) {
                history.clear();
            }
        }

        history.push_back(InterpolationSample{serverTimeUs, entity});
        while (history.size() > config_.maxSamplesPerEntity) {
            history.pop_front();
        }
    }
}

std::vector<sim::RemoteActorState> InterpolationBuffer::sample(std::uint64_t targetServerTimeUs) const {
    std::vector<sim::RemoteActorState> entities;
    entities.reserve(latestEntityOrder_.size());

    for (int entityId : latestEntityOrder_) {
        const auto historyIt = histories_.find(entityId);
        if (historyIt == histories_.end() || historyIt->second.empty()) {
            continue;
        }

        const auto& history = historyIt->second;
        if (history.size() == 1u) {
            entities.push_back(history.back().state);
            continue;
        }

        if (targetServerTimeUs <= history.front().serverTimeUs ||
            targetServerTimeUs >= history.back().serverTimeUs) {
            entities.push_back(history.back().state);
            continue;
        }

        bool addedInterpolatedState = false;
        for (std::size_t index = 1; index < history.size(); ++index) {
            const auto& previous = history[index - 1];
            const auto& current = history[index];
            if (targetServerTimeUs >= previous.serverTimeUs &&
                targetServerTimeUs <= current.serverTimeUs) {
                entities.push_back(interpolateState(previous, current, targetServerTimeUs));
                addedInterpolatedState = true;
                break;
            }
        }

        if (!addedInterpolatedState) {
            entities.push_back(history.back().state);
        }
    }

    return entities;
}

}  // namespace net
