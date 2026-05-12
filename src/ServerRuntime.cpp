#include "net/ServerRuntime.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "replay/ReplayRecorder.hpp"
#include "server/BotCommandSource.hpp"
#include "sim/SimulationRules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string_view>
#include <type_traits>

namespace net {
namespace {

constexpr float kBotSpawnInset = 12.0f;
constexpr float kBotSpawnSpacing = 3.0f;
constexpr float kStudyBotSpawnForwardDistance = 4.0f;
constexpr float kStudyBotSpawnLateralDistance = 2.0f;
constexpr float kStudyBotSpawnMinSeparation = 1.6f;
constexpr std::uint16_t kAuthoritativeHostPeerId = 1u;
constexpr std::uint32_t kDefaultSmoothCorrectionWindowMs = 250u;
constexpr std::uint64_t kDefaultTickIntervalUs = 16'667u;
constexpr std::uint64_t kDefaultSnapshotIntervalUs = 50'000u;
constexpr std::string_view kEventLoggingAppliedPrefix = "applied:";
constexpr std::uint32_t kAllowedGameplayButtons =
    sim::commandButtonBit(sim::CommandButton::Jump) |
    sim::commandButtonBit(sim::CommandButton::Fire);

sim::PlayerCommand idleCommandFor(const sim::PlayerState& state, float dtSeconds) {
    sim::PlayerCommand command;
    command.dtSeconds = dtSeconds;
    command.yaw = state.yaw;
    command.pitch = state.pitch;
    return command;
}

sim::HitscanRay rayForCommand(const sim::PlayerState& player,
                              const sim::PlayerCommand& command,
                              const sim::SimConfig& config) {
    return sim::buildRifleHitscan(player, command.yaw, command.pitch, config);
}

bool isFiniteGameplayValue(float value) {
    return std::isfinite(value);
}

bool tryParseRuntimeBool(float rawValue, bool* valueOut) {
    if (valueOut == nullptr || !std::isfinite(rawValue)) {
        return false;
    }

    if (std::fabs(rawValue) <= 0.0001f) {
        *valueOut = false;
        return true;
    }
    if (std::fabs(rawValue - 1.0f) <= 0.0001f) {
        *valueOut = true;
        return true;
    }
    return false;
}

bool runtimeRequestTargetsActorKey(const RuntimeParamChangeRequest& request,
                                   const char* suffix) {
    if ((request.scope != RuntimeParamScope::Player &&
         request.scope != RuntimeParamScope::Bot) ||
        request.targetId <= 0 ||
        request.targetId > static_cast<std::int32_t>(
            std::numeric_limits<std::uint16_t>::max())) {
        return false;
    }

    return request.key ==
           runtimeParamKeyForTarget(static_cast<std::uint16_t>(request.targetId), suffix);
}

bool tryParseAdminTeam(float rawValue, sim::TeamId* teamOut) {
    if (teamOut == nullptr || !std::isfinite(rawValue)) {
        return false;
    }

    const long rounded = std::lround(rawValue);
    if (std::fabs(rawValue - static_cast<float>(rounded)) > 0.0001f) {
        return false;
    }

    switch (rounded) {
        case static_cast<long>(sim::TeamId::Attacker):
            *teamOut = sim::TeamId::Attacker;
            return true;
        case static_cast<long>(sim::TeamId::Defender):
            *teamOut = sim::TeamId::Defender;
            return true;
        case static_cast<long>(sim::TeamId::Spectator):
            *teamOut = sim::TeamId::Spectator;
            return true;
        default:
            return false;
    }
}

bool tryParseRuntimeReconciliationStrategy(
    float rawValue,
    sim::RuntimeReconciliationStrategy* strategyOut) {
    if (strategyOut == nullptr || !std::isfinite(rawValue)) {
        return false;
    }

    const float rounded = std::round(rawValue);
    if (std::fabs(rawValue - rounded) > 0.0001f) {
        return false;
    }

    switch (static_cast<int>(rounded)) {
        case static_cast<int>(sim::RuntimeReconciliationStrategy::Snap):
            *strategyOut = sim::RuntimeReconciliationStrategy::Snap;
            return true;
        case static_cast<int>(sim::RuntimeReconciliationStrategy::Smooth):
            *strategyOut = sim::RuntimeReconciliationStrategy::Smooth;
            return true;
        default:
            return false;
    }
}

bool tryParseSmoothCorrectionWindowMs(float rawValue, std::uint32_t* windowOut) {
    if (windowOut == nullptr || !std::isfinite(rawValue)) {
        return false;
    }

    const float rounded = std::round(rawValue);
    if (std::fabs(rawValue - rounded) > 0.0001f || rounded < 0.0f || rounded > 1000.0f) {
        return false;
    }

    *windowOut = static_cast<std::uint32_t>(rounded);
    return true;
}

std::uint64_t intervalUsForRateHz(std::uint16_t rateHz, std::uint64_t fallbackUs) {
    if (rateHz == 0u) {
        return fallbackUs;
    }

    return static_cast<std::uint64_t>(
        std::llround(1'000'000.0 / static_cast<double>(rateHz)));
}

sim::TimingCadence timingCadenceFor(const ServerConfig& config) {
    return sim::TimingCadence{
        config.tickRateHz,
        config.snapshotRateHz,
        config.tickRateHz
    };
}

LagCompensationConfig lagCompensationConfigFor(const ServerConfig& config) {
    const std::uint32_t rewindMs = config.maxRewindMs;
    const std::uint16_t effectiveTickRateHz = config.tickRateHz > 0u ? config.tickRateHz : 60u;
    const std::size_t samplesForWindow = static_cast<std::size_t>(
        std::ceil((static_cast<double>(rewindMs) / 1000.0) * effectiveTickRateHz)) + 2u;

    return LagCompensationConfig{
        static_cast<std::uint64_t>(rewindMs) * 1'000u,
        std::max<std::size_t>(samplesForWindow, 2u)
    };
}

struct SessionControlState {
    int controlActorId{-1};
    int followTargetActorId{-1};
    sim::PaneViewMode paneMode{sim::PaneViewMode::PlayerControlled};
};

using WorldSessionControlStates =
    std::unordered_map<const sim::WorldState*, std::unordered_map<std::uint16_t, SessionControlState>>;

WorldSessionControlStates gSessionControlStates;

bool isObservationPaneMode(sim::PaneViewMode mode) {
    return mode == sim::PaneViewMode::SpectatorFreeFly ||
           mode == sim::PaneViewMode::SpectatorFollowFirstPerson ||
           mode == sim::PaneViewMode::SpectatorFollowThirdPerson ||
           mode == sim::PaneViewMode::ReplayCamera;
}

int firstSpectatorTargetActorId(const sim::WorldState& worldState, int preferredActorId) {
    if (preferredActorId >= 0) {
        if (const sim::RosterEntry* preferred = sim::findRosterEntry(worldState, preferredActorId);
            preferred != nullptr && sim::isSpectatorTargetEligible(*preferred)) {
            return preferredActorId;
        }
    }

    for (const auto& entry : worldState.roster) {
        if (sim::isSpectatorTargetEligible(entry)) {
            return entry.actorId;
        }
    }
    return -1;
}

SessionControlState defaultSessionControlState(const sim::WorldState& worldState,
                                               const ClientSession& session) {
    SessionControlState state;
    const sim::ParticipantState participant = session.participantState();
    state.controlActorId = participant.control.controlsActor() ? participant.control.actorId : -1;
    if (participant.participation == sim::ParticipationState::Spectating) {
        state.followTargetActorId = firstSpectatorTargetActorId(worldState, state.controlActorId);
        state.paneMode = state.followTargetActorId >= 0
            ? sim::PaneViewMode::SpectatorFollowThirdPerson
            : sim::PaneViewMode::SpectatorFreeFly;
    } else {
        state.followTargetActorId = state.controlActorId;
        state.paneMode = sim::PaneViewMode::PlayerControlled;
    }
    return state;
}

SessionControlState sessionControlStateFor(const sim::WorldState& worldState,
                                           const ClientSession& session) {
    const auto worldIt = gSessionControlStates.find(&worldState);
    if (worldIt == gSessionControlStates.end()) {
        return defaultSessionControlState(worldState, session);
    }

    const auto peerIt = worldIt->second.find(session.peerId);
    if (peerIt == worldIt->second.end()) {
        return defaultSessionControlState(worldState, session);
    }
    return peerIt->second;
}

SessionControlState& ensureSessionControlState(sim::WorldState& worldState,
                                               const ClientSession& session) {
    auto& perWorld = gSessionControlStates[&worldState];
    return perWorld.try_emplace(session.peerId,
                                defaultSessionControlState(worldState, session)).first->second;
}

void clearSessionControlState(sim::WorldState* worldState, std::uint16_t peerId) {
    if (worldState == nullptr) {
        return;
    }
    const auto worldIt = gSessionControlStates.find(worldState);
    if (worldIt == gSessionControlStates.end()) {
        return;
    }
    worldIt->second.erase(peerId);
    if (worldIt->second.empty()) {
        gSessionControlStates.erase(worldIt);
    }
}

sim::ParticipantState participantStateFor(const sim::WorldState& worldState,
                                          const ClientSession& session) {
    sim::ParticipantState participant = session.participantState();
    if (participant.participation == sim::ParticipationState::Spectating ||
        session.team == sim::TeamId::Spectator) {
        participant.team = sim::TeamId::Spectator;
        participant.participation = sim::ParticipationState::Spectating;
        participant.control = sim::ControlBinding{};
        return participant;
    }

    const SessionControlState controlState = sessionControlStateFor(worldState, session);
    if (controlState.controlActorId >= 0) {
        participant.control =
            sim::ControlBinding{sim::ControlBindingKind::Actor, controlState.controlActorId};
        if (controlState.controlActorId != static_cast<int>(session.peerId)) {
            if (const sim::RosterEntry* entry =
                    sim::findRosterEntry(worldState, controlState.controlActorId);
                entry != nullptr && sim::isPlayableTeam(entry->team)) {
                participant.team = entry->team;
            }
        }
    }
    return participant;
}

sim::PaneViewState paneViewFor(const sim::WorldState& worldState, const ClientSession& session) {
    sim::PaneViewState paneView;
    const SessionControlState controlState = sessionControlStateFor(worldState, session);
    paneView.slot = sim::PaneSlot::Left;
    paneView.mode = controlState.paneMode;
    paneView.focused = true;
    paneView.followTargetActorId = isObservationPaneMode(controlState.paneMode)
        ? controlState.followTargetActorId
        : controlState.controlActorId;
    return paneView;
}

HostedSessionMetadata makeHostedSessionMetadata(const ServerConfig& config,
                                                const sim::SessionMetadataState& sessionMetadata,
                                                bool botsFrozen,
                                                bool botsCanShoot) {
    HostedSessionMetadata metadata;
    metadata.sessionLabel = config.sessionLabel;
    metadata.hostPlayerName = config.hostPlayerName.empty() ? "player" : config.hostPlayerName;
    metadata.hostPeerId = kAuthoritativeHostPeerId;
    metadata.levelSlot = sessionMetadata.levelSlot;
    metadata.levelHash = sessionMetadata.levelHash;
    metadata.publicJoinPort = config.publicJoinPort;
    metadata.maxHumanPlayers = sessionMetadata.maxHumanPlayers;
    metadata.shotEvaluationMode = config.shotEvaluationMode;
    metadata.visualizationMode = config.visualizationMode;
    metadata.botsFrozen = botsFrozen;
    metadata.botsCanShoot = botsCanShoot;
    metadata.studyEventLoggingEnabled = config.studyEventLoggingEnabled;
    metadata.studyEventRunId = config.studyEventRunId;
    return metadata;
}

std::uint64_t rewindTimeForTiming(const sim::CommandTiming& timing) {
    const std::uint64_t interpDelayUs =
        static_cast<std::uint64_t>(timing.interpolationDelayMs) * 1'000u;
    if (timing.viewedServerTimeUs <= interpDelayUs) {
        return 0u;
    }
    return timing.viewedServerTimeUs - interpDelayUs;
}

std::uint64_t rewindTimeForCommand(const sim::PlayerCommand& command) {
    return rewindTimeForTiming(command.toCommandTiming());
}

bool hasActiveHumanControlTrack(const ClientSession& session) {
    return session.connected &&
           session.hasControlPlayerState &&
           (session.lastAppliedControlSeq > 0u || !session.pendingControlCommands.empty());
}

int controlledActorIdForSession(const sim::WorldState& worldState,
                                const ClientSession& session) {
    const SessionControlState controlState = sessionControlStateFor(worldState, session);
    return controlState.controlActorId >= 0
        ? controlState.controlActorId
        : static_cast<int>(session.peerId);
}

bool overlayControlPlayer(sim::WorldState* controlWorld,
                          const sim::PlayerState& controlPlayer) {
    if (controlWorld == nullptr) {
        return false;
    }

    sim::PlayerState* player = sim::findPlayer(controlWorld, controlPlayer.playerId);
    if (player == nullptr) {
        return false;
    }

    const float health = player->health;
    const float maxHealth = player->maxHealth;
    const float weaponCooldownRemaining = player->weaponCooldownRemaining;
    *player = controlPlayer;
    player->playerId = controlPlayer.playerId;
    player->health = health;
    player->maxHealth = maxHealth;
    player->weaponCooldownRemaining = weaponCooldownRemaining;
    return true;
}

std::vector<sim::HitscanTarget> buildControlGhostTargets(
    const sim::WorldState& worldState,
    const std::vector<ClientSession>& sessions,
    const std::vector<sim::PlayerState>& serverControlPlayers,
    const sim::SimConfig& simConfig) {
    sim::WorldState controlWorld = worldState;
    bool sawControlState = false;
    for (const ClientSession& session : sessions) {
        if (!hasActiveHumanControlTrack(session)) {
            continue;
        }
        sawControlState =
            overlayControlPlayer(&controlWorld, session.controlPlayerState) ||
            sawControlState;
    }

    for (const sim::PlayerState& player : serverControlPlayers) {
        sawControlState = overlayControlPlayer(&controlWorld, player) || sawControlState;
    }

    if (!sawControlState) {
        return {};
    }
    return sim::buildHitscanTargets(controlWorld, simConfig);
}

std::vector<sim::HitscanTarget> buildEvaluationTargetsForFire(
    const sim::WorldState& worldState,
    const std::vector<ClientSession>& sessions,
    const std::vector<sim::PlayerState>& serverControlPlayers,
    const sim::SimConfig& simConfig,
    const sim::PlayerCommand& command,
    ShotEvaluationMode shotEvaluationMode,
    LagCompensationHistory& lagCompensation,
    std::uint64_t currentServerTimeUs) {
    if (shotEvaluationMode == ShotEvaluationMode::SeenPosition) {
        std::vector<sim::HitscanTarget> targets =
            lagCompensation.rewindTargets(rewindTimeForCommand(command), currentServerTimeUs);
        return targets.empty() ? sim::buildHitscanTargets(worldState, simConfig) : targets;
    }

    std::vector<sim::HitscanTarget> controlGhostTargets =
        buildControlGhostTargets(worldState, sessions, serverControlPlayers, simConfig);
    if (!controlGhostTargets.empty()) {
        return controlGhostTargets;
    }

    if (command.hasControlTiming()) {
        std::vector<sim::HitscanTarget> controlTargets = lagCompensation.rewindTargets(
            rewindTimeForTiming(command.toControlTiming()), currentServerTimeUs);
        if (!controlTargets.empty()) {
            return controlTargets;
        }
    }

    return sim::buildHitscanTargets(worldState, simConfig);
}

std::vector<sim::PlayerState> buildRemotePlayers(const sim::WorldState& world,
                                                 int primaryExcludedActorId,
                                                 int secondaryExcludedActorId = -1) {
    std::vector<sim::PlayerState> remotePlayers;
    remotePlayers.reserve(world.players.size());

    for (const auto& player : world.players) {
        if (player.playerId == primaryExcludedActorId ||
            player.playerId == secondaryExcludedActorId) {
            continue;
        }
        remotePlayers.push_back(player);
    }

    return remotePlayers;
}

std::vector<sim::PlayerState> buildControlRemotePlayers(
    const sim::WorldState& world,
    const std::vector<ClientSession>& sessions,
    const std::vector<sim::PlayerState>& serverControlPlayers,
    int primaryExcludedActorId,
    int secondaryExcludedActorId = -1) {
    std::vector<sim::PlayerState> remotePlayers;
    remotePlayers.reserve(sessions.size() + serverControlPlayers.size());

    for (const ClientSession& session : sessions) {
        if (!hasActiveHumanControlTrack(session)) {
            continue;
        }

        const sim::PlayerState& player = session.controlPlayerState;
        if (player.playerId == primaryExcludedActorId ||
            player.playerId == secondaryExcludedActorId) {
            continue;
        }

        if (const sim::RosterEntry* entry = sim::findRosterEntry(world, player.playerId);
            entry == nullptr || !sim::isActivePlayableRosterEntry(*entry)) {
            continue;
        }

        remotePlayers.push_back(player);
    }

    for (const sim::PlayerState& player : serverControlPlayers) {
        if (player.playerId == primaryExcludedActorId ||
            player.playerId == secondaryExcludedActorId) {
            continue;
        }

        if (const sim::RosterEntry* entry = sim::findRosterEntry(world, player.playerId);
            entry == nullptr || !sim::isActivePlayableRosterEntry(*entry)) {
            continue;
        }

        remotePlayers.push_back(player);
    }

    return remotePlayers;
}

std::vector<sim::PlayerState> buildControlReplayPlayers(
    const sim::WorldState& world,
    const std::vector<ClientSession>& sessions,
    const std::vector<sim::PlayerState>& serverControlPlayers) {
    std::vector<sim::PlayerState> players;
    players.reserve(sessions.size() + serverControlPlayers.size());

    for (const ClientSession& session : sessions) {
        if (!hasActiveHumanControlTrack(session)) {
            continue;
        }
        if (const sim::RosterEntry* entry = sim::findRosterEntry(world,
                                                                 session.controlPlayerState.playerId);
            entry == nullptr || !sim::isActivePlayableRosterEntry(*entry)) {
            continue;
        }
        players.push_back(session.controlPlayerState);
    }

    for (const sim::PlayerState& player : serverControlPlayers) {
        if (const sim::RosterEntry* entry = sim::findRosterEntry(world, player.playerId);
            entry == nullptr || !sim::isActivePlayableRosterEntry(*entry)) {
            continue;
        }
        players.push_back(player);
    }

    return players;
}

std::vector<sim::RemoteActorState> buildRemoteEnemies(const sim::WorldState& world) {
    return world.enemies;
}

std::size_t countTeamMembers(const sim::WorldState& world, sim::TeamId team) {
    return static_cast<std::size_t>(std::count_if(
        world.roster.begin(),
        world.roster.end(),
        [team](const sim::RosterEntry& entry) {
            return entry.team == team;
        }));
}

sim::TeamId chooseBalancedTeam(const sim::WorldState& world) {
    const std::size_t attackerCount = countTeamMembers(world, sim::TeamId::Attacker);
    const std::size_t defenderCount = countTeamMembers(world, sim::TeamId::Defender);
    return attackerCount <= defenderCount ? sim::TeamId::Attacker
                                          : sim::TeamId::Defender;
}

sim::TeamId chooseBalancedTeam(const sim::WorldState& world, sim::TeamId teamBias) {
    std::size_t attackerCount = countTeamMembers(world, sim::TeamId::Attacker);
    std::size_t defenderCount = countTeamMembers(world, sim::TeamId::Defender);
    if (sim::isPlayableTeam(teamBias)) {
        if (teamBias == sim::TeamId::Attacker) {
            ++attackerCount;
        } else {
            ++defenderCount;
        }
    }
    return attackerCount <= defenderCount ? sim::TeamId::Attacker
                                          : sim::TeamId::Defender;
}

sim::RemoteActorState* findEnemyMutable(sim::WorldState* world, int entityId) {
    if (world == nullptr) {
        return nullptr;
    }

    for (auto& enemy : world->enemies) {
        if (enemy.entityId == entityId) {
            return &enemy;
        }
    }
    return nullptr;
}

float laneOffsetForIndex(std::size_t index) {
    if (index == 0u) {
        return 0.0f;
    }

    const std::size_t lane = (index + 1u) / 2u;
    const float magnitude = static_cast<float>(lane) * kBotSpawnSpacing;
    return (index % 2u) == 0u ? magnitude : -magnitude;
}

sim::Vec3 botSpawnPositionFor(sim::TeamId team,
                              std::size_t teamIndex,
                              const sim::MovementEnvironment& environment,
                              const sim::SimConfig& config) {
    const float maxInset = std::max(4.0f, environment.arenaHalfSize - 4.0f);
    const float depth = std::min(kBotSpawnInset, maxInset);
    return sim::Vec3{
        laneOffsetForIndex(teamIndex),
        config.playerEyeHeight,
        team == sim::TeamId::Attacker ? depth : -depth
    };
}

std::uint32_t mixBotSpawnSeed(std::uint32_t seed, std::uint32_t value) {
    seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
    return seed;
}

std::mt19937 makeBotSpawnRng(const ServerConfig& config) {
    std::uint32_t seed = std::random_device{}();
    seed = mixBotSpawnSeed(seed, static_cast<std::uint32_t>(config.listenPort));
    seed = mixBotSpawnSeed(seed, static_cast<std::uint32_t>(config.publicJoinPort));
    seed = mixBotSpawnSeed(seed, config.levelHash);
    seed = mixBotSpawnSeed(seed, static_cast<std::uint32_t>(config.attackerBotCount) << 16u |
                                 static_cast<std::uint32_t>(config.defenderBotCount));
    return std::mt19937{seed};
}

float defaultYawForTeam(sim::TeamId team) {
    constexpr float kPi = 3.14159265358979323846f;
    return team == sim::TeamId::Defender ? kPi : 0.0f;
}

bool isSpawnProtected(const std::vector<ClientSession>& sessions,
                      int actorId,
                      std::uint64_t currentServerTimeUs) {
    if (currentServerTimeUs == 0u) {
        return false;
    }

    for (const auto& session : sessions) {
        if (static_cast<int>(session.peerId) != actorId) {
            continue;
        }
        return session.connected && currentServerTimeUs < session.spawnProtectionUntilUs;
    }

    return false;
}

float planarDistanceSquared(const sim::Vec3& lhs, const sim::Vec3& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dz = lhs.z - rhs.z;
    return (dx * dx) + (dz * dz);
}

int nextAvailableBotActorId(const sim::WorldState& worldState) {
    int nextActorId = static_cast<int>(kFirstBotTransportTargetId);
    for (const sim::RosterEntry& entry : worldState.roster) {
        if (!entry.isBot) {
            continue;
        }
        nextActorId = std::max(nextActorId, entry.actorId + 1);
    }
    return nextActorId;
}

bool isSpawnPositionClear(const sim::WorldState& worldState,
                          const sim::Vec3& candidate,
                          const sim::SimConfig& simConfig) {
    const sim::Vec3 clamped =
        sim::clampToArena(candidate, worldState.environment.arenaHalfSize - simConfig.playerRadius);
    if (planarDistanceSquared(clamped, candidate) > 0.001f) {
        return false;
    }

    const sim::Vec3 resolved = sim::resolveCollisions(candidate,
                                                      candidate,
                                                      simConfig.playerRadius,
                                                      simConfig.playerCollisionHeight,
                                                      worldState.environment.collisionBoxes);
    if (planarDistanceSquared(resolved, candidate) > 0.05f) {
        return false;
    }

    for (const sim::PlayerState& player : worldState.players) {
        if (player.health <= 0.0f) {
            continue;
        }
        if (planarDistanceSquared(player.position, candidate) <
            (kStudyBotSpawnMinSeparation * kStudyBotSpawnMinSeparation)) {
            return false;
        }
    }

    return true;
}

std::optional<sim::Vec3> randomBotSpawnPositionFor(
    sim::TeamId team,
    const sim::WorldState& worldState,
    const sim::SimConfig& simConfig,
    std::mt19937& rng) {
    const float arenaLimit = std::max(1.0f,
                                      worldState.environment.arenaHalfSize -
                                          simConfig.playerRadius - 1.0f);
    std::uniform_real_distribution<float> xDistribution(-arenaLimit, arenaLimit);
    std::uniform_real_distribution<float> zDistribution(
        team == sim::TeamId::Attacker ? 0.0f : -arenaLimit,
        team == sim::TeamId::Attacker ? arenaLimit : 0.0f);

    for (int attempt = 0; attempt < 64; ++attempt) {
        sim::Vec3 candidate{xDistribution(rng), simConfig.playerEyeHeight, zDistribution(rng)};
        candidate.y = sim::getGroundHeightAt(candidate,
                                             worldState.environment.collisionBoxes) +
                      simConfig.playerEyeHeight;
        if (isSpawnPositionClear(worldState, candidate, simConfig)) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::optional<sim::Vec3> spawnPositionAheadOfPlayer(const sim::WorldState& worldState,
                                                    const sim::PlayerState& requester,
                                                    const sim::SimConfig& simConfig) {
    const sim::Vec3 forward = sim::forwardFromYaw(requester.yaw);
    const sim::Vec3 right = sim::rightFromYaw(requester.yaw);
    const float arenaLimit = std::max(1.0f, worldState.environment.arenaHalfSize - 1.0f);
    const float forwardDistances[] = {
        kStudyBotSpawnForwardDistance,
        std::min(kStudyBotSpawnForwardDistance + 2.0f, arenaLimit),
        std::min(kStudyBotSpawnForwardDistance + 4.0f, arenaLimit)
    };
    const float lateralOffsets[] = {
        0.0f,
        -kStudyBotSpawnLateralDistance,
        kStudyBotSpawnLateralDistance
    };

    for (float forwardDistance : forwardDistances) {
        for (float lateralOffset : lateralOffsets) {
            sim::Vec3 candidate = requester.position;
            candidate.x += (forward.x * forwardDistance) + (right.x * lateralOffset);
            candidate.z += (forward.z * forwardDistance) + (right.z * lateralOffset);
            candidate.y = sim::getGroundHeightAt(candidate,
                                                 worldState.environment.collisionBoxes) +
                          simConfig.playerEyeHeight;
            if (isSpawnPositionClear(worldState, candidate, simConfig)) {
                return candidate;
            }
        }
    }

    return std::nullopt;
}

const sim::PlayerState* chooseNearestOpposingPlayer(const sim::WorldState& world,
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

bool hasClearLineOfSight(const sim::WorldState& world,
                         const sim::PlayerState& shooter,
                         const sim::PlayerState& target,
                         const sim::SimConfig& simConfig) {
    const float yaw = std::atan2(target.position.x - shooter.position.x,
                                 shooter.position.z - target.position.z);
    const sim::HitscanRay shotRay = sim::buildRifleHitscan(shooter, yaw, 0.0f, simConfig);
    const sim::FireResult targetHit = sim::traceHitscan(
        shotRay, {sim::buildPlayerHitscanTarget(target, simConfig)});
    if (!targetHit.hit || targetHit.hitEntityId != target.playerId) {
        return false;
    }

    const float obstacleDistance =
        sim::traceHitscanObstacleDistance(shotRay, world.environment.collisionBoxes);
    return obstacleDistance < 0.0f || obstacleDistance >= targetHit.hitDistance;
}

void applySessionRosterState(sim::WorldState* world, const ClientSession& session) {
    if (world == nullptr) {
        return;
    }

    sim::ensureRosterEntry(world, session.peerId, session.team, false);
    const sim::ParticipantState participant = participantStateFor(*world, session);
    const int trackedActorId =
        participant.control.controlsActor() ? participant.control.actorId : static_cast<int>(session.peerId);
    const sim::PlayerState* player = sim::findPlayer(*world, trackedActorId);
    if (sim::RosterEntry* rosterEntry = sim::findRosterEntry(world, session.peerId)) {
        rosterEntry->team = participant.team;
        rosterEntry->sessionPresence = participant.presence;
        rosterEntry->participation = participant.participation;
        rosterEntry->control = participant.control;
        rosterEntry->isBot = false;
        rosterEntry->alive = player != nullptr ? player->health > 0.0f : participant.control.controlsActor();
        rosterEntry->interpolationEnabled = session.interpolationEnabled;
        rosterEntry->predictionEnabled = session.predictionEnabled;
        rosterEntry->reconciliationStrategy = session.reconciliationStrategy;
        rosterEntry->smoothCorrectionWindowMs = session.smoothCorrectionWindowMs;
        rosterEntry->latencyMs = session.reportedLatencyMs;
        rosterEntry->lossPct = session.reportedLossPct;
        rosterEntry->displayName = session.playerName;
    }
}

void respawnSessionPlayer(sim::WorldState* world,
                          ClientSession* session,
                          const sim::SimConfig& config,
                          float spawnProtectionSeconds,
                          std::uint64_t nowUs) {
    if (world == nullptr || session == nullptr) {
        return;
    }

    sim::PlayerState* player = sim::findPlayer(world, session->peerId);
    if (player == nullptr) {
        return;
    }

    player->position = session->spawnPosition;
    player->velocity = sim::Vec3{};
    player->health = config.playerMaxHealth;
    player->maxHealth = config.playerMaxHealth;
    player->weaponCooldownRemaining = 0.0f;
    player->jumpsUsed = 0;
    player->grounded = true;
    player->position = sim::clampToArena(player->position, world->environment.arenaHalfSize);
    player->position = sim::resolveCollisions(player->position,
                                              player->position,
                                              config.playerRadius,
                                              config.playerCollisionHeight,
                                              world->environment.collisionBoxes);
    const float groundHeight = sim::getGroundHeightAt(player->position, world->environment.collisionBoxes);
    player->position.y = groundHeight + config.playerEyeHeight;
    session->respawnTimerSeconds = 0.0f;
    session->spawnProtectionUntilUs = spawnProtectionSeconds > 0.0f
        ? nowUs + static_cast<std::uint64_t>(std::llround(
              static_cast<double>(spawnProtectionSeconds) * 1'000'000.0))
        : 0u;
    session->pendingCommands.clear();
    session->pendingControlCommands.clear();
    session->hasControlPlayerState = false;
    sim::setRosterAlive(world, session->peerId, true);
    applySessionRosterState(world, *session);
}

void advanceSessionRespawns(sim::WorldState* world,
                            std::vector<ClientSession>& sessions,
                            float dtSeconds,
                            float respawnDelaySeconds,
                            float spawnProtectionSeconds,
                            std::uint64_t nowUs,
                            const sim::SimConfig& config,
                            std::vector<SnapshotEvent>* outEvents) {
    if (world == nullptr || dtSeconds <= 0.0f) {
        return;
    }

    for (auto& session : sessions) {
        if (!session.connected || !sim::isPlayableTeam(session.team)) {
            session.respawnTimerSeconds = 0.0f;
            continue;
        }

        sim::PlayerState* player = sim::findPlayer(world, session.peerId);
        if (player == nullptr) {
            session.respawnTimerSeconds = 0.0f;
            continue;
        }

        if (player->health > 0.0f) {
            session.respawnTimerSeconds = 0.0f;
            continue;
        }

        player->velocity = sim::Vec3{};
        session.pendingCommands.clear();
        session.pendingControlCommands.clear();
        session.hasControlPlayerState = false;
        session.respawnTimerSeconds += dtSeconds;
        if (session.respawnTimerSeconds < respawnDelaySeconds) {
            continue;
        }

        respawnSessionPlayer(world, &session, config, spawnProtectionSeconds, nowUs);
        if (outEvents != nullptr) {
            outEvents->push_back(
                SnapshotEvent{SnapshotEventKind::PlayerRespawned,
                              static_cast<int>(session.peerId),
                              static_cast<int>(session.peerId),
                              player->position,
                              sim::Vec3{},
                              false});
        }
    }
}

ClientSession* findSessionByPeerId(std::vector<ClientSession>& sessions, std::int32_t targetId) {
    for (auto& candidate : sessions) {
        if (candidate.peerId == static_cast<std::uint16_t>(targetId)) {
            return &candidate;
        }
    }
    return nullptr;
}

sim::RosterEntry* findBotRosterEntry(sim::WorldState* world, std::int32_t actorId) {
    if (world == nullptr) {
        return nullptr;
    }

    sim::RosterEntry* entry = sim::findRosterEntry(world, actorId);
    if (entry == nullptr || !entry->isBot) {
        return nullptr;
    }
    return entry;
}

std::uint32_t botTransportSeedFor(const ProxyLinkConfig& config, int actorId) {
    return config.seed ^ 0xB07B07u ^ static_cast<std::uint32_t>(actorId);
}

double uniformTransportPercent(std::mt19937& rng) {
    static std::uniform_real_distribution<double> distribution(0.0, 100.0);
    return distribution(rng);
}

double uniformTransportJitter(std::mt19937& rng, float magnitudeMs) {
    if (magnitudeMs <= 0.0f) {
        return 0.0;
    }

    std::uniform_real_distribution<double> distribution(-static_cast<double>(magnitudeMs),
                                                        static_cast<double>(magnitudeMs));
    return distribution(rng);
}

float uniformUnit(std::mt19937& rng) {
    static std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    return distribution(rng);
}

float uniformRange(std::mt19937& rng, float minValue, float maxValue) {
    if (minValue >= maxValue) {
        return minValue;
    }

    std::uniform_real_distribution<float> distribution(minValue, maxValue);
    return distribution(rng);
}

std::uint64_t microsFromMillis(double milliseconds) {
    if (milliseconds <= 0.0) {
        return 0u;
    }

    const double micros = milliseconds * 1000.0;
    if (micros >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::round(micros));
}

bool shouldTriggerTransport(float pct, std::mt19937& rng) {
    if (pct <= 0.0f) {
        return false;
    }
    return uniformTransportPercent(rng) < static_cast<double>(pct);
}

bool applyTransportArtifactParamValue(ProxyLinkConfig* config,
                                      sim::RosterEntry* rosterEntry,
                                      const std::string& expectedKey,
                                      const RuntimeParamChangeRequest& request) {
    if (config == nullptr || request.key != expectedKey) {
        return false;
    }

    const auto hasSuffix = [&request](const char* suffix) {
        const std::string_view key{request.key};
        const std::string_view expectedSuffix{suffix};
        return key.size() >= expectedSuffix.size() &&
               key.compare(key.size() - expectedSuffix.size(),
                           expectedSuffix.size(),
                           expectedSuffix) == 0;
    };

    if (hasSuffix(".latency_ms")) {
        config->baseDelayMs = std::max(0.0f, request.value);
        if (rosterEntry != nullptr) {
            rosterEntry->latencyMs = static_cast<std::uint16_t>(std::clamp(
                std::lround(request.value),
                0l,
                static_cast<long>(std::numeric_limits<std::uint16_t>::max())));
        }
        return true;
    }

    if (hasSuffix(".jitter_ms")) {
        config->jitterMs = std::max(0.0f, request.value);
        return true;
    }

    if (hasSuffix(".loss_pct")) {
        config->lossPct = std::clamp(request.value, 0.0f, 100.0f);
        if (rosterEntry != nullptr) {
            rosterEntry->lossPct = static_cast<std::uint8_t>(std::clamp(
                std::lround(request.value),
                0l,
                100l));
        }
        return true;
    }

    if (hasSuffix(".duplicate_pct")) {
        config->duplicatePct = std::clamp(request.value, 0.0f, 100.0f);
        return true;
    }

    if (hasSuffix(".reorder_pct")) {
        config->reorderPct = std::clamp(request.value, 0.0f, 100.0f);
        return true;
    }

    return false;
}

}  // namespace

namespace server {

AuthoritativeSimulation::AuthoritativeSimulation(BotCommandSource botCommandSource,
                                                 BotDirectorConfig botDirectorConfig)
    : botCommandSource_(botCommandSource),
      botDirectorConfig_(botDirectorConfig),
      botsFrozen_(botDirectorConfig.startFrozen),
      botShootingEnabled_(botDirectorConfig.shootingEnabled) {}

void AuthoritativeSimulation::resetBotRuntimeState(BotRuntimeState& state) {
    state.scheduled.clear();
    resetBotCombatState(state);
    resetBotControlState(state);
    state.movementDecisionRemaining = 0.0f;
    state.lateralMove = 0.0f;
    state.forwardBias = 0.0f;
}

void AuthoritativeSimulation::resetBotCombatState(BotRuntimeState& state) {
    const std::uint32_t fireButton = sim::commandButtonBit(sim::CommandButton::Fire);
    for (PendingBotCommand& pending : state.scheduled) {
        pending.command.buttons &= ~fireButton;
    }
    state.targetActorId = -1;
    state.visibleTargetSeconds = 0.0f;
    state.shotCooldownRemaining = 0.0f;
}

void AuthoritativeSimulation::resetBotControlState(BotRuntimeState& state) {
    state.controlPlayerState = sim::PlayerState{};
    state.hasControlPlayerState = false;
    state.lastControlSeq = 0u;
}

void AuthoritativeSimulation::syncBotControlStateFromAuthoritative(
    BotRuntimeState& state,
    const sim::PlayerState& authoritativePlayer,
    int actorId) {
    if (authoritativePlayer.health <= 0.0f) {
        resetBotControlState(state);
        return;
    }

    const bool needsReset =
        !state.hasControlPlayerState ||
        state.controlPlayerState.playerId != actorId ||
        state.controlPlayerState.health <= 0.0f;
    if (needsReset) {
        state.controlPlayerState = authoritativePlayer;
        state.controlPlayerState.playerId = actorId;
        state.hasControlPlayerState = true;
        state.lastControlSeq = 0u;
        return;
    }

    syncBotControlVitalsFromAuthoritative(state, authoritativePlayer);
}

void AuthoritativeSimulation::syncBotControlVitalsFromAuthoritative(
    BotRuntimeState& state,
    const sim::PlayerState& authoritativePlayer) {
    if (!state.hasControlPlayerState) {
        return;
    }

    state.controlPlayerState.health = authoritativePlayer.health;
    state.controlPlayerState.maxHealth = authoritativePlayer.maxHealth;
    state.controlPlayerState.weaponCooldownRemaining =
        authoritativePlayer.weaponCooldownRemaining;
}

std::vector<sim::PlayerState> AuthoritativeSimulation::botControlPlayers(
    const sim::WorldState& worldState) const {
    std::vector<sim::PlayerState> players;
    players.reserve(worldState.players.size());
    for (const sim::PlayerState& authoritativePlayer : worldState.players) {
        const sim::RosterEntry* entry =
            sim::findRosterEntry(worldState, authoritativePlayer.playerId);
        if (entry == nullptr ||
            !entry->isBot ||
            !sim::isActivePlayableRosterEntry(*entry) ||
            authoritativePlayer.health <= 0.0f) {
            continue;
        }

        const auto runtimeIt = botRuntimeStates_.find(authoritativePlayer.playerId);
        if (runtimeIt == botRuntimeStates_.end() ||
            !runtimeIt->second.hasControlPlayerState) {
            continue;
        }

        sim::PlayerState controlPlayer = runtimeIt->second.controlPlayerState;
        controlPlayer.playerId = authoritativePlayer.playerId;
        controlPlayer.health = authoritativePlayer.health;
        controlPlayer.maxHealth = authoritativePlayer.maxHealth;
        controlPlayer.weaponCooldownRemaining =
            authoritativePlayer.weaponCooldownRemaining;
        players.push_back(controlPlayer);
    }
    return players;
}

void AuthoritativeSimulation::setBotsFrozen(bool frozen) {
    botsFrozen_ = frozen;
    if (!botsFrozen_) {
        return;
    }

    for (auto& [_, runtimeState] : botRuntimeStates_) {
        resetBotRuntimeState(runtimeState);
    }
}

bool AuthoritativeSimulation::botsFrozen() const {
    return botsFrozen_;
}

void AuthoritativeSimulation::setBotShootingEnabled(bool enabled) {
    if (botShootingEnabled_ == enabled) {
        return;
    }

    botShootingEnabled_ = enabled;
    if (botShootingEnabled_) {
        return;
    }

    for (auto& [_, runtimeState] : botRuntimeStates_) {
        resetBotCombatState(runtimeState);
    }
}

bool AuthoritativeSimulation::botShootingEnabled() const {
    return botShootingEnabled_;
}

void AuthoritativeSimulation::setBotFrozenPassive(int actorId, bool frozenPassive) {
    BotRuntimeState& state = botRuntimeStates_[actorId];
    state.frozenPassive = frozenPassive;
    if (frozenPassive) {
        resetBotRuntimeState(state);
    }
}

void AuthoritativeSimulation::setBotSpawnPosition(int actorId,
                                                  const sim::Vec3& spawnPosition) {
    BotRuntimeState& state = botRuntimeStates_[actorId];
    state.spawnPosition = spawnPosition;
    state.hasSpawnPosition = true;
}

bool AuthoritativeSimulation::applyTeamChangeRequest(sim::WorldState& worldState,
                                                     ClientSession& session,
                                                     const TeamChangeRequest& request,
                                                     const sim::SimConfig& simConfig,
                                                     std::vector<SnapshotEvent>* outEvents) const {
    if (!sim::isPlayableTeam(request.requestedTeam) ||
        request.requestedTeam == session.team) {
        return false;
    }

    session.team = request.requestedTeam;
    session.spawnPosition = worldState.playerSpawns.at(session.peerId - 1);
    sim::ensurePlayer(&worldState, session.peerId, session.spawnPosition, simConfig);
    respawnSessionPlayer(&worldState, &session, simConfig, 0.0f, 0u);

    if (outEvents != nullptr) {
        if (const sim::PlayerState* player = sim::findPlayer(worldState, session.peerId)) {
            outEvents->push_back(
                SnapshotEvent{SnapshotEventKind::PlayerRespawned,
                              static_cast<int>(session.peerId),
                              static_cast<int>(session.peerId),
                              player->position,
                              sim::Vec3{},
                              false});
        }
    }
    return true;
}

bool AuthoritativeSimulation::applyRuntimeParamChangeRequest(
    sim::WorldState& worldState,
    std::vector<ClientSession>& sessions,
    ClientSession& session,
    const RuntimeParamChangeRequest& request,
    RuntimeParamApplyResult* resultOut) {
    const auto tryParseActorId = [](float rawValue, int* actorIdOut) {
        if (actorIdOut == nullptr || !std::isfinite(rawValue)) {
            return false;
        }
        const long rounded = std::lround(rawValue);
        if (rounded < 0l || rounded > static_cast<long>(std::numeric_limits<int>::max())) {
            return false;
        }
        *actorIdOut = static_cast<int>(rounded);
        return true;
    };

    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (request.scope == RuntimeParamScope::Player) {
        ClientSession* targetSession = request.targetId > 0
            ? findSessionByPeerId(sessions, request.targetId)
            : nullptr;
        if (targetSession == nullptr) {
            result.message = "target_missing";
        } else {
            const bool requesterIsHost = session.peerId == kAuthoritativeHostPeerId;
            const bool requesterOwnsTarget =
                request.targetId == static_cast<std::int32_t>(session.peerId);
            const bool canEditParticipantSyncSettings = requesterIsHost;
            const bool canEditParticipantTransport = requesterIsHost || requesterOwnsTarget;
            const bool canEditParticipantControl = requesterIsHost || requesterOwnsTarget;

            if (request.key == runtimeParamKeyForTarget(targetSession->peerId, "interpolation_enabled")) {
                if (!canEditParticipantSyncSettings) {
                    result.message = "host_only";
                } else {
                    bool enabled = false;
                    if (!tryParseRuntimeBool(request.value, &enabled)) {
                        result.message = "invalid_interpolation_enabled";
                    } else {
                        targetSession->interpolationEnabled = enabled;
                        applySessionRosterState(&worldState, *targetSession);
                        result.applied = true;
                        result.message = "applied";
                        accepted = true;
                    }
                }
            } else if (request.key == runtimeParamKeyForTarget(targetSession->peerId, "prediction_enabled")) {
                if (!canEditParticipantSyncSettings) {
                    result.message = "host_only";
                } else {
                    bool enabled = false;
                    if (!tryParseRuntimeBool(request.value, &enabled)) {
                        result.message = "invalid_prediction_enabled";
                    } else {
                        targetSession->predictionEnabled = enabled;
                        applySessionRosterState(&worldState, *targetSession);
                        result.applied = true;
                        result.message = "applied";
                        accepted = true;
                    }
                }
            } else if (request.key ==
                       runtimeParamKeyForTarget(targetSession->peerId, "reconciliation_strategy")) {
                if (!canEditParticipantSyncSettings) {
                    result.message = "host_only";
                } else {
                    sim::RuntimeReconciliationStrategy strategy;
                    if (!tryParseRuntimeReconciliationStrategy(request.value, &strategy)) {
                        result.message = "invalid_reconciliation_strategy";
                    } else {
                        targetSession->reconciliationStrategy = strategy;
                        applySessionRosterState(&worldState, *targetSession);
                        result.applied = true;
                        result.message = "applied";
                        accepted = true;
                    }
                }
            } else if (request.key ==
                       runtimeParamKeyForTarget(targetSession->peerId, "smooth_correction_window_ms")) {
                if (!canEditParticipantSyncSettings) {
                    result.message = "host_only";
                } else {
                    std::uint32_t smoothWindowMs = kDefaultSmoothCorrectionWindowMs;
                    if (!tryParseSmoothCorrectionWindowMs(request.value, &smoothWindowMs)) {
                        result.message = "invalid_smooth_correction_window_ms";
                    } else {
                        targetSession->smoothCorrectionWindowMs = smoothWindowMs;
                        applySessionRosterState(&worldState, *targetSession);
                        result.applied = true;
                        result.message = "applied";
                        accepted = true;
                    }
                }
            } else if (request.key == runtimeParamKeyForTarget(targetSession->peerId, "latency_ms")) {
                if (!canEditParticipantTransport) {
                    result.message = "host_only";
                } else {
                    targetSession->reportedLatencyMs = static_cast<std::uint16_t>(std::clamp(
                        std::lround(request.value),
                        0l,
                        static_cast<long>(std::numeric_limits<std::uint16_t>::max())));
                    applySessionRosterState(&worldState, *targetSession);
                    result.applied = true;
                    result.message = "applied";
                    accepted = true;
                }
            } else if (request.key == runtimeParamKeyForTarget(targetSession->peerId, "loss_pct")) {
                if (!canEditParticipantTransport) {
                    result.message = "host_only";
                } else {
                    targetSession->reportedLossPct = static_cast<std::uint8_t>(std::clamp(
                        std::lround(request.value),
                        0l,
                        100l));
                    applySessionRosterState(&worldState, *targetSession);
                    result.applied = true;
                    result.message = "applied";
                    accepted = true;
                }
            } else if (request.key == runtimeParamKeyForTarget(targetSession->peerId, "control_actor_id")) {
                if (!canEditParticipantControl) {
                    result.message = "host_only";
                } else {
                    int requestedActorId = -1;
                    if (!tryParseActorId(request.value, &requestedActorId)) {
                        result.message = "invalid_control_actor";
                    } else if (const sim::RosterEntry* rosterEntry =
                                   sim::findRosterEntry(worldState, requestedActorId);
                               rosterEntry == nullptr ||
                               !sim::isLocalControlEligible(*rosterEntry, static_cast<int>(targetSession->peerId))) {
                        result.message = "remote_human_rejected";
                    } else {
                        SessionControlState& controlState =
                            ensureSessionControlState(worldState, *targetSession);
                        controlState.controlActorId = requestedActorId;
                        controlState.followTargetActorId = requestedActorId;
                        controlState.paneMode = sim::PaneViewMode::PlayerControlled;
                        targetSession->team = sim::findRosterEntry(worldState, requestedActorId)->team;
                        applySessionRosterState(&worldState, *targetSession);
                        result.applied = true;
                        result.message = "applied";
                        accepted = true;
                    }
                }
            } else if (request.key == runtimeParamKeyForTarget(targetSession->peerId, "follow_target_actor_id")) {
                if (!canEditParticipantControl) {
                    result.message = "host_only";
                } else {
                    int requestedActorId = -1;
                    if (!tryParseActorId(request.value, &requestedActorId)) {
                        result.message = "invalid_follow_target";
                    } else if (const sim::RosterEntry* rosterEntry =
                                   sim::findRosterEntry(worldState, requestedActorId);
                               rosterEntry == nullptr || !sim::isSpectatorTargetEligible(*rosterEntry)) {
                        result.message = "target_missing";
                    } else {
                        SessionControlState& controlState =
                            ensureSessionControlState(worldState, *targetSession);
                        controlState.followTargetActorId = requestedActorId;
                        controlState.paneMode = sim::PaneViewMode::SpectatorFollowThirdPerson;
                        applySessionRosterState(&worldState, *targetSession);
                        result.applied = true;
                        result.message = "applied";
                        accepted = true;
                    }
                }
            } else {
                result.message = "unsupported_player_param";
            }
        }
    } else if (request.scope == RuntimeParamScope::Bot) {
        if (session.peerId != kAuthoritativeHostPeerId) {
            result.message = "host_only";
        } else {
            const bool targetLooksLikeBot =
                request.targetId > 0 &&
                isBotTransportTargetId(static_cast<std::uint16_t>(request.targetId));
            sim::RosterEntry* rosterEntry = targetLooksLikeBot
                ? findBotRosterEntry(&worldState, request.targetId)
                : nullptr;
            if (rosterEntry == nullptr) {
                result.message = "target_missing";
            } else {
                const std::uint16_t targetId = static_cast<std::uint16_t>(request.targetId);
                BotRuntimeState& botTransportState = botRuntimeStates_[request.targetId];
                botTransportState.rng.seed(botTransportSeedFor(botTransportState.config, request.targetId));
                const bool applied =
                    applyTransportArtifactParamValue(&botTransportState.config,
                                                     rosterEntry,
                                                     runtimeParamKeyForTarget(targetId, "latency_ms"),
                                                     request) ||
                    applyTransportArtifactParamValue(&botTransportState.config,
                                                     rosterEntry,
                                                     runtimeParamKeyForTarget(targetId, "jitter_ms"),
                                                     request) ||
                    applyTransportArtifactParamValue(&botTransportState.config,
                                                     rosterEntry,
                                                     runtimeParamKeyForTarget(targetId, "loss_pct"),
                                                     request) ||
                    applyTransportArtifactParamValue(&botTransportState.config,
                                                     rosterEntry,
                                                     runtimeParamKeyForTarget(targetId, "duplicate_pct"),
                                                     request) ||
                    applyTransportArtifactParamValue(&botTransportState.config,
                                                     rosterEntry,
                                                     runtimeParamKeyForTarget(targetId, "reorder_pct"),
                                                     request);
                result.applied = applied;
                result.message = applied ? "applied" : "unsupported_bot_param";
                accepted = applied;
            }
        }
    } else {
        result.message = "unsupported_param";
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

void AuthoritativeSimulation::stepWorld(sim::WorldState& worldState,
                                        std::vector<ClientSession>& sessions,
                                        float dtSeconds,
                                        const sim::SimConfig& simConfig,
                                        float respawnDelaySeconds,
                                        float spawnProtectionSeconds,
                                        ShotEvaluationMode shotEvaluationMode,
                                        LagCompensationHistory& lagCompensation,
                                        std::uint64_t currentServerTimeUs,
                                        std::vector<SnapshotEvent>* outEvents,
                                        std::vector<CombatStudyEvent>* outStudyEvents,
                                        replay::CommandReplayRecorder* replayRecorder,
                                        std::uint32_t serverTick) {
    for (auto& session : sessions) {
        applyQueuedCommands(worldState,
                            sessions,
                            session,
                            dtSeconds,
                            simConfig,
                            shotEvaluationMode,
                            lagCompensation,
                            currentServerTimeUs,
                            outEvents,
                            outStudyEvents,
                            replayRecorder,
                            serverTick);
    }
    applyBotCommands(worldState,
                     sessions,
                     dtSeconds,
                     simConfig,
                     respawnDelaySeconds,
                     shotEvaluationMode,
                     lagCompensation,
                     currentServerTimeUs,
                     outEvents,
                     outStudyEvents,
                     replayRecorder,
                     serverTick);
    sim::advanceAi(&worldState, dtSeconds, simConfig);
    sim::advanceRespawns(&worldState, dtSeconds, respawnDelaySeconds, simConfig);
    advanceSessionRespawns(&worldState,
                           sessions,
                           dtSeconds,
                           respawnDelaySeconds,
                           spawnProtectionSeconds,
                           currentServerTimeUs,
                           simConfig,
                           outEvents);
}

void AuthoritativeSimulation::applyQueuedCommands(sim::WorldState& worldState,
                                                  const std::vector<ClientSession>& sessions,
                                                  ClientSession& session,
                                                  float dtSeconds,
                                                  const sim::SimConfig& simConfig,
                                                  ShotEvaluationMode shotEvaluationMode,
                                                  LagCompensationHistory& lagCompensation,
                                                  std::uint64_t currentServerTimeUs,
                                                  std::vector<SnapshotEvent>* outEvents,
                                                  std::vector<CombatStudyEvent>* outStudyEvents,
                                                  replay::CommandReplayRecorder* replayRecorder,
                                                  std::uint32_t serverTick) const {
    const SessionControlState controlState = sessionControlStateFor(worldState, session);
    const int controlledActorId =
        controlState.controlActorId >= 0 ? controlState.controlActorId : static_cast<int>(session.peerId);
    sim::PlayerState* player = sim::findPlayer(&worldState, controlledActorId);
    if (player == nullptr) {
        return;
    }

    if (player->health <= 0.0f) {
        session.pendingCommands.clear();
        session.pendingControlCommands.clear();
        sim::setRosterAlive(&worldState, controlledActorId, false);
        applySessionRosterState(&worldState, session);
        return;
    }

    if (session.pendingCommands.empty()) {
        *player = sim::applyPlayerCommand(*player,
                                          idleCommandFor(*player, dtSeconds),
                                          worldState.environment,
                                          simConfig);
        applySessionRosterState(&worldState, session);
        return;
    }

    for (auto it = session.pendingCommands.begin(); it != session.pendingCommands.end(); ) {
        if (!isNewerSequence(it->first, session.lastAppliedInputSeq)) {
            it = session.pendingCommands.erase(it);
            continue;
        }

        const sim::PlayerCommand& command = it->second;
        const sim::PlayerState beforeCommand = *player;
        *player = sim::applyPlayerCommand(*player, command, worldState.environment, simConfig);
        player->playerId = controlledActorId;
        sim::setRosterAlive(&worldState, controlledActorId, player->health > 0.0f);
        if (replayRecorder != nullptr) {
            replayRecorder->recordCommandApplied(replay::ReplayTrack::Authoritative,
                                                 session.peerId,
                                                 controlledActorId,
                                                 serverTick,
                                                 currentServerTimeUs,
                                                 command);
        }

        if (command.has(sim::CommandButton::Fire) && sim::canFire(beforeCommand)) {
            const std::vector<sim::HitscanTarget> evaluationTargets =
                buildEvaluationTargetsForFire(worldState,
                                              sessions,
                                              botControlPlayers(worldState),
                                              simConfig,
                                              command,
                                              shotEvaluationMode,
                                              lagCompensation,
                                              currentServerTimeUs);
            processFireCommand(worldState,
                               sessions,
                               controlledActorId,
                               *player,
                               command,
                               evaluationTargets,
                               simConfig,
                               currentServerTimeUs,
                               shotEvaluationMode,
                               outEvents,
                               outStudyEvents);
        }

        session.lastAppliedInputSeq = command.seq;
        session.lastAckedInputSeq = command.seq;
        it = session.pendingCommands.erase(it);
    }
    applySessionRosterState(&worldState, session);
}

void AuthoritativeSimulation::applyBotCommands(sim::WorldState& worldState,
                                               const std::vector<ClientSession>& sessions,
                                               float dtSeconds,
                                               const sim::SimConfig& simConfig,
                                               float respawnDelaySeconds,
                                               ShotEvaluationMode shotEvaluationMode,
                                               LagCompensationHistory& lagCompensation,
                                               std::uint64_t currentServerTimeUs,
                                               std::vector<SnapshotEvent>* outEvents,
                                               std::vector<CombatStudyEvent>* outStudyEvents,
                                               replay::CommandReplayRecorder* replayRecorder,
                                               std::uint32_t serverTick) {
    auto botHasCommandSession = [&worldState, &sessions](int actorId) {
        return std::any_of(sessions.begin(),
                           sessions.end(),
                           [&worldState, actorId](const ClientSession& session) {
                               return controlledActorIdForSession(worldState, session) == actorId;
                           });
    };

    for (const auto& entry : worldState.roster) {
        if (!entry.isBot) {
            continue;
        }
        if (botHasCommandSession(entry.actorId)) {
            resetBotRuntimeState(botRuntimeStates_[entry.actorId]);
            continue;
        }

        sim::PlayerState* bot = sim::findPlayer(&worldState, entry.actorId);
        if (bot == nullptr) {
            continue;
        }

        BotRuntimeState& transportState = botRuntimeStates_[entry.actorId];
        if (transportState.orderCounter == 0u && transportState.scheduled.empty()) {
            transportState.rng.seed(botTransportSeedFor(transportState.config, entry.actorId));
        }
        transportState.shotCooldownRemaining =
            std::max(0.0f, transportState.shotCooldownRemaining - dtSeconds);
        transportState.movementDecisionRemaining -= dtSeconds;
        if (bot->health <= 0.0f) {
            sim::setRosterAlive(&worldState, entry.actorId, false);
            bot->velocity = sim::Vec3{};
            transportState.scheduled.clear();
            resetBotControlState(transportState);
            transportState.targetActorId = -1;
            transportState.visibleTargetSeconds = 0.0f;
            transportState.shotCooldownRemaining = 0.0f;
            transportState.movementDecisionRemaining = 0.0f;
            transportState.respawnTimerSeconds += dtSeconds;
            if (transportState.respawnTimerSeconds >= respawnDelaySeconds) {
                sim::Vec3 spawnPosition = transportState.hasSpawnPosition
                    ? transportState.spawnPosition
                    : bot->position;
                spawnPosition = sim::clampToArena(spawnPosition,
                                                  worldState.environment.arenaHalfSize);
                spawnPosition = sim::resolveCollisions(spawnPosition,
                                                       spawnPosition,
                                                       simConfig.playerRadius,
                                                       simConfig.playerCollisionHeight,
                                                       worldState.environment.collisionBoxes);
                spawnPosition.y = sim::getGroundHeightAt(
                                      spawnPosition,
                                      worldState.environment.collisionBoxes) +
                                  simConfig.playerEyeHeight;

                bot->position = spawnPosition;
                bot->velocity = sim::Vec3{};
                bot->health = simConfig.playerMaxHealth;
                bot->maxHealth = simConfig.playerMaxHealth;
                bot->weaponCooldownRemaining = 0.0f;
                bot->jumpsUsed = 0;
                bot->grounded = true;
                transportState.respawnTimerSeconds = 0.0f;
                resetBotRuntimeState(transportState);
                sim::setRosterAlive(&worldState, entry.actorId, true);
                if (outEvents != nullptr) {
                    outEvents->push_back(
                        SnapshotEvent{SnapshotEventKind::PlayerRespawned,
                                      entry.actorId,
                                      entry.actorId,
                                      bot->position,
                                      sim::Vec3{},
                                      false});
                }
            }
            continue;
        }
        transportState.respawnTimerSeconds = 0.0f;
        if (botsFrozen_ || transportState.frozenPassive) {
            resetBotRuntimeState(transportState);
            *bot = sim::applyPlayerCommand(*bot,
                                           idleCommandFor(*bot, dtSeconds),
                                           worldState.environment,
                                           simConfig);
            bot->playerId = entry.actorId;
            sim::setRosterAlive(&worldState, entry.actorId, bot->health > 0.0f);
            continue;
        }
        syncBotControlStateFromAuthoritative(transportState, *bot, entry.actorId);

        const sim::PlayerState* target =
            chooseNearestOpposingPlayer(worldState, entry.actorId, entry.team);
        const int targetActorId = target != nullptr ? target->playerId : -1;
        const bool targetVisible =
            target != nullptr && hasClearLineOfSight(worldState, *bot, *target, simConfig);
        if (transportState.targetActorId != targetActorId) {
            transportState.targetActorId = targetActorId;
            transportState.visibleTargetSeconds = 0.0f;
            transportState.movementDecisionRemaining = 0.0f;
        } else if (targetVisible) {
            transportState.visibleTargetSeconds += dtSeconds;
        } else {
            transportState.visibleTargetSeconds = 0.0f;
        }

        if (transportState.movementDecisionRemaining <= 0.0f) {
            transportState.movementDecisionRemaining = uniformRange(
                transportState.rng,
                botDirectorConfig_.decisionIntervalMinSeconds,
                botDirectorConfig_.decisionIntervalMaxSeconds);
            transportState.lateralMove = uniformRange(
                transportState.rng,
                -botDirectorConfig_.lateralMoveMagnitude,
                botDirectorConfig_.lateralMoveMagnitude);
            transportState.forwardBias = uniformRange(
                transportState.rng,
                -botDirectorConfig_.forwardBiasMagnitude,
                botDirectorConfig_.forwardBiasMagnitude);
        }

        const bool readyToAttemptShot =
            targetVisible &&
            transportState.visibleTargetSeconds >= botDirectorConfig_.reactionDelaySeconds &&
            transportState.shotCooldownRemaining <= 0.0f;
        const bool canAttemptShot = botShootingEnabled_ && readyToAttemptShot;
        const bool accurateShot =
            canAttemptShot && uniformUnit(transportState.rng) <= botDirectorConfig_.accuracy;

        BotCommandSource::BotCommandContext commandContext;
        commandContext.dtSeconds = dtSeconds;
        commandContext.aim.fire = canAttemptShot;
        commandContext.movement.preferredDistance = botDirectorConfig_.preferredDistance;
        commandContext.movement.retreatDistance = botDirectorConfig_.retreatDistance;
        commandContext.movement.lateralMove = transportState.lateralMove;
        commandContext.movement.forwardBias = transportState.forwardBias;
        if (canAttemptShot && !accurateShot) {
            commandContext.aim.yawOffsetRadians = uniformRange(
                transportState.rng,
                -botDirectorConfig_.missYawJitterRadians,
                botDirectorConfig_.missYawJitterRadians);
            commandContext.aim.pitchOffsetRadians = uniformRange(
                transportState.rng,
                -botDirectorConfig_.missPitchJitterRadians,
                botDirectorConfig_.missPitchJitterRadians);
        }

        sim::PlayerCommand generatedCommand =
            botCommandSource_.buildCommand(worldState, entry.actorId, entry.team, commandContext);
        generatedCommand.viewedServerTimeUs = currentServerTimeUs;
        generatedCommand.interpDelayMs = 0u;

        auto queueCommand = [&](const sim::PlayerCommand& command) {
            if (shouldTriggerTransport(transportState.config.lossPct, transportState.rng)) {
                return;
            }

            double delayMs = static_cast<double>(transportState.config.baseDelayMs) +
                             uniformTransportJitter(transportState.rng,
                                                    transportState.config.jitterMs);
            if (delayMs < 0.0) {
                delayMs = 0.0;
            }

            std::uint64_t deliverAtUs = currentServerTimeUs + microsFromMillis(delayMs);
            if (shouldTriggerTransport(transportState.config.reorderPct, transportState.rng) &&
                !transportState.scheduled.empty()) {
                deliverAtUs = currentServerTimeUs;
            }

            transportState.scheduled.push_back(
                PendingBotCommand{command, deliverAtUs, transportState.orderCounter++});
            if (shouldTriggerTransport(transportState.config.duplicatePct, transportState.rng)) {
                transportState.scheduled.push_back(
                    PendingBotCommand{command, deliverAtUs + 1u, transportState.orderCounter++});
            }
        };

        queueCommand(generatedCommand);
        if (transportState.hasControlPlayerState) {
            sim::PlayerCommand controlCommand = generatedCommand;
            controlCommand.seq = ++transportState.lastControlSeq;
            transportState.controlPlayerState =
                sim::applyPlayerCommand(transportState.controlPlayerState,
                                        controlCommand,
                                        worldState.environment,
                                        simConfig);
            transportState.controlPlayerState.playerId = entry.actorId;
            syncBotControlVitalsFromAuthoritative(transportState, *bot);
        }

        std::stable_sort(transportState.scheduled.begin(),
                         transportState.scheduled.end(),
                         [](const PendingBotCommand& lhs, const PendingBotCommand& rhs) {
                             if (lhs.deliverAtUs != rhs.deliverAtUs) {
                                 return lhs.deliverAtUs < rhs.deliverAtUs;
                             }
                             return lhs.order < rhs.order;
                         });

        bool appliedCommand = false;
        auto scheduledIt = transportState.scheduled.begin();
        while (scheduledIt != transportState.scheduled.end() &&
               scheduledIt->deliverAtUs <= currentServerTimeUs) {
            sim::PlayerCommand deliveredCommand = scheduledIt->command;
            deliveredCommand.dtSeconds = dtSeconds;
            const sim::PlayerState beforeCommand = *bot;
            *bot = sim::applyPlayerCommand(*bot, deliveredCommand, worldState.environment, simConfig);
            bot->playerId = entry.actorId;
            sim::setRosterAlive(&worldState, entry.actorId, bot->health > 0.0f);
            appliedCommand = true;
            if (replayRecorder != nullptr) {
                sim::PlayerCommand replayCommand = deliveredCommand;
                replayCommand.seq = ++transportState.replayCommandSeq;
                replayRecorder->recordCommandApplied(
                    replay::ReplayTrack::Authoritative,
                    static_cast<std::uint16_t>(entry.actorId),
                    entry.actorId,
                    serverTick,
                    currentServerTimeUs,
                    replayCommand);
            }

            if (botShootingEnabled_ &&
                deliveredCommand.has(sim::CommandButton::Fire) &&
                sim::canFire(beforeCommand) &&
                transportState.shotCooldownRemaining <= 0.0f) {
                const std::vector<sim::HitscanTarget> evaluationTargets =
                    buildEvaluationTargetsForFire(worldState,
                                                  sessions,
                                                  botControlPlayers(worldState),
                                                  simConfig,
                                                  deliveredCommand,
                                                  shotEvaluationMode,
                                                  lagCompensation,
                                                  currentServerTimeUs);
                processFireCommand(worldState,
                                   sessions,
                                   entry.actorId,
                                   *bot,
                                   deliveredCommand,
                                   evaluationTargets,
                                   simConfig,
                                   currentServerTimeUs,
                                   shotEvaluationMode,
                                   outEvents,
                                   outStudyEvents);
                transportState.shotCooldownRemaining = botDirectorConfig_.shotCooldownSeconds;
            }
            scheduledIt = transportState.scheduled.erase(scheduledIt);
        }

        if (!appliedCommand) {
            *bot = sim::applyPlayerCommand(*bot,
                                           idleCommandFor(*bot, dtSeconds),
                                           worldState.environment,
                                           simConfig);
            bot->playerId = entry.actorId;
            sim::setRosterAlive(&worldState, entry.actorId, bot->health > 0.0f);
        }
        syncBotControlVitalsFromAuthoritative(transportState, *bot);
    }
}

void AuthoritativeSimulation::processFireCommand(sim::WorldState& worldState,
                                                 const std::vector<ClientSession>& sessions,
                                                 int shooterActorId,
                                                 const sim::PlayerState& shooterState,
                                                 const sim::PlayerCommand& command,
                                                 const std::vector<sim::HitscanTarget>& evaluationTargets,
                                                 const sim::SimConfig& simConfig,
                                                 std::uint64_t currentServerTimeUs,
                                                 ShotEvaluationMode shotEvaluationMode,
                                                 std::vector<SnapshotEvent>* outEvents,
                                                 std::vector<CombatStudyEvent>* outStudyEvents) const {
    std::vector<sim::HitscanTarget> targets = evaluationTargets;

    targets.erase(std::remove_if(targets.begin(),
                                 targets.end(),
                                 [&worldState,
                                  &sessions,
                                  shooterActorId,
                                  currentServerTimeUs](const sim::HitscanTarget& target) {
                                     return target.entityId == shooterActorId ||
                                            isSpawnProtected(sessions,
                                                             target.entityId,
                                                             currentServerTimeUs) ||
                                            sim::shouldIgnoreFriendlyFire(worldState,
                                                                          shooterActorId,
                                                                          target.entityId);
                                 }),
                  targets.end());

    const sim::HitscanRay shotRay = rayForCommand(shooterState, command, simConfig);
    const sim::FireResult fireResult = sim::traceHitscan(shotRay, targets);
    const float obstacleDistance =
        sim::traceHitscanObstacleDistance(shotRay, worldState.environment.collisionBoxes);
    const bool blockedByGeometry =
        fireResult.hit && obstacleDistance >= 0.0f && obstacleDistance < fireResult.hitDistance;
    const bool resolvedHit = fireResult.hit && !blockedByGeometry;
    const int resolvedTargetId = resolvedHit ? fireResult.hitEntityId : -1;

    if (outEvents != nullptr) {
        outEvents->push_back(
            SnapshotEvent{SnapshotEventKind::WeaponFired,
                          shooterActorId,
                          resolvedTargetId,
                          shotRay.origin,
                          shotRay.direction,
                          resolvedHit});
    }
    if (outStudyEvents != nullptr) {
        outStudyEvents->push_back(
            CombatStudyEvent{CombatStudyEventKind::ShotResolved,
                             shooterActorId,
                             resolvedTargetId,
                             command.seq,
                             command.viewedServerTimeUs,
                             command.interpDelayMs,
                             shotRay.origin,
                             shotRay.direction,
                             resolvedHit,
                             blockedByGeometry,
                             fireResult.hit ? fireResult.hitDistance : 0.0f,
                             resolvedHit ? simConfig.weaponDamage : 0.0f,
                             0.0f,
                             0.0f,
                             shotEvaluationMode});
    }

    if (!resolvedHit) {
        return;
    }

    bool killedPlayer = false;
    float targetHealthBefore = 0.0f;
    float targetHealthAfter = 0.0f;
    if (sim::PlayerState* targetPlayer = sim::findPlayer(&worldState, fireResult.hitEntityId)) {
        targetHealthBefore = targetPlayer->health;
        targetPlayer->health = std::max(0.0f,
                                        targetPlayer->health - simConfig.weaponDamage);
        targetHealthAfter = targetPlayer->health;
        sim::setRosterAlive(&worldState,
                            fireResult.hitEntityId,
                            targetPlayer->health > 0.0f);
        killedPlayer = targetHealthBefore > 0.0f && targetPlayer->health <= 0.0f;
        if (killedPlayer) {
            sim::recordKill(&worldState, shooterActorId, fireResult.hitEntityId);
        }
    } else if (sim::RemoteActorState* targetEnemy =
                   findEnemyMutable(&worldState, fireResult.hitEntityId)) {
        targetEnemy->health = std::max(0.0f,
                                       targetEnemy->health - simConfig.weaponDamage);
        if (targetEnemy->health <= 0.0f) {
            targetEnemy->alive = false;
        }
    }

    if (outEvents != nullptr) {
        outEvents->push_back(
            SnapshotEvent{SnapshotEventKind::ConfirmedHit,
                          shooterActorId,
                          fireResult.hitEntityId,
                          shotRay.origin,
                          shotRay.direction,
                          true});
        outEvents->push_back(
            SnapshotEvent{SnapshotEventKind::DamageApplied,
                          shooterActorId,
                          fireResult.hitEntityId,
                          shotRay.origin,
                          shotRay.direction,
                          true});
        if (killedPlayer) {
            outEvents->push_back(
                SnapshotEvent{SnapshotEventKind::PlayerKilled,
                              shooterActorId,
                              fireResult.hitEntityId,
                              shotRay.origin,
                              shotRay.direction,
                              true});
        }
    }
    if (outStudyEvents != nullptr && killedPlayer) {
        outStudyEvents->push_back(
            CombatStudyEvent{CombatStudyEventKind::PlayerKilled,
                             shooterActorId,
                             fireResult.hitEntityId,
                             command.seq,
                             command.viewedServerTimeUs,
                             command.interpDelayMs,
                             shotRay.origin,
                             shotRay.direction,
                             true,
                             false,
                             fireResult.hitDistance,
                             simConfig.weaponDamage,
                             targetHealthBefore,
                             targetHealthAfter,
                             shotEvaluationMode});
    }
}

bool ServerGateway::acceptClient(const HelloMessage& hello,
                                 const ServerConfig& config,
                                 const HostedSessionMetadata& hostedMetadata,
                                 const sim::SimConfig& simConfig,
                                 sim::WorldState* worldState,
                                 std::uint32_t serverTick,
                                 std::uint64_t nowUs,
                                 WelcomeMessage* welcomeOut,
                                 std::string* rejectReasonOut) {
    if (worldState == nullptr) {
        return false;
    }

    const auto existingSessionIt = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [sessionId = hello.sessionId](const ClientSession& session) {
            return session.sessionId == sessionId;
        });
    if (existingSessionIt != sessions_.end()) {
        existingSessionIt->lastHeardTimeUs = nowUs;
        if (!hello.playerName.empty()) {
            existingSessionIt->playerName = hello.playerName;
            applySessionRosterState(worldState, *existingSessionIt);
        }

        if (welcomeOut != nullptr) {
            welcomeOut->sessionId = existingSessionIt->sessionId;
            welcomeOut->assignedPeerId = existingSessionIt->peerId;
            welcomeOut->snapshotRateHz = config.snapshotRateHz;
            welcomeOut->levelSlot = config.levelSlot;
            welcomeOut->levelHash = config.levelHash;
            welcomeOut->cadence = worldState->cadence;
            welcomeOut->participantState = participantStateFor(*worldState, *existingSessionIt);
            welcomeOut->paneView = paneViewFor(*worldState, *existingSessionIt);
            welcomeOut->authoritativeTime = worldState->authoritativeTime;
            welcomeOut->sessionMetadata = hostedMetadata;
        }

        const std::vector<sim::PlayerState> serverControlPlayers;
        pendingPackets_.push_back(
            buildSnapshotPacket(config,
                                hostedMetadata,
                                *worldState,
                                serverControlPlayers,
                                serverTick,
                                *existingSessionIt,
                                nowUs));
        return true;
    }

    if (sessions_.size() >= config.maxPlayers) {
        if (rejectReasonOut != nullptr) {
            *rejectReasonOut = "server_full";
        }
        return false;
    }

    const std::uint16_t peerId = nextAvailablePeerId(config.maxPlayers);
    if (peerId == 0u) {
        if (rejectReasonOut != nullptr) {
            *rejectReasonOut = "no_peer_id_available";
        }
        return false;
    }

    ClientSession session;
    session.peerId = peerId;
    session.sessionId = hello.sessionId;
    session.team = hello.requestedTeam == sim::TeamId::Spectator
        ? sim::TeamId::Spectator
        : (sim::isPlayableTeam(hello.requestedTeam)
               ? hello.requestedTeam
               : chooseBalancedTeam(*worldState));
    session.connected = true;
    session.lastHeardTimeUs = nowUs;
    session.lastAckedInputSeq = 0;
    session.snapshotSeq = 0;
    session.lastAppliedInputSeq = 0;
    session.playerName = hello.playerName;
    session.spawnPosition = worldState->playerSpawns.at(peerId - 1);
    sessions_.push_back(session);

    if (sim::isPlayableTeam(session.team)) {
        sim::ensurePlayer(worldState, peerId, session.spawnPosition, simConfig);
        respawnSessionPlayer(worldState,
                             &sessions_.back(),
                             simConfig,
                             config.spawnProtectionSeconds,
                             nowUs);
    } else {
        applySessionRosterState(worldState, sessions_.back());
    }
    syncConnectedSessionMetadata(worldState, sessions_);

    if (welcomeOut != nullptr) {
        welcomeOut->sessionId = hello.sessionId;
        welcomeOut->assignedPeerId = peerId;
        welcomeOut->snapshotRateHz = config.snapshotRateHz;
        welcomeOut->levelSlot = config.levelSlot;
        welcomeOut->levelHash = config.levelHash;
        welcomeOut->cadence = worldState->cadence;
        welcomeOut->participantState = participantStateFor(*worldState, sessions_.back());
        welcomeOut->paneView = paneViewFor(*worldState, sessions_.back());
        welcomeOut->authoritativeTime = worldState->authoritativeTime;
        welcomeOut->sessionMetadata = hostedMetadata;
    }

    const std::vector<sim::PlayerState> serverControlPlayers;
    pendingPackets_.push_back(
        buildSnapshotPacket(config,
                            hostedMetadata,
                            *worldState,
                            serverControlPlayers,
                            serverTick,
                            sessions_.back(),
                            nowUs));
    return true;
}

bool ServerGateway::disconnectClient(std::uint16_t peerId, sim::WorldState* worldState) {
    const auto it = std::find_if(sessions_.begin(),
                                 sessions_.end(),
                                 [peerId](const ClientSession& session) { return session.peerId == peerId; });
    if (it == sessions_.end()) {
        return false;
    }

    sim::removePlayer(worldState, peerId);
    clearSessionControlState(worldState, peerId);
    sessions_.erase(it);
    syncConnectedSessionMetadata(worldState, sessions_);
    return true;
}

void ServerGateway::enqueueControlPayload(ClientSession& session, PacketPayload payload) {
    pendingPackets_.push_back(buildControlPacket(session, std::move(payload)));
}

void ServerGateway::recordSnapshotEvent(const SnapshotEvent& event) {
    pendingEvents_.push_back(event);
}

void ServerGateway::publishSnapshots(const ServerConfig& config,
                                     const HostedSessionMetadata& hostedMetadata,
                                     const sim::WorldState& worldState,
                                     const std::vector<sim::PlayerState>& serverControlPlayers,
                                     std::uint32_t serverTick,
                                     std::uint64_t nowUs) {
    for (auto& session : sessions_) {
        ++session.snapshotSeq;
        pendingPackets_.push_back(
            buildSnapshotPacket(config,
                                hostedMetadata,
                                worldState,
                                serverControlPlayers,
                                serverTick,
                                session,
                                nowUs));
    }
    pendingEvents_.clear();
}

void ServerGateway::pruneTimedOutSessions(const ServerConfig& config,
                                          sim::WorldState* worldState,
                                          std::uint64_t nowUs) {
    if (config.clientTimeoutUs == 0u) {
        return;
    }

    bool pruned = false;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (nowUs >= it->lastHeardTimeUs &&
            (nowUs - it->lastHeardTimeUs) > config.clientTimeoutUs) {
            sim::removePlayer(worldState, it->peerId);
            it = sessions_.erase(it);
            pruned = true;
        } else {
            ++it;
        }
    }
    if (pruned) {
        syncConnectedSessionMetadata(worldState, sessions_);
    }
}

std::vector<Packet> ServerGateway::takePendingPackets() {
    std::vector<Packet> packets = std::move(pendingPackets_);
    pendingPackets_.clear();
    return packets;
}

std::vector<ClientSession>& ServerGateway::sessions() {
    return sessions_;
}

const std::vector<ClientSession>& ServerGateway::sessions() const {
    return sessions_;
}

const ClientSession* ServerGateway::findSession(std::uint16_t peerId) const {
    const auto it = std::find_if(sessions_.begin(),
                                 sessions_.end(),
                                 [peerId](const ClientSession& session) { return session.peerId == peerId; });
    return it != sessions_.end() ? &(*it) : nullptr;
}

ClientSession* ServerGateway::findSessionMutable(std::uint16_t peerId) {
    const auto it = std::find_if(sessions_.begin(),
                                 sessions_.end(),
                                 [peerId](const ClientSession& session) { return session.peerId == peerId; });
    return it != sessions_.end() ? &(*it) : nullptr;
}

std::uint16_t ServerGateway::nextAvailablePeerId(std::uint16_t maxPlayers) const {
    for (std::uint16_t candidate = 1; candidate <= maxPlayers; ++candidate) {
        const bool inUse = std::any_of(sessions_.begin(),
                                       sessions_.end(),
                                       [candidate](const ClientSession& session) {
                                           return session.peerId == candidate;
                                       });
        if (!inUse) {
            return candidate;
        }
    }
    return 0u;
}

Packet ServerGateway::buildControlPacket(ClientSession& session, PacketPayload payload) {
    Packet packet;
    packet.header.peerId = session.peerId;
    packet.header.channel = Channel::Control;
    packet.header.seq = ++session.controlSeq;
    packet.header.ack = session.lastAppliedInputSeq;
    packet.header.ackBits = 0u;
    packet.header.kind = packetKindForPayload(payload);
    packet.payload = std::move(payload);
    return packet;
}

Packet ServerGateway::buildSnapshotPacket(const ServerConfig& config,
                                          const HostedSessionMetadata& hostedMetadata,
                                          const sim::WorldState& worldState,
                                          const std::vector<sim::PlayerState>& serverControlPlayers,
                                          std::uint32_t serverTick,
                                          const ClientSession& session,
                                          std::uint64_t nowUs) const {
    Packet packet;
    packet.header.peerId = session.peerId;
    packet.header.channel = Channel::Snapshot;
    packet.header.seq = session.snapshotSeq;
    packet.header.ack = session.lastAppliedInputSeq;
    packet.header.ackBits = 0u;
    packet.header.kind = PacketKind::WorldSnapshot;

    const AuthoritativeSnapshot authoritative =
        AuthoritativeSnapshot::fromSession(worldState, session, config.shotEvaluationMode);

    ReplicationSnapshot replication;
    replication.serverTick = serverTick;
    replication.serverTimeUs = nowUs;
    replication.ackedInputSeq = authoritative.ackedInputSeq;
    replication.cadence = authoritative.worldState.cadence;
    replication.authoritativeTime = authoritative.worldState.authoritativeTime;
    if (replication.authoritativeTime.serverTick == 0u) {
        replication.authoritativeTime.serverTick = serverTick;
    }
    if (replication.authoritativeTime.serverTimeUs == 0u) {
        replication.authoritativeTime.serverTimeUs = nowUs;
    }
    replication.localParticipantState = participantStateFor(worldState, session);
    replication.localPaneView = paneViewFor(worldState, session);

    const int controlledActorId = replication.localParticipantState.control.controlsActor()
        ? replication.localParticipantState.control.actorId
        : static_cast<int>(session.peerId);
    if (const sim::PlayerState* player = sim::findPlayer(authoritative.worldState, controlledActorId)) {
        replication.localPlayerState = *player;
    }
    replication.remotePlayers = buildRemotePlayers(authoritative.worldState,
                                                   controlledActorId,
                                                   static_cast<int>(session.peerId));
    replication.controlRemotePlayers = buildControlRemotePlayers(authoritative.worldState,
                                                                 sessions_,
                                                                 serverControlPlayers,
                                                                 controlledActorId,
                                                                 static_cast<int>(session.peerId));
    replication.remoteEnemies = buildRemoteEnemies(authoritative.worldState);

    SessionSummary summary;
    summary.sessionMetadata = hostedMetadata;
    summary.roster = authoritative.worldState.roster;
    for (auto& entry : summary.roster) {
        if (const sim::PlayerState* player = sim::findPlayer(worldState, entry.actorId)) {
            entry.alive = player->health > 0.0f;
        }
    }
    summary.teamScores = authoritative.worldState.teamScores;

    GameplayEventBatch gameplayEvents;
    gameplayEvents.events = pendingEvents_;

    packet.payload = WorldSnapshot::fromContracts(replication, summary, gameplayEvents);
    return packet;
}

}  // namespace server

ServerRuntime::ServerRuntime(const ServerConfig& config,
                             const sim::SimConfig& simConfig,
                             const sim::MovementEnvironment& environment)
    : config_(config),
      simConfig_(simConfig),
      worldState_(sim::createDefaultWorld(config.maxPlayers, environment, simConfig)),
      lagCompensation_(lagCompensationConfigFor(config)),
      authoritativeSimulation_(server::BotCommandSource{}, config.botDirector),
      tickIntervalUs_(intervalUsForRateHz(config.tickRateHz, kDefaultTickIntervalUs)),
      snapshotIntervalUs_(intervalUsForRateHz(config.snapshotRateHz, kDefaultSnapshotIntervalUs))
      ,
      spectatorReturnTeams_(static_cast<std::size_t>(std::max<std::uint16_t>(config.maxPlayers, 1u)) + 1u,
                            sim::TeamId::None)
{
    gSessionControlStates.erase(&worldState_);
    if (config_.maxHumanPlayers == 0u) {
        config_.maxHumanPlayers = config_.maxPlayers;
    }
    if (config_.levelHash == 0u) {
        config_.levelHash = makeLevelIdentityHash(config_.levelSlot);
    }
    if (config_.publicJoinPort == 0u) {
        config_.publicJoinPort = config_.listenPort;
    }
    if (config_.hostPlayerName.empty()) {
        config_.hostPlayerName = "player";
    }
    if (config_.studyEventLoggingEnabled && config_.studyEventRunId.empty()) {
        config_.studyEventRunId = telemetry::sanitizeRunId(
            "server_" + std::to_string(config_.listenPort));
    }

    spawnConfiguredBots();
    refreshAuthoritativeWorldStateMetadata();
    lagCompensation_.recordWorldState(worldState_, simConfig_, currentServerTimeUs_);
}

bool ServerRuntime::acceptClient(const HelloMessage& hello,
                                 std::uint64_t nowUs,
                                 WelcomeMessage* welcomeOut,
                                 std::string* rejectReasonOut) {
    currentServerTimeUs_ = nowUs;
    refreshAuthoritativeWorldStateMetadata();
    const std::size_t sessionCountBefore = gateway_.sessions().size();
    const HostedSessionMetadata hostedMetadata = hostedSessionMetadata();
    const bool accepted = gateway_.acceptClient(
        hello,
        config_,
        hostedMetadata,
        simConfig_,
        &worldState_,
        serverTick_,
        nowUs,
        welcomeOut,
        rejectReasonOut);
    if (accepted) {
        refreshAuthoritativeWorldStateMetadata();
        if (welcomeOut != nullptr) {
            if (ClientSession* session = findSessionMutable(welcomeOut->assignedPeerId)) {
                syncControlPlayerStateFromAuthoritative(*session);
            }
        }
    }
    if (accepted && gateway_.sessions().size() > sessionCountBefore) {
        lagCompensation_.recordWorldState(worldState_, simConfig_, nowUs);
    }
    return accepted;
}

bool ServerRuntime::disconnectClient(std::uint16_t peerId, const std::string&) {
    const bool disconnected = gateway_.disconnectClient(peerId, &worldState_);
    if (disconnected) {
        if (sim::TeamId* returnTeam = spectatorReturnTeam(peerId)) {
            *returnTeam = sim::TeamId::None;
        }
        refreshAuthoritativeWorldStateMetadata();
    }
    return disconnected;
}

bool ServerRuntime::handleShotEvaluationModeChange(ClientSession& session,
                                                   const RuntimeParamChangeRequest& request,
                                                   RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        ShotEvaluationMode mode;
        if (!tryParseShotEvaluationModeValue(request.value, &mode)) {
            result.message = "invalid_shot_evaluation_mode";
        } else {
            pendingShotEvaluationChange_.active = true;
            pendingShotEvaluationChange_.mode = mode;
            result.message = "staged_for_next_tick";
            accepted = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleSessionTickRateChange(ClientSession& session,
                                                const RuntimeParamChangeRequest& request,
                                                RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        std::uint16_t tickRateHz = 0u;
        if (!tryParseSessionTickRateHzValue(request.value, &tickRateHz)) {
            result.message = "invalid_tick_rate";
        } else {
            pendingSessionTickRateHz_ = tickRateHz;
            result.value = static_cast<float>(tickRateHz);
            result.message = "staged_for_next_tick";
            accepted = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleSessionSnapshotRateChange(ClientSession& session,
                                                    const RuntimeParamChangeRequest& request,
                                                    RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        std::uint16_t snapshotRateHz = 0u;
        if (!tryParseSessionSnapshotRateHzValue(request.value, &snapshotRateHz)) {
            result.message = "invalid_snapshot_rate";
        } else {
            const std::uint16_t maxSnapshotRateHz =
                pendingSessionTickRateHz_.value_or(config_.tickRateHz);
            if (snapshotRateHz > maxSnapshotRateHz) {
                result.message = "snapshot_rate_above_tickrate";
            } else {
                pendingSessionSnapshotRateHz_ = snapshotRateHz;
                result.value = static_cast<float>(snapshotRateHz);
                result.message = "staged_for_next_tick";
                accepted = true;
            }
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleStudyEventLoggingChange(ClientSession& session,
                                                  const RuntimeParamChangeRequest& request,
                                                  RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        bool enabled = false;
        if (!tryParseRuntimeBool(request.value, &enabled)) {
            result.message = "invalid_event_logging";
        } else {
            config_.studyEventLoggingEnabled = enabled;
            if (config_.studyEventRunId.empty()) {
                const std::uint16_t port = config_.publicJoinPort != 0u
                    ? config_.publicJoinPort
                    : config_.listenPort;
                config_.studyEventRunId = telemetry::sanitizeRunId(
                    "session_" + telemetry::currentLocalTimestampStamp() +
                    "_port_" + std::to_string(port));
            }
            refreshAuthoritativeWorldStateMetadata();
            result.value = enabled ? 1.0f : 0.0f;
            result.applied = true;
            result.message = enabled
                ? std::string(kEventLoggingAppliedPrefix) + config_.studyEventRunId
                : "applied";
            accepted = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleSessionVisualizationModeChange(
    ClientSession& session,
    const RuntimeParamChangeRequest& request,
    RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextSnapshot;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        SessionVisualizationMode mode;
        if (!tryParseSessionVisualizationModeValue(request.value, &mode)) {
            result.message = "invalid_visualization_mode";
        } else {
            config_.visualizationMode = mode;
            refreshAuthoritativeWorldStateMetadata();
            result.value = static_cast<float>(static_cast<std::uint8_t>(mode));
            result.applied = true;
            result.message = "applied";
            accepted = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleBotDirectorActiveChange(ClientSession& session,
                                                  const RuntimeParamChangeRequest& request,
                                                  RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        bool active = false;
        if (!tryParseRuntimeBool(request.value, &active)) {
            result.message = "invalid_bots_active";
        } else {
            authoritativeSimulation_.setBotsFrozen(!active);
            refreshAuthoritativeWorldStateMetadata();
            result.value = active ? 1.0f : 0.0f;
            result.applied = true;
            result.message = "applied";
            accepted = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleBotShootingEnabledChange(ClientSession& session,
                                                   const RuntimeParamChangeRequest& request,
                                                   RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool accepted = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        bool enabled = false;
        if (!tryParseRuntimeBool(request.value, &enabled)) {
            result.message = "invalid_bots_can_shoot";
        } else {
            authoritativeSimulation_.setBotShootingEnabled(enabled);
            refreshAuthoritativeWorldStateMetadata();
            result.value = enabled ? 1.0f : 0.0f;
            result.applied = true;
            result.message = "applied";
            accepted = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return accepted;
}

bool ServerRuntime::handleSessionAction(ClientSession& session,
                                        const SessionActionRequest& request,
                                        SessionActionResult* resultOut) {
    SessionActionResult result;
    result.kind = request.kind;
    result.applied = false;
    result.actorId = -1;
    result.message = "unsupported_action";

    const bool applied = request.kind == SessionActionKind::SpawnFrozenBotAhead
        ? spawnFrozenBotAhead(session, &result)
        : false;

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return applied;
}

bool ServerRuntime::spawnFrozenBotAhead(ClientSession& session,
                                        SessionActionResult* resultOut) {
    SessionActionResult result;
    result.kind = SessionActionKind::SpawnFrozenBotAhead;
    result.applied = false;
    result.actorId = -1;
    result.message = "rejected";

    bool applied = false;
    if (!config_.studyActionsEnabled) {
        result.message = "study_only";
    } else if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        const int controlledActorId = controlledActorIdForSession(worldState_, session);
        const sim::PlayerState* requester = sim::findPlayer(worldState_, controlledActorId);
        if (requester == nullptr || requester->health <= 0.0f) {
            result.message = "requester_unavailable";
        } else if (const std::optional<sim::Vec3> spawnPosition =
                       spawnPositionAheadOfPlayer(worldState_, *requester, simConfig_);
                   !spawnPosition.has_value()) {
            result.message = "invalid_spawn_point";
        } else {
            const float requesterYaw = requester->yaw;
            const int actorId = nextAvailableBotActorId(worldState_);
            const sim::TeamId botTeam = session.team == sim::TeamId::Attacker
                ? sim::TeamId::Defender
                : sim::TeamId::Attacker;

            sim::ensurePlayer(&worldState_, actorId, *spawnPosition, simConfig_);
            if (sim::PlayerState* bot = sim::findPlayer(&worldState_, actorId)) {
                bot->position = *spawnPosition;
                bot->yaw = requesterYaw + 3.14159265358979323846f;
                bot->pitch = 0.0f;
                bot->health = bot->maxHealth;
                bot->weaponCooldownRemaining = 0.0f;
                bot->grounded = true;
            }
            if (sim::RosterEntry* rosterEntry = sim::findRosterEntry(&worldState_, actorId)) {
                rosterEntry->team = botTeam;
                rosterEntry->sessionPresence = sim::SessionPresence::Connected;
                rosterEntry->participation = sim::ParticipationState::Playing;
                rosterEntry->control =
                    sim::ControlBinding{sim::ControlBindingKind::Actor, actorId};
                rosterEntry->isBot = true;
                rosterEntry->alive = true;
                rosterEntry->displayName = "Frozen BOT " + std::to_string(actorId);
            }
            authoritativeSimulation_.setBotFrozenPassive(actorId, true);
            refreshAuthoritativeWorldStateMetadata();
            lagCompensation_.recordWorldState(worldState_, simConfig_, currentServerTimeUs_);

            result.applied = true;
            result.actorId = actorId;
            result.message = "spawned";
            applied = true;
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return applied;
}

bool ServerRuntime::handleHostAdminTeamChange(ClientSession& session,
                                              const RuntimeParamChangeRequest& request,
                                              RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool applied = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else if (request.targetId <= 0 ||
               request.targetId > static_cast<std::int32_t>(
                   std::numeric_limits<std::uint16_t>::max())) {
        result.message = "target_missing";
    } else if (request.targetId == static_cast<std::int32_t>(session.peerId)) {
        result.message = "self_rejected";
    } else {
        sim::TeamId requestedTeam = sim::TeamId::None;
        ClientSession* targetSession =
            findSessionMutable(static_cast<std::uint16_t>(request.targetId));
        if (!tryParseAdminTeam(request.value, &requestedTeam)) {
            result.message = "invalid_team";
        } else if (targetSession != nullptr) {
            applied = handleTeamChangeRequest(*targetSession,
                                              TeamChangeRequest{requestedTeam});
            result.applied = applied;
            result.message = applied ? "applied" : "unchanged";
        } else {
            sim::RosterEntry* rosterEntry = sim::findRosterEntry(&worldState_, request.targetId);
            if (rosterEntry == nullptr || !rosterEntry->isBot) {
                result.message = "target_missing";
            } else if (!sim::isPlayableTeam(requestedTeam)) {
                result.message = "invalid_team";
            } else if (rosterEntry->team == requestedTeam) {
                result.message = "unchanged";
            } else {
                rosterEntry->team = requestedTeam;
                rosterEntry->participation = sim::ParticipationState::Playing;
                rosterEntry->sessionPresence = sim::SessionPresence::Connected;
                if (sim::PlayerState* bot = sim::findPlayer(&worldState_, request.targetId)) {
                    bot->yaw = defaultYawForTeam(requestedTeam);
                    if (bot->health <= 0.0f) {
                        bot->health = bot->maxHealth;
                        bot->velocity = sim::Vec3{};
                        rosterEntry->alive = true;
                    }
                }
                refreshAuthoritativeWorldStateMetadata();
                applied = true;
                result.applied = true;
                result.message = "applied";
            }
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return applied;
}

bool ServerRuntime::handleHostAdminAddBot(ClientSession& session,
                                          const RuntimeParamChangeRequest& request,
                                          RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool applied = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else {
        sim::TeamId requestedTeam = sim::TeamId::None;
        const sim::TeamId botTeam = tryParseAdminTeam(request.value, &requestedTeam) &&
                                            sim::isPlayableTeam(requestedTeam)
                                        ? requestedTeam
                                        : chooseBalancedTeam(worldState_);
        const int actorId = nextAvailableBotActorId(worldState_);
        std::mt19937 botSpawnRng = makeBotSpawnRng(config_);
        const sim::Vec3 spawnPosition =
            randomBotSpawnPositionFor(botTeam, worldState_, simConfig_, botSpawnRng)
                .value_or(botSpawnPositionFor(botTeam,
                                              countTeamMembers(worldState_, botTeam),
                                              worldState_.environment,
                                              simConfig_));

        sim::ensurePlayer(&worldState_, actorId, spawnPosition, simConfig_);
        if (sim::PlayerState* bot = sim::findPlayer(&worldState_, actorId)) {
            bot->position = spawnPosition;
            bot->yaw = defaultYawForTeam(botTeam);
            bot->pitch = 0.0f;
            bot->health = bot->maxHealth;
            bot->weaponCooldownRemaining = 0.0f;
            bot->velocity = sim::Vec3{};
            bot->grounded = true;
        }
        if (sim::RosterEntry* rosterEntry = sim::findRosterEntry(&worldState_, actorId)) {
            rosterEntry->team = botTeam;
            rosterEntry->sessionPresence = sim::SessionPresence::Connected;
            rosterEntry->participation = sim::ParticipationState::Playing;
            rosterEntry->control = sim::ControlBinding{sim::ControlBindingKind::Actor, actorId};
            rosterEntry->isBot = true;
            rosterEntry->alive = true;
            rosterEntry->displayName = "BOT " + std::to_string(actorId);
        }

        authoritativeSimulation_.setBotSpawnPosition(actorId, spawnPosition);
        refreshAuthoritativeWorldStateMetadata();
        lagCompensation_.recordWorldState(worldState_, simConfig_, currentServerTimeUs_);
        result.targetId = actorId;
        result.value = static_cast<float>(static_cast<std::uint8_t>(botTeam));
        result.applied = true;
        result.message = "applied";
        applied = true;
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return applied;
}

bool ServerRuntime::handleHostAdminKick(ClientSession& session,
                                        const RuntimeParamChangeRequest& request,
                                        RuntimeParamApplyResult* resultOut) {
    RuntimeParamApplyResult result;
    result.scope = request.scope;
    result.targetId = request.targetId;
    result.key = request.key;
    result.value = request.value;
    result.applied = false;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "rejected";

    bool applied = false;
    if (session.peerId != kAuthoritativeHostPeerId) {
        result.message = "host_only";
    } else if (request.targetId <= 0 ||
               request.targetId > static_cast<std::int32_t>(
                   std::numeric_limits<std::uint16_t>::max())) {
        result.message = "target_missing";
    } else if (request.targetId == static_cast<std::int32_t>(session.peerId)) {
        result.message = "self_rejected";
    } else {
        ClientSession* targetSession =
            findSessionMutable(static_cast<std::uint16_t>(request.targetId));
        if (targetSession == nullptr) {
            sim::RosterEntry* rosterEntry = sim::findRosterEntry(&worldState_, request.targetId);
            if (rosterEntry == nullptr || !rosterEntry->isBot) {
                result.message = "target_missing";
            } else {
                sim::removePlayer(&worldState_, request.targetId);
                refreshAuthoritativeWorldStateMetadata();
                lagCompensation_.recordWorldState(worldState_,
                                                  simConfig_,
                                                  currentServerTimeUs_);
                applied = true;
                result.applied = true;
                result.message = "applied";
            }
        } else {
            gateway_.enqueueControlPayload(*targetSession,
                                           DisconnectMessage{2u, "kicked by host"});
            applied = disconnectClient(targetSession->peerId, "kicked by host");
            result.applied = applied;
            result.message = applied ? "applied" : "target_missing";
        }
    }

    if (resultOut != nullptr) {
        *resultOut = result;
    }
    return applied;
}

bool ServerRuntime::handleControlPayload(std::uint16_t peerId,
                                         const PacketPayload& payload,
                                         std::uint64_t nowUs) {
    currentServerTimeUs_ = nowUs;
    ClientSession* session = findSessionMutable(peerId);
    if (session == nullptr) {
        return false;
    }

    session->lastHeardTimeUs = nowUs;
    return std::visit([this, session](const auto& message) {
        using T = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<T, TeamChangeRequest>) {
            return handleTeamChangeRequest(*session, message);
        } else if constexpr (std::is_same_v<T, RuntimeParamChangeRequest>) {
            RuntimeParamApplyResult result;
            const bool applied =
                message.scope == RuntimeParamScope::Session && message.key == "sv.shot_mode"
                    ? handleShotEvaluationModeChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.tickrate"
                        ? handleSessionTickRateChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.snapshot_rate"
                        ? handleSessionSnapshotRateChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.event_logging"
                        ? handleStudyEventLoggingChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.visualization_mode"
                        ? handleSessionVisualizationModeChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.bots_active"
                        ? handleBotDirectorActiveChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.bots_can_shoot"
                        ? handleBotShootingEnabledChange(*session, message, &result)
                    : message.scope == RuntimeParamScope::Session && message.key == "sv.admin_add_bot"
                        ? handleHostAdminAddBot(*session, message, &result)
                    : runtimeRequestTargetsActorKey(message, "admin_team")
                        ? handleHostAdminTeamChange(*session, message, &result)
                    : runtimeRequestTargetsActorKey(message, "admin_kick")
                        ? handleHostAdminKick(*session, message, &result)
                    : authoritativeSimulation_.applyRuntimeParamChangeRequest(
                          worldState_, gateway_.sessions(), *session, message, &result);
            gateway_.enqueueControlPayload(*session, result);
            return applied;
        } else if constexpr (std::is_same_v<T, SessionActionRequest>) {
            SessionActionResult result;
            const bool applied = handleSessionAction(*session, message, &result);
            gateway_.enqueueControlPayload(*session, result);
            return applied;
        } else {
            return false;
        }
    }, payload);
}

bool ServerRuntime::enqueueCommandBundle(std::uint16_t peerId,
                                         const CommandBundle& bundle,
                                         std::uint64_t nowUs) {
    ClientSession* session = findSessionMutable(peerId);
    if (session == nullptr) {
        return false;
    }

    session->lastHeardTimeUs = nowUs;
    for (const auto& command : bundle.commands) {
        if (!shouldAcceptGameplayCommand(*session, command)) {
            continue;
        }
        if (commandReplayRecorder_.recording()) {
            commandReplayRecorder_.recordCommandReceived(replay::ReplayTrack::Authoritative,
                                                         peerId,
                                                         serverTick_,
                                                         nowUs,
                                                         command);
        }
        session->pendingCommands.emplace(command.seq, command);
    }
    return true;
}

bool ServerRuntime::enqueueControlCommandBundle(std::uint16_t peerId,
                                                const ControlCommandBundle& bundle,
                                                std::uint64_t nowUs) {
    ClientSession* session = findSessionMutable(peerId);
    if (session == nullptr) {
        return false;
    }

    session->lastHeardTimeUs = nowUs;
    for (const auto& command : bundle.commands) {
        if (!shouldAcceptControlGameplayCommand(*session, command)) {
            continue;
        }
        if (commandReplayRecorder_.recording()) {
            commandReplayRecorder_.recordCommandReceived(replay::ReplayTrack::Control,
                                                         peerId,
                                                         serverTick_,
                                                         nowUs,
                                                         command);
        }
        session->pendingControlCommands.emplace(command.seq, command);
    }
    return true;
}

void ServerRuntime::syncControlPlayerStateFromAuthoritative(ClientSession& session) {
    const int controlledActorId = controlledActorIdForSession(worldState_, session);
    const sim::PlayerState* authoritativePlayer =
        sim::findPlayer(&worldState_, controlledActorId);
    if (!sim::isPlayableTeam(session.team) || authoritativePlayer == nullptr) {
        session.pendingControlCommands.clear();
        session.hasControlPlayerState = false;
        return;
    }

    const bool needsReset =
        !session.hasControlPlayerState ||
        session.controlPlayerState.playerId != controlledActorId ||
        authoritativePlayer->health <= 0.0f ||
        session.controlPlayerState.health <= 0.0f ||
        (session.lastAppliedControlSeq == 0u && session.pendingControlCommands.empty());
    if (!needsReset) {
        return;
    }

    session.controlPlayerState = *authoritativePlayer;
    session.controlPlayerState.playerId = controlledActorId;
    session.pendingControlCommands.clear();
    session.lastAppliedControlSeq = session.lastAppliedInputSeq;
    session.hasControlPlayerState = true;
}

void ServerRuntime::syncControlGhostStates(float dtSeconds) {
    for (ClientSession& session : gateway_.sessions()) {
        syncControlPlayerStateFromAuthoritative(session);
        if (!session.hasControlPlayerState) {
            continue;
        }

        if (session.pendingControlCommands.empty()) {
            session.controlPlayerState = sim::applyPlayerCommand(session.controlPlayerState,
                                                                 idleCommandFor(session.controlPlayerState,
                                                                                dtSeconds),
                                                                 worldState_.environment,
                                                                 simConfig_);
            continue;
        }

        for (auto it = session.pendingControlCommands.begin();
             it != session.pendingControlCommands.end();) {
            if (!isNewerSequence(it->first, session.lastAppliedControlSeq)) {
                it = session.pendingControlCommands.erase(it);
                continue;
            }

            session.controlPlayerState =
                sim::applyPlayerCommand(session.controlPlayerState,
                                        it->second,
                                        worldState_.environment,
                                        simConfig_);
            session.controlPlayerState.playerId =
                controlledActorIdForSession(worldState_, session);
            if (commandReplayRecorder_.recording()) {
                commandReplayRecorder_.recordCommandApplied(replay::ReplayTrack::Control,
                                                            session.peerId,
                                                            session.controlPlayerState.playerId,
                                                            serverTick_,
                                                            currentServerTimeUs_,
                                                            it->second);
            }
            session.lastAppliedControlSeq = it->second.seq;
            it = session.pendingControlCommands.erase(it);
        }

        if (const sim::PlayerState* authoritativePlayer =
                sim::findPlayer(&worldState_, session.controlPlayerState.playerId);
            authoritativePlayer != nullptr) {
            session.controlPlayerState.health = authoritativePlayer->health;
            session.controlPlayerState.maxHealth = authoritativePlayer->maxHealth;
        }
    }
}

void ServerRuntime::tickOnce(std::uint64_t nowUs) {
    const std::uint64_t tickIntervalUs = tickIntervalUs_;
    currentServerTimeUs_ = nowUs != 0 ? nowUs : currentServerTimeUs_ + tickIntervalUs;
    ++serverTick_;
    if (pendingShotEvaluationChange_.active) {
        config_.shotEvaluationMode = pendingShotEvaluationChange_.mode;
        pendingShotEvaluationChange_.active = false;
    }

    const float dtSeconds = static_cast<float>(tickIntervalUs) / 1'000'000.0f;
    refreshAuthoritativeWorldStateMetadata();
    server::StepContext stepContext;
    stepContext.dtSeconds = dtSeconds;
    stepContext.respawnDelaySeconds = config_.respawnDelaySeconds;
    stepContext.spawnProtectionSeconds = config_.spawnProtectionSeconds;
    stepContext.shotEvaluationMode = config_.shotEvaluationMode;
    stepContext.cadence = worldState_.cadence;
    stepContext.authoritativeTime = worldState_.authoritativeTime;
    std::vector<SnapshotEvent> events;
    std::vector<server::CombatStudyEvent> studyEvents;
    authoritativeSimulation_.stepWorld(worldState_,
                                       gateway_.sessions(),
                                       stepContext,
                                       simConfig_,
                                       lagCompensation_,
                                       &events,
                                       &studyEvents,
                                       commandReplayRecorder_.recording()
                                           ? &commandReplayRecorder_
                                           : nullptr);
    for (const auto& event : events) {
        gateway_.recordSnapshotEvent(event);
        if (commandReplayRecorder_.recording()) {
            commandReplayRecorder_.recordCombatEvent(serverTick_,
                                                     currentServerTimeUs_,
                                                     event);
        }
    }
    for (const server::CombatStudyEvent& event : studyEvents) {
        recordCombatStudyEvent(event);
    }
    syncControlGhostStates(dtSeconds);
    lagCompensation_.recordWorldState(worldState_, simConfig_, currentServerTimeUs_);
    gateway_.pruneTimedOutSessions(config_, &worldState_, currentServerTimeUs_);
    refreshAuthoritativeWorldStateMetadata();

    snapshotAccumulatorUs_ += tickIntervalUs;
    if (snapshotAccumulatorUs_ >= snapshotIntervalUs_) {
        const std::vector<sim::PlayerState> serverControlPlayers =
            authoritativeSimulation_.botControlPlayers(worldState_);
        gateway_.publishSnapshots(config_,
                                  hostedSessionMetadata(),
                                  worldState_,
                                  serverControlPlayers,
                                  serverTick_,
                                  currentServerTimeUs_);
        if (commandReplayRecorder_.recording()) {
            commandReplayRecorder_.recordAuthoritativeKeyframe(serverTick_,
                                                               currentServerTimeUs_,
                                                               worldState_);
            commandReplayRecorder_.recordControlKeyframe(
                serverTick_,
                currentServerTimeUs_,
                worldState_,
                buildControlReplayPlayers(worldState_,
                                          gateway_.sessions(),
                                          serverControlPlayers));
        }
        snapshotAccumulatorUs_ %= snapshotIntervalUs_;
    }

    if (pendingSessionTickRateHz_.has_value()) {
        // Commit cadence changes after the current tick completes so dt, scheduling,
        // and replicated cadence metadata move to the new rate together.
        applyTickRateHz(*pendingSessionTickRateHz_);
        pendingSessionTickRateHz_.reset();
        refreshAuthoritativeWorldStateMetadata();
    }
    if (pendingSessionSnapshotRateHz_.has_value()) {
        applySnapshotRateHz(*pendingSessionSnapshotRateHz_);
        pendingSessionSnapshotRateHz_.reset();
        refreshAuthoritativeWorldStateMetadata();
    }
}

std::vector<Packet> ServerRuntime::takePendingPackets() {
    return gateway_.takePendingPackets();
}

bool ServerRuntime::ensureStudyEventSink() {
    if (!config_.studyEventLoggingEnabled) {
        return false;
    }
    if (studyEventSink_ != nullptr) {
        return true;
    }

    if (config_.studyEventRunId.empty()) {
        config_.studyEventRunId = "server_" + std::to_string(config_.listenPort);
    }
    config_.studyEventRunId = telemetry::sanitizeRunId(config_.studyEventRunId);
    const std::filesystem::path runDirectory =
        config_.studyEventLogDirectory.empty()
            ? telemetry::defaultStudyEventRunDirectory(config_.studyEventRunId)
            : config_.studyEventLogDirectory;
    auto writer = std::make_unique<telemetry::JsonlStudyEventWriter>(
        runDirectory / "events_server.jsonl");
    if (!writer->isOpen()) {
        return false;
    }

    studyEventSink_ = std::move(writer);
    return true;
}

telemetry::StudyEventRecord ServerRuntime::makeServerStudyEvent(const std::string& eventName) {
    telemetry::StudyEventRecord record;
    record.add("schema_version", std::int32_t{1})
          .add("event_name", eventName)
          .add("source", "server")
          .add("session_id", config_.studyEventRunId)
          .add("event_seq", ++studyEventSeq_)
          .add("server_tick", serverTick_)
          .add("server_time_us", currentServerTimeUs_)
          .add("tick_rate_hz", config_.tickRateHz)
          .add("snapshot_rate_hz", config_.snapshotRateHz);
    return record;
}

void ServerRuntime::recordCombatStudyEvent(const server::CombatStudyEvent& event) {
    if (!ensureStudyEventSink()) {
        return;
    }

    const sim::RosterEntry* shooterEntry = sim::findRosterEntry(worldState_, event.shooterActorId);
    const sim::RosterEntry* targetEntry = sim::findRosterEntry(worldState_, event.targetActorId);
    telemetry::StudyEventRecord record = makeServerStudyEvent(
        event.kind == server::CombatStudyEventKind::PlayerKilled
            ? "combat.player_killed"
            : "combat.shot_resolved");
    record.add("shooter_actor_id", event.shooterActorId)
          .add("target_actor_id", event.targetActorId)
          .add("shooter_team_id",
               shooterEntry != nullptr ? static_cast<std::int32_t>(shooterEntry->team)
                                       : static_cast<std::int32_t>(sim::TeamId::None))
          .add("target_team_id",
               targetEntry != nullptr ? static_cast<std::int32_t>(targetEntry->team)
                                      : static_cast<std::int32_t>(sim::TeamId::None))
          .add("shooter_team",
               shooterEntry != nullptr ? sim::toString(shooterEntry->team)
                                       : sim::toString(sim::TeamId::None))
          .add("target_team",
               targetEntry != nullptr ? sim::toString(targetEntry->team)
                                      : sim::toString(sim::TeamId::None))
          .add("shooter_is_bot", shooterEntry != nullptr && shooterEntry->isBot)
          .add("target_is_bot", targetEntry != nullptr && targetEntry->isBot)
          .add("command_seq", event.commandSeq)
          .add("command_viewed_server_time_us", event.commandViewedServerTimeUs)
          .add("command_interp_delay_ms", event.commandInterpolationDelayMs)
          .add("shot_evaluation_mode", toString(event.shotEvaluationMode))
          .add("shot_hit", event.hit)
          .add("blocked_by_geometry", event.blockedByGeometry)
          .add("hit_distance_m", event.hitDistance)
          .add("damage", event.damage)
          .add("target_health_before", event.targetHealthBefore)
          .add("target_health_after", event.targetHealthAfter)
          .add("shot_origin_x", event.shotOrigin.x)
          .add("shot_origin_y", event.shotOrigin.y)
          .add("shot_origin_z", event.shotOrigin.z)
          .add("shot_direction_x", event.shotDirection.x)
          .add("shot_direction_y", event.shotDirection.y)
          .add("shot_direction_z", event.shotDirection.z);
    studyEventSink_->write(record);
}

const ServerConfig& ServerRuntime::config() const {
    return config_;
}

std::uint64_t ServerRuntime::tickIntervalUs() const {
    return tickIntervalUs_;
}

std::uint64_t ServerRuntime::snapshotIntervalUs() const {
    return snapshotIntervalUs_;
}

const sim::WorldState& ServerRuntime::worldState() const {
    return worldState_;
}

sim::WorldState& ServerRuntime::worldState() {
    return worldState_;
}

const std::vector<ClientSession>& ServerRuntime::sessions() const {
    return gateway_.sessions();
}

const ClientSession* ServerRuntime::findSession(std::uint16_t peerId) const {
    return gateway_.findSession(peerId);
}

void ServerRuntime::setCommandReplayRecordingEnabled(bool enabled) {
    if (enabled == commandReplayRecorder_.recording()) {
        return;
    }

    if (!enabled) {
        commandReplayRecorder_.stop(currentServerTimeUs_);
        return;
    }

    refreshAuthoritativeWorldStateMetadata();
    replay::ReplayRecordingInfo info;
    info.protocolVersion = kProtocolVersion;
    info.levelSlot = config_.levelSlot;
    info.levelHash = config_.levelHash;
    info.tickRateHz = config_.tickRateHz;
    info.snapshotRateHz = config_.snapshotRateHz;
    info.maxRewindMs = config_.maxRewindMs;
    info.respawnDelaySeconds = config_.respawnDelaySeconds;
    info.spawnProtectionSeconds = config_.spawnProtectionSeconds;
    info.shotEvaluationMode = config_.shotEvaluationMode;
    info.startedServerTimeUs = currentServerTimeUs_;
    info.title = config_.sessionLabel.empty() ? "Command Replay" : config_.sessionLabel;
    info.sourceLabel = config_.hostPlayerName;
    commandReplayRecorder_.start(info, worldState_, simConfig_);
    commandReplayRecorder_.recordAuthoritativeKeyframe(serverTick_,
                                                       currentServerTimeUs_,
                                                       worldState_);
    const std::vector<sim::PlayerState> serverControlPlayers =
        authoritativeSimulation_.botControlPlayers(worldState_);
    commandReplayRecorder_.recordControlKeyframe(serverTick_,
                                                 currentServerTimeUs_,
                                                 worldState_,
                                                 buildControlReplayPlayers(worldState_,
                                                                           gateway_.sessions(),
                                                                           serverControlPlayers));
}

bool ServerRuntime::commandReplayRecordingEnabled() const {
    return commandReplayRecorder_.recording();
}

const replay::ReplayDemo& ServerRuntime::commandReplayDemo() const {
    return commandReplayRecorder_.demo();
}

ClientSession* ServerRuntime::findSessionMutable(std::uint16_t peerId) {
    return gateway_.findSessionMutable(peerId);
}

bool ServerRuntime::shouldAcceptGameplayCommand(const ClientSession& session,
                                                const sim::PlayerCommand& command) const {
    const sim::ParticipantState participant = participantStateFor(worldState_, session);
    if (participant.participation != sim::ParticipationState::Playing ||
        !participant.control.controlsActor()) {
        return false;
    }

    if (command.seq == 0u ||
        session.pendingCommands.find(command.seq) != session.pendingCommands.end() ||
        !isNewerSequence(command.seq, session.lastAppliedInputSeq)) {
        return false;
    }

    if (!isFiniteGameplayValue(command.dtSeconds) ||
        command.dtSeconds < 0.0f ||
        command.dtSeconds > 0.25f ||
        !isFiniteGameplayValue(command.moveX) ||
        !isFiniteGameplayValue(command.moveY) ||
        std::fabs(command.moveX) > 1.0f ||
        std::fabs(command.moveY) > 1.0f ||
        !isFiniteGameplayValue(command.yaw) ||
        !isFiniteGameplayValue(command.pitch)) {
        return false;
    }

    if ((command.buttons & ~kAllowedGameplayButtons) != 0u ||
        command.requestedTeam != sim::TeamId::None) {
        return false;
    }

    return true;
}

bool ServerRuntime::shouldAcceptControlGameplayCommand(const ClientSession& session,
                                                       const sim::PlayerCommand& command) const {
    const sim::ParticipantState participant = participantStateFor(worldState_, session);
    if (participant.participation != sim::ParticipationState::Playing ||
        !participant.control.controlsActor()) {
        return false;
    }

    if (command.seq == 0u ||
        session.pendingControlCommands.find(command.seq) != session.pendingControlCommands.end() ||
        !isNewerSequence(command.seq, session.lastAppliedControlSeq)) {
        return false;
    }

    if (!isFiniteGameplayValue(command.dtSeconds) ||
        command.dtSeconds < 0.0f ||
        command.dtSeconds > 0.25f ||
        !isFiniteGameplayValue(command.moveX) ||
        !isFiniteGameplayValue(command.moveY) ||
        std::fabs(command.moveX) > 1.0f ||
        std::fabs(command.moveY) > 1.0f ||
        !isFiniteGameplayValue(command.yaw) ||
        !isFiniteGameplayValue(command.pitch)) {
        return false;
    }

    if ((command.buttons & ~kAllowedGameplayButtons) != 0u ||
        command.requestedTeam != sim::TeamId::None) {
        return false;
    }

    return true;
}

bool ServerRuntime::handleTeamChangeRequest(ClientSession& session,
                                            const TeamChangeRequest& request) {
    auto applyEvents = [this](const std::vector<SnapshotEvent>& events) {
        for (const auto& event : events) {
            gateway_.recordSnapshotEvent(event);
        }
    };

    if (request.requestedTeam == sim::TeamId::Spectator) {
        if (session.team == sim::TeamId::Spectator) {
            return false;
        }

        if (sim::TeamId* returnTeam = spectatorReturnTeam(session.peerId)) {
            *returnTeam = sim::isPlayableTeam(session.team) ? session.team : sim::TeamId::None;
        }

        session.pendingCommands.clear();
        session.pendingControlCommands.clear();
        session.hasControlPlayerState = false;
        session.team = sim::TeamId::Spectator;
        sim::removePlayer(&worldState_, session.peerId);
        applySessionRosterState(&worldState_, session);
        SessionControlState& controlState = ensureSessionControlState(worldState_, session);
        controlState.controlActorId = -1;
        controlState.followTargetActorId =
            firstSpectatorTargetActorId(worldState_, static_cast<int>(session.peerId));
        controlState.paneMode = controlState.followTargetActorId >= 0
            ? sim::PaneViewMode::SpectatorFollowThirdPerson
            : sim::PaneViewMode::SpectatorFreeFly;
        refreshAuthoritativeWorldStateMetadata();
        return true;
    }

    if (request.requestedTeam == sim::TeamId::None && session.team == sim::TeamId::Spectator) {
        const sim::TeamId restoredTeam = [this, &session]() {
            if (const sim::TeamId* returnTeam = spectatorReturnTeam(session.peerId);
                returnTeam != nullptr && sim::isPlayableTeam(*returnTeam)) {
                return *returnTeam;
            }
            return sim::TeamId::None;
        }();

        if (sim::isPlayableTeam(restoredTeam)) {
            std::vector<SnapshotEvent> events;
            const bool applied = authoritativeSimulation_.applyTeamChangeRequest(
                worldState_,
                session,
                TeamChangeRequest{restoredTeam},
                simConfig_,
                &events);
            applyEvents(events);
            if (applied) {
                session.hasControlPlayerState = false;
                session.spawnProtectionUntilUs = config_.spawnProtectionSeconds > 0.0f
                    ? currentServerTimeUs_ + static_cast<std::uint64_t>(std::llround(
                          static_cast<double>(config_.spawnProtectionSeconds) * 1'000'000.0))
                    : 0u;
                SessionControlState& controlState = ensureSessionControlState(worldState_, session);
                controlState.controlActorId = static_cast<int>(session.peerId);
                controlState.followTargetActorId = static_cast<int>(session.peerId);
                controlState.paneMode = sim::PaneViewMode::PlayerControlled;
                refreshAuthoritativeWorldStateMetadata();
            }
            return applied;
        }

        session.pendingCommands.clear();
        session.pendingControlCommands.clear();
        session.hasControlPlayerState = false;
        session.team = sim::TeamId::None;
        sim::removePlayer(&worldState_, session.peerId);
        applySessionRosterState(&worldState_, session);
        refreshAuthoritativeWorldStateMetadata();
        return true;
    }

    std::vector<SnapshotEvent> events;
    const bool applied = authoritativeSimulation_.applyTeamChangeRequest(
        worldState_, session, request, simConfig_, &events);
    applyEvents(events);
    if (applied) {
        session.hasControlPlayerState = false;
        session.spawnProtectionUntilUs = config_.spawnProtectionSeconds > 0.0f
            ? currentServerTimeUs_ + static_cast<std::uint64_t>(std::llround(
                  static_cast<double>(config_.spawnProtectionSeconds) * 1'000'000.0))
            : 0u;
        SessionControlState& controlState = ensureSessionControlState(worldState_, session);
        controlState.controlActorId = static_cast<int>(session.peerId);
        controlState.followTargetActorId = static_cast<int>(session.peerId);
        controlState.paneMode = sim::PaneViewMode::PlayerControlled;
        if (sim::TeamId* returnTeam = spectatorReturnTeam(session.peerId);
            returnTeam != nullptr && sim::isPlayableTeam(request.requestedTeam)) {
            *returnTeam = request.requestedTeam;
        }
        refreshAuthoritativeWorldStateMetadata();
    }
    return applied;
}

void ServerRuntime::applyTickRateHz(std::uint16_t tickRateHz) {
    config_.tickRateHz = tickRateHz;
    if (config_.snapshotRateHz > config_.tickRateHz) {
        config_.snapshotRateHz = config_.tickRateHz;
        snapshotIntervalUs_ = intervalUsForRateHz(config_.snapshotRateHz, kDefaultSnapshotIntervalUs);
    }
    tickIntervalUs_ = intervalUsForRateHz(config_.tickRateHz, kDefaultTickIntervalUs);
    lagCompensation_.reconfigure(lagCompensationConfigFor(config_), currentServerTimeUs_);
}

void ServerRuntime::applySnapshotRateHz(std::uint16_t snapshotRateHz) {
    config_.snapshotRateHz = std::min(snapshotRateHz, config_.tickRateHz);
    snapshotIntervalUs_ = intervalUsForRateHz(config_.snapshotRateHz, kDefaultSnapshotIntervalUs);
}

sim::TeamId* ServerRuntime::spectatorReturnTeam(std::uint16_t peerId) {
    if (peerId >= spectatorReturnTeams_.size()) {
        return nullptr;
    }
    return &spectatorReturnTeams_[peerId];
}

void ServerRuntime::refreshAuthoritativeWorldStateMetadata() {
    worldState_.cadence = timingCadenceFor(config_);
    worldState_.authoritativeTime.serverTick = serverTick_;
    worldState_.authoritativeTime.serverTimeUs = currentServerTimeUs_;
    worldState_.authoritativeTime.viewedServerTimeUs = currentServerTimeUs_;
    worldState_.sessionMetadata.levelSlot = config_.levelSlot;
    worldState_.sessionMetadata.levelHash = config_.levelHash;
    worldState_.sessionMetadata.maxHumanPlayers = config_.maxHumanPlayers;
    syncConnectedSessionMetadata(&worldState_, gateway_.sessions());
}

HostedSessionMetadata ServerRuntime::hostedSessionMetadata() const {
    return makeHostedSessionMetadata(config_,
                                     worldState_.sessionMetadata,
                                     authoritativeSimulation_.botsFrozen(),
                                     authoritativeSimulation_.botShootingEnabled());
}

void ServerRuntime::spawnConfiguredBots() {
    std::size_t nextBotIndex = 0u;
    std::mt19937 botSpawnRng = makeBotSpawnRng(config_);
    if (config_.suppressDefaultEnemiesForAuthoredLevel ||
        !config_.authoredBotSpawns.empty()) {
        worldState_.enemies.clear();
        worldState_.enemySpawns.clear();
        worldState_.enemyWaypointIndices.clear();
        worldState_.enemyRespawnTimers.clear();
    }

    auto spawnBot = [this, &nextBotIndex](
                       sim::TeamId team,
                       const sim::Vec3& requestedSpawnPosition) {
        const int actorId = static_cast<int>(kFirstBotTransportTargetId) +
                            static_cast<int>(nextBotIndex++);
        sim::Vec3 spawnPosition = requestedSpawnPosition;
        spawnPosition = sim::clampToArena(spawnPosition, worldState_.environment.arenaHalfSize);
        spawnPosition = sim::resolveCollisions(spawnPosition,
                                               spawnPosition,
                                               simConfig_.playerRadius,
                                               simConfig_.playerCollisionHeight,
                                               worldState_.environment.collisionBoxes);
        spawnPosition.y = sim::getGroundHeightAt(spawnPosition,
                                                 worldState_.environment.collisionBoxes) +
                          simConfig_.playerEyeHeight;

        sim::ensurePlayer(&worldState_, actorId, spawnPosition, simConfig_);
        if (sim::PlayerState* bot = sim::findPlayer(&worldState_, actorId)) {
            bot->position = spawnPosition;
            bot->yaw = defaultYawForTeam(team);
            bot->pitch = 0.0f;
            bot->health = bot->maxHealth;
            bot->weaponCooldownRemaining = 0.0f;
            bot->grounded = true;
        }

        if (sim::RosterEntry* rosterEntry = sim::findRosterEntry(&worldState_, actorId)) {
            rosterEntry->team = team;
            rosterEntry->sessionPresence = sim::SessionPresence::Connected;
            rosterEntry->participation = sim::ParticipationState::Playing;
            rosterEntry->control = sim::ControlBinding{sim::ControlBindingKind::Actor, actorId};
            rosterEntry->isBot = true;
            rosterEntry->alive = true;
            rosterEntry->displayName = "BOT " + std::to_string(actorId);
        }
        authoritativeSimulation_.setBotSpawnPosition(actorId, spawnPosition);
    };

    auto spawnTeamBots = [this, &botSpawnRng, &spawnBot](
                             sim::TeamId team,
                             std::uint16_t count) {
        for (std::uint16_t spawned = 0; spawned < count; ++spawned) {
            const sim::Vec3 spawnPosition =
                randomBotSpawnPositionFor(team, worldState_, simConfig_, botSpawnRng)
                    .value_or(botSpawnPositionFor(team,
                                                  spawned,
                                                  worldState_.environment,
                                                  simConfig_));
            spawnBot(team, spawnPosition);
        }
    };

    spawnTeamBots(sim::TeamId::Attacker, config_.attackerBotCount);
    spawnTeamBots(sim::TeamId::Defender, config_.defenderBotCount);
    for (const sim::Vec3& authoredSpawn : config_.authoredBotSpawns) {
        spawnBot(chooseBalancedTeam(worldState_, config_.authoredBotTeamBias), authoredSpawn);
    }
}

}  // namespace net
