#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <raylib.h>

#include "LaserBeam3D.hpp"
#include "client/ClientViewState.hpp"

namespace client {

struct ArenaRenderLayer {
    bool visible{true};
    float dimFactor{0.0f};
};

struct WaitingOverlayFrame {
    bool visible{false};
    std::string title{"Netcode Simulator Network Client"};
    std::string stateLine{};
    std::string statusMessage{};
    std::string hostedSessionLine{};
    std::string waitingText{};
    std::string joinHint{};
};

struct HudOverlayFrame {
    std::vector<std::string> lines{};
};

struct CompactScoreOverlayFrame {
    bool visible{false};
    CompactScoreView score{};
};

struct KillFeedOverlayFrame {
    bool visible{false};
    std::vector<KillFeedEntryView> entries{};
};

struct RemotePlayerRenderItem {
    int actorId{0};
    Vector3 eyePosition{};
    Vector3 rootPosition{};
    float yawRadians{0.0f};
    float pitchRadians{0.0f};
    float healthPercent{0.0f};
    bool alive{true};
    sim::TeamId team{sim::TeamId::None};
    Color tint{180, 180, 180, 255};
    bool ghost{false};
};

struct RemoteEnemyRenderItem {
    int entityId{0};
    Vector3 displayPosition{};
    float yawRadians{0.0f};
    float healthPercent{0.0f};
    bool alive{true};
    Color tint{255, 39, 104, 255};
};

struct ScoreboardOverlayFrame {
    bool visible{false};
    std::uint16_t attackerScore{0u};
    std::uint16_t defenderScore{0u};
    std::vector<ScoreboardSectionView> sections{};
};

struct TeamMenuOverlayFrame {
    bool visible{false};
    std::string currentTeamLabel{};
    std::string selectedTeamLabel{};
};

struct RenderWorldFrame {
    bool hasSnapshot{false};
    ArenaRenderLayer arena{};
    Camera3D camera{};
    std::vector<LaserBeam3D> combatTraces{};
    std::vector<RemoteEnemyRenderItem> remoteEnemies{};
    std::vector<RemotePlayerRenderItem> remotePlayers{};
    std::vector<RemotePlayerRenderItem> remotePlayerGhosts{};
};

struct SharedOverlayRenderFrame {
    WaitingOverlayFrame waiting{};
    HudOverlayFrame hud{};
    CompactScoreOverlayFrame compactScore{};
    KillFeedOverlayFrame killFeed{};
    ScoreboardOverlayFrame scoreboard{};
    TeamMenuOverlayFrame teamMenu{};
    ReplayStatusView replay{};
    bool localNetworkPanelVisible{false};
    bool diagnosticsPanelVisible{false};
};

struct RenderFrame {
    bool hasSnapshot{false};
    ArenaRenderLayer arena{};
    Camera3D camera{};
    std::vector<LaserBeam3D> combatTraces{};
    std::vector<RemoteEnemyRenderItem> remoteEnemies{};
    std::vector<RemotePlayerRenderItem> remotePlayers{};
    std::vector<RemotePlayerRenderItem> remotePlayerGhosts{};
    WaitingOverlayFrame waiting{};
    HudOverlayFrame hud{};
    CompactScoreOverlayFrame compactScore{};
    KillFeedOverlayFrame killFeed{};
    ScoreboardOverlayFrame scoreboard{};
    TeamMenuOverlayFrame teamMenu{};
    ReplayStatusView replay{};
    bool localNetworkPanelVisible{false};
    bool diagnosticsPanelVisible{false};

    RenderWorldFrame paneWorldFrame() const {
        RenderWorldFrame world;
        world.hasSnapshot = hasSnapshot;
        world.arena = arena;
        world.camera = camera;
        world.combatTraces = combatTraces;
        world.remoteEnemies = remoteEnemies;
        world.remotePlayers = remotePlayers;
        world.remotePlayerGhosts = remotePlayerGhosts;
        return world;
    }

    SharedOverlayRenderFrame sharedOverlayFrame() const {
        SharedOverlayRenderFrame overlays;
        overlays.waiting = waiting;
        overlays.hud = hud;
        overlays.compactScore = compactScore;
        overlays.killFeed = killFeed;
        overlays.scoreboard = scoreboard;
        overlays.teamMenu = teamMenu;
        overlays.replay = replay;
        overlays.localNetworkPanelVisible = localNetworkPanelVisible;
        overlays.diagnosticsPanelVisible = diagnosticsPanelVisible;
        return overlays;
    }
};

}  // namespace client
