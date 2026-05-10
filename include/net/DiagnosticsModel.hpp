#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/Protocol.hpp"

namespace net {

class TransportArtifactAdapter;

struct LocalNetworkSettings {
    float latencyMs{0.0f};
    float lossPct{0.0f};
};

struct RuntimeParameterViewState {
    bool available{false};
    float requestedLatencyMs{0.0f};
    float requestedLossPct{0.0f};
    bool hasAuthoritativeValues{false};
    float effectiveLatencyMs{0.0f};
    float effectiveLossPct{0.0f};
    bool hasApplyResult{false};
    bool applied{false};
    sim::StagedApplyBoundary stagedApplyBoundary{sim::StagedApplyBoundary::NextTick};
    std::string statusMessage{};
};

enum class ShotEvaluationStateView : std::uint8_t {
    LiveState = 0,
    RewoundState = 1
};

struct ShotEvaluationRecord {
    std::uint64_t viewedTimeUs{0u};
    std::uint64_t rewindTimeUs{0u};
    ShotEvaluationStateView evaluatedState{ShotEvaluationStateView::LiveState};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    bool authoritativeHit{false};
};

struct ShotEvaluationViewState {
    bool available{false};
    std::uint64_t viewedTimeUs{0u};
    std::uint64_t rewindTimeUs{0u};
    ShotEvaluationStateView evaluatedState{ShotEvaluationStateView::LiveState};
    std::string evaluatedStateLabel{};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    std::string activeRuleLabel{};
    std::string activeRuleExplanation{};
    bool authoritativeHit{false};
    std::string outcomeLabel{};
};

enum class DiagnosticsReconciliationStrategyView : std::uint8_t {
    Snap = 0,
    Smooth = 1
};

enum class DiagnosticsCorrectionModeView : std::uint8_t {
    None = 0,
    Smooth = 1,
    Snap = 2
};

struct DiagnosticsStudyRecord {
    sim::ParticipantState participantState{};
    sim::PaneViewState paneView{};
    std::vector<sim::RosterEntry> roster{};
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
    std::uint32_t ackedInputSeq{0u};
    DiagnosticsReconciliationStrategyView reconciliationStrategy{
        DiagnosticsReconciliationStrategyView::Smooth};
    DiagnosticsCorrectionModeView correctionMode{DiagnosticsCorrectionModeView::None};
    float correctionMagnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
    std::uint32_t pendingInputCount{0u};
    std::uint32_t smoothWindowMs{0u};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    bool canReturnToCharacter{false};
};

struct DiagnosticsAuthorityViewState {
    bool available{false};
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
    std::uint32_t ackedInputSeq{0u};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    std::string activeRuleLabel{};
    std::string activeRuleExplanation{};
};

struct DiagnosticsPredictionViewState {
    bool available{false};
    DiagnosticsReconciliationStrategyView reconciliationStrategy{
        DiagnosticsReconciliationStrategyView::Smooth};
    DiagnosticsCorrectionModeView correctionMode{DiagnosticsCorrectionModeView::None};
    std::string reconciliationStrategyLabel{};
    std::string correctionModeLabel{};
    float correctionMagnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
    std::uint32_t pendingInputCount{0u};
    std::uint32_t smoothWindowMs{0u};
};

struct DiagnosticsPaneLabelViewState {
    bool available{false};
    sim::PaneSlot slot{sim::PaneSlot::Left};
    bool focused{true};
    sim::PaneViewMode mode{sim::PaneViewMode::PlayerControlled};
    int boundActorId{-1};
    std::string slotLabel{};
    std::string focusLabel{};
    std::string modeLabel{};
    std::string bindingLabel{};
    std::string paneLabel{};
};

struct DiagnosticsSpectatorViewState {
    bool available{false};
    bool sessionSpectator{false};
    bool paneObservation{false};
    bool canReturnToCharacter{false};
    sim::TeamId participantTeam{sim::TeamId::None};
    int followTargetActorId{-1};
    std::string participantTeamLabel{};
    std::string followTargetLabel{};
};

struct DiagnosticsStudyViewState {
    DiagnosticsAuthorityViewState authority{};
    DiagnosticsPredictionViewState prediction{};
    DiagnosticsPaneLabelViewState pane{};
    DiagnosticsSpectatorViewState spectator{};
    ShotEvaluationViewState shot{};
};

class DiagnosticsModel {
public:
    explicit DiagnosticsModel(std::uint16_t targetPeerId = 0u);

    void setTargetPeerId(std::uint16_t peerId);
    std::uint16_t targetPeerId() const;
    RuntimeParamScope targetScope() const;

    const ProxyLinkConfig& linkConfig(bool upstream) const;
    const ProxyStats& linkStats(bool upstream) const;

    void setBaseDelayMs(bool upstream, float value);
    void setJitterMs(bool upstream, float value);
    void setLossPct(bool upstream, float value);
    void setDuplicatePct(bool upstream, float value);
    void setReorderPct(bool upstream, float value);
    const LocalNetworkSettings& localNetworkSettings() const;
    const LocalNetworkSettings& authoritativeLocalNetworkSettings() const;
    bool hasAuthoritativeLocalNetworkSettings() const;
    void setLocalLatencyMs(float value);
    void setLocalLossPct(float value);
    void syncAuthoritativeLocalNetworkSettings(std::uint16_t latencyMs, std::uint8_t lossPct);

    ProxyControl buildControl(bool upstream) const;
    std::vector<RuntimeParamChangeRequest> buildLocalNetworkRequests() const;
    void applyControl(TransportArtifactAdapter* proxy, bool upstream) const;
    bool consumeRuntimeParamSnapshot(const RuntimeParamSnapshot& snapshot);
    bool consumeRuntimeParamApplyResult(const RuntimeParamApplyResult& result);
    void refreshFromProxy(const TransportArtifactAdapter& proxy);
    RuntimeParameterViewState runtimeParameterView() const;
    void recordShotEvaluation(const ShotEvaluationRecord& record);
    ShotEvaluationViewState shotEvaluationView() const;
    void recordStudyState(const DiagnosticsStudyRecord& record);
    DiagnosticsStudyViewState studyView() const;
    std::vector<std::string> shotEvaluationSummaryLines() const;

    std::vector<std::string> summaryLines() const;
    std::vector<std::string> localNetworkSummaryLines() const;

private:
    ProxyLinkConfig* mutableLinkConfig(bool upstream);
    bool applyLocalNetworkValue(RuntimeParamScope scope,
                                std::int32_t targetId,
                                const std::string& key,
                                float value,
                                LocalNetworkSettings* settings);
    void syncLocalNetworkSettings();
    void clearBaseDelayOverrides();
    void clearLossOverrides();
    void clearDirectionalOverrides();

    std::uint16_t targetPeerId_{0u};
    LocalNetworkSettings localNetworkSettings_{};
    LocalNetworkSettings authoritativeLocalNetworkSettings_{};
    ProxyLinkConfig upstreamConfig_{};
    ProxyLinkConfig downstreamConfig_{};
    ProxyStats upstreamStats_{};
    ProxyStats downstreamStats_{};
    RuntimeParamApplyResult lastLocalNetworkApplyResult_{};
    bool hasAuthoritativeLocalNetworkSettings_{false};
    bool hasLocalNetworkApplyResult_{false};
    bool upstreamBaseDelayOverridden_{false};
    bool downstreamBaseDelayOverridden_{false};
    bool upstreamLossOverridden_{false};
    bool downstreamLossOverridden_{false};
    ShotEvaluationViewState shotEvaluationView_{};
    DiagnosticsStudyViewState studyView_{};
};

}  // namespace net
