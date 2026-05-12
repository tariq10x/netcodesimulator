#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "client/ClientViewState.hpp"
#include "client/SplitScreenViewState.hpp"
#include "net/DiagnosticsModel.hpp"
#include "net/Protocol.hpp"
#include "sim/SimulationRules.hpp"

namespace client {

struct PresentationStateInputs {
    const net::HostedSessionMetadata& sessionMetadata;
    const std::vector<sim::RosterEntry>& roster;
    const sim::TeamScores& teamScores;
    const sim::PlayerState& localPlayerState;
    const std::vector<sim::PlayerState>& remotePlayers;
    const std::vector<sim::PlayerState>& controlRemotePlayers;
    const std::vector<sim::RemoteActorState>& remoteEnemies;
    const net::DiagnosticsModel* diagnosticsModel{nullptr};
    std::uint16_t peerId{0u};
    sim::TeamId preferredTeam{sim::TeamId::None};
    bool connected{false};
    bool hasSnapshot{false};
    std::string connectionStateLabel{};
    std::string statusMessage{};
    float viewYaw{0.0f};
    float viewPitch{0.0f};
    bool hasLocalNetworkControls{false};
    bool hasHostDiagnostics{false};
    bool localNetworkPanelVisible{false};
    bool teamMenuVisible{false};
    sim::TeamId teamMenuSelection{sim::TeamId::None};
    bool uiModeActive{false};
    bool scoreboardVisible{false};
    const std::string& combatEventText;
    ReplayStatusView replay{};
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
    std::uint32_t ackedInputSeq{0u};
    ReconciliationStrategyView reconciliationStrategy{ReconciliationStrategyView::Smooth};
    CorrectionModeView correctionMode{CorrectionModeView::None};
    float correctionMagnitude{0.0f};
    std::uint32_t replayedCommandCount{0u};
    std::uint32_t pendingCommandCount{0u};
    std::uint32_t smoothWindowMs{0u};
    sim::ParticipantState localParticipantState{};
    sim::PaneViewState localPaneState{};
    std::string localParticipantLabel{};
    std::string paneFollowTargetLabel{};
    int spectatorCheckpointIndex{-1};
    std::size_t spectatorCheckpointCount{0u};
    bool spectatorCanReturnToCharacter{false};
    bool hasCameraEyePositionOverride{false};
    sim::Vec3 cameraEyePosition{};
    bool studyEnvironmentDimmed{false};
    float studyEnvironmentDimFactor{0.0f};
    bool studyAreasVisible{true};
    AreaFilterView studyAreaFilter{AreaFilterView::All};
    std::vector<KillFeedEntryView> killFeedEntries{};
};

class PresentationStateSubsystem {
public:
    ClientViewState build(const PresentationStateInputs& inputs) const {
        ClientViewState viewState;
        viewState.connection.connected = inputs.connected;
        viewState.connection.hasSnapshot = inputs.hasSnapshot;
        viewState.connection.stateLabel = inputs.connectionStateLabel;
        viewState.connection.statusMessage = inputs.statusMessage;
        viewState.camera = cameraView(inputs);
        viewState.hostedSession = hostedSessionView(inputs.sessionMetadata);
        viewState.hud.localHealth = static_cast<int>(std::lround(inputs.localPlayerState.health));
        viewState.hud.localTeam = resolveLocalTeam(inputs);
        viewState.hud.combatEventText = inputs.combatEventText;
        viewState.studyPresentation = studyPresentationView(inputs);
        viewState.killFeed = inputs.killFeedEntries;
        viewState.localParticipant = participantIdentityView(inputs, viewState.hud.localTeam);
        viewState.compactScore = compactScoreView(inputs,
                                                  viewState.localParticipant.label,
                                                  viewState.hud.localTeam);
        viewState.pane = paneStatusView(inputs, viewState.localParticipant, viewState.hud.localTeam);
        viewState.timing = timingView(inputs);
        viewState.attackerScore = inputs.teamScores.attackers;
        viewState.defenderScore = inputs.teamScores.defenders;
        viewState.scoreboardSections = buildScoreboardSections(inputs);
        viewState.diagnostics = diagnosticsView(inputs);
        viewState.replay = inputs.replay;
        viewState.remotePlayers = buildRemotePlayers(inputs);
        viewState.remotePlayerGhosts = viewState.hostedSession.ghostTracksVisible
            ? buildRemotePlayers(inputs, inputs.controlRemotePlayers, true)
            : std::vector<RemotePlayerView>{};
        viewState.remoteEnemies = buildRemoteEnemies(inputs);
        viewState.teamMenu.visible = inputs.teamMenuVisible;
        viewState.teamMenu.currentTeam = viewState.hud.localTeam;
        viewState.teamMenu.selectedTeam = inputs.teamMenuSelection;
        viewState.uiModeActive = inputs.uiModeActive;
        viewState.scoreboardVisible = inputs.scoreboardVisible;
        return viewState;
    }

    static std::string hostedSessionSummary(const ClientViewState& viewState) {
        if (!viewState.hostedSession.available) {
            return {};
        }

        std::string summary = "Session: " + viewState.hostedSession.label +
                              " | Rule: " + viewState.hostedSession.shotRuleLabel +
                              " | Bots: " + viewState.hostedSession.botDirectorLabel;
        if (viewState.hostedSession.publicJoinPort != 0u) {
            summary += " | Join Port " + std::to_string(viewState.hostedSession.publicJoinPort);
        }
        return summary;
    }

    static std::string teamScoreSummary(const ClientViewState& viewState) {
        return std::string("Team Kills | Attackers ") + std::to_string(viewState.attackerScore) +
               " | Defenders " + std::to_string(viewState.defenderScore);
    }

    static std::vector<std::string> compactHudLines(const ClientViewState& viewState) {
        static_cast<void>(viewState);
        return {};
    }

    static std::string paneStatusSummary(const ClientViewState& viewState) {
        if (viewState.pane.observationContext == ObservationContextView::Gameplay &&
            viewState.pane.state.mode == sim::PaneViewMode::PlayerControlled) {
            return {};
        }

        std::string summary = viewState.pane.slotLabel;
        if (!viewState.pane.focusLabel.empty()) {
            summary += " | " + viewState.pane.focusLabel;
        }
        summary += " | " + observationContextLabel(viewState.pane.observationContext);
        summary += " | " + viewState.pane.modeLabel;
        if (!viewState.pane.bindingLabel.empty()) {
            summary += " | " + viewState.pane.bindingLabel;
        }
        if (!viewState.pane.followTargetLabel.empty()) {
            summary += " | Target " + viewState.pane.followTargetLabel;
        }
        if (!viewState.pane.participantTeamLabel.empty()) {
            summary += " | Team " + viewState.pane.participantTeamLabel;
        }
        if (viewState.pane.canReturnToCharacter) {
            summary += " | Return available";
        }
        return summary;
    }

    static std::vector<std::string> scoreboardLines(const ClientViewState& viewState) {
        std::vector<std::string> lines;
        lines.push_back(teamScoreSummary(viewState));
        for (const auto& section : viewState.scoreboardSections) {
            lines.push_back(std::string(sim::toString(section.team)) + ":");
            if (section.entries.empty()) {
                lines.push_back("  (empty)");
                continue;
            }

            for (const auto& entry : section.entries) {
                lines.push_back("  " + entry.rowLabel);
            }
        }
        return lines;
    }

    static SplitScreenViewState buildSplitScreenView(const PaneViewState& left,
                                                     const PaneViewState& right,
                                                     sim::PaneSlot focus) {
        SplitScreenViewState view = client::buildSplitScreenView(left, right, focus);
        syncPanePresentationState(view.left, sim::PaneSlot::Left, focus == sim::PaneSlot::Left);
        syncPanePresentationState(view.right, sim::PaneSlot::Right, focus == sim::PaneSlot::Right);

        const PaneViewState* overlaySource = view.paneFor(focus);
        if (overlaySource == nullptr) {
            overlaySource = &view.left;
        }

        view.sharedOverlays = sharedOverlayViewState(overlaySource->viewState);
        return view;
    }

private:
    static bool isObservationalPaneMode(sim::PaneViewMode mode) {
        return mode == sim::PaneViewMode::SpectatorFreeFly ||
               mode == sim::PaneViewMode::SpectatorFollowFirstPerson ||
               mode == sim::PaneViewMode::SpectatorFollowThirdPerson ||
               mode == sim::PaneViewMode::ReplayCamera;
    }

    static const sim::RosterEntry* findRosterEntry(const PresentationStateInputs& inputs, int actorId) {
        const auto it = std::find_if(inputs.roster.begin(),
                                     inputs.roster.end(),
                                     [actorId](const sim::RosterEntry& entry) {
                                         return entry.actorId == actorId;
                                     });
        return it != inputs.roster.end() ? &(*it) : nullptr;
    }

    static std::string participantLabel(const PresentationStateInputs& inputs) {
        if (!inputs.localParticipantLabel.empty()) {
            return inputs.localParticipantLabel;
        }
        if (const sim::RosterEntry* entry = findRosterEntry(inputs, static_cast<int>(inputs.peerId))) {
            return scoreboardIdentityLabel(*entry);
        }
        return "P" + std::to_string(inputs.peerId);
    }

    static ParticipantIdentityView participantIdentityView(const PresentationStateInputs& inputs,
                                                           sim::TeamId localTeam) {
        ParticipantIdentityView participant;
        participant.actorId = static_cast<int>(inputs.peerId);
        participant.label = participantLabel(inputs);
        participant.state = inputs.localParticipantState;
        if (participant.state.team == sim::TeamId::None) {
            participant.state.team = localTeam;
        }
        if (const sim::RosterEntry* entry = findRosterEntry(inputs, participant.actorId)) {
            participant.isBot = entry->isBot;
        }
        return participant;
    }

    static ObservationContextView observationContextFor(const ParticipantIdentityView& participant,
                                                        const sim::PaneViewState& paneState) {
        if (!isObservationalPaneMode(paneState.mode)) {
            return ObservationContextView::Gameplay;
        }
        if (participant.state.participation == sim::ParticipationState::Spectating ||
            participant.state.team == sim::TeamId::Spectator) {
            return ObservationContextView::SessionSpectator;
        }
        return ObservationContextView::PaneLocalObservation;
    }

    static std::string observationContextLabel(ObservationContextView context) {
        switch (context) {
            case ObservationContextView::Gameplay:
                return "Gameplay";
            case ObservationContextView::PaneLocalObservation:
                return "Pane Observer";
            case ObservationContextView::SessionSpectator:
                return "Session Spectator";
        }
        return "Gameplay";
    }

    static std::string paneModeLabel(sim::PaneViewMode mode) {
        switch (mode) {
            case sim::PaneViewMode::PlayerControlled:
                return "Player Controlled";
            case sim::PaneViewMode::SpectatorFreeFly:
                return "Spectator Free-Fly";
            case sim::PaneViewMode::SpectatorFollowFirstPerson:
                return "Spectator Follow First Person";
            case sim::PaneViewMode::SpectatorFollowThirdPerson:
                return "Spectator Follow Third Person";
            case sim::PaneViewMode::ReplayCamera:
                return "Replay Camera";
        }
        return "Player Controlled";
    }

    static std::string paneSlotLabel(sim::PaneSlot slot) {
        return slot == sim::PaneSlot::Left ? "Left Pane" : "Right Pane";
    }

    static void syncPanePresentationState(PaneViewState& pane,
                                          sim::PaneSlot slot,
                                          bool focused) {
        pane.slot = slot;
        pane.focused = focused;
        pane.viewState.pane.state.slot = slot;
        pane.viewState.pane.state.focused = focused;
        pane.viewState.pane.slotLabel = paneSlotLabel(slot);
        pane.viewState.pane.focusLabel = focused ? "Focused" : "Unfocused";
    }

    static std::string checkpointLabel(const sim::PaneViewState& paneState,
                                       int activeIndex,
                                       std::size_t checkpointCount) {
        if (!isObservationalPaneMode(paneState.mode)) {
            return {};
        }
        if (checkpointCount == 0u) {
            return "Checkpoint none";
        }

        const int clampedIndex = std::clamp(activeIndex, 0, static_cast<int>(checkpointCount) - 1);
        return "Checkpoint " + std::to_string(clampedIndex + 1) +
               "/" + std::to_string(checkpointCount);
    }

    static PaneStatusView paneStatusView(const PresentationStateInputs& inputs,
                                         const ParticipantIdentityView& participant,
                                         sim::TeamId localTeam) {
        PaneStatusView pane;
        pane.state = inputs.localPaneState;
        pane.observationContext = observationContextFor(participant, pane.state);
        pane.participantTeam =
            participant.state.team == sim::TeamId::None ? localTeam : participant.state.team;
        pane.participantTeamLabel = std::string(sim::toString(pane.participantTeam));
        pane.slotLabel = paneSlotLabel(pane.state.slot);
        pane.focusLabel = pane.state.focused ? "Focused" : "Unfocused";
        pane.modeLabel = paneModeLabel(pane.state.mode);
        pane.bindingLabel = paneBindingLabel(inputs);
        pane.followTargetLabel = paneFollowTargetLabel(inputs);
        pane.activeCheckpointIndex = inputs.spectatorCheckpointIndex;
        pane.checkpointCount = inputs.spectatorCheckpointCount;
        pane.checkpointLabel = checkpointLabel(pane.state,
                                               pane.activeCheckpointIndex,
                                               pane.checkpointCount);
        pane.canReturnToCharacter = inputs.spectatorCanReturnToCharacter;
        return pane;
    }

    static SharedOverlayViewState sharedOverlayViewState(const ClientViewState& viewState) {
        SharedOverlayViewState overlays;
        overlays.hostedSessionLine = hostedSessionSummary(viewState);
        overlays.hudLines = compactHudLines(viewState);
        overlays.compactScore = viewState.compactScore;
        overlays.killFeed = viewState.killFeed;
        overlays.scoreboardSections = viewState.scoreboardSections;
        overlays.replay = viewState.replay;
        overlays.scoreboardVisible = viewState.scoreboardVisible;
        overlays.localNetworkPanelVisible = viewState.diagnostics.localNetworkPanelVisible;
        overlays.diagnosticsPanelVisible =
            viewState.uiModeActive &&
            viewState.diagnostics.hasHostDiagnostics &&
            !viewState.diagnostics.localNetworkPanelVisible;
        return overlays;
    }

    static std::string studyRuleSummary(const DiagnosticsViewState& diagnostics) {
        if (!diagnostics.authority.available ||
            diagnostics.authority.shotRuleLabel.empty() ||
            diagnostics.authority.shotRuleExplanation.empty()) {
            return {};
        }

        return "Rule " + diagnostics.authority.shotRuleLabel +
               ": " + diagnostics.authority.shotRuleExplanation;
    }

    static std::string predictionSummary(const DiagnosticsViewState& diagnostics) {
        if (!diagnostics.prediction.available) {
            return {};
        }

        std::ostringstream line;
        line << "Prediction " << diagnostics.prediction.reconciliationStrategyLabel
             << " | Pending " << diagnostics.prediction.pendingInputCount
             << " | Replayed " << diagnostics.prediction.replayedCommandCount
             << " | Correction " << diagnostics.prediction.correctionModeLabel;
        return line.str();
    }

    static std::string shotStudySummary(const DiagnosticsViewState& diagnostics) {
        if (!diagnostics.shotStudy.available) {
            return {};
        }

        return "Shot " + diagnostics.shotStudy.outcomeLabel +
               " | " + diagnostics.shotStudy.evaluatedStateLabel;
    }

    static std::string replayRuleSummary(const ReplayStatusView& replay) {
        if (!replay.rule.available ||
            replay.rule.label.empty() ||
            replay.rule.explanation.empty()) {
            return {};
        }

        return "Replay Rule " + replay.rule.label + ": " + replay.rule.explanation;
    }

    static std::string replaySpectatorSummary(const ReplayStatusView& replay) {
        if (!replay.spectator.available || replay.spectator.modeLabel.empty()) {
            return {};
        }

        std::string summary = "Replay View " + replay.spectator.modeLabel;
        if (!replay.spectator.followTargetLabel.empty()) {
            summary += " | Target " + replay.spectator.followTargetLabel;
        }
        if (replay.spectator.sessionSpectator) {
            summary += " | Session Spectator";
        }
        if (replay.spectator.canReturnToCharacter) {
            summary += " | Return available";
        }
        return summary;
    }

    static std::string paneBindingLabel(const PresentationStateInputs& inputs) {
        if (inputs.localPaneState.mode != sim::PaneViewMode::ReplayCamera ||
            !inputs.replay.paneBinding.available) {
            return {};
        }

        return inputs.replay.paneBinding.bindingLabel;
    }

    static std::string paneFollowTargetLabel(const PresentationStateInputs& inputs) {
        if (inputs.localPaneState.mode == sim::PaneViewMode::ReplayCamera &&
            inputs.replay.spectator.available &&
            !inputs.replay.spectator.followTargetLabel.empty()) {
            return inputs.replay.spectator.followTargetLabel;
        }
        return inputs.paneFollowTargetLabel;
    }

    static CameraViewState cameraView(const PresentationStateInputs& inputs) {
        CameraViewState camera;
        camera.available = inputs.hasSnapshot;
        camera.eyePosition = inputs.hasCameraEyePositionOverride
            ? inputs.cameraEyePosition
            : inputs.localPlayerState.position;
        camera.lookDirection = sim::lookDirection(inputs.viewYaw, inputs.viewPitch);
        camera.fovY = 70.0f;
        return camera;
    }

    static StudyPresentationView studyPresentationView(const PresentationStateInputs& inputs) {
        StudyPresentationView view;
        view.available = true;
        view.environmentDimmed = inputs.studyEnvironmentDimmed;
        view.environmentDimFactor = std::clamp(inputs.studyEnvironmentDimFactor, 0.0f, 1.0f);
        view.areasVisible = inputs.studyAreasVisible;
        view.areaFilter = inputs.studyAreaFilter;
        view.clutterLabel = view.environmentDimmed ? "Clutter dimmed" : "Clutter normal";
        view.areaLabel = view.areasVisible ? "Areas visible" : "Areas hidden";
        switch (view.areaFilter) {
            case AreaFilterView::All:
                view.filterLabel = "All areas";
                break;
            case AreaFilterView::RedOnly:
                view.filterLabel = "Red areas";
                break;
            case AreaFilterView::GreenOnly:
                view.filterLabel = "Green areas";
                break;
        }
        return view;
    }

    static bool shouldRenderLocalPlayerInWorld(const PresentationStateInputs& inputs) {
        return inputs.localParticipantState.participation == sim::ParticipationState::Playing &&
               isObservationalPaneMode(inputs.localPaneState.mode);
    }

    static TimingViewState timingView(const PresentationStateInputs& inputs) {
        TimingViewState timing;
        timing.cadence = inputs.cadence;
        timing.authoritativeTime = inputs.authoritativeTime;
        timing.ackedInputSeq = inputs.ackedInputSeq;
        timing.reconciliation.strategy = inputs.reconciliationStrategy;
        timing.reconciliation.lastMode = inputs.correctionMode;
        timing.reconciliation.correctionMagnitude = inputs.correctionMagnitude;
        timing.reconciliation.replayedCommandCount = inputs.replayedCommandCount;
        timing.reconciliation.pendingCommandCount = inputs.pendingCommandCount;
        timing.reconciliation.smoothWindowMs = inputs.smoothWindowMs;
        return timing;
    }

    static HostedSessionView hostedSessionView(const net::HostedSessionMetadata& metadata) {
        HostedSessionView view;
        view.available = !metadata.sessionLabel.empty() ||
                         !metadata.hostPlayerName.empty() ||
                         metadata.levelSlot >= 0 ||
                         metadata.levelHash != 0u ||
                         metadata.publicJoinPort != 0u;
        if (!view.available) {
            return view;
        }

        view.label = !metadata.sessionLabel.empty()
            ? metadata.sessionLabel
            : (!metadata.hostPlayerName.empty() ? metadata.hostPlayerName : "Hosted Session");
        view.shotRuleLabel = std::string(net::toString(metadata.shotEvaluationMode));
        view.visualizationModeLabel = std::string(net::toString(metadata.visualizationMode));
        view.botDirectorLabel = metadata.botsFrozen
            ? "Frozen"
            : (metadata.botsCanShoot ? "Active" : "Peace");
        view.publicJoinPort = metadata.publicJoinPort;
        view.botsFrozen = metadata.botsFrozen;
        view.botsCanShoot = metadata.botsCanShoot;
        view.ghostTracksVisible =
            metadata.visualizationMode == net::SessionVisualizationMode::Diagnostic;
        return view;
    }

    static sim::TeamId resolveLocalTeam(const PresentationStateInputs& inputs) {
        if (inputs.localParticipantState.team != sim::TeamId::None) {
            return inputs.localParticipantState.team;
        }
        const auto it = std::find_if(inputs.roster.begin(),
                                     inputs.roster.end(),
                                     [&inputs](const sim::RosterEntry& entry) {
                                         return entry.actorId == static_cast<int>(inputs.peerId);
                                     });
        if (it != inputs.roster.end()) {
            return it->team;
        }
        return inputs.preferredTeam;
    }

    static sim::TeamId resolveActorTeam(const PresentationStateInputs& inputs, int actorId) {
        const auto it = std::find_if(inputs.roster.begin(),
                                     inputs.roster.end(),
                                     [actorId](const sim::RosterEntry& entry) {
                                         return entry.actorId == actorId;
                                     });
        return it != inputs.roster.end() ? it->team : sim::TeamId::None;
    }

    static bool resolveActorAlive(const PresentationStateInputs& inputs,
                                  int actorId,
                                  bool fallbackAlive) {
        const auto it = std::find_if(inputs.roster.begin(),
                                     inputs.roster.end(),
                                     [actorId](const sim::RosterEntry& entry) {
                                         return entry.actorId == actorId;
                                     });
        return it != inputs.roster.end() ? it->alive : fallbackAlive;
    }

    static std::vector<ScoreboardSectionView> buildScoreboardSections(const PresentationStateInputs& inputs) {
        std::vector<ScoreboardSectionView> sections;
        sections.reserve(2u);
        sections.push_back(buildSection(inputs, sim::TeamId::Attacker, inputs.teamScores.attackers));
        sections.push_back(buildSection(inputs, sim::TeamId::Defender, inputs.teamScores.defenders));
        return sections;
    }

    static CompactScoreView compactScoreView(const PresentationStateInputs& inputs,
                                             const std::string& localIdentity,
                                             sim::TeamId localTeam) {
        CompactScoreView view;
        view.available = inputs.hasSnapshot;
        view.attackerScore = inputs.teamScores.attackers;
        view.defenderScore = inputs.teamScores.defenders;
        view.localIdentity = localIdentity;
        view.localTeam = localTeam;
        view.localAlive = inputs.localPlayerState.health > 0.0f;
        const sim::RosterEntry* localEntry =
            findRosterEntry(inputs, static_cast<int>(inputs.peerId));
        if (localEntry != nullptr) {
            view.localKills = static_cast<int>(localEntry->kills);
            view.localDeaths = static_cast<int>(localEntry->deaths);
            view.localAlive = localEntry->alive;
            if (view.localIdentity.empty()) {
                view.localIdentity = scoreboardIdentityLabel(*localEntry);
            }
        }
        if (view.localIdentity.empty() && inputs.peerId != 0u) {
            view.localIdentity = "P" + std::to_string(inputs.peerId);
        }
        return view;
    }

    static std::vector<RemotePlayerView> buildRemotePlayers(const PresentationStateInputs& inputs) {
        return buildRemotePlayers(inputs, inputs.remotePlayers, false);
    }

    static std::vector<RemotePlayerView> buildRemotePlayers(
        const PresentationStateInputs& inputs,
        const std::vector<sim::PlayerState>& playerStates,
        bool ghost) {
        std::vector<RemotePlayerView> remotePlayers;
        remotePlayers.reserve(playerStates.size() + ((!ghost && shouldRenderLocalPlayerInWorld(inputs)) ? 1u : 0u));
        for (const auto& state : playerStates) {
            RemotePlayerView view;
            view.actorId = state.playerId;
            view.eyePosition = state.position;
            view.yaw = state.yaw;
            view.pitch = state.pitch;
            view.healthPercent = state.maxHealth > 0.0f
                ? std::clamp(state.health / state.maxHealth, 0.0f, 1.0f)
                : 0.0f;
            view.alive = resolveActorAlive(inputs, state.playerId, state.health > 0.0f);
            view.team = resolveActorTeam(inputs, state.playerId);
            view.ghost = ghost;
            remotePlayers.push_back(view);
        }

        if (!ghost && shouldRenderLocalPlayerInWorld(inputs)) {
            RemotePlayerView localView;
            localView.actorId = inputs.localPlayerState.playerId;
            localView.eyePosition = inputs.localPlayerState.position;
            localView.yaw = inputs.localPlayerState.yaw;
            localView.pitch = inputs.localPlayerState.pitch;
            localView.healthPercent = inputs.localPlayerState.maxHealth > 0.0f
                ? std::clamp(inputs.localPlayerState.health / inputs.localPlayerState.maxHealth, 0.0f, 1.0f)
                : 0.0f;
            localView.alive = resolveActorAlive(inputs,
                                                inputs.localPlayerState.playerId,
                                                inputs.localPlayerState.health > 0.0f);
            localView.team = resolveActorTeam(inputs, inputs.localPlayerState.playerId);
            localView.ghost = false;
            remotePlayers.push_back(localView);
        }
        return remotePlayers;
    }

    static std::string ghostTrackSummary(const ClientViewState& viewState) {
        if (!viewState.hostedSession.ghostTracksVisible) {
            return {};
        }

        if (viewState.remotePlayers.empty() && viewState.remotePlayerGhosts.empty()) {
            return {};
        }

        std::size_t matchedGhosts = 0u;
        float maxOffsetMeters = 0.0f;
        for (const auto& ghost : viewState.remotePlayerGhosts) {
            const auto it = std::find_if(
                viewState.remotePlayers.begin(),
                viewState.remotePlayers.end(),
                [&ghost](const RemotePlayerView& player) {
                    return player.actorId == ghost.actorId;
                });
            if (it == viewState.remotePlayers.end()) {
                continue;
            }

            const float dx = ghost.eyePosition.x - it->eyePosition.x;
            const float dz = ghost.eyePosition.z - it->eyePosition.z;
            const float offsetMeters = std::sqrt((dx * dx) + (dz * dz));
            maxOffsetMeters = std::max(maxOffsetMeters, offsetMeters);
            ++matchedGhosts;
        }

        std::ostringstream summary;
        summary << "Ghosts " << viewState.remotePlayerGhosts.size();
        if (!viewState.remotePlayerGhosts.empty()) {
            summary << " | Matched " << matchedGhosts;
            if (matchedGhosts > 0u) {
                summary << " | Offset " << std::fixed << std::setprecision(2) << maxOffsetMeters
                        << "m";
            } else {
                summary << " | Offset n/a";
            }
        }
        return summary.str();
    }

    static std::vector<RemoteEnemyView> buildRemoteEnemies(const PresentationStateInputs& inputs) {
        std::vector<RemoteEnemyView> remoteEnemies;
        remoteEnemies.reserve(inputs.remoteEnemies.size());
        for (const auto& state : inputs.remoteEnemies) {
            RemoteEnemyView view;
            view.entityId = state.entityId;
            view.position = state.position;
            view.yaw = state.yaw;
            view.pitch = state.pitch;
            view.healthPercent = sim::defaults::kEnemyMaxHealth > 0.0f
                ? std::clamp(state.health / sim::defaults::kEnemyMaxHealth, 0.0f, 1.0f)
                : 0.0f;
            view.alive = state.alive;
            remoteEnemies.push_back(view);
        }
        return remoteEnemies;
    }

    static ScoreboardSectionView buildSection(const PresentationStateInputs& inputs,
                                              sim::TeamId team,
                                              std::uint16_t score) {
        ScoreboardSectionView section;
        section.team = team;
        section.score = score;

        for (const auto& entry : inputs.roster) {
            if (entry.team != team) {
                continue;
            }

            ScoreboardEntryView view;
            view.identity = scoreboardIdentityLabel(entry);
            view.team = entry.team;
            view.actorId = entry.actorId;
            view.kills = entry.kills;
            view.deaths = entry.deaths;
            view.pingMs = entry.isBot ? 0u : static_cast<std::uint16_t>(entry.latencyMs * 2u);
            view.alive = entry.alive;
            view.isBot = entry.isBot;
            view.isLocalPlayer = entry.actorId == static_cast<int>(inputs.peerId);
            view.rowLabel = scoreboardRowLabel(view);
            section.entries.push_back(std::move(view));
        }

        return section;
    }

    static std::string scoreboardIdentityLabel(const sim::RosterEntry& entry) {
        if (!entry.displayName.empty()) {
            return entry.displayName;
        }

        return entry.isBot
            ? "BOT " + std::to_string(entry.actorId)
            : "P" + std::to_string(entry.actorId);
    }

    static std::string scoreboardRowLabel(const ScoreboardEntryView& entry) {
        std::ostringstream row;
        row << entry.identity
            << "  K/D "
            << entry.kills << "/" << entry.deaths;

        if (entry.isBot) {
            row << "  Ping --";
        } else {
            row << "  Ping " << entry.pingMs << "ms";
        }

        row << (entry.alive ? "  Alive" : "  Down");
        return row.str();
    }

    static DiagnosticsViewState diagnosticsView(const PresentationStateInputs& inputs) {
        DiagnosticsViewState view;
        view.hasLocalNetworkControls = inputs.hasLocalNetworkControls;
        view.hasHostDiagnostics = inputs.hasHostDiagnostics;
        view.localNetworkPanelVisible = inputs.localNetworkPanelVisible;
        if (inputs.diagnosticsModel == nullptr) {
            return view;
        }

        const auto& requested = inputs.diagnosticsModel->localNetworkSettings();
        const auto& authoritative = inputs.diagnosticsModel->authoritativeLocalNetworkSettings();
        view.requestedLatencyMs = requested.latencyMs;
        view.requestedLossPct = requested.lossPct;
        view.hasAuthoritativeLocalNetworkSettings =
            inputs.diagnosticsModel->hasAuthoritativeLocalNetworkSettings();
        view.authoritativeLatencyMs = authoritative.latencyMs;
        view.authoritativeLossPct = authoritative.lossPct;
        view.summaryLines = inputs.diagnosticsModel->summaryLines();
        view.localNetworkSummaryLines = inputs.diagnosticsModel->localNetworkSummaryLines();
        const net::DiagnosticsStudyViewState study = inputs.diagnosticsModel->studyView();
        view.authority.available = study.authority.available;
        view.authority.cadence = study.authority.cadence;
        view.authority.authoritativeTime = study.authority.authoritativeTime;
        view.authority.ackedInputSeq = study.authority.ackedInputSeq;
        view.authority.shotRuleLabel = study.authority.activeRuleLabel;
        view.authority.shotRuleExplanation = study.authority.activeRuleExplanation;
        view.prediction.available = study.prediction.available;
        view.prediction.reconciliationStrategyLabel =
            study.prediction.reconciliationStrategyLabel;
        view.prediction.correctionModeLabel = study.prediction.correctionModeLabel;
        view.prediction.correctionMagnitude = study.prediction.correctionMagnitude;
        view.prediction.replayedCommandCount = study.prediction.replayedCommandCount;
        view.prediction.pendingInputCount = study.prediction.pendingInputCount;
        view.prediction.smoothWindowMs = study.prediction.smoothWindowMs;
        view.pane.available = study.pane.available;
        view.pane.slot = study.pane.slot;
        view.pane.focused = study.pane.focused;
        view.pane.mode = study.pane.mode;
        view.pane.boundActorId = study.pane.boundActorId;
        view.pane.slotLabel = study.pane.slotLabel;
        view.pane.focusLabel = study.pane.focusLabel;
        view.pane.modeLabel = study.pane.modeLabel;
        view.pane.bindingLabel = study.pane.bindingLabel;
        view.pane.paneLabel = study.pane.paneLabel;
        view.spectator.available = study.spectator.available;
        view.spectator.sessionSpectator = study.spectator.sessionSpectator;
        view.spectator.paneObservation = study.spectator.paneObservation;
        view.spectator.canReturnToCharacter = study.spectator.canReturnToCharacter;
        view.spectator.participantTeam = study.spectator.participantTeam;
        view.spectator.followTargetActorId = study.spectator.followTargetActorId;
        view.spectator.participantTeamLabel = study.spectator.participantTeamLabel;
        view.spectator.followTargetLabel = study.spectator.followTargetLabel;
        view.shotStudy.available = study.shot.available;
        view.shotStudy.viewedTimeUs = study.shot.viewedTimeUs;
        view.shotStudy.rewindTimeUs = study.shot.rewindTimeUs;
        view.shotStudy.evaluatedStateLabel = study.shot.evaluatedStateLabel;
        view.shotStudy.activeRuleLabel = study.shot.activeRuleLabel;
        view.shotStudy.activeRuleExplanation = study.shot.activeRuleExplanation;
        view.shotStudy.authoritativeHit = study.shot.authoritativeHit;
        view.shotStudy.outcomeLabel = study.shot.outcomeLabel;
        return view;
    }
};

}  // namespace client
