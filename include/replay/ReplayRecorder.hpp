#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "replay/ReplayDemo.hpp"

namespace replay {

struct ReplayRecordingInfo {
    std::uint16_t protocolVersion{net::kProtocolVersion};
    int levelSlot{-1};
    std::uint32_t levelHash{0u};
    std::uint16_t tickRateHz{60u};
    std::uint16_t snapshotRateHz{20u};
    std::uint32_t maxRewindMs{500u};
    float respawnDelaySeconds{5.0f};
    float spawnProtectionSeconds{0.0f};
    net::ShotEvaluationMode shotEvaluationMode{net::ShotEvaluationMode::SeenPosition};
    std::uint64_t startedServerTimeUs{0u};
    std::string title{};
    std::string sourceLabel{};
};

class CommandReplayRecorder {
public:
    void start(const ReplayRecordingInfo& info,
               const sim::WorldState& initialWorldState,
               const sim::SimConfig& simConfig) {
        demo_ = ReplayDemo{};
        demo_.header.protocolVersion = info.protocolVersion;
        demo_.header.levelSlot = info.levelSlot;
        demo_.header.levelHash = info.levelHash;
        demo_.header.tickRateHz = info.tickRateHz;
        demo_.header.snapshotRateHz = info.snapshotRateHz;
        demo_.header.maxRewindMs = info.maxRewindMs;
        demo_.header.respawnDelaySeconds = info.respawnDelaySeconds;
        demo_.header.spawnProtectionSeconds = info.spawnProtectionSeconds;
        demo_.header.shotEvaluationMode = info.shotEvaluationMode;
        demo_.header.startedServerTimeUs = info.startedServerTimeUs;
        demo_.header.recordedAtUnixSeconds = currentUnixSeconds();
        demo_.header.title = info.title;
        demo_.header.sourceLabel = info.sourceLabel;
        demo_.initialState.worldState = initialWorldState;
        demo_.initialState.simConfig = simConfig;
        recording_ = true;
    }

    void stop(std::uint64_t serverTimeUs) {
        if (!recording_) {
            return;
        }
        updateDuration(serverTimeUs);
        recording_ = false;
    }

    void clear() {
        demo_ = ReplayDemo{};
        recording_ = false;
    }

    bool recording() const {
        return recording_;
    }

    const ReplayDemo& demo() const {
        return demo_;
    }

    ReplayDemo snapshot() const {
        return demo_;
    }

    void recordCommandReceived(ReplayTrack track,
                               std::uint16_t peerId,
                               std::uint32_t serverTick,
                               std::uint64_t serverTimeUs,
                               const sim::PlayerCommand& command) {
        recordCommand(track,
                      ReplayCommandStage::Received,
                      peerId,
                      -1,
                      serverTick,
                      serverTimeUs,
                      command);
    }

    void recordCommandApplied(ReplayTrack track,
                              std::uint16_t peerId,
                              int actorId,
                              std::uint32_t serverTick,
                              std::uint64_t serverTimeUs,
                              const sim::PlayerCommand& command) {
        recordCommand(track,
                      ReplayCommandStage::Applied,
                      peerId,
                      actorId,
                      serverTick,
                      serverTimeUs,
                      command);
    }

    void recordRuntimeEvent(const ReplayRuntimeEvent& event) {
        if (!recording_) {
            return;
        }
        demo_.runtimeEvents.push_back(event);
        updateDuration(event.serverTimeUs);
    }

    void recordCombatEvent(std::uint32_t serverTick,
                           std::uint64_t serverTimeUs,
                           const net::SnapshotEvent& event) {
        if (!recording_) {
            return;
        }
        demo_.combatEvents.push_back(ReplayCombatEvent{serverTick, serverTimeUs, event});
        updateDuration(serverTimeUs);
    }

    void recordAuthoritativeKeyframe(std::uint32_t serverTick,
                                     std::uint64_t serverTimeUs,
                                     const sim::WorldState& worldState) {
        if (!recording_) {
            return;
        }
        WorldKeyframe keyframe;
        keyframe.track = ReplayTrack::Authoritative;
        keyframe.serverTick = serverTick;
        keyframe.serverTimeUs = serverTimeUs;
        keyframe.worldState = worldState;
        demo_.keyframes.push_back(std::move(keyframe));
        updateDuration(serverTimeUs);
    }

    void recordControlKeyframe(std::uint32_t serverTick,
                               std::uint64_t serverTimeUs,
                               const sim::WorldState& sourceWorldState,
                               std::vector<sim::PlayerState> controlPlayers) {
        if (!recording_) {
            return;
        }
        WorldKeyframe keyframe;
        keyframe.track = ReplayTrack::Control;
        keyframe.serverTick = serverTick;
        keyframe.serverTimeUs = serverTimeUs;
        keyframe.worldState = sourceWorldState;
        keyframe.controlPlayers = std::move(controlPlayers);
        if (!keyframe.controlPlayers.empty()) {
            demo_.header.hasControlLane = true;
        }
        demo_.keyframes.push_back(std::move(keyframe));
        updateDuration(serverTimeUs);
    }

    void recordClientPerceptionTrack(ClientPerceptionTrack track) {
        if (!recording_) {
            return;
        }
        demo_.clientTracks.push_back(std::move(track));
        demo_.header.hasClientPerceptionTracks = true;
    }

private:
    static std::uint64_t currentUnixSeconds() {
        return static_cast<std::uint64_t>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    void recordCommand(ReplayTrack track,
                       ReplayCommandStage stage,
                       std::uint16_t peerId,
                       int actorId,
                       std::uint32_t serverTick,
                       std::uint64_t serverTimeUs,
                       const sim::PlayerCommand& command) {
        if (!recording_) {
            return;
        }
        demo_.commandEvents.push_back(
            ServerCommandEvent{track, stage, peerId, actorId, serverTick, serverTimeUs, command});
        if (track == ReplayTrack::Control) {
            demo_.header.hasControlLane = true;
        }
        updateDuration(serverTimeUs);
    }

    void updateDuration(std::uint64_t serverTimeUs) {
        if (serverTimeUs < demo_.header.startedServerTimeUs) {
            return;
        }
        const std::uint64_t duration = serverTimeUs - demo_.header.startedServerTimeUs;
        if (duration > demo_.header.durationUs) {
            demo_.header.durationUs = duration;
        }
    }

    ReplayDemo demo_{};
    bool recording_{false};
};

}  // namespace replay
