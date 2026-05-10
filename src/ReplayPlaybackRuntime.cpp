#include "replay/ReplayPlaybackRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>

#include "net/LagCompensation.hpp"
#include "server/AuthoritativeSimulation.hpp"
#include "sim/SimulationRules.hpp"

namespace replay {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct PlaybackSample {
    sim::WorldState worldState{};
    sim::SimConfig simConfig{};
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
};

using ServerCommandEventRef = std::reference_wrapper<const ServerCommandEvent>;

const WorldKeyframe* latestKeyframeAtOrBefore(const ReplayDemo& demo,
                                              ReplayTrack track,
                                              std::uint32_t serverTick) {
    const WorldKeyframe* best = nullptr;
    for (const WorldKeyframe& keyframe : demo.keyframes) {
        if (keyframe.track != track || keyframe.serverTick > serverTick) {
            continue;
        }
        if (best == nullptr || keyframe.serverTick >= best->serverTick) {
            best = &keyframe;
        }
    }
    return best;
}

const WorldKeyframe* earliestKeyframeAtOrAfter(const ReplayDemo& demo,
                                               ReplayTrack track,
                                               std::uint32_t serverTick) {
    const WorldKeyframe* best = nullptr;
    for (const WorldKeyframe& keyframe : demo.keyframes) {
        if (keyframe.track != track || keyframe.serverTick < serverTick) {
            continue;
        }
        if (best == nullptr || keyframe.serverTick <= best->serverTick) {
            best = &keyframe;
        }
    }
    return best;
}

std::uint16_t tickRateFor(const ReplayDemo& demo) {
    return demo.header.tickRateHz == 0u ? 60u : demo.header.tickRateHz;
}

std::uint64_t tickIntervalUsFor(const ReplayDemo& demo) {
    return static_cast<std::uint64_t>(
        std::llround(1'000'000.0 / static_cast<double>(tickRateFor(demo))));
}

net::LagCompensationConfig lagCompensationConfigFor(const ReplayDemo& demo) {
    const std::uint32_t rewindMs = demo.header.maxRewindMs == 0u
        ? 500u
        : demo.header.maxRewindMs;
    const std::size_t samplesForWindow = static_cast<std::size_t>(
        std::ceil((static_cast<double>(rewindMs) / 1000.0) *
                  static_cast<double>(tickRateFor(demo)))) + 2u;
    return net::LagCompensationConfig{
        static_cast<std::uint64_t>(rewindMs) * 1'000u,
        std::max<std::size_t>(samplesForWindow, 2u)
    };
}

std::uint32_t replayLookbackTicksFor(const ReplayDemo& demo) {
    const double rewindSeconds = static_cast<double>(demo.header.maxRewindMs == 0u
                                                        ? 500u
                                                        : demo.header.maxRewindMs) /
                                 1000.0;
    return static_cast<std::uint32_t>(
        std::ceil(rewindSeconds * static_cast<double>(tickRateFor(demo)))) + 2u;
}

const WorldKeyframe* replayStartKeyframeFor(const ReplayDemo& demo,
                                            ReplayTrack track,
                                            std::uint32_t targetTick) {
    const std::uint32_t initialTick =
        demo.initialState.worldState.authoritativeTime.serverTick;
    const std::uint32_t lookbackTicks = replayLookbackTicksFor(demo);
    const std::uint32_t desiredStartTick = targetTick > initialTick + lookbackTicks
        ? targetTick - lookbackTicks
        : initialTick;
    return latestKeyframeAtOrBefore(demo, track, desiredStartTick);
}

sim::PlayerState* findControlPlayer(std::vector<sim::PlayerState>* players, int actorId) {
    if (players == nullptr) {
        return nullptr;
    }
    auto it = std::find_if(players->begin(),
                           players->end(),
                           [actorId](const sim::PlayerState& player) {
                               return player.playerId == actorId;
                           });
    return it != players->end() ? &*it : nullptr;
}

sim::PlayerCommand idleCommandFor(const sim::PlayerState& state, float dtSeconds) {
    sim::PlayerCommand command;
    command.dtSeconds = dtSeconds;
    command.yaw = state.yaw;
    command.pitch = state.pitch;
    return command;
}

sim::TeamId teamForActor(const sim::WorldState& world, int actorId) {
    if (const sim::RosterEntry* entry = sim::findRosterEntry(world, actorId);
        entry != nullptr && sim::isPlayableTeam(entry->team)) {
        return entry->team;
    }
    return sim::TeamId::Attacker;
}

bool hasAuthoritativeCommandsForActor(const ReplayDemo& demo, int actorId) {
    return std::any_of(demo.commandEvents.begin(),
                       demo.commandEvents.end(),
                       [actorId](const ServerCommandEvent& event) {
                           const int eventActorId = event.actorId >= 0
                               ? event.actorId
                               : static_cast<int>(event.peerId);
                           return event.track == ReplayTrack::Authoritative &&
                                  event.stage == ReplayCommandStage::Applied &&
                                  eventActorId == actorId;
                       });
}

const sim::PlayerState* findPlayerById(const std::vector<sim::PlayerState>& players,
                                       int playerId) {
    const auto it = std::find_if(players.begin(),
                                 players.end(),
                                 [playerId](const sim::PlayerState& player) {
                                     return player.playerId == playerId;
                                 });
    return it == players.end() ? nullptr : &(*it);
}

const sim::RemoteActorState* findEnemyById(const std::vector<sim::RemoteActorState>& enemies,
                                          int entityId) {
    const auto it = std::find_if(enemies.begin(),
                                 enemies.end(),
                                 [entityId](const sim::RemoteActorState& enemy) {
                                     return enemy.entityId == entityId;
                                 });
    return it == enemies.end() ? nullptr : &(*it);
}

float lerpValue(float from, float to, float alpha) {
    return from + ((to - from) * alpha);
}

sim::Vec3 lerpVec3(const sim::Vec3& from, const sim::Vec3& to, float alpha) {
    return sim::Vec3{
        lerpValue(from.x, to.x, alpha),
        lerpValue(from.y, to.y, alpha),
        lerpValue(from.z, to.z, alpha)
    };
}

float lerpAngle(float from, float to, float alpha) {
    float delta = std::fmod(to - from + kPi, 2.0f * kPi);
    if (delta < 0.0f) {
        delta += 2.0f * kPi;
    }
    delta -= kPi;
    return from + (delta * alpha);
}

void smoothKeyframeOnlyActors(const ReplayDemo& demo,
                              std::uint32_t serverTick,
                              sim::WorldState* worldState) {
    if (worldState == nullptr) {
        return;
    }

    const WorldKeyframe* previous =
        latestKeyframeAtOrBefore(demo, ReplayTrack::Authoritative, serverTick);
    const WorldKeyframe* next =
        earliestKeyframeAtOrAfter(demo, ReplayTrack::Authoritative, serverTick);
    if (previous == nullptr ||
        next == nullptr ||
        next->serverTick < previous->serverTick) {
        return;
    }

    const float alpha = next->serverTick > previous->serverTick
        ? static_cast<float>(serverTick - previous->serverTick) /
              static_cast<float>(next->serverTick - previous->serverTick)
        : 0.0f;

    for (sim::PlayerState& player : worldState->players) {
        if (hasAuthoritativeCommandsForActor(demo, player.playerId)) {
            continue;
        }
        const sim::PlayerState* from =
            findPlayerById(previous->worldState.players, player.playerId);
        const sim::PlayerState* to =
            findPlayerById(next->worldState.players, player.playerId);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        player.position = lerpVec3(from->position, to->position, alpha);
        player.velocity = lerpVec3(from->velocity, to->velocity, alpha);
        player.yaw = lerpAngle(from->yaw, to->yaw, alpha);
        player.pitch = lerpAngle(from->pitch, to->pitch, alpha);
        player.health = lerpValue(from->health, to->health, alpha);
        player.maxHealth = lerpValue(from->maxHealth, to->maxHealth, alpha);
        player.weaponCooldownRemaining =
            lerpValue(from->weaponCooldownRemaining, to->weaponCooldownRemaining, alpha);
        player.grounded = alpha >= 1.0f ? to->grounded : from->grounded;
        player.jumpsUsed = alpha >= 1.0f ? to->jumpsUsed : from->jumpsUsed;
    }

    for (sim::RemoteActorState& enemy : worldState->enemies) {
        const sim::RemoteActorState* from =
            findEnemyById(previous->worldState.enemies, enemy.entityId);
        const sim::RemoteActorState* to =
            findEnemyById(next->worldState.enemies, enemy.entityId);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        enemy.position = lerpVec3(from->position, to->position, alpha);
        enemy.velocity = lerpVec3(from->velocity, to->velocity, alpha);
        enemy.yaw = lerpAngle(from->yaw, to->yaw, alpha);
        enemy.pitch = lerpAngle(from->pitch, to->pitch, alpha);
        enemy.health = lerpValue(from->health, to->health, alpha);
        enemy.radius = lerpValue(from->radius, to->radius, alpha);
        enemy.alive = alpha >= 1.0f ? to->alive : from->alive;
    }
}

std::vector<net::ClientSession> buildReplaySessions(const ReplayDemo& demo,
                                                    const sim::WorldState& world,
                                                    std::uint32_t startTick) {
    std::vector<net::ClientSession> sessions;
    sessions.reserve(world.players.size());

    for (const sim::PlayerState& player : world.players) {
        if (player.playerId <= 0 ||
            player.playerId > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
            continue;
        }
        const sim::RosterEntry* entry = sim::findRosterEntry(world, player.playerId);
        if (entry != nullptr &&
            entry->isBot &&
            !hasAuthoritativeCommandsForActor(demo, player.playerId)) {
            continue;
        }

        net::ClientSession session;
        session.peerId = static_cast<std::uint16_t>(player.playerId);
        session.team = teamForActor(world, player.playerId);
        session.connected = sim::isPlayableTeam(session.team);
        session.spawnPosition = player.position;
        if (entry != nullptr) {
            session.playerName = entry->displayName;
            session.interpolationEnabled = entry->interpolationEnabled;
            session.predictionEnabled = entry->predictionEnabled;
            session.reconciliationStrategy = entry->reconciliationStrategy;
            session.smoothCorrectionWindowMs = entry->smoothCorrectionWindowMs;
            session.reportedLatencyMs = entry->latencyMs;
            session.reportedLossPct = entry->lossPct;
        }

        for (const ServerCommandEvent& event : demo.commandEvents) {
            if (event.track != ReplayTrack::Authoritative ||
                event.stage != ReplayCommandStage::Applied ||
                event.serverTick > startTick) {
                continue;
            }
            const int controlledActorId = event.actorId >= 0
                ? event.actorId
                : static_cast<int>(event.peerId);
            if (controlledActorId == player.playerId) {
                session.lastAppliedInputSeq = event.command.seq;
                session.lastAckedInputSeq = event.command.seq;
            }
        }

        sessions.push_back(std::move(session));
    }

    return sessions;
}

net::ClientSession* sessionForActor(std::vector<net::ClientSession>* sessions, int actorId) {
    if (sessions == nullptr || actorId <= 0) {
        return nullptr;
    }
    const auto it = std::find_if(sessions->begin(),
                                 sessions->end(),
                                 [actorId](const net::ClientSession& session) {
                                     return static_cast<int>(session.peerId) == actorId;
                                 });
    return it == sessions->end() ? nullptr : &*it;
}

std::vector<ServerCommandEventRef> appliedCommandsFor(const ReplayDemo& demo,
                                                       ReplayTrack track,
                                                       std::uint32_t startTick,
                                                       std::uint32_t targetTick) {
    std::vector<ServerCommandEventRef> commands;
    for (const ServerCommandEvent& event : demo.commandEvents) {
        if (event.track != track ||
            event.stage != ReplayCommandStage::Applied ||
            event.serverTick <= startTick ||
            event.serverTick > targetTick) {
            continue;
        }
        commands.emplace_back(event);
    }
    std::sort(commands.begin(),
              commands.end(),
              [](const ServerCommandEventRef lhsRef, const ServerCommandEventRef rhsRef) {
                  const ServerCommandEvent& lhs = lhsRef.get();
                  const ServerCommandEvent& rhs = rhsRef.get();
                  if (lhs.serverTick != rhs.serverTick) {
                      return lhs.serverTick < rhs.serverTick;
                  }
                  if (lhs.serverTimeUs != rhs.serverTimeUs) {
                      return lhs.serverTimeUs < rhs.serverTimeUs;
                  }
                  return lhs.command.seq < rhs.command.seq;
              });
    return commands;
}

std::map<std::uint32_t, std::uint64_t> buildTickTimes(
    const ReplayDemo& demo,
    const std::vector<ServerCommandEventRef>& commands) {
    std::map<std::uint32_t, std::uint64_t> tickTimes;
    for (const WorldKeyframe& keyframe : demo.keyframes) {
        tickTimes[keyframe.serverTick] = keyframe.serverTimeUs;
    }
    for (const ServerCommandEvent& event : commands) {
        auto& time = tickTimes[event.serverTick];
        time = std::max(time, event.serverTimeUs);
    }
    return tickTimes;
}

std::uint64_t serverTimeForTick(const std::map<std::uint32_t, std::uint64_t>& tickTimes,
                                std::uint32_t tick,
                                std::uint32_t startTick,
                                std::uint64_t startTimeUs,
                                std::uint64_t tickIntervalUs) {
    if (const auto it = tickTimes.find(tick); it != tickTimes.end() && it->second > 0u) {
        return it->second;
    }
    return startTimeUs + static_cast<std::uint64_t>(tick - startTick) * tickIntervalUs;
}

void applyControlCommand(std::vector<sim::PlayerState>* controlPlayers,
                         const sim::MovementEnvironment& environment,
                         const sim::PlayerCommand& command,
                         int actorId,
                         const sim::SimConfig& simConfig) {
    if (actorId < 0) {
        return;
    }
    sim::PlayerState* player = findControlPlayer(controlPlayers, actorId);
    if (player == nullptr) {
        return;
    }
    *player = sim::applyPlayerCommand(*player, command, environment, simConfig);
    player->playerId = actorId;
}

void stepControlPlayers(std::vector<sim::PlayerState>* controlPlayers,
                        const sim::MovementEnvironment& environment,
                        const std::vector<ServerCommandEventRef>& commands,
                        const sim::SimConfig& simConfig,
                        float dtSeconds) {
    if (controlPlayers == nullptr) {
        return;
    }

    std::unordered_map<int, std::vector<ServerCommandEventRef>> commandsByActor;
    for (const ServerCommandEvent& event : commands) {
        const int actorId = event.actorId >= 0 ? event.actorId : static_cast<int>(event.peerId);
        commandsByActor[actorId].emplace_back(event);
    }

    for (sim::PlayerState& player : *controlPlayers) {
        const auto commandsIt = commandsByActor.find(player.playerId);
        if (commandsIt == commandsByActor.end()) {
            player = sim::applyPlayerCommand(player,
                                             idleCommandFor(player, dtSeconds),
                                             environment,
                                             simConfig);
            continue;
        }
        for (const ServerCommandEvent& event : commandsIt->second) {
            applyControlCommand(controlPlayers,
                                environment,
                                event.command,
                                player.playerId,
                                simConfig);
        }
    }
}

void syncControlPlayerVitals(std::vector<sim::PlayerState>* controlPlayers,
                             const sim::WorldState& authoritativeWorld) {
    if (controlPlayers == nullptr) {
        return;
    }

    for (sim::PlayerState& player : *controlPlayers) {
        if (const sim::PlayerState* authoritativePlayer =
                sim::findPlayer(authoritativeWorld, player.playerId);
            authoritativePlayer != nullptr) {
            player.health = authoritativePlayer->health;
            player.maxHealth = authoritativePlayer->maxHealth;
        }
    }
}

bool replayAuthoritativeWorldToTick(const ReplayDemo& demo,
                                    std::uint32_t serverTick,
                                    PlaybackSample* sampleOut) {
    if (sampleOut == nullptr) {
        return false;
    }

    const WorldKeyframe* keyframe =
        replayStartKeyframeFor(demo, ReplayTrack::Authoritative, serverTick);
    if (keyframe != nullptr) {
        sampleOut->worldState = keyframe->worldState;
        sampleOut->serverTick = keyframe->serverTick;
        sampleOut->serverTimeUs = keyframe->serverTimeUs;
    } else {
        sampleOut->worldState = demo.initialState.worldState;
        sampleOut->serverTick = sampleOut->worldState.authoritativeTime.serverTick;
        sampleOut->serverTimeUs = sampleOut->worldState.authoritativeTime.serverTimeUs;
    }
    sampleOut->simConfig = demo.initialState.simConfig;

    const std::uint32_t targetTick = std::max(serverTick, sampleOut->serverTick);
    const std::uint64_t tickIntervalUs = tickIntervalUsFor(demo);
    const float dtSeconds = static_cast<float>(tickIntervalUs) / 1'000'000.0f;
    const std::vector<ServerCommandEventRef> commands =
        appliedCommandsFor(demo, ReplayTrack::Authoritative, sampleOut->serverTick, targetTick);
    const std::map<std::uint32_t, std::uint64_t> tickTimes =
        buildTickTimes(demo, commands);
    const std::uint32_t startTick = sampleOut->serverTick;
    const std::uint64_t startTimeUs = sampleOut->serverTimeUs;

    std::vector<net::ClientSession> sessions =
        buildReplaySessions(demo, sampleOut->worldState, startTick);
    net::LagCompensationHistory lagCompensation(lagCompensationConfigFor(demo));
    lagCompensation.recordWorldState(sampleOut->worldState,
                                     sampleOut->simConfig,
                                     sampleOut->serverTimeUs);
    net::server::AuthoritativeSimulation simulation;

    for (std::uint32_t tick = startTick + 1u; tick <= targetTick; ++tick) {
        for (const ServerCommandEvent& event : commands) {
            if (event.serverTick != tick) {
                continue;
            }
            const int actorId = event.actorId >= 0
                ? event.actorId
                : static_cast<int>(event.peerId);
            if (net::ClientSession* session = sessionForActor(&sessions, actorId);
                session != nullptr) {
                session->pendingCommands.emplace(event.command.seq, event.command);
            }
        }

        const std::uint64_t tickTimeUs =
            serverTimeForTick(tickTimes, tick, startTick, startTimeUs, tickIntervalUs);
        net::server::StepContext context;
        context.dtSeconds = dtSeconds;
        context.respawnDelaySeconds = demo.header.respawnDelaySeconds;
        context.spawnProtectionSeconds = demo.header.spawnProtectionSeconds;
        context.shotEvaluationMode = demo.header.shotEvaluationMode;
        context.cadence = sim::TimingCadence{
            tickRateFor(demo),
            static_cast<std::uint16_t>(
                demo.header.snapshotRateHz == 0u ? 20u : demo.header.snapshotRateHz),
            tickRateFor(demo)
        };
        context.authoritativeTime = sim::AuthoritativeTime{tick, tickTimeUs, tickTimeUs};

        std::vector<net::SnapshotEvent> events;
        simulation.stepWorld(sampleOut->worldState,
                             sessions,
                             context,
                             sampleOut->simConfig,
                             lagCompensation,
                             &events);
        lagCompensation.recordWorldState(sampleOut->worldState,
                                         sampleOut->simConfig,
                                         tickTimeUs);
        sampleOut->serverTick = tick;
        sampleOut->serverTimeUs = tickTimeUs;
    }

    sampleOut->worldState.authoritativeTime.serverTick = sampleOut->serverTick;
    sampleOut->worldState.authoritativeTime.serverTimeUs = sampleOut->serverTimeUs;
    sampleOut->worldState.authoritativeTime.viewedServerTimeUs = sampleOut->serverTimeUs;
    smoothKeyframeOnlyActors(demo, targetTick, &sampleOut->worldState);
    return true;
}

std::vector<sim::PlayerState> replayControlPlayersToTick(
    const ReplayDemo& demo,
    std::uint32_t serverTick,
    const sim::MovementEnvironment& environment,
    const sim::SimConfig& simConfig,
    const sim::WorldState& authoritativeWorld) {
    std::vector<sim::PlayerState> controlPlayers;
    std::uint32_t controlStartTick = demo.initialState.worldState.authoritativeTime.serverTick;
    if (const WorldKeyframe* keyframe =
            latestKeyframeAtOrBefore(demo, ReplayTrack::Control, serverTick);
        keyframe != nullptr) {
        controlPlayers = keyframe->controlPlayers;
        controlStartTick = keyframe->serverTick;
    } else {
        controlPlayers = demo.initialState.worldState.players;
    }

    const std::uint32_t targetTick = std::max(serverTick, controlStartTick);
    const std::uint64_t tickIntervalUs = tickIntervalUsFor(demo);
    const float dtSeconds = static_cast<float>(tickIntervalUs) / 1'000'000.0f;
    const std::vector<ServerCommandEventRef> commands =
        appliedCommandsFor(demo, ReplayTrack::Control, controlStartTick, targetTick);

    for (std::uint32_t tick = controlStartTick + 1u; tick <= targetTick; ++tick) {
        std::vector<ServerCommandEventRef> tickCommands;
        for (const ServerCommandEvent& event : commands) {
            if (event.serverTick == tick) {
                tickCommands.emplace_back(event);
            }
        }
        stepControlPlayers(&controlPlayers,
                           environment,
                           tickCommands,
                           simConfig,
                           dtSeconds);
    }

    syncControlPlayerVitals(&controlPlayers, authoritativeWorld);
    return controlPlayers;
}

}  // namespace

bool ReplayPlaybackRuntime::load(const ReplayDemo& demo, ReplayPlaybackTrack track) {
    demo_ = &demo;
    track_ = track;
    return seekToTick(0u);
}

bool ReplayPlaybackRuntime::seekToTick(std::uint32_t serverTick) {
    if (demo_ == nullptr) {
        return false;
    }

    PlaybackSample authoritativeSample;
    if (!replayAuthoritativeWorldToTick(*demo_, serverTick, &authoritativeSample)) {
        return false;
    }
    worldState_ = authoritativeSample.worldState;
    simConfig_ = authoritativeSample.simConfig;
    serverTick_ = authoritativeSample.serverTick;
    serverTimeUs_ = authoritativeSample.serverTimeUs;
    if (track_ == ReplayPlaybackTrack::Control) {
        controlPlayers_ = replayControlPlayersToTick(*demo_,
                                                     serverTick_,
                                                     worldState_.environment,
                                                     simConfig_,
                                                     worldState_);
    } else {
        controlPlayers_.clear();
    }
    return true;
}

const sim::WorldState& ReplayPlaybackRuntime::worldState() const {
    return worldState_;
}

const std::vector<sim::PlayerState>& ReplayPlaybackRuntime::controlPlayers() const {
    return controlPlayers_;
}

std::uint32_t ReplayPlaybackRuntime::serverTick() const {
    return serverTick_;
}

std::uint64_t ReplayPlaybackRuntime::serverTimeUs() const {
    return serverTimeUs_;
}

}  // namespace replay
