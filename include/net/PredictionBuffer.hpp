#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sim/SimulationTypes.hpp"

namespace net {

enum class ReconciliationMode : std::uint8_t {
    None = 0,
    Smooth = 1,
    Snap = 2
};

enum class ReconciliationStrategy : std::uint8_t {
    Snap = 0,
    Smooth = 1
};

struct ReconciliationResult {
    ReconciliationMode mode{ReconciliationMode::None};
    float magnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
};

struct CorrectionRecord {
    ReconciliationStrategy strategy{ReconciliationStrategy::Smooth};
    std::uint32_t smoothWindowMs{250u};
    ReconciliationMode mode{ReconciliationMode::None};
    float magnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
};

class PredictionBuffer {
public:
    void reset(const sim::PlayerState& state);
    void recordCommand(const sim::PlayerCommand& command,
                       const sim::MovementEnvironment& environment,
                       const sim::SimConfig& config = {});
    ReconciliationResult reconcile(const sim::PlayerState& authoritativeState,
                                   std::uint32_t ackedInputSeq,
                                   const sim::MovementEnvironment& environment,
                                   const sim::SimConfig& config = {},
                                   ReconciliationStrategy strategy = ReconciliationStrategy::Smooth,
                                   std::uint32_t smoothWindowMs = 250u);
    void advanceDisplay(float dtSeconds);

    const sim::PlayerState& predictedState() const;
    const sim::PlayerState& displayState() const;
    std::size_t pendingCommandCount() const;

private:
    struct SmoothCorrectionState {
        bool active{false};
        sim::PlayerState start{};
        sim::PlayerState target{};
        float elapsedSeconds{0.0f};
        float durationSeconds{0.25f};
    };

    sim::PlayerState predictedState_{};
    sim::PlayerState displayState_{};
    std::vector<sim::PlayerCommand> pendingCommands_{};
    SmoothCorrectionState smoothCorrection_{};
};

struct ParticipantPredictionState {
    std::uint16_t participantId{0u};
    PredictionBuffer buffer{};
    std::uint32_t lastAckedInputSeq{0u};
    std::vector<ReconciliationResult> correctionHistory{};
};

class PredictionBufferMap {
public:
    void clear();
    void resetParticipant(std::uint16_t participantId, const sim::PlayerState& state);
    void recordCommand(std::uint16_t participantId,
                       const sim::PlayerCommand& command,
                       const sim::MovementEnvironment& environment,
                       const sim::SimConfig& config = {});
    ReconciliationResult reconcile(std::uint16_t participantId,
                                   const sim::PlayerState& authoritativeState,
                                   std::uint32_t ackedInputSeq,
                                   const sim::MovementEnvironment& environment,
                                   const sim::SimConfig& config = {},
                                   ReconciliationStrategy strategy = ReconciliationStrategy::Smooth,
                                   std::uint32_t smoothWindowMs = 250u);

    bool hasParticipant(std::uint16_t participantId) const;
    std::size_t participantCount() const;
    std::size_t pendingCommandCount(std::uint16_t participantId) const;
    std::uint32_t lastAckedInputSeq(std::uint16_t participantId) const;
    const std::vector<ReconciliationResult>& correctionHistory(std::uint16_t participantId) const;
    const sim::PlayerState& predictedState(std::uint16_t participantId) const;
    const sim::PlayerState& displayState(std::uint16_t participantId) const;

private:
    ParticipantPredictionState* findParticipantState(std::uint16_t participantId);
    const ParticipantPredictionState* findParticipantState(std::uint16_t participantId) const;
    ParticipantPredictionState& ensureParticipantState(std::uint16_t participantId);

    std::vector<ParticipantPredictionState> participantStates_{};
};

}  // namespace net
