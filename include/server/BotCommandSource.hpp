#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "sim/WorldState.hpp"

namespace net {
namespace server {

class BotCommandSource {
public:
    struct BotAimPlan {
        bool fire{false};
        float yawOffsetRadians{0.0f};
        float pitchOffsetRadians{0.0f};
    };

    struct BotMovementPlan {
        float preferredDistance{10.0f};
        float retreatDistance{0.0f};
        float lateralMove{0.0f};
        float forwardBias{0.0f};
    };

    struct BotCommandContext {
        float dtSeconds{0.0f};
        BotAimPlan aim{};
        BotMovementPlan movement{};
    };

    sim::PlayerCommand buildCommand(const sim::WorldState& world,
                                    int actorId,
                                    sim::TeamId team,
                                    float dtSeconds) const {
        BotCommandContext context;
        context.dtSeconds = dtSeconds;
        context.aim.fire = true;
        context.movement.preferredDistance = kLegacyAdvanceDistance;
        return buildCommand(world, actorId, team, context);
    }

    sim::PlayerCommand buildCommand(const sim::WorldState& world,
                                    int actorId,
                                    sim::TeamId team,
                                    const BotCommandContext& context) const {
        const sim::PlayerState* self = sim::findPlayer(world, actorId);
        if (self == nullptr) {
            return idleCommand(context.dtSeconds, 0.0f, 0.0f);
        }

        const sim::PlayerState* target = chooseNearestOpposingPlayer(world, actorId, team);
        if (target == nullptr) {
            sim::PlayerCommand command = idleCommand(context.dtSeconds, self->yaw, self->pitch);
            command.moveX = std::clamp(context.movement.lateralMove, -1.0f, 1.0f);
            command.moveY = std::clamp(context.movement.forwardBias, -1.0f, 1.0f);
            return command;
        }

        const float dx = target->position.x - self->position.x;
        const float dz = target->position.z - self->position.z;
        const float planarDistance = std::sqrt(std::max((dx * dx) + (dz * dz), 0.0f));
        const float yaw = std::atan2(dx, -dz);
        const float pitch = planarDistance > kMinimumPlanarDistance
            ? std::atan2(target->position.y - self->position.y, planarDistance)
            : 0.0f;

        sim::PlayerCommand command = idleCommand(context.dtSeconds,
                                                 yaw + context.aim.yawOffsetRadians,
                                                 pitch + context.aim.pitchOffsetRadians);
        if (planarDistance > context.movement.preferredDistance) {
            command.moveY = 1.0f;
        } else if (planarDistance < context.movement.retreatDistance) {
            command.moveY = -0.55f;
        } else {
            command.moveY = std::clamp(context.movement.forwardBias, -1.0f, 1.0f);
        }
        command.moveX = std::clamp(context.movement.lateralMove, -1.0f, 1.0f);
        if (context.aim.fire) {
            command.buttons |= sim::commandButtonBit(sim::CommandButton::Fire);
        }
        return command;
    }

private:
    static constexpr float kLegacyAdvanceDistance = 10.0f;
    static constexpr float kMinimumPlanarDistance = 0.0001f;

    static sim::PlayerCommand idleCommand(float dtSeconds, float yaw, float pitch) {
        sim::PlayerCommand command;
        command.dtSeconds = dtSeconds;
        command.yaw = yaw;
        command.pitch = pitch;
        return command;
    }

    static float planarDistanceSquared(const sim::Vec3& lhs, const sim::Vec3& rhs) {
        const float dx = lhs.x - rhs.x;
        const float dz = lhs.z - rhs.z;
        return dx * dx + dz * dz;
    }

    static const sim::PlayerState* chooseNearestOpposingPlayer(const sim::WorldState& world,
                                                               int actorId,
                                                               sim::TeamId team) {
        const sim::PlayerState* self = sim::findPlayer(world, actorId);
        if (self == nullptr) {
            return nullptr;
        }

        const sim::PlayerState* bestTarget = nullptr;
        float bestDistanceSquared = std::numeric_limits<float>::max();

        for (const auto& candidate : world.players) {
            if (candidate.playerId == actorId || candidate.health <= 0.0f) {
                continue;
            }

            const sim::RosterEntry* candidateEntry = sim::findRosterEntry(world, candidate.playerId);
            if (candidateEntry == nullptr ||
                candidateEntry->team == sim::TeamId::None ||
                candidateEntry->team == team) {
                continue;
            }

            const float distanceSquared = planarDistanceSquared(self->position, candidate.position);
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestTarget = &candidate;
            }
        }

        return bestTarget;
    }
};

}  // namespace server
}  // namespace net
