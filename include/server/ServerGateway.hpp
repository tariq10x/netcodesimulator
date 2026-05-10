#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "net/Protocol.hpp"
#include "sim/WorldState.hpp"

namespace net {

struct ServerConfig;

struct ClientSession {
    std::uint16_t peerId{0};
    std::uint32_t sessionId{0};
    sim::TeamId team{sim::TeamId::None};
    bool connected{false};
    std::uint64_t lastHeardTimeUs{0};
    std::uint32_t lastAckedInputSeq{0};
    std::uint32_t snapshotSeq{0};
    std::uint32_t controlSeq{0};
    std::uint32_t lastAppliedInputSeq{0};
    std::uint32_t lastAppliedControlSeq{0};
    std::string playerName{};
    sim::Vec3 spawnPosition{};
    float respawnTimerSeconds{0.0f};
    std::uint64_t spawnProtectionUntilUs{0u};
    bool interpolationEnabled{true};
    bool predictionEnabled{true};
    sim::RuntimeReconciliationStrategy reconciliationStrategy{
        sim::RuntimeReconciliationStrategy::Smooth};
    std::uint32_t smoothCorrectionWindowMs{250u};
    std::uint16_t reportedLatencyMs{0u};
    std::uint8_t reportedLossPct{0u};
    std::map<std::uint32_t, sim::PlayerCommand> pendingCommands{};
    std::map<std::uint32_t, sim::PlayerCommand> pendingControlCommands{};
    sim::PlayerState controlPlayerState{};
    bool hasControlPlayerState{false};

    sim::ParticipantState participantState() const {
        sim::ParticipantState state;
        state.presence = connected ? sim::SessionPresence::Connected
                                   : sim::SessionPresence::Disconnected;
        state.team = team;
        state.participation = team == sim::TeamId::Spectator
            ? sim::ParticipationState::Spectating
            : (sim::isPlayableTeam(team)
                   ? sim::ParticipationState::Playing
                   : sim::ParticipationState::TeamSelection);
        state.control = sim::ControlBinding{
            sim::isPlayableTeam(team) ? sim::ControlBindingKind::Actor
                                      : sim::ControlBindingKind::None,
            sim::isPlayableTeam(team) ? static_cast<int>(peerId) : -1};
        return state;
    }
};

inline void syncConnectedSessionMetadata(sim::WorldState* worldState,
                                         const std::vector<ClientSession>& sessions) {
    if (worldState == nullptr) {
        return;
    }

    std::uint16_t connectedHumanPlayers = 0u;
    for (const auto& session : sessions) {
        if (session.connected) {
            ++connectedHumanPlayers;
        }
    }

    std::uint16_t connectedBotPlayers = 0u;
    for (const auto& entry : worldState->roster) {
        if (entry.isBot) {
            ++connectedBotPlayers;
        }
    }

    worldState->sessionMetadata.connectedHumanPlayers = connectedHumanPlayers;
    worldState->sessionMetadata.connectedBotPlayers = connectedBotPlayers;
}

struct AuthoritativeSnapshot {
    sim::WorldState worldState{};
    sim::ParticipantState localParticipantState{};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    std::uint32_t ackedInputSeq{0u};

    static AuthoritativeSnapshot fromSession(const sim::WorldState& worldState,
                                             const ClientSession& session,
                                             ShotEvaluationMode shotEvaluationMode) {
        AuthoritativeSnapshot snapshot;
        snapshot.worldState = worldState;
        snapshot.localParticipantState = session.participantState();
        snapshot.shotEvaluationMode = shotEvaluationMode;
        snapshot.ackedInputSeq = session.lastAckedInputSeq;
        return snapshot;
    }
};

namespace server {

class ServerGateway {
public:
    bool acceptClient(const HelloMessage& hello,
                      const ServerConfig& config,
                      const HostedSessionMetadata& hostedMetadata,
                      const sim::SimConfig& simConfig,
                      sim::WorldState* worldState,
                      std::uint32_t serverTick,
                      std::uint64_t nowUs,
                      WelcomeMessage* welcomeOut = nullptr,
                      std::string* rejectReasonOut = nullptr);
    bool disconnectClient(std::uint16_t peerId, sim::WorldState* worldState);

    void enqueueControlPayload(ClientSession& session, PacketPayload payload);
    void recordSnapshotEvent(const SnapshotEvent& event);
    void publishSnapshots(const ServerConfig& config,
                          const HostedSessionMetadata& hostedMetadata,
                          const sim::WorldState& worldState,
                          const std::vector<sim::PlayerState>& serverControlPlayers,
                          std::uint32_t serverTick,
                          std::uint64_t nowUs);
    void pruneTimedOutSessions(const ServerConfig& config,
                               sim::WorldState* worldState,
                               std::uint64_t nowUs);

    std::vector<Packet> takePendingPackets();

    std::vector<ClientSession>& sessions();
    const std::vector<ClientSession>& sessions() const;
    const ClientSession* findSession(std::uint16_t peerId) const;
    ClientSession* findSessionMutable(std::uint16_t peerId);

private:
    std::uint16_t nextAvailablePeerId(std::uint16_t maxPlayers) const;
    Packet buildControlPacket(ClientSession& session, PacketPayload payload);
    Packet buildSnapshotPacket(const ServerConfig& config,
                               const HostedSessionMetadata& hostedMetadata,
                               const sim::WorldState& worldState,
                               const std::vector<sim::PlayerState>& serverControlPlayers,
                               std::uint32_t serverTick,
                               const ClientSession& session,
                               std::uint64_t nowUs) const;

    std::vector<ClientSession> sessions_{};
    std::vector<Packet> pendingPackets_{};
    std::vector<SnapshotEvent> pendingEvents_{};
};

}  // namespace server
}  // namespace net
