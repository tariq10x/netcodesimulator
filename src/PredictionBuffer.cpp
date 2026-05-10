#include "net/PredictionBuffer.hpp"

#include <algorithm>
#include <cmath>

#include "sim/SimulationRules.hpp"

namespace net {
namespace {

constexpr float kSmoothingThreshold = 0.35f;
constexpr float kForcedSnapThreshold = 1.5f;

float distance(const sim::Vec3& lhs, const sim::Vec3& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    const float dz = lhs.z - rhs.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

sim::Vec3 lerp(const sim::Vec3& from, const sim::Vec3& to, float alpha) {
    return sim::Vec3{
        from.x + (to.x - from.x) * alpha,
        from.y + (to.y - from.y) * alpha,
        from.z + (to.z - from.z) * alpha
    };
}

float lerpScalar(float from, float to, float alpha) {
    return from + (to - from) * alpha;
}

sim::PlayerState lerpPlayerState(const sim::PlayerState& from,
                                 const sim::PlayerState& to,
                                 float alpha) {
    sim::PlayerState state = to;
    state.position = lerp(from.position, to.position, alpha);
    state.velocity = lerp(from.velocity, to.velocity, alpha);
    state.yaw = lerpScalar(from.yaw, to.yaw, alpha);
    state.pitch = lerpScalar(from.pitch, to.pitch, alpha);
    return state;
}

}  // namespace

namespace {

const sim::PlayerState& emptyPlayerState() {
    static const sim::PlayerState state{};
    return state;
}

const std::vector<net::ReconciliationResult>& emptyCorrectionHistory() {
    static const std::vector<net::ReconciliationResult> history{};
    return history;
}

}  // namespace

void PredictionBuffer::reset(const sim::PlayerState& state) {
    predictedState_ = state;
    displayState_ = state;
    pendingCommands_.clear();
    smoothCorrection_ = SmoothCorrectionState{};
}

void PredictionBuffer::recordCommand(const sim::PlayerCommand& command,
                                     const sim::MovementEnvironment& environment,
                                     const sim::SimConfig& config) {
    pendingCommands_.push_back(command);
    predictedState_ = sim::applyPlayerCommand(predictedState_, command, environment, config);
    displayState_ = predictedState_;
    smoothCorrection_.active = false;
}

ReconciliationResult PredictionBuffer::reconcile(const sim::PlayerState& authoritativeState,
                                                 std::uint32_t ackedInputSeq,
                                                 const sim::MovementEnvironment& environment,
                                                 const sim::SimConfig& config,
                                                 ReconciliationStrategy strategy,
                                                 std::uint32_t smoothWindowMs) {
    pendingCommands_.erase(
        std::remove_if(pendingCommands_.begin(),
                       pendingCommands_.end(),
                       [ackedInputSeq](const sim::PlayerCommand& command) {
                           return command.seq <= ackedInputSeq;
                       }),
        pendingCommands_.end());

    sim::PlayerState replayedState = authoritativeState;
    for (const auto& command : pendingCommands_) {
        replayedState = sim::applyPlayerCommand(replayedState, command, environment, config);
    }

    ReconciliationResult result;
    const sim::PlayerState previousDisplayState = displayState_;
    result.magnitude = distance(displayState_.position, replayedState.position);
    result.replayedCommandCount = static_cast<std::uint32_t>(pendingCommands_.size());
    predictedState_ = replayedState;
    displayState_ = replayedState;
    smoothCorrection_.active = false;

    if (result.magnitude <= 0.0001f) {
        result.mode = ReconciliationMode::None;
        return result;
    }

    if (strategy == ReconciliationStrategy::Smooth &&
        result.magnitude <= kSmoothingThreshold &&
        result.magnitude < kForcedSnapThreshold) {
        result.mode = ReconciliationMode::Smooth;
        smoothCorrection_.active = true;
        smoothCorrection_.start = previousDisplayState;
        smoothCorrection_.target = replayedState;
        smoothCorrection_.elapsedSeconds = 0.0f;
        smoothCorrection_.durationSeconds =
            std::max(0.001f, static_cast<float>(smoothWindowMs) / 1000.0f);
        displayState_ = lerpPlayerState(previousDisplayState, replayedState, 0.0f);
        return result;
    }

    result.mode = ReconciliationMode::Snap;
    return result;
}

void PredictionBuffer::advanceDisplay(float dtSeconds) {
    if (!smoothCorrection_.active || dtSeconds <= 0.0f) {
        return;
    }

    smoothCorrection_.elapsedSeconds += dtSeconds;
    const float alpha = std::clamp(smoothCorrection_.elapsedSeconds /
                                       smoothCorrection_.durationSeconds,
                                   0.0f,
                                   1.0f);
    displayState_ = lerpPlayerState(smoothCorrection_.start, smoothCorrection_.target, alpha);
    if (alpha >= 1.0f) {
        displayState_ = smoothCorrection_.target;
        smoothCorrection_.active = false;
    }
}

const sim::PlayerState& PredictionBuffer::predictedState() const {
    return predictedState_;
}

const sim::PlayerState& PredictionBuffer::displayState() const {
    return displayState_;
}

std::size_t PredictionBuffer::pendingCommandCount() const {
    return pendingCommands_.size();
}

void PredictionBufferMap::clear() {
    participantStates_.clear();
}

void PredictionBufferMap::resetParticipant(std::uint16_t participantId,
                                           const sim::PlayerState& state) {
    ParticipantPredictionState& participantState = ensureParticipantState(participantId);
    participantState.buffer.reset(state);
    participantState.lastAckedInputSeq = 0u;
    participantState.correctionHistory.clear();
}

void PredictionBufferMap::recordCommand(std::uint16_t participantId,
                                        const sim::PlayerCommand& command,
                                        const sim::MovementEnvironment& environment,
                                        const sim::SimConfig& config) {
    ensureParticipantState(participantId).buffer.recordCommand(command, environment, config);
}

ReconciliationResult PredictionBufferMap::reconcile(std::uint16_t participantId,
                                                    const sim::PlayerState& authoritativeState,
                                                    std::uint32_t ackedInputSeq,
                                                    const sim::MovementEnvironment& environment,
                                                    const sim::SimConfig& config,
                                                    ReconciliationStrategy strategy,
                                                    std::uint32_t smoothWindowMs) {
    ParticipantPredictionState& participantState = ensureParticipantState(participantId);
    participantState.lastAckedInputSeq = ackedInputSeq;
    const ReconciliationResult result =
        participantState.buffer.reconcile(authoritativeState,
                                          ackedInputSeq,
                                          environment,
                                          config,
                                          strategy,
                                          smoothWindowMs);
    participantState.correctionHistory.push_back(result);
    return result;
}

bool PredictionBufferMap::hasParticipant(std::uint16_t participantId) const {
    return findParticipantState(participantId) != nullptr;
}

std::size_t PredictionBufferMap::participantCount() const {
    return participantStates_.size();
}

std::size_t PredictionBufferMap::pendingCommandCount(std::uint16_t participantId) const {
    const ParticipantPredictionState* participantState = findParticipantState(participantId);
    return participantState != nullptr ? participantState->buffer.pendingCommandCount() : 0u;
}

std::uint32_t PredictionBufferMap::lastAckedInputSeq(std::uint16_t participantId) const {
    const ParticipantPredictionState* participantState = findParticipantState(participantId);
    return participantState != nullptr ? participantState->lastAckedInputSeq : 0u;
}

const std::vector<ReconciliationResult>& PredictionBufferMap::correctionHistory(
    std::uint16_t participantId) const {
    const ParticipantPredictionState* participantState = findParticipantState(participantId);
    return participantState != nullptr ? participantState->correctionHistory : emptyCorrectionHistory();
}

const sim::PlayerState& PredictionBufferMap::predictedState(std::uint16_t participantId) const {
    const ParticipantPredictionState* participantState = findParticipantState(participantId);
    return participantState != nullptr ? participantState->buffer.predictedState() : emptyPlayerState();
}

const sim::PlayerState& PredictionBufferMap::displayState(std::uint16_t participantId) const {
    const ParticipantPredictionState* participantState = findParticipantState(participantId);
    return participantState != nullptr ? participantState->buffer.displayState() : emptyPlayerState();
}

ParticipantPredictionState* PredictionBufferMap::findParticipantState(std::uint16_t participantId) {
    const PredictionBufferMap* constThis = this;
    return const_cast<ParticipantPredictionState*>(constThis->findParticipantState(participantId));
}

const ParticipantPredictionState* PredictionBufferMap::findParticipantState(
    std::uint16_t participantId) const {
    const auto it = std::find_if(participantStates_.begin(),
                                 participantStates_.end(),
                                 [participantId](const ParticipantPredictionState& participantState) {
                                     return participantState.participantId == participantId;
                                 });
    return it != participantStates_.end() ? &(*it) : nullptr;
}

ParticipantPredictionState& PredictionBufferMap::ensureParticipantState(std::uint16_t participantId) {
    if (ParticipantPredictionState* participantState = findParticipantState(participantId)) {
        return *participantState;
    }

    participantStates_.push_back(ParticipantPredictionState{});
    ParticipantPredictionState& participantState = participantStates_.back();
    participantState.participantId = participantId;
    return participantState;
}

}  // namespace net
