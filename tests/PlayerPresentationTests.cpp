#include "Arena3D.hpp"
#include "Player3D.hpp"
#include "app/AppFlow.hpp"
#include "client/ClientPresentation.hpp"
#include "client/PaneViewState.hpp"
#include "client/PresentationStateSubsystem.hpp"
#include "client/SplitScreenRenderFrame.hpp"
#include "client/SplitScreenViewState.hpp"
#include "net/SessionDiscovery.hpp"

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectContains(const std::string& value,
                    const std::string& needle,
                    const std::string& message) {
    expect(value.find(needle) != std::string::npos, message);
}

bool containsLine(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::filesystem::path findRepoRoot() {
    std::filesystem::path probe = std::filesystem::current_path();
    while (!probe.empty()) {
        if (std::filesystem::exists(probe / "CMakeLists.txt") &&
            std::filesystem::exists(probe / "src/main_3d.cpp")) {
            return probe;
        }
        if (probe == probe.root_path()) {
            break;
        }
        probe = probe.parent_path();
    }

    throw std::runtime_error("failed to locate repository root for presentation characterization tests");
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    expect(file.is_open(), "expected to open source fixture: " + path.string());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0u;
    }

    std::size_t count = 0u;
    std::size_t cursor = 0u;
    while ((cursor = text.find(needle, cursor)) != std::string::npos) {
        ++count;
        cursor += needle.size();
    }
    return count;
}

bool nearlyEqual(float lhs, float rhs, float epsilon = 0.001f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

client::ClientViewState makeRichClientViewState(sim::PaneSlot slot = sim::PaneSlot::Left,
                                                bool focused = true,
                                                bool botsFrozen = true,
                                                bool botsCanShoot = true,
                                                net::SessionVisualizationMode visualizationMode =
                                                    net::SessionVisualizationMode::Diagnostic) {
    client::PresentationStateSubsystem subsystem;

    net::HostedSessionMetadata sessionMetadata;
    sessionMetadata.sessionLabel = "Study Session";
    sessionMetadata.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    sessionMetadata.visualizationMode = visualizationMode;
    sessionMetadata.publicJoinPort = 41000u;
    sessionMetadata.botsFrozen = botsFrozen;
    sessionMetadata.botsCanShoot = botsCanShoot;

    sim::RosterEntry localEntry;
    localEntry.actorId = 7;
    localEntry.team = sim::TeamId::Attacker;
    localEntry.displayName = "Local Player";
    localEntry.kills = 5u;
    localEntry.deaths = 2u;
    localEntry.alive = true;

    sim::RosterEntry remoteEntry;
    remoteEntry.actorId = 8;
    remoteEntry.team = sim::TeamId::Defender;
    remoteEntry.displayName = "Remote Player";
    remoteEntry.kills = 2u;
    remoteEntry.deaths = 4u;
    remoteEntry.latencyMs = 33u;
    remoteEntry.alive = true;

    sim::RosterEntry botEntry;
    botEntry.actorId = 101;
    botEntry.team = sim::TeamId::Defender;
    botEntry.isBot = true;
    botEntry.kills = 3u;
    botEntry.deaths = 1u;
    botEntry.alive = false;
    botEntry.displayName = "Defense Bot";

    sim::TeamScores teamScores;
    teamScores.attackers = 4u;
    teamScores.defenders = 2u;

    sim::PlayerState localPlayerState;
    localPlayerState.position = {1.0f, Config::PLAYER_EYE_HEIGHT, 2.0f};
    localPlayerState.health = 87.6f;

    sim::PlayerState remotePlayerState;
    remotePlayerState.playerId = 8;
    remotePlayerState.position = {10.0f, 4.5f, -12.0f};
    remotePlayerState.yaw = 0.65f;
    remotePlayerState.pitch = -0.15f;
    remotePlayerState.health = 73.0f;
    remotePlayerState.maxHealth = 100.0f;
    remotePlayerState.grounded = true;

    sim::PlayerState controlRemotePlayerState = remotePlayerState;
    controlRemotePlayerState.position = {12.0f, 4.5f, -10.0f};

    sim::RemoteActorState remoteEnemyState;
    remoteEnemyState.entityId = 31;
    remoteEnemyState.position = {-6.0f, 0.0f, 9.0f};
    remoteEnemyState.yaw = -0.4f;
    remoteEnemyState.pitch = 0.1f;
    remoteEnemyState.health = sim::defaults::kEnemyMaxHealth * 0.5f;
    remoteEnemyState.alive = true;

    net::DiagnosticsModel diagnostics(7u);
    diagnostics.setLocalLatencyMs(120.0f);
    diagnostics.setLocalLossPct(5.0f);
    diagnostics.consumeRuntimeParamApplyResult(net::RuntimeParamApplyResult{
        net::RuntimeParamScope::Player,
        7,
        "net.player[7].latency_ms",
        120.0f,
        true,
        sim::StagedApplyBoundary::NextTick,
        "applied"
    });
    diagnostics.consumeRuntimeParamApplyResult(net::RuntimeParamApplyResult{
        net::RuntimeParamScope::Player,
        7,
        "net.player[7].loss_pct",
        5.0f,
        true,
        sim::StagedApplyBoundary::NextTick,
        "applied"
    });

    const std::string combatEventText =
        "Requested switch to Defenders (applies immediately and respawns you)";
    std::vector<client::KillFeedEntryView> killFeedEntries;
    client::KillFeedEntryView killFeedEntry;
    killFeedEntry.attackerLabel = "Local Player";
    killFeedEntry.victimLabel = "Defense Bot";
    killFeedEntry.attackerTeam = sim::TeamId::Attacker;
    killFeedEntry.victimTeam = sim::TeamId::Defender;
    killFeedEntry.attackerIsLocalPlayer = true;
    killFeedEntries.push_back(killFeedEntry);
    sim::TimingCadence cadence;
    cadence.authoritativeTickHz = 60u;
    cadence.snapshotCadenceHz = 20u;
    cadence.commandCadenceHz = 60u;

    sim::AuthoritativeTime authoritativeTime;
    authoritativeTime.serverTick = 18244u;
    authoritativeTime.serverTimeUs = 304066666ULL;
    authoritativeTime.viewedServerTimeUs = 303900000ULL;

    sim::ParticipantState participantState;
    participantState.presence = sim::SessionPresence::Connected;
    participantState.team = sim::TeamId::Attacker;
    participantState.participation = sim::ParticipationState::Playing;
    participantState.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};

    sim::PaneViewState paneState;
    paneState.slot = slot;
    paneState.mode = sim::PaneViewMode::PlayerControlled;
    paneState.focused = focused;

    const std::vector<sim::RosterEntry> roster{localEntry, remoteEntry, botEntry};
    diagnostics.recordShotEvaluation(net::ShotEvaluationRecord{
        304'000'000u,
        303'900'000u,
        net::ShotEvaluationStateView::LiveState,
        net::ShotEvaluationMode::LivePosition,
        false
    });
    diagnostics.recordStudyState(net::DiagnosticsStudyRecord{
        participantState,
        paneState,
        roster,
        cadence,
        authoritativeTime,
        142u,
        net::DiagnosticsReconciliationStrategyView::Smooth,
        net::DiagnosticsCorrectionModeView::Snap,
        1.75f,
        3u,
        1u,
        250u,
        net::ShotEvaluationMode::LivePosition,
        false
    });

    return subsystem.build(client::PresentationStateInputs{
        sessionMetadata,
        roster,
        teamScores,
        localPlayerState,
        std::vector<sim::PlayerState>{remotePlayerState},
        std::vector<sim::PlayerState>{controlRemotePlayerState},
        std::vector<sim::RemoteActorState>{remoteEnemyState},
        &diagnostics,
        7u,
        sim::TeamId::Attacker,
        true,
        true,
        "connected",
        "Authoritative session ready",
        0.5f,
        -0.25f,
        true,
        false,
        true,
        true,
        sim::TeamId::Defender,
        false,
        true,
        combatEventText,
        {},
        cadence,
        authoritativeTime,
        142u,
        client::ReconciliationStrategyView::Smooth,
        client::CorrectionModeView::Snap,
        1.75f,
        3u,
        1u,
        250u,
        participantState,
        paneState,
        "Local Player",
        {},
        -1,
        0u,
        false,
        false,
        {},
        true,
        1.0f,
        true,
        client::AreaFilterView::GreenOnly,
        std::move(killFeedEntries)
    });
}

client::ClientViewState makeSpectatorClientViewState(sim::PaneSlot slot = sim::PaneSlot::Right,
                                                     bool focused = false) {
    client::PresentationStateSubsystem subsystem;

    net::HostedSessionMetadata sessionMetadata;
    sessionMetadata.sessionLabel = "Observer Session";
    sessionMetadata.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    sessionMetadata.publicJoinPort = 41001u;

    sim::RosterEntry localEntry;
    localEntry.actorId = 7;
    localEntry.team = sim::TeamId::Spectator;
    localEntry.displayName = "host-player";
    localEntry.participation = sim::ParticipationState::Spectating;
    localEntry.alive = false;

    sim::PlayerState localPlayerState;
    localPlayerState.position = {0.0f, Config::PLAYER_EYE_HEIGHT, 0.0f};
    localPlayerState.health = 100.0f;

    sim::ParticipantState participantState;
    participantState.presence = sim::SessionPresence::Connected;
    participantState.team = sim::TeamId::Spectator;
    participantState.participation = sim::ParticipationState::Spectating;

    sim::PaneViewState paneState;
    paneState.slot = slot;
    paneState.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    paneState.focused = focused;
    paneState.followTargetActorId = 22;

    const std::string combatEventText{};
    return subsystem.build(client::PresentationStateInputs{
        sessionMetadata,
        std::vector<sim::RosterEntry>{localEntry},
        {},
        localPlayerState,
        {},
        {},
        {},
        nullptr,
        7u,
        sim::TeamId::Defender,
        true,
        true,
        "connected",
        "Spectator pane active",
        0.25f,
        -0.15f,
        false,
        false,
        false,
        false,
        sim::TeamId::None,
        false,
        false,
        combatEventText,
        {},
        {},
        {},
        0u,
        client::ReconciliationStrategyView::Smooth,
        client::CorrectionModeView::None,
        0.0f,
        0u,
        0u,
        0u,
        participantState,
        paneState,
        "host-player",
        "BOT 22",
        1,
        3u,
        true
    });
}

client::ClientViewState makeReplayClientViewState(sim::PaneSlot slot = sim::PaneSlot::Right,
                                                  bool focused = false) {
    client::PresentationStateSubsystem subsystem;

    net::HostedSessionMetadata sessionMetadata;
    sessionMetadata.sessionLabel = "Replay Review";
    sessionMetadata.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    sessionMetadata.publicJoinPort = 41002u;

    sim::RosterEntry localEntry;
    localEntry.actorId = 7;
    localEntry.team = sim::TeamId::Attacker;
    localEntry.displayName = "replay-host";
    localEntry.participation = sim::ParticipationState::Playing;
    localEntry.alive = true;

    sim::RosterEntry replayTarget;
    replayTarget.actorId = 8;
    replayTarget.team = sim::TeamId::Defender;
    replayTarget.displayName = "Replay Target";
    replayTarget.alive = true;

    sim::TeamScores teamScores;
    teamScores.attackers = 6u;
    teamScores.defenders = 3u;

    sim::PlayerState localPlayerState;
    localPlayerState.position = {-4.0f, Config::PLAYER_EYE_HEIGHT + 1.0f, 6.0f};
    localPlayerState.health = 100.0f;

    sim::PlayerState remotePlayerState;
    remotePlayerState.playerId = 8;
    remotePlayerState.position = {5.0f, Config::PLAYER_EYE_HEIGHT, -2.0f};
    remotePlayerState.yaw = 0.2f;
    remotePlayerState.pitch = -0.1f;
    remotePlayerState.health = 66.0f;
    remotePlayerState.maxHealth = 100.0f;
    remotePlayerState.grounded = true;

    sim::ParticipantState participantState;
    participantState.presence = sim::SessionPresence::Connected;
    participantState.team = sim::TeamId::Attacker;
    participantState.participation = sim::ParticipationState::Playing;
    participantState.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};

    sim::PaneViewState paneState;
    paneState.slot = slot;
    paneState.mode = sim::PaneViewMode::ReplayCamera;
    paneState.focused = focused;

    const std::string combatEventText = "Replay scrubbed to checkpoint 3";
    const std::string replayCameraLabel =
        slot == sim::PaneSlot::Left ? "Replay Camera A" : "Replay Camera B";
    client::ReplayStatusView replayStatus;
    replayStatus.recordingActive = true;
    replayStatus.playbackActive = true;
    replayStatus.statusLine = "Playback tick 87";
    replayStatus.rule.available = true;
    replayStatus.rule.label = "Seen Position";
    replayStatus.rule.explanation =
        net::shotEvaluationModeExplanation(net::ShotEvaluationMode::SeenPosition);
    replayStatus.paneBinding.available = true;
    replayStatus.paneBinding.slot = slot;
    replayStatus.paneBinding.mode = sim::PaneViewMode::ReplayCamera;
    replayStatus.paneBinding.slotLabel =
        slot == sim::PaneSlot::Left ? "Left Pane" : "Right Pane";
    replayStatus.paneBinding.bindingLabel = replayCameraLabel;
    replayStatus.paneBinding.modeLabel = "Replay Camera";
    replayStatus.spectator.available = true;
    replayStatus.spectator.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    replayStatus.spectator.modeLabel = "Spectator Follow Third Person";
    replayStatus.spectator.followTargetLabel = "Replay Target";
    replayStatus.spectator.sessionSpectator = false;
    replayStatus.spectator.canReturnToCharacter = true;

    return subsystem.build(client::PresentationStateInputs{
        sessionMetadata,
        std::vector<sim::RosterEntry>{localEntry, replayTarget},
        teamScores,
        localPlayerState,
        std::vector<sim::PlayerState>{remotePlayerState},
        {},
        {},
        nullptr,
        7u,
        sim::TeamId::Attacker,
        true,
        true,
        "connected",
        "Replay pane active",
        -0.3f,
        -0.2f,
        false,
        false,
        false,
        false,
        sim::TeamId::None,
        false,
        false,
        combatEventText,
        replayStatus,
        {},
        {},
        0u,
        client::ReconciliationStrategyView::Smooth,
        client::CorrectionModeView::None,
        0.0f,
        0u,
        0u,
        0u,
        participantState,
        paneState,
        "replay-host",
        {},
        2,
        4u,
        true
    });
}

client::PaneViewState makePlayerPane(sim::PaneSlot slot, bool focused) {
    client::PaneViewState pane;
    pane.slot = slot;
    pane.focused = focused;
    pane.binding = client::PaneBinding{
        client::PaneBindingKind::LocalParticipant,
        static_cast<std::uint16_t>(slot == sim::PaneSlot::Left ? 7u : 8u),
        slot == sim::PaneSlot::Left ? 7 : 8,
        slot == sim::PaneSlot::Left ? "Left Player" : "Right Player"
    };
    pane.viewState = makeRichClientViewState(slot, focused);
    pane.predictionEnabled = true;
    return pane;
}

client::PaneViewState makeSpectatorPane(sim::PaneSlot slot, bool focused) {
    client::PaneViewState pane;
    pane.slot = slot;
    pane.focused = focused;
    pane.binding = client::PaneBinding{
        client::PaneBindingKind::SpectatorTarget,
        0u,
        22,
        "BOT 22"
    };
    pane.viewState = makeSpectatorClientViewState(slot, focused);
    pane.authoritativeOnly = true;
    return pane;
}

client::PaneViewState makeReplayPane(sim::PaneSlot slot, bool focused) {
    client::PaneViewState pane;
    pane.slot = slot;
    pane.focused = focused;
    pane.binding = client::PaneBinding{
        client::PaneBindingKind::ReplayCamera,
        0u,
        -1,
        slot == sim::PaneSlot::Left ? "Replay Camera A" : "Replay Camera B"
    };
    pane.viewState = makeReplayClientViewState(slot, focused);
    pane.authoritativeOnly = true;
    return pane;
}

void testGroundedRemotePlayerUsesGroundAlignedRenderRoot() {
    Arena3D arena;
    arena.clearObstacles();

    sim::PlayerState state;
    state.position = {2.0f, Config::PLAYER_EYE_HEIGHT, -3.0f};
    state.grounded = true;

    const Vector3 renderRoot = Player3D::renderRootFromSimState(state, arena);
    expect(nearlyEqual(renderRoot.x, 2.0f), "render root should preserve x position");
    expect(nearlyEqual(renderRoot.z, -3.0f), "render root should preserve z position");
    expect(nearlyEqual(renderRoot.y, 0.0f),
           "grounded player render root should sit on the flat ground plane");
}

void testAirborneRemotePlayerStaysAboveGround() {
    Arena3D arena;
    arena.clearObstacles();

    sim::PlayerState state;
    state.position = {1.5f, Config::PLAYER_EYE_HEIGHT + 1.25f, 4.0f};
    state.velocity = {0.0f, 2.0f, 0.0f};
    state.grounded = false;

    const Vector3 renderRoot = Player3D::renderRootFromSimState(state, arena);
    expect(nearlyEqual(renderRoot.y, 1.25f),
           "airborne player render root should remain elevated by the eye-height offset");
}

void testRemotePlayerOnRaisedGeometryUsesSurfaceAnchor() {
    Arena3D arena;

    sim::PlayerState state;
    state.position = {10.0f, 4.5f, -12.0f};
    state.grounded = true;

    const float expectedGround = arena.getGroundHeightAt(Vector3{state.position.x, state.position.y, state.position.z});
    const Vector3 renderRoot = Player3D::renderRootFromSimState(state, arena);
    expect(nearlyEqual(expectedGround, 2.8f),
           "test fixture should target the default raised platform");
    expect(nearlyEqual(renderRoot.y, expectedGround),
           "raised-surface player render root should attach to the local surface height");
}

void testPresentationStateSubsystemBuildsTypedScoreboardAndDiagnosticsState() {
    const client::ClientViewState viewState = makeRichClientViewState();

    expect(viewState.hostedSession.available &&
               viewState.hostedSession.label == "Study Session" &&
               viewState.hostedSession.publicJoinPort == 41000u,
           "typed client view state should preserve hosted-session metadata");
    expect(viewState.connection.connected &&
               viewState.connection.hasSnapshot &&
               viewState.connection.stateLabel == "connected" &&
               viewState.connection.statusMessage == "Authoritative session ready",
           "typed client view state should preserve connection-facing presentation state");
    expect(viewState.camera.available &&
               nearlyEqual(viewState.camera.eyePosition.x, 1.0f) &&
               nearlyEqual(viewState.camera.eyePosition.y, Config::PLAYER_EYE_HEIGHT) &&
               nearlyEqual(viewState.camera.eyePosition.z, 2.0f),
           "typed client view state should preserve the active camera eye position");
    expect(viewState.timing.cadence.authoritativeTickHz == 60u &&
               viewState.timing.authoritativeTime.serverTick == 18244u &&
               viewState.timing.ackedInputSeq == 142u,
           "typed client view state should preserve authoritative cadence and sequencing");
    expect(viewState.timing.reconciliation.strategy == client::ReconciliationStrategyView::Smooth &&
               viewState.timing.reconciliation.lastMode == client::CorrectionModeView::Snap &&
               nearlyEqual(viewState.timing.reconciliation.correctionMagnitude, 1.75f) &&
               viewState.timing.reconciliation.replayedCommandCount == 3u &&
               viewState.timing.reconciliation.pendingCommandCount == 1u &&
               viewState.timing.reconciliation.smoothWindowMs == 250u,
           "typed client view state should preserve reconciliation diagnostics for presentation");
    expect(viewState.studyPresentation.available &&
               viewState.studyPresentation.environmentDimmed &&
               nearlyEqual(viewState.studyPresentation.environmentDimFactor, 1.0f) &&
               viewState.studyPresentation.areasVisible &&
               viewState.studyPresentation.areaFilter == client::AreaFilterView::GreenOnly,
           "typed client view state should expose client-local study presentation controls separately from authoritative gameplay state");
    expect(viewState.compactScore.available &&
               viewState.compactScore.attackerScore == 4u &&
               viewState.compactScore.defenderScore == 2u &&
               viewState.compactScore.localIdentity == "Local Player" &&
               viewState.compactScore.localKills == 5 &&
               viewState.compactScore.localDeaths == 2 &&
               viewState.compactScore.localAlive,
           "typed client view state should expose the compact score widget as structured overlay state");
    expect(viewState.killFeed.size() == 1u &&
               viewState.killFeed.front().attackerLabel == "Local Player" &&
               viewState.killFeed.front().victimLabel == "Defense Bot" &&
               viewState.killFeed.front().attackerIsLocalPlayer,
           "typed client view state should preserve recent kill-feed entries as structured overlay data");
    expect(viewState.attackerScore == 4u && viewState.defenderScore == 2u,
           "typed client view state should preserve authoritative team scores");
    expect(viewState.scoreboardSections.size() == 2u,
           "typed client view state should keep attacker and defender scoreboard sections");
    expect(viewState.scoreboardSections[0].team == sim::TeamId::Attacker &&
               viewState.scoreboardSections[0].entries.size() == 1u &&
               viewState.scoreboardSections[0].entries[0].isLocalPlayer,
           "typed client view state should keep the local attacker entry marked explicitly");
    expect(viewState.scoreboardSections[1].team == sim::TeamId::Defender &&
               viewState.scoreboardSections[1].entries.size() == 2u &&
               !viewState.scoreboardSections[1].entries[0].isBot &&
               viewState.scoreboardSections[1].entries[1].isBot &&
               viewState.scoreboardSections[1].entries[1].rowLabel.find("Ping --") != std::string::npos,
           "typed client view state should preserve human and bot defender scoreboard row semantics");
    expect(viewState.diagnostics.hasLocalNetworkControls &&
               viewState.diagnostics.localNetworkPanelVisible &&
               viewState.diagnostics.requestedLatencyMs == 120.0f &&
               viewState.diagnostics.authoritativeLossPct == 5.0f &&
               viewState.diagnostics.authority.available &&
               viewState.diagnostics.authority.shotRuleLabel == "Live Position" &&
               viewState.diagnostics.prediction.reconciliationStrategyLabel == "smooth" &&
               viewState.diagnostics.pane.paneLabel == "LEFT: Local Player" &&
               viewState.diagnostics.shotStudy.activeRuleLabel == "Live Position" &&
               !viewState.diagnostics.localNetworkSummaryLines.empty(),
           "typed client view state should preserve local-network diagnostics semantics");
    expect(viewState.hostedSession.visualizationModeLabel == "Diagnostic" &&
               viewState.hostedSession.ghostTracksVisible,
           "typed hosted-session view state should expose diagnostic ghost visibility");
    expect(viewState.remotePlayers.size() == 1u &&
               viewState.remotePlayers[0].team == sim::TeamId::Defender &&
               nearlyEqual(viewState.remotePlayers[0].eyePosition.x, 10.0f) &&
               viewState.remotePlayerGhosts.size() == 1u &&
               viewState.remotePlayerGhosts[0].ghost &&
               nearlyEqual(viewState.remotePlayerGhosts[0].eyePosition.x, 12.0f) &&
               viewState.remoteEnemies.size() == 1u &&
               nearlyEqual(viewState.remoteEnemies[0].healthPercent, 0.5f),
           "typed client view state should preserve remote actor presentation inputs");
    expect(viewState.teamMenu.visible &&
               viewState.teamMenu.currentTeam == sim::TeamId::Attacker &&
               viewState.teamMenu.selectedTeam == sim::TeamId::Defender,
           "typed client view state should preserve team-menu presentation state");
    expect(client::PresentationStateSubsystem::teamScoreSummary(viewState).find("Attackers 4") != std::string::npos &&
               !containsLine(client::PresentationStateSubsystem::compactHudLines(viewState),
                             "Team Kills") &&
               client::PresentationStateSubsystem::compactHudLines(viewState).empty(),
           "typed presentation helpers should keep score, health, and diagnostics out of the removed player HUD");
}

void testHostedSessionBotPolicyLabelsPeaceMode() {
    const client::ClientViewState peaceView =
        makeRichClientViewState(sim::PaneSlot::Left, true, false, false);
    expect(peaceView.hostedSession.available &&
               peaceView.hostedSession.botDirectorLabel == "Peace" &&
               !peaceView.hostedSession.botsFrozen &&
               !peaceView.hostedSession.botsCanShoot,
           "hosted-session presentation should label active non-shooting bots as Peace mode");

    const client::ClientViewState armedView =
        makeRichClientViewState(sim::PaneSlot::Left, true, false, true);
    expect(armedView.hostedSession.botDirectorLabel == "Active" &&
               armedView.hostedSession.botsCanShoot,
           "hosted-session presentation should still label active shooting bots as Active");
}

void testPresentationStateSubsystemBuildsTypedSpectatorPaneState() {
    const client::ClientViewState viewState = makeSpectatorClientViewState();

    expect(viewState.localParticipant.actorId == 7 &&
               viewState.localParticipant.label == "host-player" &&
               viewState.localParticipant.state.team == sim::TeamId::Spectator &&
               viewState.localParticipant.state.participation == sim::ParticipationState::Spectating,
           "typed spectator presentation state should expose the participant identity and authoritative spectator participation");
    expect(viewState.pane.state.slot == sim::PaneSlot::Right &&
               viewState.pane.state.mode == sim::PaneViewMode::SpectatorFollowThirdPerson &&
               viewState.pane.state.followTargetActorId == 22 &&
               viewState.pane.observationContext == client::ObservationContextView::SessionSpectator,
           "typed spectator pane state should expose pane mode and session-spectator observation context explicitly");
    expect(viewState.pane.modeLabel == "Spectator Follow Third Person" &&
               viewState.pane.followTargetLabel == "BOT 22" &&
               viewState.pane.participantTeam == sim::TeamId::Spectator &&
               viewState.pane.participantTeamLabel == "Spectator" &&
               viewState.pane.checkpointLabel == "Checkpoint 2/3" &&
               viewState.pane.canReturnToCharacter,
           "typed spectator pane state should expose mode, target, team, checkpoint, and returnability labels without reading camera ownership");
    expect(client::PresentationStateSubsystem::paneStatusSummary(viewState).find("Session Spectator") != std::string::npos &&
               client::ClientPresentation::paneOverlayLabel(viewState).find("BOT 22") != std::string::npos,
           "presentation helpers should render spectator pane labels from the typed spectator state");
}

void testPresentationStateSubsystemBuildsTypedReplayPaneState() {
    const client::ClientViewState viewState = makeReplayClientViewState();

    expect(viewState.pane.state.slot == sim::PaneSlot::Right &&
               viewState.pane.state.mode == sim::PaneViewMode::ReplayCamera &&
               viewState.pane.bindingLabel == "Replay Camera B" &&
               viewState.pane.followTargetLabel == "Replay Target",
           "typed replay presentation state should expose pane-local replay bindings and preserved spectator target labels");
    expect(viewState.replay.rule.available &&
               viewState.replay.rule.label == "Seen Position" &&
               !viewState.replay.rule.explanation.empty() &&
               viewState.replay.paneBinding.available &&
               viewState.replay.paneBinding.bindingLabel == "Replay Camera B" &&
               viewState.replay.spectator.available &&
               viewState.replay.spectator.followTargetLabel == "Replay Target" &&
               viewState.replay.spectator.canReturnToCharacter,
           "typed replay presentation state should preserve replay rule, binding, and spectator context without parsing the status line");
    expect(client::PresentationStateSubsystem::compactHudLines(viewState).empty(),
           "typed replay presentation helpers should keep replay diagnostics out of the removed player HUD");
    expectContains(client::ClientPresentation::paneOverlayLabel(viewState),
                   "Replay Camera B",
                   "replay pane labels should surface the pane-local replay binding from typed view state");
    expectContains(client::ClientPresentation::paneOverlayLabel(viewState),
                   "Replay Target",
                   "replay pane labels should surface the preserved spectator follow target from typed view state");
}

void testClientPresentationBuildsRenderFrameFromClientViewState() {
    Arena3D arena;
    client::ClientPresentation presentation;
    const client::ClientViewState viewState = makeRichClientViewState();
    const std::vector<LaserBeam3D> combatTraces{
        LaserBeam3D(Vector3{0.0f, 1.0f, 0.0f}, Vector3{2.0f, 1.0f, 0.0f}, RED, 0.15f, 0.05f)
    };

    const client::RenderFrame frame = presentation.build(
        client::ClientPresentationInputs{viewState, &arena, &combatTraces});

    expect(frame.hasSnapshot,
           "render frame should stay in world-render mode when a snapshot-backed client view is available");
    expect(frame.remotePlayers.size() == 1u &&
               frame.remotePlayerGhosts.size() == 1u &&
               frame.remoteEnemies.size() == 1u &&
               frame.combatTraces.size() == 1u,
           "render frame should carry remote player, remote enemy, and combat-trace drawables");
    expect(frame.hud.lines.empty() &&
               frame.compactScore.visible &&
               frame.compactScore.score.localIdentity == "Local Player" &&
               frame.killFeed.visible &&
               frame.killFeed.entries.size() == 1u &&
               frame.scoreboard.visible &&
               frame.scoreboard.sections.size() == 2u,
           "render frame should preserve compact score, kill-feed, hud, and scoreboard overlays");
    expect(nearlyEqual(frame.arena.dimFactor, 1.0f),
           "render frames should carry the typed study-presentation dim factor through the arena layer");
    expect(!containsLine(frame.hud.lines, "Ghosts") &&
               !containsLine(frame.hud.lines, "Shot") &&
               !containsLine(frame.hud.lines, "Prediction"),
           "render frame hud should omit all player-facing text after the HUD removal");
    expect(frame.teamMenu.visible &&
               frame.teamMenu.currentTeamLabel == "Attackers" &&
               frame.teamMenu.selectedTeamLabel == "Defenders",
           "render frame should preserve team-menu labels");
    expect(frame.localNetworkPanelVisible && !frame.diagnosticsPanelVisible,
           "render frame should preserve panel visibility without re-reading runtime flags directly");

    const float expectedGround = arena.getGroundHeightAt(Vector3{10.0f, 4.5f, -12.0f});
    expect(nearlyEqual(frame.remotePlayers[0].rootPosition.y, expectedGround) &&
               frame.remotePlayers[0].tint.b > frame.remotePlayers[0].tint.r,
           "render frame should anchor remote players to arena surfaces and preserve team tinting");
    expect(frame.remotePlayerGhosts[0].ghost &&
               frame.remotePlayerGhosts[0].rootPosition.x > frame.remotePlayers[0].rootPosition.x,
           "render frame should preserve separate ghost-control drawables for the clean snapshot stream");
    expect(frame.remotePlayerGhosts[0].tint.r == frame.remotePlayers[0].tint.r &&
               frame.remotePlayerGhosts[0].tint.g == frame.remotePlayers[0].tint.g &&
               frame.remotePlayerGhosts[0].tint.b == frame.remotePlayers[0].tint.b &&
               frame.remotePlayerGhosts[0].tint.a < frame.remotePlayers[0].tint.a,
           "ghost-control drawables should keep the same team color while rendering at lower opacity");
}

void testClientPresentationSuppressesOverlappedGhostDrawables() {
    Arena3D arena;
    client::ClientPresentation presentation;
    client::ClientViewState viewState = makeRichClientViewState();

    viewState.remotePlayerGhosts.front().eyePosition =
        viewState.remotePlayers.front().eyePosition;

    const client::RenderFrame overlappedFrame =
        presentation.build(client::ClientPresentationInputs{viewState, &arena, nullptr});
    expect(viewState.remotePlayerGhosts.size() == 1u &&
               overlappedFrame.remotePlayers.size() == 1u &&
               overlappedFrame.remotePlayerGhosts.empty(),
           "render frames should suppress zero-offset ghosts while retaining the control track in client view state");

    viewState.remotePlayerGhosts.front().eyePosition.x =
        viewState.remotePlayers.front().eyePosition.x + 0.5f;

    const client::RenderFrame separatedFrame =
        presentation.build(client::ClientPresentationInputs{viewState, &arena, nullptr});
    expect(separatedFrame.remotePlayerGhosts.size() == 1u &&
               separatedFrame.remotePlayerGhosts.front().ghost,
           "render frames should draw ghosts once latency creates a visible separation from the solid actor");
}

void testRealityVisualizationSuppressesGhostPresentation() {
    Arena3D arena;
    client::ClientPresentation presentation;
    const client::ClientViewState viewState =
        makeRichClientViewState(sim::PaneSlot::Left,
                                true,
                                true,
                                true,
                                net::SessionVisualizationMode::Reality);

    expect(viewState.hostedSession.visualizationModeLabel == "Reality" &&
               !viewState.hostedSession.ghostTracksVisible,
           "Reality visualization should surface a typed no-ghost presentation state");
    expect(viewState.remotePlayers.size() == 1u &&
               viewState.remotePlayerGhosts.empty(),
           "Reality visualization should omit ghost-control views while preserving solid players");
    expect(!containsLine(client::PresentationStateSubsystem::compactHudLines(viewState),
                         "Ghosts"),
           "Reality visualization should suppress ghost-track debug HUD summaries");

    client::ClientViewState defensiveView = viewState;
    defensiveView.remotePlayerGhosts.push_back(client::RemotePlayerView{
        8,
        sim::Vec3{12.0f, Config::PLAYER_EYE_HEIGHT, -10.0f},
        0.0f,
        0.0f,
        1.0f,
        true,
        sim::TeamId::Defender,
        true
    });
    const client::RenderFrame defensiveFrame =
        presentation.build(client::ClientPresentationInputs{defensiveView, &arena, nullptr});
    expect(defensiveFrame.remotePlayers.size() == 1u &&
               defensiveFrame.remotePlayerGhosts.empty(),
           "render-frame construction should defensively hide ghosts when Reality mode is active");
}

void testRemovedHudKeepsStructuredOverlays() {
    Arena3D arena;
    client::ClientPresentation presentation;
    client::ClientViewState viewState = makeRichClientViewState();

    expect(client::PresentationStateSubsystem::compactHudLines(viewState).empty(),
           "removed HUD should suppress bottom-left compact text lines");

    const client::RenderFrame frame =
        presentation.build(client::ClientPresentationInputs{viewState, &arena, nullptr});
    expect(frame.hud.lines.empty() &&
               frame.compactScore.visible &&
               frame.compactScore.score.attackerScore == viewState.compactScore.attackerScore &&
               frame.compactScore.score.defenderScore == viewState.compactScore.defenderScore,
           "removed HUD should not disable the separate compact score overlay");
}

void testDeadPlayersRenderAsDownAndHudReflectsRespawnWindow() {
    Arena3D arena;
    client::ClientPresentation presentation;
    client::ClientViewState viewState = makeRichClientViewState();
    viewState.hud.localHealth = 0;
    viewState.remotePlayers.front().alive = false;
    viewState.remotePlayers.front().healthPercent = 0.0f;

    expect(client::PresentationStateSubsystem::compactHudLines(viewState).empty(),
           "compact hud should stay removed when the local player is down");

    const client::RenderFrame frame = presentation.build(
        client::ClientPresentationInputs{viewState, &arena, nullptr});
    expect(frame.remotePlayers.size() == 1u &&
               !frame.remotePlayers[0].alive &&
               frame.remotePlayerGhosts.size() == 1u,
           "render frames should preserve dead remote-player state so presentation can desaturate defeated players");
}

void testAppFlowKeepsPaneLocalObservationDistinctFromSessionSpectating() {
    net::SessionLaunchConfig runtimeConfig =
        net::makeHostSessionLaunchConfig(2, "routing-host", 45160u, 0u, 0u);

    sim::ParticipantState playingParticipant;
    playingParticipant.presence = sim::SessionPresence::Connected;
    playingParticipant.team = sim::TeamId::Attacker;
    playingParticipant.participation = sim::ParticipationState::Playing;
    playingParticipant.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};

    sim::PaneViewState localObserverPane;
    localObserverPane.mode = sim::PaneViewMode::SpectatorFollowFirstPerson;
    localObserverPane.followTargetActorId = 8;

    sim::ParticipantState spectatorParticipant;
    spectatorParticipant.presence = sim::SessionPresence::Connected;
    spectatorParticipant.team = sim::TeamId::Spectator;
    spectatorParticipant.participation = sim::ParticipationState::Spectating;

    sim::PaneViewState spectatorPane;
    spectatorPane.mode = sim::PaneViewMode::SpectatorFreeFly;

    const app::AppFlow::PaneRoute localObserverRoute =
        app::AppFlow::paneRouteFor(runtimeConfig, playingParticipant, localObserverPane);
    const app::AppFlow::PaneRoute sessionSpectatorRoute =
        app::AppFlow::paneRouteFor(runtimeConfig, spectatorParticipant, spectatorPane);
    const app::AppFlow::PaneRoute replayRoute =
        app::AppFlow::paneRouteFor(net::makeReplaySessionLaunchConfig(2),
                                   {},
                                   sim::PaneViewState{sim::PaneSlot::Left, sim::PaneViewMode::ReplayCamera, true, -1});

    expect(localObserverRoute.usesRuntimeSession &&
               localObserverRoute.launchTarget == app::AppFlow::LaunchTarget::RuntimeSession &&
               localObserverRoute.observationContext == app::AppFlow::ObservationContext::PaneLocalObservation,
           "AppFlow routing should keep pane-local spectator observation distinct while the participant remains in gameplay");
    expect(sessionSpectatorRoute.usesRuntimeSession &&
               sessionSpectatorRoute.observationContext == app::AppFlow::ObservationContext::SessionSpectator,
           "AppFlow routing should keep true session-level spectating distinct from pane-local observation");
    expect(!replayRoute.usesRuntimeSession &&
               replayRoute.launchTarget == app::AppFlow::LaunchTarget::Replay &&
               replayRoute.observationContext == app::AppFlow::ObservationContext::PaneLocalObservation,
           "AppFlow routing should keep non-runtime observation surfaces typed separately from runtime spectating");
}

void testSplitScreenContractsKeepStablePaneBindingsAndCameraState() {
    client::PaneViewState leftPane;
    leftPane.slot = sim::PaneSlot::Left;
    leftPane.focused = true;
    leftPane.binding.kind = client::PaneBindingKind::LocalParticipant;
    leftPane.binding.participantId = 7u;
    leftPane.binding.actorId = 7;
    leftPane.binding.label = "Local Player";
    leftPane.viewState = makeRichClientViewState();
    leftPane.predictionEnabled = true;

    client::PaneViewState rightPane;
    rightPane.slot = sim::PaneSlot::Right;
    rightPane.focused = false;
    rightPane.binding.kind = client::PaneBindingKind::SpectatorTarget;
    rightPane.binding.actorId = 22;
    rightPane.binding.label = "BOT 22";
    rightPane.viewState = makeSpectatorClientViewState();
    rightPane.authoritativeOnly = true;

    client::SplitScreenViewState splitView =
        client::buildSplitScreenView(leftPane, rightPane, sim::PaneSlot::Left);
    splitView.sharedOverlays.hudLines =
        client::PresentationStateSubsystem::compactHudLines(leftPane.viewState);
    splitView.sharedOverlays.scoreboardSections = leftPane.viewState.scoreboardSections;
    splitView.sharedOverlays.scoreboardVisible = leftPane.viewState.scoreboardVisible;

    expect(splitView.focusedSlot == sim::PaneSlot::Left &&
               splitView.paneFor(sim::PaneSlot::Left)->binding.kind == client::PaneBindingKind::LocalParticipant &&
               splitView.paneFor(sim::PaneSlot::Right)->binding.kind == client::PaneBindingKind::SpectatorTarget,
           "split-screen view contracts should preserve stable left and right pane bindings without bool-based ownership");
    expect(splitView.paneFor(sim::PaneSlot::Left)->predictionEnabled &&
               splitView.paneFor(sim::PaneSlot::Right)->authoritativeOnly &&
               splitView.paneFor(sim::PaneSlot::Right)->viewState.pane.state.mode ==
                   sim::PaneViewMode::SpectatorFollowThirdPerson,
           "split-screen view contracts should keep independent focus, binding source, and pane-local mode for each slot");
    expect(nearlyEqual(splitView.paneFor(sim::PaneSlot::Left)->viewState.camera.eyePosition.x, 1.0f) &&
               splitView.paneFor(sim::PaneSlot::Right)->viewState.pane.followTargetLabel == "BOT 22",
           "split-screen view contracts should keep each pane's camera and spectator target state independent");
}

void testSplitScreenRenderFrameCarriesTwoPaneWorldsAndSharedOverlays() {
    Arena3D arena;
    client::ClientPresentation presentation;
    const client::ClientViewState leftView = makeRichClientViewState();
    const client::ClientViewState rightView = makeSpectatorClientViewState();
    const std::vector<LaserBeam3D> combatTraces{
        LaserBeam3D(Vector3{0.0f, 1.0f, 0.0f}, Vector3{2.0f, 1.0f, 0.0f}, RED, 0.15f, 0.05f)
    };

    const client::RenderFrame leftFrame =
        presentation.build(client::ClientPresentationInputs{leftView, &arena, &combatTraces});
    const client::RenderFrame rightFrame =
        presentation.build(client::ClientPresentationInputs{rightView, &arena, &combatTraces});

    client::PaneRenderFrame leftPaneFrame;
    leftPaneFrame.slot = sim::PaneSlot::Left;
    leftPaneFrame.focused = true;
    leftPaneFrame.binding = client::PaneBinding{client::PaneBindingKind::LocalParticipant, 7u, 7, "Local Player"};
    leftPaneFrame.world = leftFrame.paneWorldFrame();

    client::PaneRenderFrame rightPaneFrame;
    rightPaneFrame.slot = sim::PaneSlot::Right;
    rightPaneFrame.focused = false;
    rightPaneFrame.binding = client::PaneBinding{client::PaneBindingKind::SpectatorTarget, 0u, 22, "BOT 22"};
    rightPaneFrame.world = rightFrame.paneWorldFrame();

    const client::SplitScreenRenderFrame splitFrame =
        client::buildSplitScreenRenderFrame(leftPaneFrame,
                                            rightPaneFrame,
                                            leftFrame.sharedOverlayFrame(),
                                            sim::PaneSlot::Left);

    expect(splitFrame.left.world.remotePlayers.size() == 1u &&
               splitFrame.left.world.remotePlayerGhosts.size() == 1u &&
               splitFrame.right.world.hasSnapshot &&
               splitFrame.right.binding.kind == client::PaneBindingKind::SpectatorTarget,
           "split-screen render frames should carry both left and right pane world frames explicitly");
    expect(splitFrame.sharedOverlays.hud.lines.size() == leftFrame.hud.lines.size() &&
               splitFrame.sharedOverlays.compactScore.visible == leftFrame.compactScore.visible &&
               splitFrame.sharedOverlays.compactScore.score.localIdentity ==
                   leftFrame.compactScore.score.localIdentity &&
               splitFrame.sharedOverlays.killFeed.entries.size() ==
                   leftFrame.killFeed.entries.size() &&
               splitFrame.sharedOverlays.scoreboard.sections.size() == leftFrame.scoreboard.sections.size() &&
               splitFrame.sharedOverlays.teamMenu.visible == leftFrame.teamMenu.visible,
           "split-screen render frames should carry one shared overlay bundle, including compact score and kill feed, instead of duplicating full UI trees per pane");
}

void testSplitScreenPresentationBuildsAllPaneCombinationsThroughSharedModel() {
    Arena3D arena;
    client::ClientPresentation presentation;
    const std::vector<LaserBeam3D> combatTraces{
        LaserBeam3D(Vector3{0.0f, 1.0f, 0.0f}, Vector3{2.0f, 1.0f, 0.0f}, RED, 0.15f, 0.05f)
    };

    struct Scenario {
        const char* name;
        client::PaneViewState left;
        client::PaneViewState right;
        sim::PaneSlot focus;
        const char* leftModeToken;
        const char* rightModeToken;
    };

    const std::vector<Scenario> scenarios{
        {"player-player",
         makePlayerPane(sim::PaneSlot::Left, true),
         makePlayerPane(sim::PaneSlot::Right, false),
         sim::PaneSlot::Left,
         "Player Controlled",
         "Player Controlled"},
        {"player-spectator",
         makePlayerPane(sim::PaneSlot::Left, false),
         makeSpectatorPane(sim::PaneSlot::Right, true),
         sim::PaneSlot::Right,
         "Player Controlled",
         "Session Spectator"},
        {"spectator-spectator",
         makeSpectatorPane(sim::PaneSlot::Left, true),
         makeSpectatorPane(sim::PaneSlot::Right, false),
         sim::PaneSlot::Left,
         "Session Spectator",
         "Session Spectator"},
        {"player-replay",
         makePlayerPane(sim::PaneSlot::Left, false),
         makeReplayPane(sim::PaneSlot::Right, true),
         sim::PaneSlot::Right,
         "Player Controlled",
         "Replay Camera"}
    };

    for (const auto& scenario : scenarios) {
        const client::SplitScreenViewState splitView =
            client::PresentationStateSubsystem::buildSplitScreenView(
                scenario.left, scenario.right, scenario.focus);
        const client::SplitScreenRenderFrame splitFrame =
            presentation.buildSplitScreen(splitView, arena, &combatTraces);

        expect(splitFrame.left.slot == sim::PaneSlot::Left &&
                   splitFrame.right.slot == sim::PaneSlot::Right,
               std::string(scenario.name) +
                   ": two-slot presentation should preserve canonical left and right pane slots");
        expect(splitFrame.focusedSlot == scenario.focus &&
                   splitFrame.paneFor(scenario.focus)->focused,
               std::string(scenario.name) +
                   ": two-slot presentation should preserve one focused pane through the shared split-screen builder");
        expect(splitFrame.left.world.hasSnapshot &&
                   splitFrame.right.world.hasSnapshot &&
                   splitFrame.left.world.combatTraces.size() == combatTraces.size() &&
                   splitFrame.right.world.combatTraces.size() == combatTraces.size(),
               std::string(scenario.name) +
                   ": every pane combination should flow through the same world-frame composition path");
        expect(splitFrame.left.binding.kind == scenario.left.binding.kind &&
                   splitFrame.right.binding.kind == scenario.right.binding.kind,
               std::string(scenario.name) +
                   ": pane bindings should survive the shared split-screen presentation path");
        expectContains(client::ClientPresentation::paneOverlayLabel(splitView.left.viewState),
                       scenario.leftModeToken,
                       std::string(scenario.name) +
                           ": left pane should expose a data-driven pane label for its presentation mode");
        expectContains(client::ClientPresentation::paneOverlayLabel(splitView.right.viewState),
                       scenario.rightModeToken,
                       std::string(scenario.name) +
                           ": right pane should expose a data-driven pane label for its presentation mode");
    }
}

void testSplitScreenPresentationKeepsSharedOverlaysGlobalAndPaneMarkersDataDriven() {
    Arena3D arena;
    client::ClientPresentation presentation;
    const std::vector<LaserBeam3D> combatTraces{
        LaserBeam3D(Vector3{0.0f, 1.0f, 0.0f}, Vector3{2.0f, 1.0f, 0.0f}, RED, 0.15f, 0.05f)
    };

    const client::SplitScreenViewState splitView =
        client::PresentationStateSubsystem::buildSplitScreenView(
            makePlayerPane(sim::PaneSlot::Left, false),
            makeSpectatorPane(sim::PaneSlot::Right, true),
            sim::PaneSlot::Right);
    const client::SplitScreenRenderFrame splitFrame =
        presentation.buildSplitScreen(splitView, arena, &combatTraces);

    expect(splitView.sharedOverlays.hudLines ==
               client::PresentationStateSubsystem::compactHudLines(splitView.right.viewState) &&
               splitView.sharedOverlays.compactScore.localIdentity ==
                   splitView.right.viewState.compactScore.localIdentity &&
               splitView.sharedOverlays.killFeed.size() ==
                   splitView.right.viewState.killFeed.size() &&
               splitFrame.sharedOverlays.hud.lines == splitView.sharedOverlays.hudLines,
           "shared split-screen overlays should stay global and match the focused pane's shared overlay source");
    expect(splitFrame.sharedOverlays.waiting.hostedSessionLine ==
               splitView.sharedOverlays.hostedSessionLine &&
               splitFrame.sharedOverlays.compactScore.score.localIdentity ==
                   splitView.sharedOverlays.compactScore.localIdentity &&
               splitFrame.sharedOverlays.killFeed.entries.size() ==
                   splitView.sharedOverlays.killFeed.size() &&
               splitFrame.sharedOverlays.scoreboard.visible ==
                   splitView.sharedOverlays.scoreboardVisible &&
               splitFrame.sharedOverlays.scoreboard.sections.size() ==
                   splitView.sharedOverlays.scoreboardSections.size() &&
               splitFrame.sharedOverlays.localNetworkPanelVisible ==
                   splitView.sharedOverlays.localNetworkPanelVisible &&
               splitFrame.sharedOverlays.diagnosticsPanelVisible ==
                   splitView.sharedOverlays.diagnosticsPanelVisible,
           "shared split-screen overlays should remain centralized instead of being duplicated per pane");

    const std::string leftLabel = client::ClientPresentation::paneOverlayLabel(splitView.left.viewState);
    const std::string rightLabel = client::ClientPresentation::paneOverlayLabel(splitView.right.viewState);

    expectContains(leftLabel,
                   "Left Pane",
                   "player pane labels should keep the left slot marker data-driven");
    expectContains(leftLabel,
                   "Unfocused",
                   "player pane labels should keep the unfocused marker data-driven");
    expectContains(leftLabel,
                   "Player Controlled",
                   "player pane labels should keep the gameplay mode data-driven");
    expectContains(rightLabel,
                   "Right Pane",
                   "spectator pane labels should keep the right slot marker data-driven");
    expectContains(rightLabel,
                   "Focused",
                   "spectator pane labels should keep the focused marker data-driven");
    expectContains(rightLabel,
                   "Session Spectator",
                   "spectator pane labels should keep the observation context data-driven");
    expectContains(rightLabel,
                   "BOT 22",
                   "spectator pane labels should keep the follow target data-driven");
}

void testSplitScreenSharedOverlaysPreserveStudyHudAndReplayStateGlobally() {
    Arena3D arena;
    client::ClientPresentation presentation;
    const std::vector<LaserBeam3D> combatTraces{
        LaserBeam3D(Vector3{0.0f, 1.0f, 0.0f}, Vector3{2.0f, 1.0f, 0.0f}, RED, 0.15f, 0.05f)
    };

    client::SplitScreenViewState splitView =
        client::PresentationStateSubsystem::buildSplitScreenView(
            makePlayerPane(sim::PaneSlot::Left, true),
            makeReplayPane(sim::PaneSlot::Right, false),
            sim::PaneSlot::Left);
    splitView.sharedOverlays.replay = makeReplayClientViewState(sim::PaneSlot::Right, false).replay;
    const client::SplitScreenRenderFrame splitFrame =
        presentation.buildSplitScreen(splitView, arena, &combatTraces);

    expect(splitFrame.sharedOverlays.hud.lines.empty() &&
               splitFrame.sharedOverlays.replay.statusLine == splitView.sharedOverlays.replay.statusLine &&
               splitFrame.sharedOverlays.replay.rule.label == splitView.sharedOverlays.replay.rule.label &&
               splitFrame.sharedOverlays.replay.paneBinding.bindingLabel ==
                   splitView.sharedOverlays.replay.paneBinding.bindingLabel &&
               splitFrame.sharedOverlays.replay.spectator.followTargetLabel ==
                   splitView.sharedOverlays.replay.spectator.followTargetLabel,
           "shared split-screen overlays should carry replay status without restoring the removed HUD");
}

void testRuntimeOverlayWidgetsUseSharedTypographyService() {
    const std::filesystem::path repoRoot = findRepoRoot();

    for (const char* relativePath : {
             "include/RuntimeSettingsOverlay.hpp",
         }) {
        const std::string source = readTextFile(repoRoot / relativePath);
        expect(source.find("TypographyService") != std::string::npos,
               std::string(relativePath) +
                   " should depend on the shared typography service for runtime overlay text");
        expect(countOccurrences(source, "DrawText(") == 0u,
               std::string(relativePath) +
                   " should no longer issue raw DrawText calls for runtime overlay text");
        expect(countOccurrences(source, "MeasureText(") == 0u,
               std::string(relativePath) +
                   " should no longer issue raw MeasureText calls for runtime overlay text");
        expect(countOccurrences(source, "DrawTextEx(") == 0u,
               std::string(relativePath) +
                   " should no longer own per-widget DrawTextEx font rendering");
        expect(countOccurrences(source, "MeasureTextEx(") == 0u,
               std::string(relativePath) +
                   " should no longer own per-widget MeasureTextEx font measurement");
    }
}

void testPlayerHealthBarsUseEyeToRootAnchorHelper() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string playerSource = readTextFile(repoRoot / "include/Player3D.hpp");

    expect(playerSource.find("static Vector3 healthBarRootFromEyePosition(Vector3 eyePosition)") != std::string::npos,
           "Player3D should expose one helper that converts eye-height positions into health-bar root anchors");
    expect(playerSource.find("static Vector3 healthBarCenterFromRoot(Vector3 rootPosition)") != std::string::npos &&
               playerSource.find("Config::HEALTH_BAR_VERTICAL_OFFSET") != std::string::npos,
           "Player3D should anchor health bars with one shared above-head offset constant");
    expect(playerSource.find("Vector3 headPos = headCenterFromRoot(rootPos);") != std::string::npos,
           "Player3D ghost and solid heads should share one root-based head anchor so smaller ghost shells stay inside the character");

    const std::size_t ghostStart = playerSource.find("void renderGhostAtRoot");
    const std::size_t ghostEnd = playerSource.find("void render(const client::RemotePlayerRenderItem& item)");
    expect(ghostStart != std::string::npos && ghostEnd != std::string::npos && ghostEnd > ghostStart,
           "Player3D should keep the ghost render function discoverable for presentation characterization");
    const std::string ghostRenderer = playerSource.substr(ghostStart, ghostEnd - ghostStart);
    expect(ghostRenderer.find("rlDisableDepthTest") == std::string::npos &&
               ghostRenderer.find("kGhostHeadScale") != std::string::npos,
           "Player3D ghosts should obey normal world depth and shrink around the shared head anchor");
    expect(ghostRenderer.find("kGhostHeadLowerOffset") != std::string::npos &&
               ghostRenderer.find("DrawCylinderEx") == std::string::npos,
           "Player3D ghosts should lower the contained head slightly without drawing a ghost-only neck marker");
}

}  // namespace

int main() {
    try {
        testGroundedRemotePlayerUsesGroundAlignedRenderRoot();
        testAirborneRemotePlayerStaysAboveGround();
        testRemotePlayerOnRaisedGeometryUsesSurfaceAnchor();
        testPresentationStateSubsystemBuildsTypedScoreboardAndDiagnosticsState();
        testHostedSessionBotPolicyLabelsPeaceMode();
        testPresentationStateSubsystemBuildsTypedSpectatorPaneState();
        testPresentationStateSubsystemBuildsTypedReplayPaneState();
        testClientPresentationBuildsRenderFrameFromClientViewState();
        testClientPresentationSuppressesOverlappedGhostDrawables();
        testRealityVisualizationSuppressesGhostPresentation();
        testRemovedHudKeepsStructuredOverlays();
        testDeadPlayersRenderAsDownAndHudReflectsRespawnWindow();
        testAppFlowKeepsPaneLocalObservationDistinctFromSessionSpectating();
        testSplitScreenContractsKeepStablePaneBindingsAndCameraState();
        testSplitScreenRenderFrameCarriesTwoPaneWorldsAndSharedOverlays();
        testSplitScreenPresentationBuildsAllPaneCombinationsThroughSharedModel();
        testSplitScreenPresentationKeepsSharedOverlaysGlobalAndPaneMarkersDataDriven();
        testSplitScreenSharedOverlaysPreserveStudyHudAndReplayStateGlobally();
        testRuntimeOverlayWidgetsUseSharedTypographyService();
        testPlayerHealthBarsUseEyeToRootAnchorHelper();
        std::cout << "PlayerPresentationTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "PlayerPresentationTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
