#include "sim/WorldState.hpp"
#include "sim/SimulationRules.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim {
namespace {

Vec3 add(const Vec3& lhs, const Vec3& rhs) {
    return Vec3{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 subtract(const Vec3& lhs, const Vec3& rhs) {
    return Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 scale(const Vec3& value, float scalar) {
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

float lengthSquared(const Vec3& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

float length(const Vec3& value) {
    return std::sqrt(lengthSquared(value));
}

Vec3 normalize(const Vec3& value) {
    const float valueLength = length(value);
    if (valueLength <= 0.0001f) {
        return Vec3{};
    }
    return scale(value, 1.0f / valueLength);
}

std::array<Vec3, 4> patrolWaypoints(float arenaHalfSize) {
    const float inset = std::max(5.0f, arenaHalfSize - 5.0f);
    return {Vec3{-inset, 0.0f, -inset},
            Vec3{ inset, 0.0f, -inset},
            Vec3{ inset, 0.0f,  inset},
            Vec3{-inset, 0.0f,  inset}};
}

void normalizeEnemyAuxiliaryState(WorldState* world) {
    const std::size_t enemyCount = world->enemies.size();

    while (world->enemySpawns.size() < enemyCount) {
        const std::size_t spawnIndex = world->enemySpawns.size();
        world->enemySpawns.push_back(world->enemies[spawnIndex].position);
    }
    world->enemySpawns.resize(enemyCount);
    world->enemyWaypointIndices.resize(enemyCount, 0u);
    world->enemyRespawnTimers.resize(enemyCount, 0.0f);
}

}  // namespace

WorldState createDefaultWorld(std::size_t maxPlayers,
                              const MovementEnvironment& environment,
                              const SimConfig& config) {
    WorldState world;
    world.environment = environment;
    world.environment.arenaHalfSize = environment.arenaHalfSize > 0.0f
        ? environment.arenaHalfSize
        : config.arenaHalfSize;

    world.playerSpawns.reserve(maxPlayers);
    for (std::size_t index = 0; index < maxPlayers; ++index) {
        world.playerSpawns.push_back(
            Vec3{static_cast<float>(index) * 2.0f, config.playerEyeHeight, 5.0f});
    }

    world.enemySpawns.push_back(Vec3{0.0f, 0.0f, -10.0f});
    world.enemyWaypointIndices.push_back(0);
    world.enemyRespawnTimers.push_back(0.0f);

    RemoteActorState enemy;
    enemy.entityId = 100;
    enemy.position = world.enemySpawns.front();
    enemy.velocity = Vec3{};
    enemy.yaw = 0.0f;
    enemy.pitch = 0.0f;
    enemy.health = config.enemyMaxHealth;
    enemy.radius = config.enemyRadius;
    enemy.alive = true;
    world.enemies.push_back(enemy);

    return world;
}

PlayerState* findPlayer(WorldState* world, int playerId) {
    if (world == nullptr) {
        return nullptr;
    }

    for (auto& player : world->players) {
        if (player.playerId == playerId) {
            return &player;
        }
    }
    return nullptr;
}

const PlayerState* findPlayer(const WorldState& world, int playerId) {
    for (const auto& player : world.players) {
        if (player.playerId == playerId) {
            return &player;
        }
    }
    return nullptr;
}

RosterEntry* findRosterEntry(WorldState* world, int actorId) {
    if (world == nullptr) {
        return nullptr;
    }

    for (auto& entry : world->roster) {
        if (entry.actorId == actorId) {
            return &entry;
        }
    }
    return nullptr;
}

const RosterEntry* findRosterEntry(const WorldState& world, int actorId) {
    for (const auto& entry : world.roster) {
        if (entry.actorId == actorId) {
            return &entry;
        }
    }
    return nullptr;
}

std::uint16_t* findTeamScore(WorldState* world, TeamId team) {
    if (world == nullptr) {
        return nullptr;
    }

    switch (team) {
        case TeamId::Attacker:
            return &world->teamScores.attackers;
        case TeamId::Defender:
            return &world->teamScores.defenders;
        case TeamId::Spectator:
        case TeamId::None:
            return nullptr;
    }

    return nullptr;
}

const std::uint16_t* findTeamScore(const WorldState& world, TeamId team) {
    switch (team) {
        case TeamId::Attacker:
            return &world.teamScores.attackers;
        case TeamId::Defender:
            return &world.teamScores.defenders;
        case TeamId::Spectator:
        case TeamId::None:
            return nullptr;
    }

    return nullptr;
}

void ensurePlayer(WorldState* world,
                  int playerId,
                  const Vec3& spawnPosition,
                  const SimConfig& config) {
    if (world == nullptr) {
        return;
    }

    if (findPlayer(world, playerId) != nullptr) {
        ensureRosterEntry(world, playerId, TeamId::None, false);
        return;
    }

    PlayerState player;
    player.playerId = playerId;
    player.position = spawnPosition;
    player.velocity = Vec3{};
    player.yaw = 0.0f;
    player.pitch = 0.0f;
    player.health = config.playerMaxHealth;
    player.maxHealth = config.playerMaxHealth;
    player.weaponCooldownRemaining = 0.0f;
    player.jumpsUsed = 0;
    player.grounded = true;
    world->players.push_back(player);
    ensureRosterEntry(world, playerId, TeamId::None, false);
}

void ensureRosterEntry(WorldState* world,
                       int actorId,
                       TeamId team,
                       bool isBot) {
    if (world == nullptr) {
        return;
    }

    if (findRosterEntry(world, actorId) != nullptr) {
        return;
    }

    RosterEntry entry;
    entry.actorId = actorId;
    entry.team = team;
    entry.isBot = isBot;
    entry.kills = 0;
    entry.deaths = 0;
    entry.alive = true;
    world->roster.push_back(entry);
}

bool recordKill(WorldState* world, int attackerActorId, int victimActorId) {
    if (world == nullptr) {
        return false;
    }

    RosterEntry* attacker = findRosterEntry(world, attackerActorId);
    RosterEntry* victim = findRosterEntry(world, victimActorId);
    if (attacker == nullptr || victim == nullptr) {
        return false;
    }

    ++attacker->kills;
    ++victim->deaths;
    attacker->alive = true;
    victim->alive = false;

    if (std::uint16_t* teamScore = findTeamScore(world, attacker->team)) {
        ++(*teamScore);
    }
    return true;
}

void setRosterAlive(WorldState* world, int actorId, bool alive) {
    if (world == nullptr) {
        return;
    }

    if (RosterEntry* entry = findRosterEntry(world, actorId)) {
        entry->alive = alive;
    }
}

void removePlayer(WorldState* world, int playerId) {
    if (world == nullptr) {
        return;
    }

    world->players.erase(
        std::remove_if(world->players.begin(),
                       world->players.end(),
                       [playerId](const PlayerState& player) { return player.playerId == playerId; }),
        world->players.end());
    removeRosterEntry(world, playerId);
}

void removeRosterEntry(WorldState* world, int actorId) {
    if (world == nullptr) {
        return;
    }

    world->roster.erase(
        std::remove_if(world->roster.begin(),
                       world->roster.end(),
                       [actorId](const RosterEntry& entry) { return entry.actorId == actorId; }),
        world->roster.end());
}

void advanceAi(WorldState* world, float dtSeconds, const SimConfig& config) {
    if (world == nullptr || dtSeconds <= 0.0f) {
        return;
    }

    const auto waypoints = patrolWaypoints(world->environment.arenaHalfSize);
    normalizeEnemyAuxiliaryState(world);
    for (std::size_t index = 0; index < world->enemies.size(); ++index) {
        auto& enemy = world->enemies[index];
        if (!enemy.alive || enemy.health <= 0.0f) {
            enemy.velocity = Vec3{};
            continue;
        }

        const std::size_t waypointIndex = world->enemyWaypointIndices[index] % waypoints.size();
        Vec3 toTarget = subtract(waypoints[waypointIndex], enemy.position);
        float distanceToTarget = length(toTarget);
        if (distanceToTarget < defaults::kEnemyTargetThreshold) {
            world->enemyWaypointIndices[index] = (waypointIndex + 1) % waypoints.size();
            toTarget = subtract(waypoints[world->enemyWaypointIndices[index]], enemy.position);
        }

        const Vec3 direction = normalize(toTarget);
        enemy.velocity = scale(direction, defaults::kEnemySpeed);
        enemy.position = add(enemy.position, scale(enemy.velocity, dtSeconds));
        enemy.position = clampToArena(enemy.position, world->environment.arenaHalfSize);
        enemy.position = resolveCollisions(enemy.position,
                                           enemy.position,
                                           enemy.radius,
                                           defaults::kEnemyBodyHeight,
                                           world->environment.collisionBoxes);
        enemy.position.y = getGroundHeightAt(enemy.position, world->environment.collisionBoxes);

        if (lengthSquared(enemy.velocity) > 0.0001f) {
            enemy.yaw = std::atan2(enemy.velocity.x, enemy.velocity.z);
        }
        enemy.health = std::clamp(enemy.health, 0.0f, config.enemyMaxHealth);
    }
}

void advanceRespawns(WorldState* world,
                     float dtSeconds,
                     float respawnDelaySeconds,
                     const SimConfig& config) {
    if (world == nullptr || dtSeconds <= 0.0f) {
        return;
    }

    normalizeEnemyAuxiliaryState(world);
    for (std::size_t index = 0; index < world->enemies.size(); ++index) {
        auto& enemy = world->enemies[index];
        auto& respawnTimer = world->enemyRespawnTimers[index];
        if (enemy.alive && enemy.health > 0.0f) {
            respawnTimer = 0.0f;
            continue;
        }

        enemy.alive = false;
        enemy.velocity = Vec3{};
        respawnTimer += dtSeconds;
        if (respawnTimer < respawnDelaySeconds) {
            continue;
        }

        respawnTimer = 0.0f;
        enemy.position = world->enemySpawns[index];
        enemy.velocity = Vec3{};
        enemy.yaw = 0.0f;
        enemy.pitch = 0.0f;
        enemy.health = config.enemyMaxHealth;
        enemy.alive = true;
    }
}

}  // namespace sim
