#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "sim/SimulationTypes.hpp"

namespace net {

using ByteBuffer = std::vector<std::uint8_t>;

constexpr std::uint32_t kProtocolMagic = 0x4D53544Cu;  // "LTSM" as little-endian bytes.
constexpr std::uint16_t kProtocolVersion = 12u;
constexpr std::uint16_t kDefaultSessionTickRateHz = 60u;
constexpr std::uint16_t kMinSessionTickRateHz = 20u;
constexpr std::uint16_t kMaxSessionTickRateHz = 240u;
constexpr std::array<std::uint16_t, 5u> kSessionTickRateChoicesHz{
    20u, 30u, 60u, 120u, 240u
};

enum class Channel : std::uint8_t {
    Control = 0,
    Command = 1,
    Snapshot = 2,
    ProxyControl = 3,
    ProxyStats = 4
};

enum class PacketKind : std::uint8_t {
    Hello = 0,
    Welcome = 1,
    Disconnect = 2,
    CommandBundle = 3,
    WorldSnapshot = 4,
    ProxyControl = 5,
    ProxyStats = 6,
    TeamChangeRequest = 7,
    RuntimeParamChangeRequest = 8,
    RuntimeParamSnapshot = 9,
    RuntimeParamApplyResult = 10,
    ControlWorldSnapshot = 11,
    ControlCommandBundle = 12,
    SessionActionRequest = 13,
    SessionActionResult = 14
};

enum class SnapshotEventKind : std::uint8_t {
    WeaponFired = 0,
    ConfirmedHit = 1,
    DamageApplied = 2,
    PlayerRespawned = 3,
    PlayerKilled = 4
};

enum class ShotEvaluationMode : std::uint8_t {
    SeenPosition = 0,
    LivePosition = 1
};

enum class SessionVisualizationMode : std::uint8_t {
    Diagnostic = 0,
    Reality = 1
};

enum class RuntimeParamScope : std::uint8_t {
    Global = 0,
    Player = 1,
    Bot = 2,
    Session = 3
};

enum class SessionActionKind : std::uint8_t {
    SpawnFrozenBotAhead = 0
};

inline const char* toString(SessionActionKind kind) {
    switch (kind) {
        case SessionActionKind::SpawnFrozenBotAhead:
            return "spawn_frozen_bot_ahead";
    }
    return "spawn_frozen_bot_ahead";
}

enum class ParseError : std::uint8_t {
    None = 0,
    BufferUnderflow = 1,
    InvalidMagic = 2,
    UnsupportedVersion = 3,
    InvalidChannel = 4,
    InvalidKind = 5,
    InvalidEventKind = 6,
    InvalidStringLength = 7,
    TrailingData = 8,
    InvalidTeamId = 9,
    InvalidShotEvaluationMode = 10,
    InvalidRuntimeParamScope = 11,
    InvalidStagedApplyBoundary = 12,
    InvalidSessionPresence = 13,
    InvalidParticipationState = 14,
    InvalidControlBindingKind = 15,
    InvalidPaneSlot = 16,
    InvalidPaneViewMode = 17,
    InvalidRuntimeReconciliationStrategy = 18,
    InvalidSessionActionKind = 19,
    InvalidSessionVisualizationMode = 20
};

struct PacketHeader {
    std::uint32_t magic{kProtocolMagic};
    std::uint16_t version{kProtocolVersion};
    std::uint16_t peerId{0};
    Channel channel{Channel::Control};
    std::uint32_t seq{0};
    std::uint32_t ack{0};
    std::uint32_t ackBits{0};
    PacketKind kind{PacketKind::Hello};
};

struct HelloMessage {
    std::uint32_t sessionId{0};
    std::uint16_t requestedPeerId{0};
    std::string playerName{};
    // `Spectator` joins as an authoritative spectator.
    sim::TeamId requestedTeam{sim::TeamId::None};
};

struct WelcomeMessage {
    std::uint32_t sessionId{0};
    std::uint16_t assignedPeerId{0};
    std::uint16_t snapshotRateHz{0};
    std::int32_t levelSlot{-1};
    std::uint32_t levelHash{0u};
    sim::TimingCadence cadence{};
    sim::ParticipantState participantState{};
    sim::PaneViewState paneView{};
    sim::AuthoritativeTime authoritativeTime{};
    struct HostedSessionMetadata {
        std::string sessionLabel{};
        std::string hostPlayerName{};
        std::uint16_t hostPeerId{0u};
        std::int32_t levelSlot{-1};
        std::uint32_t levelHash{0u};
        std::uint16_t publicJoinPort{0u};
        std::uint16_t maxHumanPlayers{2u};
        ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
        SessionVisualizationMode visualizationMode{SessionVisualizationMode::Diagnostic};
        bool botsFrozen{true};
        bool botsCanShoot{true};
        bool studyEventLoggingEnabled{false};
        std::string studyEventRunId{};
    } sessionMetadata{};
};

using HostedSessionMetadata = WelcomeMessage::HostedSessionMetadata;

struct DisconnectMessage {
    std::uint16_t reasonCode{0};
    std::string reason{};
};

struct TeamChangeRequest {
    // `None` exits spectator to the last playable team when possible, or back to team selection.
    sim::TeamId requestedTeam{sim::TeamId::None};
};

struct RuntimeParamChangeRequest {
    RuntimeParamScope scope{RuntimeParamScope::Global};
    std::int32_t targetId{-1};
    std::string key{};
    float value{0.0f};
};

struct RuntimeParamSnapshot {
    RuntimeParamScope scope{RuntimeParamScope::Global};
    std::int32_t targetId{-1};
    std::string key{};
    float value{0.0f};
};

struct RuntimeParamApplyResult {
    RuntimeParamScope scope{RuntimeParamScope::Global};
    std::int32_t targetId{-1};
    std::string key{};
    float value{0.0f};
    bool applied{false};
    sim::StagedApplyBoundary stagedApplyBoundary{sim::StagedApplyBoundary::NextTick};
    std::string message{};
};

struct SessionActionRequest {
    SessionActionKind kind{SessionActionKind::SpawnFrozenBotAhead};
};

struct SessionActionResult {
    SessionActionKind kind{SessionActionKind::SpawnFrozenBotAhead};
    bool applied{false};
    std::int32_t actorId{-1};
    std::string message{};
};

struct CommandBundle {
    std::vector<sim::PlayerCommand> commands{};
};

struct ControlCommandBundle {
    std::vector<sim::PlayerCommand> commands{};
};

struct GameplayEvent {
    SnapshotEventKind kind{SnapshotEventKind::WeaponFired};
    std::int32_t sourcePlayerId{0};
    std::int32_t targetEntityId{0};
    sim::Vec3 origin{};
    sim::Vec3 direction{0.0f, 0.0f, -1.0f};
    bool hit{false};
};

using SnapshotEvent = GameplayEvent;

struct ReplicationSnapshot {
    std::uint32_t serverTick{0};
    std::uint64_t serverTimeUs{0};
    std::uint32_t ackedInputSeq{0};
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
    sim::ParticipantState localParticipantState{};
    sim::PaneViewState localPaneView{};
    sim::PlayerState localPlayerState{};
    std::vector<sim::PlayerState> remotePlayers{};
    std::vector<sim::PlayerState> controlRemotePlayers{};
    std::vector<sim::RemoteActorState> remoteEnemies{};
};

struct SessionSummary {
    HostedSessionMetadata sessionMetadata{};
    std::vector<sim::RosterEntry> roster{};
    sim::TeamScores teamScores{};
};

struct EligibleActorContracts {
    std::vector<sim::EligibleActor> controlTargets{};
    std::vector<sim::EligibleActor> spectatorTargets{};
};

struct ControlBindingContracts {
    sim::ParticipantState participantState{};
    sim::PaneViewState paneView{};
    EligibleActorContracts eligibleActors{};
};

struct GameplayEventBatch {
    std::vector<GameplayEvent> events{};
};

struct WorldSnapshot : ReplicationSnapshot, SessionSummary, GameplayEventBatch {
    ReplicationSnapshot& replication() {
        return *this;
    }

    const ReplicationSnapshot& replication() const {
        return *this;
    }

    SessionSummary& summary() {
        return *this;
    }

    const SessionSummary& summary() const {
        return *this;
    }

    GameplayEventBatch& gameplayEvents() {
        return *this;
    }

    const GameplayEventBatch& gameplayEvents() const {
        return *this;
    }

    static WorldSnapshot fromContracts(const ReplicationSnapshot& replication,
                                       const SessionSummary& summary,
                                       const GameplayEventBatch& gameplayEvents) {
        WorldSnapshot snapshot;
        snapshot.replication() = replication;
        snapshot.summary() = summary;
        snapshot.gameplayEvents() = gameplayEvents;
        return snapshot;
    }
};

struct ControlWorldSnapshot {
    WorldSnapshot snapshot{};
};

struct ProxyLinkConfig {
    float baseDelayMs{0.0f};
    float jitterMs{0.0f};
    float lossPct{0.0f};
    float duplicatePct{0.0f};
    float reorderPct{0.0f};
    std::uint32_t seed{0};
};

struct ProxyControl {
    std::uint16_t targetPeerId{0};
    bool upstream{true};
    ProxyLinkConfig config{};
};

struct ProxyStats {
    std::uint64_t receivedPackets{0};
    std::uint64_t forwardedPackets{0};
    std::uint64_t droppedPackets{0};
    std::uint64_t duplicatedPackets{0};
    std::uint64_t reorderedPackets{0};
    std::uint32_t queuedPackets{0};
};

using PacketPayload = std::variant<
    HelloMessage,
    WelcomeMessage,
    DisconnectMessage,
    TeamChangeRequest,
    RuntimeParamChangeRequest,
    RuntimeParamSnapshot,
    RuntimeParamApplyResult,
    SessionActionRequest,
    SessionActionResult,
    CommandBundle,
    ControlCommandBundle,
    WorldSnapshot,
    ControlWorldSnapshot,
    ProxyControl,
    ProxyStats>;

struct Packet {
    PacketHeader header{};
    PacketPayload payload{};
};

struct ParseResult {
    bool ok{false};
    ParseError error{ParseError::None};
    Packet packet{};
};

struct ReliableControlState {
    std::uint32_t lastSentSeq{0};
    std::uint32_t lastAckedSeq{0};
    std::uint64_t lastSendTimeMs{0};
    std::uint32_t retransmitTimeoutMs{250};
    bool awaitingAck{false};
};

const char* toString(ParseError error);
const char* toString(ShotEvaluationMode mode);
const char* toString(SessionVisualizationMode mode);
bool tryParseShotEvaluationMode(std::uint8_t rawMode, ShotEvaluationMode* modeOut);
bool tryParseShotEvaluationModeValue(float rawValue, ShotEvaluationMode* modeOut);
bool tryParseSessionVisualizationMode(std::uint8_t rawMode,
                                      SessionVisualizationMode* modeOut);
bool tryParseSessionVisualizationModeValue(float rawValue,
                                           SessionVisualizationMode* modeOut);
bool tryParseSessionTickRateHzValue(float rawValue, std::uint16_t* tickRateHzOut);
bool tryParseSessionSnapshotRateHzValue(float rawValue, std::uint16_t* snapshotRateHzOut);
PacketKind packetKindForPayload(const PacketPayload& payload);

ByteBuffer serializeHeader(const PacketHeader& header);
ParseError deserializeHeader(const ByteBuffer& bytes, PacketHeader* headerOut, std::size_t* bytesConsumed = nullptr);

ByteBuffer serializePacket(const Packet& packet);
ParseResult deserializePacket(const ByteBuffer& bytes);

bool isNewerSequence(std::uint32_t candidateSeq, std::uint32_t referenceSeq);
bool shouldAcceptSequence(std::uint32_t newestAcceptedSeq, std::uint32_t candidateSeq);
bool shouldRetransmit(const ReliableControlState& state, std::uint64_t nowMs);

bool operator==(const PacketHeader& lhs, const PacketHeader& rhs);
bool operator==(const HelloMessage& lhs, const HelloMessage& rhs);
bool operator==(const HostedSessionMetadata& lhs, const HostedSessionMetadata& rhs);
bool operator==(const WelcomeMessage& lhs, const WelcomeMessage& rhs);
bool operator==(const DisconnectMessage& lhs, const DisconnectMessage& rhs);
bool operator==(const TeamChangeRequest& lhs, const TeamChangeRequest& rhs);
bool operator==(const RuntimeParamChangeRequest& lhs, const RuntimeParamChangeRequest& rhs);
bool operator==(const RuntimeParamSnapshot& lhs, const RuntimeParamSnapshot& rhs);
bool operator==(const RuntimeParamApplyResult& lhs, const RuntimeParamApplyResult& rhs);
bool operator==(const SessionActionRequest& lhs, const SessionActionRequest& rhs);
bool operator==(const SessionActionResult& lhs, const SessionActionResult& rhs);
bool operator==(const CommandBundle& lhs, const CommandBundle& rhs);
bool operator==(const ControlCommandBundle& lhs, const ControlCommandBundle& rhs);
bool operator==(const GameplayEvent& lhs, const GameplayEvent& rhs);
bool operator==(const ReplicationSnapshot& lhs, const ReplicationSnapshot& rhs);
bool operator==(const SessionSummary& lhs, const SessionSummary& rhs);
bool operator==(const GameplayEventBatch& lhs, const GameplayEventBatch& rhs);
bool operator==(const WorldSnapshot& lhs, const WorldSnapshot& rhs);
bool operator==(const ControlWorldSnapshot& lhs, const ControlWorldSnapshot& rhs);
bool operator==(const ProxyLinkConfig& lhs, const ProxyLinkConfig& rhs);
bool operator==(const ProxyControl& lhs, const ProxyControl& rhs);
bool operator==(const ProxyStats& lhs, const ProxyStats& rhs);
bool operator==(const Packet& lhs, const Packet& rhs);

}  // namespace net
