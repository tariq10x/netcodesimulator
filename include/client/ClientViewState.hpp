#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/SimulationTypes.hpp"

namespace client {

struct ConnectionStatusView {
    bool connected{false};
    bool hasSnapshot{false};
    std::string stateLabel{};
    std::string statusMessage{};
};

struct CameraViewState {
    bool available{false};
    sim::Vec3 eyePosition{};
    sim::Vec3 lookDirection{0.0f, 0.0f, 1.0f};
    float fovY{70.0f};
};

struct HostedSessionView {
    bool available{false};
    std::string label{};
    std::string shotRuleLabel{};
    std::string botDirectorLabel{};
    std::string visualizationModeLabel{};
    std::uint16_t publicJoinPort{0u};
    bool botsFrozen{true};
    bool botsCanShoot{true};
    bool ghostTracksVisible{true};
};

struct HudView {
    int localHealth{0};
    sim::TeamId localTeam{sim::TeamId::None};
    std::string combatEventText{};
};

enum class AreaFilterView : std::uint8_t {
    All = 0,
    RedOnly = 1,
    GreenOnly = 2
};

struct StudyPresentationView {
    bool available{false};
    bool environmentDimmed{false};
    float environmentDimFactor{0.0f};
    bool areasVisible{true};
    AreaFilterView areaFilter{AreaFilterView::All};
    std::string clutterLabel{};
    std::string areaLabel{};
    std::string filterLabel{};
};

struct CompactScoreView {
    bool available{false};
    std::uint16_t attackerScore{0u};
    std::uint16_t defenderScore{0u};
    std::string localIdentity{};
    sim::TeamId localTeam{sim::TeamId::None};
    int localKills{0};
    int localDeaths{0};
    bool localAlive{false};
};

struct KillFeedEntryView {
    std::string attackerLabel{};
    std::string victimLabel{};
    sim::TeamId attackerTeam{sim::TeamId::None};
    sim::TeamId victimTeam{sim::TeamId::None};
    bool attackerIsLocalPlayer{false};
    bool victimIsLocalPlayer{false};
};

struct ScoreboardEntryView {
    std::string identity{};
    std::string rowLabel{};
    sim::TeamId team{sim::TeamId::None};
    int actorId{0};
    int kills{0};
    int deaths{0};
    std::uint16_t pingMs{0u};
    bool alive{false};
    bool isBot{false};
    bool isLocalPlayer{false};
};

struct ScoreboardSectionView {
    sim::TeamId team{sim::TeamId::None};
    std::uint16_t score{0u};
    std::vector<ScoreboardEntryView> entries{};
};

struct DiagnosticsAuthorityStatusView {
    bool available{false};
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
    std::uint32_t ackedInputSeq{0u};
    std::string shotRuleLabel{};
    std::string shotRuleExplanation{};
};

struct DiagnosticsPredictionStatusView {
    bool available{false};
    std::string reconciliationStrategyLabel{};
    std::string correctionModeLabel{};
    float correctionMagnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
    std::uint32_t pendingInputCount{0u};
    std::uint32_t smoothWindowMs{0u};
};

struct DiagnosticsPaneLabelStatusView {
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

struct DiagnosticsSpectatorStatusView {
    bool available{false};
    bool sessionSpectator{false};
    bool paneObservation{false};
    bool canReturnToCharacter{false};
    sim::TeamId participantTeam{sim::TeamId::None};
    int followTargetActorId{-1};
    std::string participantTeamLabel{};
    std::string followTargetLabel{};
};

struct DiagnosticsShotStudyStatusView {
    bool available{false};
    std::uint64_t viewedTimeUs{0u};
    std::uint64_t rewindTimeUs{0u};
    std::string evaluatedStateLabel{};
    std::string activeRuleLabel{};
    std::string activeRuleExplanation{};
    bool authoritativeHit{false};
    std::string outcomeLabel{};
};

struct DiagnosticsViewState {
    bool hasLocalNetworkControls{false};
    bool hasHostDiagnostics{false};
    bool localNetworkPanelVisible{false};
    float requestedLatencyMs{0.0f};
    float requestedLossPct{0.0f};
    bool hasAuthoritativeLocalNetworkSettings{false};
    float authoritativeLatencyMs{0.0f};
    float authoritativeLossPct{0.0f};
    std::vector<std::string> summaryLines{};
    std::vector<std::string> localNetworkSummaryLines{};
    DiagnosticsAuthorityStatusView authority{};
    DiagnosticsPredictionStatusView prediction{};
    DiagnosticsPaneLabelStatusView pane{};
    DiagnosticsSpectatorStatusView spectator{};
    DiagnosticsShotStudyStatusView shotStudy{};
};

struct ReplayRuleStatusView {
    bool available{false};
    std::string label{};
    std::string explanation{};
};

struct ReplayPaneBindingStatusView {
    bool available{false};
    sim::PaneSlot slot{sim::PaneSlot::Left};
    sim::PaneViewMode mode{sim::PaneViewMode::ReplayCamera};
    std::string slotLabel{};
    std::string bindingLabel{};
    std::string modeLabel{};
};

struct ReplaySpectatorStatusView {
    bool available{false};
    sim::PaneViewMode mode{sim::PaneViewMode::SpectatorFreeFly};
    std::string modeLabel{};
    std::string followTargetLabel{};
    bool sessionSpectator{false};
    bool canReturnToCharacter{false};
};

struct ReplayCheckpointStatusView {
    bool detachedCameraActive{false};
    int activeIndex{-1};
    std::size_t checkpointCount{0u};
    float transitionToNextSeconds{0.0f};
    bool transitionEditable{false};
};

struct ReplayStatusView {
    bool recordingActive{false};
    bool playbackActive{false};
    bool overlayVisible{false};
    std::string statusLine{};
    ReplayRuleStatusView rule{};
    ReplayPaneBindingStatusView paneBinding{};
    ReplaySpectatorStatusView spectator{};
    ReplayCheckpointStatusView checkpoint{};
};

struct RemotePlayerView {
    int actorId{0};
    sim::Vec3 eyePosition{};
    float yaw{0.0f};
    float pitch{0.0f};
    float healthPercent{0.0f};
    bool alive{true};
    sim::TeamId team{sim::TeamId::None};
    bool ghost{false};
};

struct RemoteEnemyView {
    int entityId{0};
    sim::Vec3 position{};
    float yaw{0.0f};
    float pitch{0.0f};
    float healthPercent{0.0f};
    bool alive{false};
};

struct TeamMenuView {
    bool visible{false};
    sim::TeamId currentTeam{sim::TeamId::None};
    sim::TeamId selectedTeam{sim::TeamId::None};
};

struct ParticipantIdentityView {
    int actorId{0};
    std::string label{};
    sim::ParticipantState state{};
    bool isBot{false};
};

enum class ObservationContextView : std::uint8_t {
    Gameplay = 0,
    PaneLocalObservation = 1,
    SessionSpectator = 2
};

struct PaneStatusView {
    sim::PaneViewState state{};
    ObservationContextView observationContext{ObservationContextView::Gameplay};
    sim::TeamId participantTeam{sim::TeamId::None};
    std::string participantTeamLabel{};
    std::string slotLabel{};
    std::string focusLabel{};
    std::string modeLabel{};
    std::string bindingLabel{};
    std::string followTargetLabel{};
    int activeCheckpointIndex{-1};
    std::size_t checkpointCount{0u};
    std::string checkpointLabel{};
    bool canReturnToCharacter{false};
};

enum class ReconciliationStrategyView : std::uint8_t {
    Snap = 0,
    Smooth = 1
};

enum class CorrectionModeView : std::uint8_t {
    None = 0,
    Smooth = 1,
    Snap = 2
};

struct ReconciliationView {
    ReconciliationStrategyView strategy{ReconciliationStrategyView::Smooth};
    CorrectionModeView lastMode{CorrectionModeView::None};
    float correctionMagnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
    std::uint32_t pendingCommandCount{0u};
    std::uint32_t smoothWindowMs{0u};
};

struct TimingViewState {
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
    std::uint32_t ackedInputSeq{0u};
    ReconciliationView reconciliation{};
};

struct ClientViewState {
    ConnectionStatusView connection{};
    CameraViewState camera{};
    HostedSessionView hostedSession{};
    HudView hud{};
    StudyPresentationView studyPresentation{};
    CompactScoreView compactScore{};
    std::vector<KillFeedEntryView> killFeed{};
    ParticipantIdentityView localParticipant{};
    PaneStatusView pane{};
    TimingViewState timing{};
    std::uint16_t attackerScore{0u};
    std::uint16_t defenderScore{0u};
    std::vector<ScoreboardSectionView> scoreboardSections{};
    DiagnosticsViewState diagnostics{};
    ReplayStatusView replay{};
    std::vector<RemotePlayerView> remotePlayers{};
    std::vector<RemotePlayerView> remotePlayerGhosts{};
    std::vector<RemoteEnemyView> remoteEnemies{};
    TeamMenuView teamMenu{};
    bool uiModeActive{false};
    bool scoreboardVisible{false};
};

}  // namespace client
