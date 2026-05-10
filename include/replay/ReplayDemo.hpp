#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/Protocol.hpp"
#include "sim/WorldState.hpp"

namespace replay {

inline constexpr std::uint32_t kCommandReplayFormatVersion = 2u;
inline constexpr std::uint32_t kLegacyCommandReplayFormatVersion = 1u;

enum class ReplayTrack : std::uint8_t {
    Authoritative = 0,
    Control = 1
};

enum class ReplayCommandStage : std::uint8_t {
    Received = 0,
    Applied = 1
};

enum class ReplayRuntimeEventKind : std::uint8_t {
    RuntimeParamChanged = 0,
    SessionAction = 1
};

enum class ClientPerceptionEventKind : std::uint8_t {
    SnapshotReceived = 0,
    PredictionCorrection = 1,
    CameraState = 2,
    LocalPendingShot = 3
};

struct ReplayHeader {
    std::uint32_t formatVersion{kCommandReplayFormatVersion};
    std::uint16_t protocolVersion{net::kProtocolVersion};
    int levelSlot{-1};
    std::uint32_t levelHash{0u};
    std::uint16_t tickRateHz{60u};
    std::uint16_t snapshotRateHz{20u};
    std::uint32_t simConfigVersion{1u};
    std::uint64_t recordedAtUnixSeconds{0u};
    std::uint64_t startedServerTimeUs{0u};
    std::uint64_t durationUs{0u};
    std::uint32_t maxRewindMs{500u};
    float respawnDelaySeconds{5.0f};
    float spawnProtectionSeconds{0.0f};
    net::ShotEvaluationMode shotEvaluationMode{net::ShotEvaluationMode::SeenPosition};
    bool hasControlLane{false};
    bool hasClientPerceptionTracks{false};
    std::string title{};
    std::string sourceLabel{};
};

struct ReplayInitialState {
    sim::WorldState worldState{};
    sim::SimConfig simConfig{};
};

struct ServerCommandEvent {
    ReplayTrack track{ReplayTrack::Authoritative};
    ReplayCommandStage stage{ReplayCommandStage::Received};
    std::uint16_t peerId{0u};
    int actorId{-1};
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
    sim::PlayerCommand command{};
};

struct ReplayRuntimeEvent {
    ReplayRuntimeEventKind kind{ReplayRuntimeEventKind::RuntimeParamChanged};
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
    std::uint16_t peerId{0u};
    std::string key{};
    float value{0.0f};
    bool applied{false};
};

struct ReplayCombatEvent {
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
    net::SnapshotEvent event{};
};

struct WorldKeyframe {
    ReplayTrack track{ReplayTrack::Authoritative};
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
    sim::WorldState worldState{};
    std::vector<sim::PlayerState> controlPlayers{};
};

struct ClientPerceptionEvent {
    ClientPerceptionEventKind kind{ClientPerceptionEventKind::SnapshotReceived};
    std::uint32_t clientFrame{0u};
    std::uint64_t clientTimeUs{0u};
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
    std::uint32_t commandSeq{0u};
    float correctionMagnitude{0.0f};
    float yaw{0.0f};
    float pitch{0.0f};
    bool predictionEnabled{false};
    bool interpolationEnabled{false};
};

struct ClientPerceptionTrack {
    std::uint16_t peerId{0u};
    std::string playerName{};
    std::vector<ClientPerceptionEvent> events{};
};

struct ReplayDemo {
    ReplayHeader header{};
    ReplayInitialState initialState{};
    std::vector<ServerCommandEvent> commandEvents{};
    std::vector<ReplayRuntimeEvent> runtimeEvents{};
    std::vector<ReplayCombatEvent> combatEvents{};
    std::vector<WorldKeyframe> keyframes{};
    std::vector<ClientPerceptionTrack> clientTracks{};
};

}  // namespace replay
