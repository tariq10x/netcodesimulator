#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "Arena3D.hpp"
#include "client/PaneViewState.hpp"
#include "client/PresentationStateSubsystem.hpp"
#include "client/RenderFrame.hpp"
#include "client/SplitScreenRenderFrame.hpp"
#include "client/SplitScreenViewState.hpp"

namespace client {

struct ClientPresentationInputs {
    const ClientViewState& viewState;
    const Arena3D* arena{nullptr};
    const std::vector<LaserBeam3D>* combatTraces{nullptr};
};

class ClientPresentation {
public:
    RenderFrame build(const ClientPresentationInputs& inputs) const {
        RenderFrame frame;
        frame.hasSnapshot = inputs.viewState.connection.hasSnapshot;
        frame.arena.dimFactor = inputs.viewState.studyPresentation.environmentDimFactor;
        frame.waiting = buildWaitingOverlay(inputs.viewState);
        frame.hud.lines = buildHudLines(inputs.viewState);
        frame.compactScore.visible = inputs.viewState.compactScore.available;
        frame.compactScore.score = inputs.viewState.compactScore;
        frame.killFeed.visible = !inputs.viewState.killFeed.empty();
        frame.killFeed.entries = inputs.viewState.killFeed;
        frame.scoreboard.visible = inputs.viewState.scoreboardVisible;
        frame.scoreboard.attackerScore = inputs.viewState.attackerScore;
        frame.scoreboard.defenderScore = inputs.viewState.defenderScore;
        frame.scoreboard.sections = inputs.viewState.scoreboardSections;
        frame.teamMenu.visible = inputs.viewState.teamMenu.visible;
        frame.replay = inputs.viewState.replay;
        frame.teamMenu.currentTeamLabel =
            std::string(sim::toString(inputs.viewState.teamMenu.currentTeam));
        frame.teamMenu.selectedTeamLabel =
            std::string(sim::toString(inputs.viewState.teamMenu.selectedTeam));
        frame.localNetworkPanelVisible =
            inputs.viewState.diagnostics.localNetworkPanelVisible;
        frame.diagnosticsPanelVisible =
            inputs.viewState.uiModeActive &&
            inputs.viewState.diagnostics.hasHostDiagnostics &&
            !inputs.viewState.diagnostics.localNetworkPanelVisible;

        if (!frame.hasSnapshot || !inputs.viewState.camera.available || inputs.arena == nullptr) {
            return frame;
        }

        frame.camera = buildCamera(inputs.viewState.camera);
        if (inputs.combatTraces != nullptr) {
            frame.combatTraces = *inputs.combatTraces;
        }
        frame.remoteEnemies = buildRemoteEnemies(inputs.viewState);
        frame.remotePlayers = buildRemotePlayers(inputs.viewState, *inputs.arena);
        frame.remotePlayerGhosts =
            buildRemotePlayerGhosts(inputs.viewState, *inputs.arena, frame.remotePlayers);
        return frame;
    }

    static std::string paneOverlayLabel(const ClientViewState& viewState) {
        std::string summary = PresentationStateSubsystem::paneStatusSummary(viewState);
        if (!summary.empty()) {
            if (viewState.pane.state.mode == sim::PaneViewMode::ReplayCamera &&
                viewState.replay.paneBinding.available &&
                !viewState.replay.paneBinding.bindingLabel.empty() &&
                summary.find(viewState.replay.paneBinding.bindingLabel) == std::string::npos) {
                summary += " | " + viewState.replay.paneBinding.bindingLabel;
            }
            return summary;
        }

        std::string label = viewState.pane.slotLabel;
        if (!viewState.pane.focusLabel.empty()) {
            label += label.empty() ? viewState.pane.focusLabel : " | " + viewState.pane.focusLabel;
        }
        if (!viewState.pane.modeLabel.empty()) {
            label += label.empty() ? viewState.pane.modeLabel : " | " + viewState.pane.modeLabel;
        }
        if (!viewState.pane.participantTeamLabel.empty()) {
            label += label.empty() ? "Team " + viewState.pane.participantTeamLabel
                                   : " | Team " + viewState.pane.participantTeamLabel;
        }
        return label;
    }

    SplitScreenRenderFrame buildSplitScreen(const SplitScreenViewState& viewState,
                                            const Arena3D& arena,
                                            const std::vector<LaserBeam3D>* combatTraces = nullptr) const {
        const RenderFrame leftFrame = build(ClientPresentationInputs{
            viewState.left.viewState,
            &arena,
            combatTraces
        });
        const RenderFrame rightFrame = build(ClientPresentationInputs{
            viewState.right.viewState,
            &arena,
            combatTraces
        });

        PaneRenderFrame leftPane;
        leftPane.slot = viewState.left.slot;
        leftPane.focused = viewState.focusedSlot == sim::PaneSlot::Left;
        leftPane.binding = viewState.left.binding;
        leftPane.world = leftFrame.paneWorldFrame();

        PaneRenderFrame rightPane;
        rightPane.slot = viewState.right.slot;
        rightPane.focused = viewState.focusedSlot == sim::PaneSlot::Right;
        rightPane.binding = viewState.right.binding;
        rightPane.world = rightFrame.paneWorldFrame();

        return client::buildSplitScreenRenderFrame(leftPane,
                                                   rightPane,
                                                   buildSharedOverlays(viewState, leftFrame, rightFrame),
                                                   viewState.focusedSlot);
    }

private:
    static Vector3 toVector3(const sim::Vec3& value) {
        return Vector3{value.x, value.y, value.z};
    }

    static std::vector<std::string> buildHudLines(const ClientViewState& viewState) {
        return PresentationStateSubsystem::compactHudLines(viewState);
    }

    static SharedOverlayRenderFrame buildSharedOverlays(const SplitScreenViewState& splitView,
                                                        const RenderFrame& leftFrame,
                                                        const RenderFrame& rightFrame) {
        const RenderFrame& sourceFrame =
            splitView.focusedSlot == sim::PaneSlot::Right ? rightFrame : leftFrame;

        SharedOverlayRenderFrame overlays = sourceFrame.sharedOverlayFrame();
        overlays.waiting.hostedSessionLine = splitView.sharedOverlays.hostedSessionLine;
        overlays.hud.lines = splitView.sharedOverlays.hudLines;
        overlays.compactScore.score = splitView.sharedOverlays.compactScore;
        overlays.compactScore.visible = splitView.sharedOverlays.compactScore.available;
        overlays.killFeed.entries = splitView.sharedOverlays.killFeed;
        overlays.killFeed.visible = !splitView.sharedOverlays.killFeed.empty();
        overlays.scoreboard.visible = splitView.sharedOverlays.scoreboardVisible;
        overlays.scoreboard.sections = splitView.sharedOverlays.scoreboardSections;
        overlays.replay = splitView.sharedOverlays.replay;
        overlays.localNetworkPanelVisible = splitView.sharedOverlays.localNetworkPanelVisible;
        overlays.diagnosticsPanelVisible = splitView.sharedOverlays.diagnosticsPanelVisible;
        return overlays;
    }

    static Camera3D buildCamera(const CameraViewState& cameraView) {
        const Vector3 eyePosition = toVector3(cameraView.eyePosition);
        const Vector3 lookDirection = toVector3(cameraView.lookDirection);

        Camera3D camera{};
        camera.position = eyePosition;
        camera.target = Vector3{eyePosition.x + lookDirection.x,
                                eyePosition.y + lookDirection.y,
                                eyePosition.z + lookDirection.z};
        camera.up = Vector3{0.0f, 1.0f, 0.0f};
        camera.fovy = cameraView.fovY;
        camera.projection = CAMERA_PERSPECTIVE;
        return camera;
    }

    static Color teamTint(sim::TeamId team) {
        switch (team) {
            case sim::TeamId::Attacker:
                return Color{255, 119, 72, 255};
            case sim::TeamId::Defender:
                return Color{72, 156, 255, 255};
            case sim::TeamId::Spectator:
                return Color{180, 180, 180, 255};
            case sim::TeamId::None:
                return Color{180, 180, 180, 255};
        }
        return Color{180, 180, 180, 255};
    }

    static WaitingOverlayFrame buildWaitingOverlay(const ClientViewState& viewState) {
        WaitingOverlayFrame waiting;
        waiting.visible = !viewState.connection.hasSnapshot;
        waiting.stateLine = "State: " + viewState.connection.stateLabel;
        waiting.statusMessage = viewState.connection.statusMessage;
        waiting.hostedSessionLine =
            PresentationStateSubsystem::hostedSessionSummary(viewState);
        waiting.waitingText = viewState.connection.connected
            ? "Connected. Waiting for authoritative snapshot..."
            : "Waiting for handshake...";
        waiting.joinHint =
            "Direct join flow is available via: netcodesim --join <host> <port> [player-name]";
        return waiting;
    }

    static std::vector<RemoteEnemyRenderItem> buildRemoteEnemies(
        const ClientViewState& viewState) {
        std::vector<RemoteEnemyRenderItem> remoteEnemies;
        remoteEnemies.reserve(viewState.remoteEnemies.size());
        for (const auto& enemyView : viewState.remoteEnemies) {
            RemoteEnemyRenderItem item;
            item.entityId = enemyView.entityId;
            item.displayPosition = toVector3(enemyView.position);
            item.yawRadians = enemyView.yaw;
            item.healthPercent = enemyView.healthPercent;
            item.alive = enemyView.alive;
            item.tint = enemyView.alive
                ? Color{255, 39, 104, 255}
                : Color{120, 120, 120, 180};
            remoteEnemies.push_back(item);
        }
        return remoteEnemies;
    }

    static std::vector<RemotePlayerRenderItem> buildRemotePlayers(
        const ClientViewState& viewState,
        const Arena3D& arena) {
        return buildRemotePlayers(viewState.remotePlayers, arena);
    }

    static std::vector<RemotePlayerRenderItem> buildRemotePlayerGhosts(
        const ClientViewState& viewState,
        const Arena3D& arena) {
        return buildRemotePlayerGhosts(viewState, arena, buildRemotePlayers(viewState, arena));
    }

    static std::vector<RemotePlayerRenderItem> buildRemotePlayerGhosts(
        const ClientViewState& viewState,
        const Arena3D& arena,
        const std::vector<RemotePlayerRenderItem>& solidPlayers) {
        if (!viewState.hostedSession.ghostTracksVisible) {
            return {};
        }

        std::vector<RemotePlayerRenderItem> ghosts =
            buildRemotePlayers(viewState.remotePlayerGhosts, arena);
        ghosts.erase(std::remove_if(ghosts.begin(),
                                    ghosts.end(),
                                    [&solidPlayers](const RemotePlayerRenderItem& ghost) {
                                        return shouldSuppressOverlappedGhost(ghost, solidPlayers);
                                    }),
                     ghosts.end());
        return ghosts;
    }

    static std::vector<RemotePlayerRenderItem> buildRemotePlayers(
        const std::vector<RemotePlayerView>& playerViews,
        const Arena3D& arena) {
        std::vector<RemotePlayerRenderItem> remotePlayers;
        remotePlayers.reserve(playerViews.size());
        for (const auto& playerView : playerViews) {
            RemotePlayerRenderItem item;
            item.actorId = playerView.actorId;
            item.eyePosition = toVector3(playerView.eyePosition);
            item.rootPosition =
                arena.playerRenderRootFromEyePosition(playerView.eyePosition);
            item.yawRadians = playerView.yaw;
            item.pitchRadians = playerView.pitch;
            item.healthPercent = playerView.healthPercent;
            item.alive = playerView.alive;
            item.team = playerView.team;
            item.tint = teamTint(playerView.team);
            if (playerView.ghost) {
                item.tint.a = static_cast<unsigned char>(item.tint.a * 0.35f);
            }
            item.ghost = playerView.ghost;
            remotePlayers.push_back(item);
        }
        return remotePlayers;
    }

    static bool shouldSuppressOverlappedGhost(
        const RemotePlayerRenderItem& ghost,
        const std::vector<RemotePlayerRenderItem>& solidPlayers) {
        constexpr float kMinimumVisibleGhostOffsetMeters = 0.18f;
        const auto solidIt = std::find_if(
            solidPlayers.begin(),
            solidPlayers.end(),
            [&ghost](const RemotePlayerRenderItem& solid) {
                return solid.actorId == ghost.actorId;
            });
        if (solidIt == solidPlayers.end()) {
            return false;
        }

        const float dx = ghost.eyePosition.x - solidIt->eyePosition.x;
        const float dy = ghost.eyePosition.y - solidIt->eyePosition.y;
        const float dz = ghost.eyePosition.z - solidIt->eyePosition.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        return distanceSquared <
               kMinimumVisibleGhostOffsetMeters * kMinimumVisibleGhostOffsetMeters;
    }
};

}  // namespace client
