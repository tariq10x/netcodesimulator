#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "InputHandler3D.hpp"
#include "LaserBeam3D.hpp"
#include "RuntimeSettingsOverlay.hpp"
#include "app/CheckpointStore.hpp"
#include "client/ClientPresentation.hpp"
#include "client/ClientSyncRuntime.hpp"
#include "client/PerceptionEventMonitor.hpp"
#include "client/PresentationStateSubsystem.hpp"
#include "client/ReplayRecording.hpp"
#include "client/ReplaySubsystem.hpp"
#include "net/InterpolationBuffer.hpp"
#include "net/PredictionBuffer.hpp"
#include "net/Protocol.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "net/UdpSocket.hpp"
#include "telemetry/StudyEventLog.hpp"

class Arena3D;
class Enemy3D;
class Player3D;

namespace net {

class DiagnosticsModel;
struct ClientRuntimeTestAccess;

enum class ClientConnectionState : std::uint8_t {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Rejected = 3,
    TimedOut = 4
};

struct ClientConfig {
    std::string serverHost{"127.0.0.1"};
    std::uint16_t serverPort{41000};
    std::string bindHost{"0.0.0.0"};
    std::uint16_t localPort{0};
    std::string playerName{"player"};
    sim::TeamId preferredTeam{sim::TeamId::None};
    std::uint32_t sessionId{0};
    std::uint16_t protocolVersion{kProtocolVersion};
    std::uint64_t connectTimeoutUs{3'000'000};
    std::uint64_t helloRetryIntervalUs{250'000};
    std::uint64_t serverSilenceTimeoutUs{5'000'000};
    std::uint64_t idleKeepaliveIntervalUs{250'000};
    bool studyActionsEnabled{false};
    bool studyEventLoggingEnabled{false};
    std::string studyEventRunId{};
    std::filesystem::path studyEventLogDirectory{};
};

const char* toString(ClientConnectionState state);

class ClientRuntime {
public:
    explicit ClientRuntime(ClientConfig config = {});
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    bool start();
    void shutdown();
    void update(float dtSeconds, const InputHandler3D::InputState* input = nullptr);
    void render() const;

    ClientConnectionState state() const;
    const std::string& statusMessage() const;
    const ClientConfig& config() const;
    bool hasSnapshot() const;
    std::uint16_t peerId() const;
    std::uint16_t localPort() const;
    std::uint32_t lastAckedInputSeq() const;
    bool hasAuthoritativeLevelIdentity() const;
    int authoritativeLevelSlot() const;
    std::uint32_t authoritativeLevelHash() const;
    bool hasAuthoritativeSessionMetadata() const;
    const HostedSessionMetadata& authoritativeSessionMetadata() const;
    std::string hostedSessionSummary() const;
    bool uiModeActive() const;
    bool interpolationEnabled() const;
    bool predictionEnabled() const;
    bool fovConesVisible() const;
    bool scoreboardVisible() const;
    bool teamMenuVisible() const;
    sim::TeamId teamMenuSelection() const;
    sim::ParticipantState localParticipantState() const;
    sim::PaneViewState localPaneView() const;
    std::vector<sim::EligibleActor> eligibleControlActors() const;
    std::vector<sim::EligibleActor> eligibleSpectatorTargets() const;
    ControlBindingContracts controlBindingContracts() const;
    const sim::PlayerState& localPlayerState() const;
    const std::vector<sim::PlayerState>& remotePlayers() const;
    const std::vector<sim::RemoteActorState>& remoteEnemies() const;
    const std::vector<sim::RosterEntry>& roster() const;
    const sim::TeamScores& teamScores() const;
    std::string teamScoreSummary() const;
    std::vector<std::string> compactHudLines() const;
    std::vector<std::string> scoreboardLines() const;
    const WorldSnapshot* latestSnapshot() const;
    bool handleBackAction();
    PredictionBufferMap& participantPredictionBuffers();
    const PredictionBufferMap& participantPredictionBuffers() const;
    void attachProxyDiagnostics(TransportArtifactAdapter* proxy, std::uint16_t targetPeerId = 0u);
    void detachProxyDiagnostics();
    void setCommandReplayStatus(bool available, bool recording, std::size_t eventCount);
    bool hasLocalNetworkControls() const;
    bool hasHostDiagnostics() const;
    bool localNetworkPanelVisible() const;
    float localNetworkLatencyMs() const;
    float localNetworkLossPct() const;
    void setLocalNetworkSettingsForTest(float latencyMs, float lossPct);
    RuntimeSettingsOverlay::State runtimeSettingsOverlayStateForTest() const;
    std::size_t combatTraceCount() const;
    const std::string& lastCombatEventText() const;
    void setLastCombatEventText(std::string text);
    const DiagnosticsModel* diagnosticsModel() const;
    client::ClientViewState clientViewState() const;
    static std::vector<sim::RemoteActorState> sampleRemoteActorsForPresentation(
        const InterpolationBuffer& buffer,
        const std::vector<sim::RemoteActorState>& newestSnapshot,
        std::uint64_t targetServerTimeUs,
        bool interpolationEnabled);

private:
#ifdef NETCODESIM_ENABLE_TEST_HOOKS
    friend struct ClientRuntimeTestAccess;
#endif

    struct ParticipantRuntimeSettingsState {
        bool interpolationEnabled{true};
        bool predictionEnabled{true};
        sim::RuntimeReconciliationStrategy reconciliationStrategy{
            sim::RuntimeReconciliationStrategy::Smooth};
        std::uint32_t smoothCorrectionWindowMs{250u};
    };

    struct CombatTrace {
        LaserBeam3D beam;
        std::uint64_t createdAtUs{0u};
        bool authoritative{false};
        bool hit{false};

        CombatTrace(const LaserBeam3D& beamValue,
                    std::uint64_t createdAtUsValue,
                    bool authoritativeValue,
                    bool hitValue)
            : beam(beamValue),
              createdAtUs(createdAtUsValue),
              authoritative(authoritativeValue),
              hit(hitValue) {}
    };

    struct StudyPresentationState {
        bool environmentDimmed{false};
        bool areasVisible{true};
        client::AreaFilterView areaFilter{client::AreaFilterView::All};
    };

    struct KillFeedItem {
        client::KillFeedEntryView view{};
        std::uint64_t createdAtUs{0u};
    };

    enum class HostScoreboardActionKind : std::uint8_t {
        AssignAttacker,
        AssignDefender,
        AssignSpectator,
        Kick,
        AddBot,
        ToggleBots,
        ToggleBotPeace
    };

    struct HostScoreboardAction {
        HostScoreboardActionKind kind{HostScoreboardActionKind::AssignAttacker};
        std::uint16_t targetPeerId{0u};
        Rectangle bounds{};
        sim::TeamId assignedTeam{sim::TeamId::None};
        bool enabled{false};
    };

    void drainIncomingPackets();
    void handlePacket(const Packet& packet);
    void applySnapshot(const WorldSnapshot& snapshot);
    void applyControlSnapshot(const ControlWorldSnapshot& snapshot);
    void updateInterpolatedRemoteEntities();
    void updateInterpolatedControlRemotePlayers();
    void recordCombatTrace(const sim::HitscanRay& ray, bool authoritative, bool hit);
    void consumeCombatEvents(const WorldSnapshot& snapshot);
    void pruneCombatTraces();
    void setUiMode(bool active);
    void openKeyboardOverlay();
    void closeKeyboardOverlay(bool restoreCursorCapture = true);
    void applyRuntimeSettingsAction(const RuntimeSettingsOverlay::Action& action);
    RuntimeSettingsOverlay::State buildRuntimeSettingsOverlayState() const;
    void setRuntimeSettingsTarget(std::uint16_t targetId);
    const sim::RosterEntry* findRosterEntry(int actorId) const;
    bool isLocalHost() const;
    bool shouldShowRuntimeSettingsTarget(std::uint16_t targetId) const;
    bool canEditParticipantSyncSettings(std::uint16_t targetId) const;
    bool canEditTransportSettings(std::uint16_t targetId) const;
    bool shouldShowHostScoreboardAdmin() const;
    bool handleHostScoreboardAdminClick();
    std::vector<HostScoreboardAction> hostScoreboardActions() const;
    void renderHostScoreboardAdminPanel() const;
    bool sendHostAddBot();
    bool sendHostToggleBots();
    bool sendHostToggleBotPeace();
    bool sendHostTeamAssignment(std::uint16_t targetPeerId, sim::TeamId team);
    bool sendHostKick(std::uint16_t targetPeerId);
    void syncDiagnosticsAuthoritativeTargetState();
    void applyAuthoritativeLocalParticipantSettings();
    void previewLocalParticipantRuntimeSetting(RuntimeSettingsOverlay::ControlId controlId,
                                               float value);
    void syncDiagnosticsTargetPeerId();
    float displayedGlobalLatencyMs() const;
    bool applyGlobalLatencyMs(float latencyMs);
    void applyLocalNetworkSettings();
    void refreshDiagnostics();
    bool maybeSendIdleKeepalive();
    bool studyEventLoggingActive() const;
    bool studyEventLoggingAvailable() const;
    std::string effectiveStudyEventRunId() const;
    std::filesystem::path effectiveStudyEventLogDirectory() const;
    std::string studyEventLogDirectoryLabel() const;
    bool ensureStudyEventSink();
    telemetry::StudyEventRecord makeClientStudyEvent(const std::string& eventName);
    void recordClientPerceptionEvents();
    void recordClientFovTransition(const client::FovVisibilityTransition& transition);
    void recordClientFirePressed(const sim::PlayerCommand& command, bool commandSent);
    void loadSpectatorCheckpoints();
    void saveSpectatorCheckpoints() const;
    SpectatorCamera::Checkpoint detachedObserverCheckpoint() const;
    void setDetachedObserverCheckpoint(const SpectatorCamera::Checkpoint& checkpoint);
    void syncSessionSpectatorObserverState(const WorldSnapshot& snapshot);
    void beginSpectatorCheckpointTransition(int targetIndex);
    void updateSpectatorCheckpointTransition(float dtSeconds);
    float defaultCheckpointTransitionSecondsForCurrentMode() const;
    float currentSpectatorCheckpointTransitionSeconds() const;
    void adjustCurrentSpectatorCheckpointTransition(float deltaSeconds);
    void updateDetachedObserverCamera(const InputHandler3D::InputState& input, float dtSeconds);
    void toggleReplayRecording();
    bool toggleReplayPlayback();
    bool stopReplayPlayback();
    bool makeLocalReplayPlayerRenderItem(const client::ClientViewState& viewState,
                                         client::RemotePlayerRenderItem* itemOut) const;
    void appendReplaySpectatorLocalPlayer(client::RenderFrame* frame,
                                          const client::RecordedReplayFrame& replayFrame) const;
    void updateReplayPlaybackObserver(float dtSeconds, const InputHandler3D::InputState* input);
    const client::RecordedReplayFrame* activeRecordedReplayFrame() const;
    Camera3D detachedObserverCamera() const;
    client::RenderFrame buildLiveRenderFrame(const client::ClientViewState& viewState) const;
    client::RenderFrame buildRenderFrame(const client::ClientViewState& viewState) const;
    sim::PaneViewState effectiveLocalPaneView() const;
    sim::PlayerCommand makeCommand(const InputHandler3D::InputState& input,
                                   float dtSeconds,
                                   std::uint32_t seq);
    sim::PlayerCommand makeIdleCommand(std::uint32_t seq);
    sim::TeamId localTeam() const;
    void toggleSpectatorMode();
    void toggleTeamMenu();
    void cycleTeamMenuSelection(int delta);
    void confirmTeamMenuSelection();
    void queueTeamSwitchRequest(sim::TeamId desiredTeam);
    bool sendHello();
    bool sendControlPayload(PacketPayload payload);
    bool sendTeamChangeRequest(sim::TeamId desiredTeam);
    bool sendSessionActionRequest(SessionActionKind kind);
    bool sendRuntimeParamChangeRequest(RuntimeParamScope scope,
                                       std::int32_t targetId,
                                       std::string key,
                                       float value);
    bool sendCommand(const sim::PlayerCommand& command);
    void syncRemoteVisuals();
    void updateLocalDeathState(const WorldSnapshot& snapshot, float previousHealth);
    bool localPlayerAwaitingRespawn() const;
    std::uint32_t defaultInterpolationDelayMs() const;
    bool saveRecordedReplay();
    client::ReplayStatusView replayStatusView() const;
    client::PresentationStateInputs presentationStateInputs() const;
    client::ClientViewState buildClientViewState() const;
    void applyStudyPresentationState(float dtSeconds);
    void pruneKillFeed();
    client::ClientSyncContext syncContext();
    client::ClientSyncContext syncContext() const;

    ClientConfig config_{};
    UdpSocket socket_{};
    UdpEndpoint serverEndpoint_{};
    ClientConnectionState state_{ClientConnectionState::Disconnected};
    std::string statusMessage_{"disconnected"};
    std::uint64_t clockUs_{0};
    std::uint64_t lastHelloSendUs_{0};
    std::uint64_t lastServerPacketUs_{0};
    std::uint64_t lastCommandSendUs_{0};
    std::uint32_t packetSeq_{0};
    std::uint32_t commandSeq_{0};
    std::uint32_t lastSnapshotPacketSeq_{0};
    std::uint32_t lastControlSnapshotPacketSeq_{0};
    std::uint32_t lastAckedInputSeq_{0};
    std::uint16_t peerId_{0};
    std::uint16_t snapshotRateHz_{0};
    std::optional<std::uint16_t> stagedSessionTickRateHz_{};
    std::optional<std::uint16_t> stagedSessionSnapshotRateHz_{};
    int authoritativeLevelSlot_{-1};
    std::uint32_t authoritativeLevelHash_{0u};
    bool hasAuthoritativeSessionMetadata_{false};
    HostedSessionMetadata authoritativeSessionMetadata_{};
    std::uint64_t latestSnapshotReceiveUs_{0u};
    std::uint64_t latestControlSnapshotReceiveUs_{0u};
    float viewYaw_{0.0f};
    float viewPitch_{0.0f};
    bool viewInitialized_{false};
    bool detachedObserverActive_{false};
    bool sessionSpectatorObserverLocked_{false};
    sim::Vec3 detachedObserverEyePosition_{};
    float detachedObserverYaw_{0.0f};
    float detachedObserverPitch_{0.0f};
    app::CheckpointStore checkpointStore_{app::CheckpointCollection::Latency};
    std::vector<SpectatorCamera::Checkpoint> spectatorCheckpoints_{};
    int currentSpectatorCheckpoint_{-1};
    bool spectatorTransitionActive_{false};
    float spectatorTransitionTimer_{0.0f};
    float spectatorTransitionDuration_{0.5f};
    SpectatorCamera::Checkpoint spectatorTransitionStart_{};
    SpectatorCamera::Checkpoint spectatorTransitionEnd_{};
    bool hasSnapshot_{false};
    std::uint64_t clientFrameCounter_{0u};
    bool hasControlSnapshot_{false};
    std::optional<ParticipantRuntimeSettingsState> authoritativeLocalParticipantSettings_{};
    bool interpolationEnabled_{true};
    bool predictionEnabled_{true};
    ReconciliationStrategy reconciliationStrategy_{ReconciliationStrategy::Smooth};
    CorrectionRecord lastCorrection_{};
    std::uint32_t smoothCorrectionWindowMs_{250u};
    sim::SimConfig simConfig_{};
    sim::MovementEnvironment environment_{};
    InterpolationBuffer remotePlayerInterpolation_{};
    InterpolationBuffer controlRemotePlayerInterpolation_{};
    InterpolationBuffer remoteEnemyInterpolation_{};
    PredictionBuffer predictionBuffer_{};
    PredictionBufferMap participantPredictionBuffers_{};
    sim::PlayerState localPlayerState_{};
    std::vector<sim::PlayerState> remotePlayers_{};
    std::vector<sim::PlayerState> controlRemotePlayers_{};
    std::vector<sim::RemoteActorState> remoteEnemies_{};
    std::vector<sim::RosterEntry> roster_{};
    sim::TeamScores teamScores_{};
    WorldSnapshot latestSnapshot_{};
    WorldSnapshot latestControlSnapshot_{};
    std::vector<CombatTrace> combatTraces_{};
    StudyPresentationState studyPresentation_{};
    std::vector<KillFeedItem> killFeed_{};
    std::string lastCombatEventText_{};
    bool commandReplayStatusAvailable_{false};
    bool commandReplayRecordingActive_{false};
    std::size_t commandReplayEventCount_{0u};
    bool localRespawnPending_{false};
    std::uint64_t localDeathStartUs_{0u};
    std::uint32_t consumedCombatEventCount_{0u};
    std::vector<client::RecordedReplayFrame> recordedReplayFrames_{};
    std::unique_ptr<Arena3D> arena_{};
    std::unique_ptr<Player3D> localPlayerVisual_{};
    std::vector<std::unique_ptr<Enemy3D>> remoteVisuals_{};
    bool autoAssignSessionId_{false};
    TransportArtifactAdapter* hostProxy_{nullptr};
    std::unique_ptr<DiagnosticsModel> diagnosticsModel_{};
    RuntimeSettingsOverlay runtimeSettingsOverlay_{};
    std::uint16_t runtimeSettingsTargetId_{0u};
    std::optional<float> lastGlobalLatencyMs_{};
    sim::TeamId pendingTeamRequest_{sim::TeamId::None};
    bool teamMenuVisible_{false};
    sim::TeamId teamMenuSelection_{sim::TeamId::Attacker};
    bool runtimeSettingsVisible_{false};
    bool keyboardOverlayVisible_{false};
    bool keyboardOverlayRestoreCapture_{false};
    input::ControlBindings controlBindings_{input::ControlBindings::defaults()};
    bool uiMode_{false};
    bool scoreboardVisible_{false};
    bool scoreboardCursorActive_{false};
    bool replayOverlayVisible_{false};
    bool fovConesVisible_{false};
    client::PerceptionEventMonitor perceptionMonitor_{};
    std::unique_ptr<telemetry::StudyEventSink> studyEventSink_{};
    std::uint64_t studyEventSeq_{0u};
    client::ClientPresentation clientPresentation_{};
    client::ReplaySubsystem replaySubsystem_{};
    client::PresentationStateSubsystem presentationStateSubsystem_{};
    client::ClientSyncRuntime syncRuntime_{};
};

#ifdef NETCODESIM_ENABLE_TEST_HOOKS
struct ClientRuntimeTestAccess {
    static void forceConnectedSnapshot(ClientRuntime& client,
                                       std::uint16_t peerId,
                                       std::uint64_t serverTimeUs) {
        client.state_ = ClientConnectionState::Connected;
        client.peerId_ = peerId;
        client.hasSnapshot_ = true;
        client.latestSnapshot_.serverTimeUs = serverTimeUs;
        client.latestSnapshot_.localPlayerState.playerId = static_cast<int>(peerId);
        client.localPlayerState_.playerId = static_cast<int>(peerId);
        client.viewInitialized_ = true;
    }

    static const WorldSnapshot& latestSnapshot(const ClientRuntime& client) {
        return client.latestSnapshot_;
    }

    static std::uint32_t commandSeq(const ClientRuntime& client) {
        return client.commandSeq_;
    }

    static void handlePacket(ClientRuntime& client, const Packet& packet) {
        client.handlePacket(packet);
    }

    static bool hasCombatTraces(const ClientRuntime& client) {
        return !client.combatTraces_.empty();
    }

    static std::size_t combatTraceCount(const ClientRuntime& client) {
        return client.combatTraces_.size();
    }

    static bool firstCombatTraceAuthoritative(const ClientRuntime& client) {
        return client.combatTraces_.front().authoritative;
    }

    static float firstCombatTraceThickness(const ClientRuntime& client) {
        return client.combatTraces_.front().beam.getThickness();
    }

    static std::uint32_t consumedCombatEventCount(const ClientRuntime& client) {
        return client.consumedCombatEventCount_;
    }

    static client::RenderFrame renderFrame(const ClientRuntime& client) {
        const client::ClientViewState viewState = client.buildClientViewState();
        return client.buildRenderFrame(viewState);
    }

    static void applyRuntimeSettingsAction(
        ClientRuntime& client,
        const RuntimeSettingsOverlay::Action& action) {
        client.applyRuntimeSettingsAction(action);
    }
};
#endif

inline PredictionBufferMap& ClientRuntime::participantPredictionBuffers() {
    return participantPredictionBuffers_;
}

inline const PredictionBufferMap& ClientRuntime::participantPredictionBuffers() const {
    return participantPredictionBuffers_;
}

inline sim::ParticipantState ClientRuntime::localParticipantState() const {
    return latestSnapshot_.localParticipantState;
}

inline sim::PaneViewState ClientRuntime::localPaneView() const {
    return effectiveLocalPaneView();
}

inline std::vector<sim::EligibleActor> ClientRuntime::eligibleControlActors() const {
    std::vector<sim::EligibleActor> actors;
    const sim::ParticipantState participant = localParticipantState();
    if (participant.participation != sim::ParticipationState::Playing ||
        !participant.control.controlsActor()) {
        return actors;
    }

    actors.reserve(roster_.size());
    for (const auto& entry : roster_) {
        if (sim::isLocalControlEligible(entry, participant.control.actorId)) {
            actors.push_back(sim::eligibleActorFromRosterEntry(entry));
        }
    }
    return actors;
}

inline std::vector<sim::EligibleActor> ClientRuntime::eligibleSpectatorTargets() const {
    std::vector<sim::EligibleActor> actors;
    actors.reserve(roster_.size());
    for (const auto& entry : roster_) {
        if (sim::isSpectatorTargetEligible(entry)) {
            actors.push_back(sim::eligibleActorFromRosterEntry(entry));
        }
    }
    return actors;
}

inline ControlBindingContracts ClientRuntime::controlBindingContracts() const {
    ControlBindingContracts contracts;
    contracts.participantState = localParticipantState();
    contracts.paneView = localPaneView();
    contracts.eligibleActors.controlTargets = eligibleControlActors();
    contracts.eligibleActors.spectatorTargets = eligibleSpectatorTargets();
    return contracts;
}

}  // namespace net
