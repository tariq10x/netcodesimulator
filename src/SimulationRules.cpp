#include "sim/SimulationRules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim {
namespace {

constexpr float kEpsilon = 0.0001f;

float clampf(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

Vec3 add(const Vec3& lhs, const Vec3& rhs) {
    return Vec3{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 subtract(const Vec3& lhs, const Vec3& rhs) {
    return Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 scale(const Vec3& value, float scalar) {
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

float dot(const Vec3& lhs, const Vec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

float lengthSquared(const Vec3& value) {
    return dot(value, value);
}

float length(const Vec3& value) {
    return std::sqrt(lengthSquared(value));
}

Vec3 normalize(const Vec3& value) {
    const float valueLength = length(value);
    if (valueLength <= kEpsilon) {
        return Vec3{};
    }
    return scale(value, 1.0f / valueLength);
}

bool overlapsAabb(const Vec3& position, float radius, const CollisionBox& box) {
    return (position.x + radius > box.center.x - box.halfSize.x &&
            position.x - radius < box.center.x + box.halfSize.x &&
            position.z + radius > box.center.z - box.halfSize.z &&
            position.z - radius < box.center.z + box.halfSize.z);
}

bool traceRayAgainstAabb(const Vec3& origin,
                         const Vec3& direction,
                         float maxDistance,
                         const CollisionBox& box,
                         float* distanceOut) {
    const Vec3 minBound{
        box.center.x - box.halfSize.x,
        box.center.y - box.halfSize.y,
        box.center.z - box.halfSize.z};
    const Vec3 maxBound{
        box.center.x + box.halfSize.x,
        box.center.y + box.halfSize.y,
        box.center.z + box.halfSize.z};

    float tMin = 0.0f;
    float tMax = maxDistance;
    const auto updateAxis = [&](float originCoord,
                                float dirCoord,
                                float minCoord,
                                float maxCoord) {
        if (std::fabs(dirCoord) <= kEpsilon) {
            return originCoord >= minCoord && originCoord <= maxCoord;
        }

        float axisEntry = (minCoord - originCoord) / dirCoord;
        float axisExit = (maxCoord - originCoord) / dirCoord;
        if (axisEntry > axisExit) {
            std::swap(axisEntry, axisExit);
        }

        tMin = std::max(tMin, axisEntry);
        tMax = std::min(tMax, axisExit);
        return tMin <= tMax;
    };

    if (!updateAxis(origin.x, direction.x, minBound.x, maxBound.x) ||
        !updateAxis(origin.y, direction.y, minBound.y, maxBound.y) ||
        !updateAxis(origin.z, direction.z, minBound.z, maxBound.z) ||
        tMax < 0.0f ||
        tMin > maxDistance) {
        return false;
    }

    if (distanceOut != nullptr) {
        *distanceOut = std::max(0.0f, tMin);
    }
    return true;
}

Vec3 resolveAabbCollision(const Vec3& position,
                          const Vec3& previousPosition,
                          float radius,
                          float entityHeight,
                          const CollisionBox& box) {
    const float halfEntityHeight = entityHeight * 0.5f;
    const float entityBottom = position.y - halfEntityHeight;
    const float entityTop = position.y + halfEntityHeight;
    const float boxTop = box.center.y + box.halfSize.y;
    const float boxBottom = box.center.y - box.halfSize.y;

    if (entityBottom >= boxTop - 0.05f || entityTop <= boxBottom + 0.05f) {
        return position;
    }

    if (!overlapsAabb(position, radius, box)) {
        return position;
    }

    Vec3 resolved = position;

    Vec3 testX = position;
    testX.x = previousPosition.x;
    if (!overlapsAabb(testX, radius, box)) {
        resolved.x = previousPosition.x;
    }

    Vec3 testZ = position;
    testZ.z = previousPosition.z;
    if (!overlapsAabb(testZ, radius, box)) {
        resolved.z = previousPosition.z;
    }

    if (!overlapsAabb(resolved, radius, box)) {
        return resolved;
    }

    const float distLeft = std::fabs(position.x - (box.center.x - box.halfSize.x - radius));
    const float distRight = std::fabs(position.x - (box.center.x + box.halfSize.x + radius));
    const float distFront = std::fabs(position.z - (box.center.z - box.halfSize.z - radius));
    const float distBack = std::fabs(position.z - (box.center.z + box.halfSize.z + radius));

    const float minDist = std::min(std::min(distLeft, distRight), std::min(distFront, distBack));

    if (minDist == distLeft) {
        resolved.x = box.center.x - box.halfSize.x - radius;
    } else if (minDist == distRight) {
        resolved.x = box.center.x + box.halfSize.x + radius;
    } else if (minDist == distFront) {
        resolved.z = box.center.z - box.halfSize.z - radius;
    } else {
        resolved.z = box.center.z + box.halfSize.z + radius;
    }

    return resolved;
}

HitscanTarget playerTargetFor(const PlayerState& player, const SimConfig& config) {
    HitscanTarget target;
    target.entityId = player.playerId;
    target.center = player.position;
    target.center.y = player.position.y - config.playerEyeHeight + (config.playerCollisionHeight * 0.5f);
    target.radius = std::max(config.playerRadius, config.playerCollisionHeight * 0.5f);
    target.alive = player.health > 0.0f;
    return target;
}

HitscanTarget remoteActorTargetFor(const RemoteActorState& actor) {
    HitscanTarget target;
    target.entityId = actor.entityId;
    target.center = actor.position;
    target.center.y += defaults::kEnemyBodyHeight * 0.5f;
    target.radius = actor.radius;
    target.alive = actor.alive && actor.health > 0.0f;
    return target;
}

}  // namespace

Vec3 forwardFromYaw(float yaw) {
    return Vec3{std::sin(yaw), 0.0f, -std::cos(yaw)};
}

Vec3 rightFromYaw(float yaw) {
    return Vec3{std::cos(yaw), 0.0f, std::sin(yaw)};
}

Vec3 lookDirection(float yaw, float pitch) {
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);

    return Vec3{
        sinYaw * cosPitch,
        sinPitch,
        -cosYaw * cosPitch
    };
}

HitscanRay buildRifleHitscan(const Vec3& eyePosition,
                             float yaw,
                             float pitch,
                             const SimConfig& config) {
    const Vec3 forward = normalize(lookDirection(yaw, pitch));
    const Vec3 right = rightFromYaw(yaw);
    const Vec3 up{0.0f, 1.0f, 0.0f};

    HitscanRay ray;
    ray.origin = add(eyePosition, scale(forward, defaults::kWeaponForwardOffset));
    ray.origin = add(ray.origin, scale(right, defaults::kWeaponRightOffset));
    ray.origin = subtract(ray.origin, scale(up, defaults::kWeaponDownOffset));

    Vec3 aimPoint = add(eyePosition, scale(forward, config.weaponRange));
    ray.direction = subtract(aimPoint, ray.origin);
    ray.direction.y = 0.0f;
    if (lengthSquared(ray.direction) <= kEpsilon) {
        ray.direction = Vec3{0.0f, 0.0f, -1.0f};
    } else {
        ray.direction = normalize(ray.direction);
    }
    ray.maxDistance = config.weaponRange;
    return ray;
}

HitscanRay buildRifleHitscan(const PlayerState& player,
                             float yaw,
                             float pitch,
                             const SimConfig& config) {
    return buildRifleHitscan(player.position, yaw, pitch, config);
}

HitscanTarget buildPlayerHitscanTarget(const PlayerState& player,
                                       const SimConfig& config) {
    return playerTargetFor(player, config);
}

float clampPitch(float pitch, const SimConfig& config) {
    return clampf(pitch, config.minPitch, config.maxPitch);
}

Vec3 clampToArena(const Vec3& position, float arenaHalfSize) {
    return Vec3{
        clampf(position.x, -arenaHalfSize, arenaHalfSize),
        position.y,
        clampf(position.z, -arenaHalfSize, arenaHalfSize)
    };
}

float getGroundHeightAt(const Vec3& position, const std::vector<CollisionBox>& collisionBoxes) {
    float groundHeight = 0.0f;
    for (const CollisionBox& box : collisionBoxes) {
        const bool insideFootprint =
            position.x >= box.center.x - box.halfSize.x &&
            position.x <= box.center.x + box.halfSize.x &&
            position.z >= box.center.z - box.halfSize.z &&
            position.z <= box.center.z + box.halfSize.z;

        if (!insideFootprint) {
            continue;
        }

        const float top = box.center.y + box.halfSize.y;
        if (top > groundHeight) {
            groundHeight = top;
        }
    }

    return groundHeight;
}

Vec3 resolveCollisions(const Vec3& position,
                       const Vec3& previousPosition,
                       float radius,
                       float entityHeight,
                       const std::vector<CollisionBox>& collisionBoxes) {
    Vec3 resolved = position;
    for (const CollisionBox& box : collisionBoxes) {
        resolved = resolveAabbCollision(resolved, previousPosition, radius, entityHeight, box);
    }
    return resolved;
}

bool canFire(const PlayerState& state) {
    return state.weaponCooldownRemaining <= 0.0f;
}

PlayerState applyPlayerCommand(const PlayerState& start,
                               const PlayerCommand& command,
                               const MovementEnvironment& environment,
                               const SimConfig& config) {
    PlayerState next = start;
    const float dtSeconds = std::max(command.dtSeconds, 0.0f);

    next.yaw = command.yaw;
    next.pitch = clampPitch(command.pitch, config);
    next.weaponCooldownRemaining = std::max(0.0f, start.weaponCooldownRemaining - dtSeconds);

    const float startGround = getGroundHeightAt(start.position, environment.collisionBoxes);
    if (start.position.y <= startGround + config.playerEyeHeight + 0.001f) {
        next.position.y = startGround + config.playerEyeHeight;
        next.velocity.y = std::max(start.velocity.y, 0.0f);
        next.jumpsUsed = 0;
        next.grounded = true;
    } else {
        next.grounded = false;
    }

    Vec3 moveDirection{};
    const float moveLengthSquared = command.moveX * command.moveX + command.moveY * command.moveY;
    if (moveLengthSquared > kEpsilon) {
        moveDirection = add(scale(forwardFromYaw(next.yaw), command.moveY),
                            scale(rightFromYaw(next.yaw), command.moveX));
        moveDirection = normalize(moveDirection);
    }

    next.velocity.x = moveDirection.x * config.playerMoveSpeed;
    next.velocity.z = moveDirection.z * config.playerMoveSpeed;

    const Vec3 previousPosition = next.position;
    next.position = add(next.position, scale(moveDirection, config.playerMoveSpeed * dtSeconds));

    if (command.has(CommandButton::Jump) && next.jumpsUsed < config.playerMaxJumps) {
        next.velocity.y = config.playerJumpVelocity;
        ++next.jumpsUsed;
        next.grounded = false;
    }

    next.velocity.y += config.playerGravity * dtSeconds;
    next.position.y += next.velocity.y * dtSeconds;

    next.position = clampToArena(next.position, environment.arenaHalfSize);
    next.position = resolveCollisions(next.position,
                                      previousPosition,
                                      config.playerRadius,
                                      config.playerCollisionHeight,
                                      environment.collisionBoxes);

    const float groundHeight = getGroundHeightAt(next.position, environment.collisionBoxes);
    if (next.position.y <= groundHeight + config.playerEyeHeight + 0.01f) {
        next.position.y = groundHeight + config.playerEyeHeight;
        next.velocity.y = 0.0f;
        next.jumpsUsed = 0;
        next.grounded = true;
    }

    if (command.has(CommandButton::Fire) && canFire(start)) {
        next.weaponCooldownRemaining = config.weaponCooldownSeconds;
    }

    next.health = clampf(next.health, 0.0f, next.maxHealth);
    return next;
}

std::vector<HitscanTarget> buildHitscanTargets(const WorldState& world, const SimConfig& config) {
    std::vector<HitscanTarget> targets;
    targets.reserve(world.players.size() + world.enemies.size());

    for (const auto& player : world.players) {
        targets.push_back(playerTargetFor(player, config));
    }
    for (const auto& enemy : world.enemies) {
        targets.push_back(remoteActorTargetFor(enemy));
    }

    return targets;
}

float traceHitscanObstacleDistance(const HitscanRay& ray,
                                   const std::vector<CollisionBox>& collisionBoxes) {
    const Vec3 direction = normalize(ray.direction);
    if (lengthSquared(direction) <= kEpsilon) {
        return -1.0f;
    }

    float closestDistance = std::max(0.0f, ray.maxDistance);
    bool hitObstacle = false;
    for (const CollisionBox& box : collisionBoxes) {
        float hitDistance = 0.0f;
        if (!traceRayAgainstAabb(ray.origin, direction, closestDistance, box, &hitDistance)) {
            continue;
        }

        closestDistance = hitDistance;
        hitObstacle = true;
    }

    return hitObstacle ? closestDistance : -1.0f;
}

bool shouldIgnoreFriendlyFire(const WorldState& world, int shooterActorId, int targetActorId) {
    const RosterEntry* shooterEntry = findRosterEntry(world, shooterActorId);
    const RosterEntry* targetEntry = findRosterEntry(world, targetActorId);
    if (shooterEntry == nullptr || targetEntry == nullptr) {
        return false;
    }

    return isPlayableTeam(shooterEntry->team) &&
           shooterEntry->team == targetEntry->team;
}

FireResult traceHitscan(const HitscanRay& ray, const std::vector<HitscanTarget>& targets) {
    FireResult result;
    result.fired = true;

    const Vec3 direction = normalize(ray.direction);
    if (lengthSquared(direction) <= kEpsilon) {
        return result;
    }

    float closestDistance = std::max(0.0f, ray.maxDistance);
    int closestEntityId = -1;
    Vec3 closestPoint{};

    for (const HitscanTarget& target : targets) {
        if (!target.alive || target.radius <= 0.0f) {
            continue;
        }

        const Vec3 offset = subtract(ray.origin, target.center);
        const float halfB = dot(offset, direction);
        const float c = dot(offset, offset) - target.radius * target.radius;
        const float discriminant = halfB * halfB - c;
        if (discriminant < 0.0f) {
            continue;
        }

        const float sqrtDiscriminant = std::sqrt(discriminant);
        float hitDistance = -halfB - sqrtDiscriminant;
        if (hitDistance < 0.0f) {
            hitDistance = -halfB + sqrtDiscriminant;
        }

        if (hitDistance < 0.0f || hitDistance > closestDistance) {
            continue;
        }

        closestDistance = hitDistance;
        closestEntityId = target.entityId;
        closestPoint = add(ray.origin, scale(direction, hitDistance));
    }

    if (closestEntityId >= 0) {
        result.hit = true;
        result.hitEntityId = closestEntityId;
        result.hitDistance = closestDistance;
        result.hitPoint = closestPoint;
    }

    return result;
}

}  // namespace sim
