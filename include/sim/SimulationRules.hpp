#pragma once

#include <vector>

#include "sim/SimulationTypes.hpp"
#include "sim/WorldState.hpp"

namespace sim {

Vec3 forwardFromYaw(float yaw);
Vec3 rightFromYaw(float yaw);
Vec3 lookDirection(float yaw, float pitch);
HitscanRay buildRifleHitscan(const Vec3& eyePosition,
                             float yaw,
                             float pitch,
                             const SimConfig& config = {});
HitscanRay buildRifleHitscan(const PlayerState& player,
                             float yaw,
                             float pitch,
                             const SimConfig& config = {});
HitscanTarget buildPlayerHitscanTarget(const PlayerState& player,
                                       const SimConfig& config = {});
float clampPitch(float pitch, const SimConfig& config);

Vec3 clampToArena(const Vec3& position, float arenaHalfSize);
float getGroundHeightAt(const Vec3& position, const std::vector<CollisionBox>& collisionBoxes);
Vec3 resolveCollisions(const Vec3& position,
                       const Vec3& previousPosition,
                       float radius,
                       float entityHeight,
                       const std::vector<CollisionBox>& collisionBoxes);

bool canFire(const PlayerState& state);
PlayerState applyPlayerCommand(const PlayerState& start,
                               const PlayerCommand& command,
                               const MovementEnvironment& environment,
                               const SimConfig& config = {});
std::vector<HitscanTarget> buildHitscanTargets(const WorldState& world,
                                               const SimConfig& config = {});
float traceHitscanObstacleDistance(const HitscanRay& ray,
                                   const std::vector<CollisionBox>& collisionBoxes);
bool shouldIgnoreFriendlyFire(const WorldState& world, int shooterActorId, int targetActorId);
FireResult traceHitscan(const HitscanRay& ray, const std::vector<HitscanTarget>& targets);

}  // namespace sim
