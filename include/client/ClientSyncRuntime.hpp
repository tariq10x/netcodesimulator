#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "InputHandler3D.hpp"
#include "net/InterpolationBuffer.hpp"
#include "net/PredictionBuffer.hpp"
#include "net/Protocol.hpp"
#include "net/UdpSocket.hpp"
#include "sim/SimulationRules.hpp"

namespace client {

struct ClientSyncContext {
    std::uint64_t& clockUs;
    std::uint64_t& lastHelloSendUs;
    std::uint64_t& lastServerPacketUs;
    std::uint64_t& lastCommandSendUs;
    std::uint32_t& packetSeq;
    std::uint32_t& commandSeq;
    std::uint32_t& lastSnapshotPacketSeq;
    std::uint32_t& lastAckedInputSeq;
    std::uint16_t& peerId;
    std::uint16_t& snapshotRateHz;
    int& authoritativeLevelSlot;
    std::uint32_t& authoritativeLevelHash;
    bool& hasAuthoritativeSessionMetadata;
    net::HostedSessionMetadata& authoritativeSessionMetadata;
    std::uint64_t& latestSnapshotReceiveUs;
    bool& hasSnapshot;
    bool& interpolationEnabled;
    bool& predictionEnabled;
    net::InterpolationBuffer& remotePlayerInterpolation;
    net::InterpolationBuffer& remoteEnemyInterpolation;
    net::PredictionBuffer& predictionBuffer;
    sim::PlayerState& localPlayerState;
    std::vector<sim::PlayerState>& remotePlayers;
    std::vector<sim::RemoteActorState>& remoteEnemies;
    std::vector<sim::RosterEntry>& roster;
    sim::TeamScores& teamScores;
    net::WorldSnapshot& latestSnapshot;
};

class ClientSyncRuntime {
public:
    void reset(ClientSyncContext context, const sim::PlayerState& initialLocalPlayerState) const {
        context.clockUs = 0u;
        context.lastHelloSendUs = 0u;
        context.lastServerPacketUs = 0u;
        context.lastCommandSendUs = 0u;
        context.packetSeq = 0u;
        context.commandSeq = 0u;
        context.lastSnapshotPacketSeq = 0u;
        context.lastAckedInputSeq = 0u;
        context.peerId = 0u;
        context.snapshotRateHz = 0u;
        context.authoritativeLevelSlot = -1;
        context.authoritativeLevelHash = 0u;
        context.hasAuthoritativeSessionMetadata = false;
        context.authoritativeSessionMetadata = net::HostedSessionMetadata{};
        context.latestSnapshotReceiveUs = 0u;
        context.hasSnapshot = false;
        context.interpolationEnabled = true;
        context.predictionEnabled = true;
        context.remotePlayerInterpolation.reset();
        context.remoteEnemyInterpolation.reset();
        context.predictionBuffer.reset(initialLocalPlayerState);
        context.localPlayerState = initialLocalPlayerState;
        context.remotePlayers.clear();
        context.remoteEnemies.clear();
        context.roster.clear();
        context.teamScores = sim::TeamScores{};
        context.latestSnapshot = net::WorldSnapshot{};
    }

    net::CorrectionRecord applySnapshot(ClientSyncContext context,
                                        const net::WorldSnapshot& snapshot,
                                        const sim::MovementEnvironment& environment,
                                        const sim::SimConfig& simConfig,
                                        sim::TeamId* preferredTeamOut,
                                        net::ReconciliationStrategy strategy =
                                            net::ReconciliationStrategy::Smooth,
                                        std::uint32_t smoothWindowMs = 250u) const {
        net::CorrectionRecord correction;
        correction.strategy = strategy;
        correction.smoothWindowMs = smoothWindowMs;
        context.latestSnapshot = snapshot;
        if (hasMeaningfulHostedSessionMetadata(snapshot.sessionMetadata)) {
            context.authoritativeSessionMetadata = snapshot.sessionMetadata;
            context.hasAuthoritativeSessionMetadata = true;
        }
        context.roster = snapshot.roster;
        context.teamScores = snapshot.teamScores;
        if (preferredTeamOut != nullptr) {
            const sim::TeamId localTeam = resolveLocalTeam(context, *preferredTeamOut);
            if (sim::isPlayableTeam(localTeam)) {
                *preferredTeamOut = localTeam;
            }
        }
        context.hasSnapshot = true;
        context.latestSnapshotReceiveUs = context.clockUs;
        if (context.peerId == 0u && snapshot.localPlayerState.playerId > 0) {
            context.peerId = static_cast<std::uint16_t>(snapshot.localPlayerState.playerId);
        }
        context.remotePlayerInterpolation.pushSnapshot(
            snapshot.serverTimeUs,
            remoteActorsFromPlayerStates(snapshot.remotePlayers, simConfig));
        context.remoteEnemyInterpolation.pushSnapshot(snapshot.serverTimeUs, snapshot.remoteEnemies);
        context.lastAckedInputSeq = snapshot.ackedInputSeq;
        const net::ReconciliationResult result = context.predictionBuffer.reconcile(snapshot.localPlayerState,
                                                                                    snapshot.ackedInputSeq,
                                                                                    environment,
                                                                                    simConfig,
                                                                                    strategy,
                                                                                    smoothWindowMs);
        correction.mode = result.mode;
        correction.magnitude = result.magnitude;
        correction.replayedCommandCount = result.replayedCommandCount;
        context.localPlayerState = context.predictionEnabled
            ? context.predictionBuffer.displayState()
            : snapshot.localPlayerState;
        return correction;
    }

    void updateInterpolatedRemoteEntities(ClientSyncContext context,
                                          const sim::SimConfig& simConfig) const {
        if (!context.hasSnapshot) {
            context.remotePlayers.clear();
            context.remoteEnemies.clear();
            return;
        }

        const std::uint64_t estimatedServerTimeUs =
            context.latestSnapshot.serverTimeUs + (context.clockUs - context.latestSnapshotReceiveUs);
        const std::uint64_t interpolationDelayUs =
            static_cast<std::uint64_t>(defaultInterpolationDelayMs(context)) * 1'000u;
        const std::uint64_t targetServerTimeUs =
            estimatedServerTimeUs > interpolationDelayUs
                ? estimatedServerTimeUs - interpolationDelayUs
                : 0u;

        const std::vector<sim::RemoteActorState> interpolatedPlayers =
            sampleRemoteActorsForPresentation(context.remotePlayerInterpolation,
                                              remoteActorsFromPlayerStates(
                                                  context.latestSnapshot.remotePlayers, simConfig),
                                              targetServerTimeUs,
                                              context.interpolationEnabled);
        context.remotePlayers.clear();
        context.remotePlayers.reserve(interpolatedPlayers.size());
        for (const auto& playerActor : interpolatedPlayers) {
            context.remotePlayers.push_back(
                playerStateFromInterpolatedActor(
                    playerActor, context.latestSnapshot.remotePlayers, simConfig));
        }
        context.remoteEnemies =
            sampleRemoteActorsForPresentation(context.remoteEnemyInterpolation,
                                              context.latestSnapshot.remoteEnemies,
                                              targetServerTimeUs,
                                              context.interpolationEnabled);
    }

    sim::PlayerCommand buildCommand(const ClientSyncContext& context,
                                    const InputHandler3D::InputState& input,
                                    float dtSeconds,
                                    std::uint32_t seq,
                                    float viewYaw,
                                    float viewPitch,
                                    sim::TeamId pendingTeamRequest,
                                    std::uint16_t reportedLatencyMs,
                                    std::uint8_t reportedLossPct) const {
        return buildCommand(context,
                            InputHandler3D::toInputFrame(input),
                            dtSeconds,
                            seq,
                            viewYaw,
                            viewPitch,
                            pendingTeamRequest,
                            reportedLatencyMs,
                            reportedLossPct);
    }

    sim::PlayerCommand buildCommand(const ClientSyncContext& context,
                                    const InputHandler3D::InputFrame& inputFrame,
                                    float dtSeconds,
                                    std::uint32_t seq,
                                    float viewYaw,
                                    float viewPitch,
                                    sim::TeamId pendingTeamRequest,
                                    std::uint16_t reportedLatencyMs,
                                    std::uint8_t reportedLossPct) const {
        sim::PlayerCommand command = InputHandler3D::toPlayerCommand(inputFrame,
                                                                     dtSeconds,
                                                                     seq,
                                                                     viewYaw,
                                                                     viewPitch,
                                                                     context.latestSnapshot.serverTimeUs,
                                                                     defaultInterpolationDelayMs(context));
        command.requestedTeam = pendingTeamRequest;
        command.reportedLatencyMs = reportedLatencyMs;
        command.reportedLossPct = reportedLossPct;
        return command;
    }

    void applyLocalPrediction(ClientSyncContext context,
                              const sim::PlayerCommand& command,
                              const sim::MovementEnvironment& environment,
                              const sim::SimConfig& simConfig) const {
        if (!context.predictionEnabled) {
            context.localPlayerState = context.hasSnapshot
                ? context.latestSnapshot.localPlayerState
                : context.localPlayerState;
            return;
        }

        context.predictionBuffer.recordCommand(command, environment, simConfig);
        context.localPlayerState = context.predictionBuffer.displayState();
    }

    void advanceLocalCorrection(ClientSyncContext context, float dtSeconds) const {
        if (!context.predictionEnabled) {
            return;
        }

        context.predictionBuffer.advanceDisplay(dtSeconds);
        context.localPlayerState = context.predictionBuffer.displayState();
    }

    bool shouldPredictFireAttempt(const ClientSyncContext& context,
                                  const InputHandler3D::InputState& input) const {
        return InputHandler3D::toPredictionFrame(input, context.predictionEnabled).firePressed;
    }

    sim::PlayerCommand buildIdleCommand(const ClientSyncContext& context,
                                        std::uint32_t seq,
                                        float viewYaw,
                                        float viewPitch,
                                        sim::TeamId pendingTeamRequest,
                                        std::uint16_t reportedLatencyMs,
                                        std::uint8_t reportedLossPct) const {
        InputHandler3D::InputFrame idleFrame;
        return buildCommand(context,
                            idleFrame,
                            0.0f,
                            seq,
                            viewYaw,
                            viewPitch,
                            pendingTeamRequest,
                            reportedLatencyMs,
                            reportedLossPct);
    }

    bool sendHello(ClientSyncContext context,
                   std::uint16_t protocolVersion,
                   std::uint32_t sessionId,
                   const std::string& playerName,
                   sim::TeamId preferredTeam,
                   net::UdpSocket& socket,
                   const net::UdpEndpoint& serverEndpoint) const {
        net::Packet packet;
        packet.header.version = protocolVersion;
        packet.header.channel = net::Channel::Control;
        packet.header.seq = ++context.packetSeq;
        packet.header.kind = net::PacketKind::Hello;
        packet.payload = net::HelloMessage{sessionId, 0u, playerName, preferredTeam};

        const bool sent = socket.sendTo(serverEndpoint, net::serializePacket(packet));
        if (sent) {
            context.lastHelloSendUs = context.clockUs;
        }
        return sent;
    }

    bool sendControlPayload(ClientSyncContext context,
                            std::uint16_t protocolVersion,
                            net::PacketPayload payload,
                            net::UdpSocket& socket,
                            const net::UdpEndpoint& serverEndpoint) const {
        if (context.peerId == 0u) {
            return false;
        }

        net::Packet packet;
        packet.header.version = protocolVersion;
        packet.header.peerId = context.peerId;
        packet.header.channel = net::Channel::Control;
        packet.header.seq = ++context.packetSeq;
        packet.header.ack = context.lastSnapshotPacketSeq;
        packet.header.kind = net::packetKindForPayload(payload);
        packet.payload = std::move(payload);
        return socket.sendTo(serverEndpoint, net::serializePacket(packet));
    }

    bool sendCommand(ClientSyncContext context,
                     std::uint16_t protocolVersion,
                     const sim::PlayerCommand& command,
                     net::UdpSocket& socket,
                     const net::UdpEndpoint& serverEndpoint) const {
        if (context.peerId == 0u) {
            return false;
        }

        net::Packet packet;
        packet.header.version = protocolVersion;
        packet.header.peerId = context.peerId;
        packet.header.channel = net::Channel::Command;
        packet.header.seq = ++context.packetSeq;
        packet.header.ack = context.lastSnapshotPacketSeq;
        packet.header.kind = net::PacketKind::CommandBundle;
        packet.payload = net::CommandBundle{{command}};

        if (!socket.sendTo(serverEndpoint, net::serializePacket(packet))) {
            return false;
        }
        context.lastCommandSendUs = context.clockUs;
        return true;
    }

    std::uint32_t defaultInterpolationDelayMs(const ClientSyncContext& context) const {
        if (!context.interpolationEnabled) {
            return 0u;
        }
        if (context.snapshotRateHz == 0u) {
            return 100u;
        }
        return std::max<std::uint32_t>(100u, static_cast<std::uint32_t>(2'000u / context.snapshotRateHz));
    }

    static std::vector<sim::RemoteActorState> sampleRemoteActorsForPresentation(
        const net::InterpolationBuffer& buffer,
        const std::vector<sim::RemoteActorState>& newestSnapshot,
        std::uint64_t targetServerTimeUs,
        bool interpolationEnabled) {
        if (!interpolationEnabled) {
            return newestSnapshot;
        }
        return buffer.sample(targetServerTimeUs);
    }

private:
    static bool hasMeaningfulHostedSessionMetadata(const net::HostedSessionMetadata& metadata) {
        return !metadata.sessionLabel.empty() ||
               !metadata.hostPlayerName.empty() ||
               metadata.levelSlot >= 0 ||
               metadata.levelHash != 0u ||
               metadata.publicJoinPort != 0u ||
               !metadata.studyEventRunId.empty();
    }

    static sim::TeamId resolveLocalTeam(const ClientSyncContext& context, sim::TeamId fallbackTeam) {
        const auto it = std::find_if(context.roster.begin(),
                                     context.roster.end(),
                                     [&context](const sim::RosterEntry& entry) {
                                         return entry.actorId == static_cast<int>(context.peerId);
                                     });
        if (it != context.roster.end()) {
            return it->team;
        }
        return fallbackTeam;
    }

    static sim::RemoteActorState remoteActorFromPlayerState(const sim::PlayerState& playerState,
                                                            const sim::SimConfig& simConfig) {
        sim::RemoteActorState actor;
        actor.entityId = playerState.playerId;
        actor.position = playerState.position;
        actor.velocity = playerState.velocity;
        actor.yaw = playerState.yaw;
        actor.pitch = playerState.pitch;
        actor.health = playerState.health;
        actor.radius = simConfig.playerRadius;
        actor.alive = playerState.health > 0.0f;
        return actor;
    }

    static std::vector<sim::RemoteActorState> remoteActorsFromPlayerStates(
        const std::vector<sim::PlayerState>& players,
        const sim::SimConfig& simConfig) {
        std::vector<sim::RemoteActorState> actors;
        actors.reserve(players.size());
        for (const auto& player : players) {
            actors.push_back(remoteActorFromPlayerState(player, simConfig));
        }
        return actors;
    }

    static sim::PlayerState playerStateFromInterpolatedActor(
        const sim::RemoteActorState& actor,
        const std::vector<sim::PlayerState>& authoritativePlayers,
        const sim::SimConfig& simConfig) {
        sim::PlayerState playerState;
        playerState.playerId = actor.entityId;
        playerState.position = actor.position;
        playerState.velocity = actor.velocity;
        playerState.yaw = actor.yaw;
        playerState.pitch = actor.pitch;
        playerState.health = actor.health;
        playerState.maxHealth = simConfig.playerMaxHealth;
        playerState.grounded = actor.position.y <= simConfig.playerEyeHeight + 0.001f &&
                               std::abs(actor.velocity.y) <= 0.01f;

        const auto authoritativeIt = std::find_if(
            authoritativePlayers.begin(),
            authoritativePlayers.end(),
            [playerId = actor.entityId](const sim::PlayerState& player) {
                return player.playerId == playerId;
            });
        if (authoritativeIt != authoritativePlayers.end()) {
            playerState.maxHealth = authoritativeIt->maxHealth;
            playerState.weaponCooldownRemaining = authoritativeIt->weaponCooldownRemaining;
            playerState.jumpsUsed = authoritativeIt->jumpsUsed;
            playerState.grounded = authoritativeIt->grounded;
        }

        return playerState;
    }
};

}  // namespace client
