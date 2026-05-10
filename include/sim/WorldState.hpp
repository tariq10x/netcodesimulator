#pragma once

#include <cstddef>
#include <vector>

#include "sim/SimulationTypes.hpp"

namespace sim {

struct SessionMetadataState {
    std::uint32_t sessionId{0u};
    std::int32_t levelSlot{-1};
    std::uint32_t levelHash{0u};
    std::uint16_t maxHumanPlayers{0u};
    std::uint16_t connectedHumanPlayers{0u};
    std::uint16_t connectedBotPlayers{0u};
};

struct WorldState {
    MovementEnvironment environment{};
    TimingCadence cadence{};
    AuthoritativeTime authoritativeTime{};
    std::vector<PlayerState> players{};
    std::vector<RemoteActorState> enemies{};
    std::vector<RosterEntry> roster{};
    TeamScores teamScores{};
    SessionMetadataState sessionMetadata{};
    std::vector<Vec3> playerSpawns{};
    std::vector<Vec3> enemySpawns{};
    std::vector<std::size_t> enemyWaypointIndices{};
    std::vector<float> enemyRespawnTimers{};
};

WorldState createDefaultWorld(std::size_t maxPlayers,
                              const MovementEnvironment& environment,
                              const SimConfig& config = {});

PlayerState* findPlayer(WorldState* world, int playerId);
const PlayerState* findPlayer(const WorldState& world, int playerId);

RosterEntry* findRosterEntry(WorldState* world, int actorId);
const RosterEntry* findRosterEntry(const WorldState& world, int actorId);
std::uint16_t* findTeamScore(WorldState* world, TeamId team);
const std::uint16_t* findTeamScore(const WorldState& world, TeamId team);

void ensurePlayer(WorldState* world,
                  int playerId,
                  const Vec3& spawnPosition,
                  const SimConfig& config = {});

void ensureRosterEntry(WorldState* world,
                       int actorId,
                       TeamId team = TeamId::None,
                       bool isBot = false);

bool recordKill(WorldState* world, int attackerActorId, int victimActorId);
void setRosterAlive(WorldState* world, int actorId, bool alive);

void removePlayer(WorldState* world, int playerId);
void removeRosterEntry(WorldState* world, int actorId);

void advanceAi(WorldState* world, float dtSeconds, const SimConfig& config = {});
void advanceRespawns(WorldState* world,
                     float dtSeconds,
                     float respawnDelaySeconds,
                     const SimConfig& config = {});

}  // namespace sim
