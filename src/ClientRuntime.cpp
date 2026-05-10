#include "net/ClientRuntime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <raylib.h>

#include "Arena3D.hpp"
#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "Enemy3D.hpp"
#include "LevelData.hpp"
#include "Player3D.hpp"
#include "TypographyService.hpp"
#include "app/ReplayArchive.hpp"
#include "app/UserDataPaths.hpp"
#include "client/HudOverlayRenderer.hpp"
#include "client/KeyboardHelpOverlay.hpp"
#include "client/ReplayTransportControls.hpp"
#include "net/DiagnosticsModel.hpp"
#include "net/SessionDiscovery.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "sim/SimulationRules.hpp"

namespace net {
namespace {

constexpr Color kBackgroundColor{15, 18, 24, 255};
constexpr Color kStatusColor{200, 210, 225, 255};
constexpr Color kRemoteEnemyColor{255, 39, 104, 255};
constexpr Color kAttackerColor{255, 119, 72, 255};
constexpr Color kDefenderColor{72, 156, 255, 255};
constexpr Color kNeutralTeamColor{180, 180, 180, 255};
constexpr std::uint64_t kMaxClientFrameAdvanceUs = 100'000u;
constexpr std::uint64_t kCombatTraceLifetimeUs = 250'000u;
constexpr std::uint64_t kKillFeedLifetimeUs = 4'000'000u;
constexpr float kCombatBeamLifetimeSeconds =
    static_cast<float>(kCombatTraceLifetimeUs) / 1'000'000.0f;
constexpr float kCombatBeamThickness = 0.05f;
constexpr float kLocalRespawnDelaySeconds = 5.0f;
constexpr std::uint16_t kAuthoritativeHostPeerId = 1u;
constexpr float kDetachedObserverSpeedMultiplier = 2.5f;
constexpr float kDetachedObserverFastMultiplier = 3.0f;
constexpr float kSpectatorCheckpointTransitionAdjustmentSeconds = 0.25f;
constexpr float kRecordingCheckpointTransitionSeconds = 1.0f;
constexpr float kStudyEnvironmentDimFactor = 1.0f;
constexpr float kStudyFovRangeMeters = 16.0f;
constexpr float kStudyFovVerticalDegrees = 70.0f;
constexpr std::size_t kMaxKillFeedEntries = 4u;
constexpr std::string_view kEventLoggingAppliedPrefix = "applied:";

Color combatTraceColor(bool authoritative, bool hit) {
    if (!authoritative) {
        return Color{255, 45, 95, 255};
    }
    return hit ? Color{80, 255, 120, 255}
               : Color{255, 110, 110, 255};
}

Color teamTint(sim::TeamId team) {
    switch (team) {
        case sim::TeamId::Attacker:
            return kAttackerColor;
        case sim::TeamId::Defender:
            return kDefenderColor;
        case sim::TeamId::Spectator:
            return kNeutralTeamColor;
        case sim::TeamId::None:
            return kNeutralTeamColor;
    }
    return kNeutralTeamColor;
}

Vector3 toVector3(const sim::Vec3& value) {
    return Vector3{value.x, value.y, value.z};
}

Vector3 add(Vector3 lhs, Vector3 rhs) {
    return Vector3{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vector3 subtract(Vector3 lhs, Vector3 rhs) {
    return Vector3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vector3 scale(Vector3 value, float scalar) {
    return Vector3{value.x * scalar, value.y * scalar, value.z * scalar};
}

float lengthSquared(Vector3 value) {
    return (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
}

Vector3 normalize(Vector3 value) {
    const float magnitudeSquared = lengthSquared(value);
    if (magnitudeSquared <= 0.000001f) {
        return Vector3{};
    }
    return scale(value, 1.0f / std::sqrt(magnitudeSquared));
}

Vector3 cross(Vector3 lhs, Vector3 rhs) {
    return Vector3{
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x
    };
}

void drawFovCone(const client::RemotePlayerRenderItem& player) {
    if (!player.alive || player.ghost) {
        return;
    }

    constexpr float kFovRangeMeters = kStudyFovRangeMeters;
    constexpr float kVerticalFovRadians = kStudyFovVerticalDegrees * DEG2RAD;
    const float aspectRatio =
        static_cast<float>(Config::SCREEN_WIDTH) / static_cast<float>(Config::SCREEN_HEIGHT);
    const float horizontalFovRadians =
        2.0f * std::atan(std::tan(kVerticalFovRadians * 0.5f) * aspectRatio);

    const Vector3 forward = normalize(
        toVector3(sim::lookDirection(player.yawRadians, player.pitchRadians)));
    const Vector3 right = normalize(toVector3(sim::rightFromYaw(player.yawRadians)));
    const Vector3 up = normalize(cross(right, forward));
    if (lengthSquared(forward) <= 0.000001f ||
        lengthSquared(right) <= 0.000001f ||
        lengthSquared(up) <= 0.000001f) {
        return;
    }

    const Vector3 eye = player.eyePosition;
    const Vector3 farCenter = add(eye, scale(forward, kFovRangeMeters));
    const float halfHeight = kFovRangeMeters * std::tan(kVerticalFovRadians * 0.5f);
    const float halfWidth = kFovRangeMeters * std::tan(horizontalFovRadians * 0.5f);

    const Vector3 farTopLeft = add(add(farCenter, scale(up, halfHeight)), scale(right, -halfWidth));
    const Vector3 farTopRight = add(add(farCenter, scale(up, halfHeight)), scale(right, halfWidth));
    const Vector3 farBottomRight = add(add(farCenter, scale(up, -halfHeight)), scale(right, halfWidth));
    const Vector3 farBottomLeft = add(add(farCenter, scale(up, -halfHeight)), scale(right, -halfWidth));

    const Color edge = Fade(player.tint, 0.52f);
    const Color guide = Fade(player.tint, 0.28f);
    DrawLine3D(eye, farTopLeft, edge);
    DrawLine3D(eye, farTopRight, edge);
    DrawLine3D(eye, farBottomRight, edge);
    DrawLine3D(eye, farBottomLeft, edge);
    DrawLine3D(farTopLeft, farTopRight, edge);
    DrawLine3D(farTopRight, farBottomRight, edge);
    DrawLine3D(farBottomRight, farBottomLeft, edge);
    DrawLine3D(farBottomLeft, farTopLeft, edge);
    DrawLine3D(eye, farCenter, guide);
    DrawLine3D(subtract(farCenter, scale(right, halfWidth)), add(farCenter, scale(right, halfWidth)), guide);
    DrawLine3D(subtract(farCenter, scale(up, halfHeight)), add(farCenter, scale(up, halfHeight)), guide);
}

const char* teamLabel(sim::TeamId team) {
    return sim::toString(team);
}

float studyFovHorizontalDegrees(float aspectRatio) {
    const float verticalRadians = kStudyFovVerticalDegrees * DEG2RAD;
    const float horizontalRadians =
        2.0f * std::atan(std::tan(verticalRadians * 0.5f) * aspectRatio);
    return horizontalRadians * RAD2DEG;
}

Arena3D::AreaFilter arenaAreaFilterFor(client::AreaFilterView filter) {
    switch (filter) {
        case client::AreaFilterView::All:
            return Arena3D::AreaFilter::ALL;
        case client::AreaFilterView::RedOnly:
            return Arena3D::AreaFilter::RED_ONLY;
        case client::AreaFilterView::GreenOnly:
            return Arena3D::AreaFilter::GREEN_ONLY;
    }
    return Arena3D::AreaFilter::ALL;
}

std::string sessionActionStatusText(const SessionActionResult& result) {
    if (result.kind != SessionActionKind::SpawnFrozenBotAhead) {
        return "Unsupported study action";
    }
    if (result.applied) {
        return result.actorId >= 0
            ? "Spawned frozen bot " + std::to_string(result.actorId) + " ahead"
            : "Spawned frozen bot ahead";
    }
    if (result.message == "host_only") {
        return "Only the host can spawn frozen study bots";
    }
    if (result.message == "study_only") {
        return "Frozen bot spawning is only available in Lab Study";
    }
    if (result.message == "invalid_spawn_point") {
        return "No valid spawn point in front of the player";
    }
    if (result.message == "requester_unavailable") {
        return "A live player is required to spawn a frozen study bot";
    }
    return "Frozen bot spawn failed";
}

sim::RemoteActorState remoteActorFromPlayerState(const sim::PlayerState& playerState,
                                                 const sim::SimConfig& simConfig) {
    sim::RemoteActorState actor;
    actor.entityId = playerState.playerId;
    actor.position = playerState.position;
    actor.velocity = playerState.velocity;
    actor.yaw = playerState.yaw;
    actor.pitch = playerState.pitch;
    actor.health = playerState.health;
    actor.radius = simConfig.playerRadius;
    actor.alive = playerState.health > 0.0f;
    return actor;
}

std::vector<sim::RemoteActorState> remoteActorsFromPlayerStates(
    const std::vector<sim::PlayerState>& players,
    const sim::SimConfig& simConfig) {
    std::vector<sim::RemoteActorState> actors;
    actors.reserve(players.size());
    for (const auto& player : players) {
        actors.push_back(remoteActorFromPlayerState(player, simConfig));
    }
    return actors;
}

sim::PlayerState playerStateFromInterpolatedActor(
    const sim::RemoteActorState& actor,
    const std::vector<sim::PlayerState>& authoritativePlayers,
    const sim::SimConfig& simConfig) {
    sim::PlayerState playerState;
    playerState.playerId = actor.entityId;
    playerState.position = actor.position;
    playerState.velocity = actor.velocity;
    playerState.yaw = actor.yaw;
    playerState.pitch = actor.pitch;
    playerState.health = actor.health;
    playerState.maxHealth = simConfig.playerMaxHealth;
    playerState.grounded = actor.position.y <= simConfig.playerEyeHeight + 0.001f &&
                           std::abs(actor.velocity.y) <= 0.01f;

    const auto authoritativeIt = std::find_if(
        authoritativePlayers.begin(),
        authoritativePlayers.end(),
        [playerId = actor.entityId](const sim::PlayerState& player) {
            return player.playerId == playerId;
        });
    if (authoritativeIt != authoritativePlayers.end()) {
        playerState.maxHealth = authoritativeIt->maxHealth;
        playerState.weaponCooldownRemaining = authoritativeIt->weaponCooldownRemaining;
        playerState.jumpsUsed = authoritativeIt->jumpsUsed;
        playerState.grounded = authoritativeIt->grounded;
    }

    return playerState;
}

std::string respawnCountdownText(float remainingSeconds) {
    std::ostringstream stream;
    stream << "You are down | Respawn in "
           << std::fixed
           << std::setprecision(1)
           << std::max(0.0f, remainingSeconds)
           << "s";
    return stream.str();
}

std::string formatSeconds(float seconds) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << seconds << "s";
    return stream.str();
}

std::string respawnOverlayText(float remainingSeconds) {
    std::ostringstream stream;
    stream << "Respawn in "
           << std::fixed
           << std::setprecision(1)
           << std::max(0.0f, remainingSeconds)
           << "s";
    return stream.str();
}

TypographyStyleId runtimeTextStyleForFontSize(int fontSize) {
    if (fontSize >= 34) {
        return TypographyStyleId::ScoreboardSummary;
    }
    if (fontSize >= 30) {
        return TypographyStyleId::ScoreboardTitle;
    }
    if (fontSize >= 28) {
        return TypographyStyleId::OverlayTitle;
    }
    if (fontSize >= 24) {
        return TypographyStyleId::AppSubtitle;
    }
    if (fontSize >= 22) {
        return TypographyStyleId::OverlayAccent;
    }
    if (fontSize >= 20) {
        return TypographyStyleId::Body;
    }
    return TypographyStyleId::Caption;
}

void drawRuntimeText(std::string_view text,
                     float x,
                     float y,
                     int fontSize,
                     Color color) {
    TypographyService::shared().draw(runtimeTextStyleForFontSize(fontSize), text, Vector2{x, y}, color);
}

int measureRuntimeTextWidth(std::string_view text, int fontSize) {
    return TypographyService::shared().measureWidth(runtimeTextStyleForFontSize(fontSize), text);
}

void drawCenteredOverlayText(const std::string& text,
                             int centerX,
                             int y,
                             TypographyStyleId styleId,
                             Color color) {
    TypographyService::shared().drawCentered(styleId, text, static_cast<float>(centerX), static_cast<float>(y), color);
}

void drawRuntimeShadowedText(std::string_view text,
                             float x,
                             float y,
                             int fontSize,
                             Color color,
                             Color shadow = Fade(BLACK, 0.55f)) {
    drawRuntimeText(text, x + 2.0f, y + 2.0f, fontSize, shadow);
    drawRuntimeText(text, x, y, fontSize, color);
}

void drawCenteredRuntimeShadowedText(std::string_view text,
                                     float centerX,
                                     float y,
                                     int fontSize,
                                     Color color,
                                     Color shadow = Fade(BLACK, 0.55f)) {
    const int textWidth = measureRuntimeTextWidth(text, fontSize);
    drawRuntimeShadowedText(text,
                            centerX - (static_cast<float>(textWidth) * 0.5f),
                            y,
                            fontSize,
                            color,
                            shadow);
}

void drawReplayOverlay(const client::ReplayStatusView& replay) {
    const bool hasCapture =
        replay.recordingActive ||
        replay.playbackActive ||
        replay.statusLine.find("Replay ready") != std::string::npos ||
        replay.statusLine.find("Command replay ready") != std::string::npos;
    const bool playbackPlaying =
        replay.playbackActive &&
        replay.statusLine.find("(playing)") != std::string::npos;

    std::string primaryLine = replay.statusLine.empty() ? "Replay idle" : replay.statusLine;
    std::string secondaryLine;
    if (const std::size_t divider = primaryLine.find(" | "); divider != std::string::npos) {
        secondaryLine = primaryLine.substr(divider + 3u);
        primaryLine = primaryLine.substr(0u, divider);
    }

    client::ReplayTransportOverlayState overlay;
    overlay.primaryLine = primaryLine;
    overlay.secondaryLine = secondaryLine;
    overlay.checkpoint = client::ReplayCheckpointOverlayState{
        true,
        replay.checkpoint.detachedCameraActive,
        replay.checkpoint.activeIndex,
        replay.checkpoint.checkpointCount,
        replay.checkpoint.transitionToNextSeconds,
        replay.checkpoint.transitionEditable
    };

    const auto buttons = client::makeReplayRecordingTransportButtons(
        replay.recordingActive,
        replay.playbackActive,
        playbackPlaying,
        hasCapture,
        true);
    client::renderReplayTransportOverlay(overlay, buttons);
}

bool isObservationPaneMode(sim::PaneViewMode mode) {
    return mode == sim::PaneViewMode::SpectatorFreeFly ||
           mode == sim::PaneViewMode::SpectatorFollowFirstPerson ||
           mode == sim::PaneViewMode::SpectatorFollowThirdPerson ||
           mode == sim::PaneViewMode::ReplayCamera;
}

int nextEligibleActorId(const std::vector<sim::EligibleActor>& actors, int currentActorId) {
    if (actors.empty()) {
        return -1;
    }

    for (std::size_t index = 0; index < actors.size(); ++index) {
        if (actors[index].actorId == currentActorId) {
            return actors[(index + 1u) % actors.size()].actorId;
        }
    }

    return actors.front().actorId;
}

std::string actorDisplayLabel(const std::vector<sim::RosterEntry>& roster, int actorId) {
    const auto it = std::find_if(roster.begin(),
                                 roster.end(),
                                 [actorId](const sim::RosterEntry& entry) {
                                     return entry.actorId == actorId;
                                 });
    if (it == roster.end()) {
        return "actor " + std::to_string(actorId);
    }
    if (!it->displayName.empty()) {
        return it->displayName;
    }
    return it->isBot ? "BOT " + std::to_string(actorId)
                     : "player " + std::to_string(actorId);
}

const sim::RosterEntry* findRosterEntry(const std::vector<sim::RosterEntry>& roster, int actorId) {
    const auto it = std::find_if(roster.begin(),
                                 roster.end(),
                                 [actorId](const sim::RosterEntry& entry) {
                                     return entry.actorId == actorId;
                                 });
    return it != roster.end() ? &(*it) : nullptr;
}

std::string participantRuntimeLabel(const sim::RosterEntry& entry,
                                    bool isLocalParticipant,
                                    bool isHostParticipant) {
    std::string label;
    if (!entry.displayName.empty()) {
        label = entry.displayName;
    } else {
        label = entry.isBot
            ? "BOT " + std::to_string(entry.actorId)
            : "Player " + std::to_string(entry.actorId);
    }
    if (entry.isBot) {
        label += " [BOT]";
    }
    if (isLocalParticipant) {
        label += " [YOU]";
    }
    if (isHostParticipant) {
        label += " [HOST]";
    }
    return label;
}

std::string participantRuntimeDetail(const sim::RosterEntry& entry,
                                     bool selected,
                                     bool editable) {
    std::ostringstream line;
    line << sim::toString(entry.team)
         << " | "
         << (entry.alive ? "Alive" : "Down");
    if (selected) {
        line << " | Selected";
    }
    line << " | " << (editable ? "Editable" : "Read Only");
    return line.str();
}

std::string participantRuntimeMetrics(const sim::RosterEntry& entry) {
    std::ostringstream line;
    line << "Latency " << entry.latencyMs << "ms"
         << " | Loss " << static_cast<int>(entry.lossPct) << "%";
    if (!entry.isBot) {
        line << " | Ping " << static_cast<std::uint32_t>(entry.latencyMs) * 2u << "ms";
    }
    return line.str();
}

std::string runtimeSettingsHostLabel(const std::vector<sim::RosterEntry>& roster,
                                     const HostedSessionMetadata& metadata,
                                     std::uint16_t hostPeerId) {
    if (const sim::RosterEntry* hostEntry = findRosterEntry(roster, static_cast<int>(hostPeerId));
        hostEntry != nullptr) {
        return participantRuntimeLabel(*hostEntry, false, true);
    }
    if (!metadata.hostPlayerName.empty()) {
        return metadata.hostPlayerName + " [HOST]";
    }
    if (hostPeerId != 0u) {
        return "Player " + std::to_string(hostPeerId) + " [HOST]";
    }
    return "Unknown";
}

std::string proxyStatsSummary(const ProxyStats& stats) {
    std::ostringstream line;
    line << "Drop " << stats.droppedPackets
         << " | Fwd " << stats.forwardedPackets
         << " | Queue " << stats.queuedPackets;
    return line.str();
}

std::string runtimeSettingsValueLabel(float value, const char* unit) {
    std::ostringstream line;
    line << static_cast<int>(std::lround(value)) << unit;
    return line.str();
}

RuntimeSettingsOverlay::ChoiceState makeChoiceState(const char* label,
                                                    int value,
                                                    bool selected,
                                                    bool enabled = true) {
    RuntimeSettingsOverlay::ChoiceState choice;
    choice.label = label;
    choice.value = value;
    choice.selected = selected;
    choice.enabled = enabled;
    return choice;
}

client::ReconciliationStrategyView toViewStrategy(ReconciliationStrategy strategy) {
    switch (strategy) {
        case ReconciliationStrategy::Snap:
            return client::ReconciliationStrategyView::Snap;
        case ReconciliationStrategy::Smooth:
            return client::ReconciliationStrategyView::Smooth;
    }
    return client::ReconciliationStrategyView::Smooth;
}

client::CorrectionModeView toViewCorrectionMode(ReconciliationMode mode) {
    switch (mode) {
        case ReconciliationMode::None:
            return client::CorrectionModeView::None;
        case ReconciliationMode::Smooth:
            return client::CorrectionModeView::Smooth;
        case ReconciliationMode::Snap:
            return client::CorrectionModeView::Snap;
    }
    return client::CorrectionModeView::None;
}

sim::RuntimeReconciliationStrategy toRuntimeReconciliationStrategy(
    ReconciliationStrategy strategy) {
    switch (strategy) {
        case ReconciliationStrategy::Snap:
            return sim::RuntimeReconciliationStrategy::Snap;
        case ReconciliationStrategy::Smooth:
            return sim::RuntimeReconciliationStrategy::Smooth;
    }
    return sim::RuntimeReconciliationStrategy::Smooth;
}

ReconciliationStrategy toPredictionReconciliationStrategy(
    sim::RuntimeReconciliationStrategy strategy) {
    switch (strategy) {
        case sim::RuntimeReconciliationStrategy::Snap:
            return ReconciliationStrategy::Snap;
        case sim::RuntimeReconciliationStrategy::Smooth:
            return ReconciliationStrategy::Smooth;
    }
    return ReconciliationStrategy::Smooth;
}

sim::TeamId defaultTeamMenuSelection(sim::TeamId currentTeam) {
    if (currentTeam == sim::TeamId::Attacker) {
        return sim::TeamId::Defender;
    }
    if (currentTeam == sim::TeamId::Defender) {
        return sim::TeamId::Attacker;
    }
    if (currentTeam == sim::TeamId::Spectator) {
        return sim::TeamId::Attacker;
    }
    return sim::TeamId::Attacker;
}

sim::TeamId cycleTeamSelection(sim::TeamId currentTeam, int delta) {
    static constexpr std::array<sim::TeamId, 3> kSelectableTeams{
        sim::TeamId::Attacker,
        sim::TeamId::Defender,
        sim::TeamId::Spectator
    };

    std::size_t currentIndex = 0u;
    for (std::size_t index = 0; index < kSelectableTeams.size(); ++index) {
        if (kSelectableTeams[index] == currentTeam) {
            currentIndex = index;
            break;
        }
    }

    const int rawIndex = static_cast<int>(currentIndex) + delta;
    const int wrappedIndex = (rawIndex % static_cast<int>(kSelectableTeams.size()) +
                             static_cast<int>(kSelectableTeams.size())) %
                             static_cast<int>(kSelectableTeams.size());
    return kSelectableTeams[static_cast<std::size_t>(wrappedIndex)];
}

bool canUseHostScoreboardTarget(const sim::RosterEntry& entry, std::uint16_t localPeerId) {
    return entry.actorId > 0 &&
           entry.actorId <= static_cast<int>(std::numeric_limits<std::uint16_t>::max()) &&
           entry.actorId != static_cast<int>(localPeerId) &&
           entry.sessionPresence == sim::SessionPresence::Connected;
}

int hostScoreboardTeamRowCount(const std::vector<sim::RosterEntry>& roster) {
    int attackers = 0;
    int defenders = 0;
    for (const sim::RosterEntry& entry : roster) {
        if (entry.sessionPresence != sim::SessionPresence::Connected) {
            continue;
        }
        if (entry.team == sim::TeamId::Attacker) {
            ++attackers;
        } else if (entry.team == sim::TeamId::Defender) {
            ++defenders;
        }
    }
    return std::max(attackers, defenders);
}

float hostScoreboardBottomY(int scoreboardRows) {
    const int overlayHeight = std::min(Config::SCREEN_HEIGHT - 120,
                                       std::max(300, 168 + (scoreboardRows * 38)));
    return 72.0f + static_cast<float>(overlayHeight);
}

std::size_t hostScoreboardVisibleTargetRows(std::size_t rowCount, int scoreboardRows) {
    constexpr float kHeaderHeight = 86.0f;
    constexpr float kRowPitch = 60.0f;
    constexpr float kBottomPadding = 18.0f;
    constexpr std::size_t kHardRowLimit = 7u;

    const float panelY = hostScoreboardBottomY(scoreboardRows) + 18.0f;
    const float availableHeight = static_cast<float>(Config::SCREEN_HEIGHT) - panelY - 44.0f;
    if (availableHeight <= kHeaderHeight + kBottomPadding + kRowPitch) {
        return 1u;
    }

    const float availableRows =
        (availableHeight - kHeaderHeight - kBottomPadding) / kRowPitch;
    const std::size_t geometricLimit = static_cast<std::size_t>(
        std::max(1.0f, std::floor(availableRows)));
    return std::min({std::max<std::size_t>(rowCount, 1u), geometricLimit, kHardRowLimit});
}

Rectangle hostScoreboardAdminPanelBounds(std::size_t rowCount, int scoreboardRows) {
    constexpr float kHeaderHeight = 86.0f;
    constexpr float kRowPitch = 60.0f;
    constexpr float kBottomPadding = 18.0f;

    const std::size_t visibleRows = hostScoreboardVisibleTargetRows(rowCount, scoreboardRows);
    const float height = kHeaderHeight + (static_cast<float>(visibleRows) * kRowPitch) +
                         kBottomPadding;
    const float width = static_cast<float>(std::min(Config::SCREEN_WIDTH - 120, 1100));
    const float desiredY = hostScoreboardBottomY(scoreboardRows) + 18.0f;
    const float y = std::min(desiredY,
                             static_cast<float>(Config::SCREEN_HEIGHT) - height - 44.0f);
    return Rectangle{
        (static_cast<float>(Config::SCREEN_WIDTH) - width) * 0.5f,
        y,
        width,
        height
    };
}

Rectangle hostScoreboardAddBotButtonBounds(const Rectangle& panel) {
    return Rectangle{panel.x + panel.width - 126.0f, panel.y + 32.0f, 104.0f, 34.0f};
}

Rectangle hostScoreboardBotPeaceButtonBounds(const Rectangle& panel) {
    return Rectangle{panel.x + panel.width - 242.0f, panel.y + 32.0f, 104.0f, 34.0f};
}

Rectangle hostScoreboardBotPlayButtonBounds(const Rectangle& panel) {
    return Rectangle{panel.x + panel.width - 358.0f, panel.y + 32.0f, 104.0f, 34.0f};
}

Rectangle hostScoreboardAdminRowBounds(const Rectangle& panel, std::size_t rowIndex) {
    return Rectangle{
        panel.x + 18.0f,
        panel.y + 86.0f + static_cast<float>(rowIndex) * 60.0f,
        panel.width - 36.0f,
        50.0f
    };
}

Rectangle hostScoreboardAdminButtonBounds(const Rectangle& row, std::size_t buttonIndex) {
    static constexpr std::array<float, 4> kButtonWidths{{54.0f, 54.0f, 66.0f, 70.0f}};
    const float gap = 8.0f;
    const float totalWidth =
        kButtonWidths[0] + kButtonWidths[1] + kButtonWidths[2] + kButtonWidths[3] + (gap * 3.0f);
    float x = row.x + row.width - totalWidth;
    for (std::size_t index = 0; index < buttonIndex; ++index) {
        x += kButtonWidths[index] + gap;
    }
    return Rectangle{x, row.y + 10.0f, kButtonWidths[buttonIndex], 30.0f};
}

std::string fitRuntimeText(std::string text, int fontSize, float maxWidth) {
    if (text.empty() ||
        static_cast<float>(measureRuntimeTextWidth(text, fontSize)) <= maxWidth) {
        return text;
    }

    const std::string suffix = "...";
    while (!text.empty()) {
        text.pop_back();
        const std::string candidate = text + suffix;
        if (static_cast<float>(measureRuntimeTextWidth(candidate, fontSize)) <= maxWidth) {
            return candidate;
        }
    }
    return suffix;
}

const char* hostScoreboardActionLabel(int kind) {
    switch (kind) {
        case 0:
            return "ATK";
        case 1:
            return "DEF";
        case 2:
            return "SPEC";
        case 3:
            return "KICK";
        case 4:
            return "+ BOT";
        case 5:
            return "PLAY";
        case 6:
            return "PEACE";
        default:
            return "";
    }
}

Color hostScoreboardActionAccent(int kind) {
    switch (kind) {
        case 0:
            return kAttackerColor;
        case 1:
            return kDefenderColor;
        case 2:
            return kNeutralTeamColor;
        case 3:
            return Color{255, 92, 112, 255};
        case 4:
            return Color{88, 210, 144, 255};
        case 5:
            return Color{88, 210, 144, 255};
        case 6:
            return Color{92, 184, 255, 255};
        default:
            return kNeutralTeamColor;
    }
}

void drawHostScoreboardPistolIcon(const Rectangle& bounds,
                                  bool shootingEnabled,
                                  Color iconColor) {
    const float scale = std::min(bounds.width / 104.0f, bounds.height / 34.0f);
    const float iconWidth = 54.0f * scale;
    const float iconHeight = 24.0f * scale;
    const float iconX = bounds.x + (bounds.width - iconWidth) * 0.5f;
    const float iconY = bounds.y + (bounds.height - iconHeight) * 0.5f;
    const float unit = scale;
    const Color shadow = Fade(BLACK, 0.45f);

    auto drawPistol = [&](float offsetX, float offsetY, Color color) {
        DrawRectangleRounded(Rectangle{iconX + offsetX + 5.0f * unit,
                                       iconY + offsetY + 5.0f * unit,
                                       37.0f * unit,
                                       8.0f * unit},
                             0.22f,
                             6,
                             color);
        DrawRectangleRounded(Rectangle{iconX + offsetX + 39.0f * unit,
                                       iconY + offsetY + 7.0f * unit,
                                       12.0f * unit,
                                       5.0f * unit},
                             0.18f,
                             5,
                             color);
        DrawRectangleRounded(Rectangle{iconX + offsetX + 2.0f * unit,
                                       iconY + offsetY + 12.0f * unit,
                                       30.0f * unit,
                                       7.0f * unit},
                             0.26f,
                             6,
                             color);
        DrawRectanglePro(Rectangle{iconX + offsetX + 13.0f * unit,
                                   iconY + offsetY + 15.0f * unit,
                                   9.0f * unit,
                                   18.0f * unit},
                         Vector2{1.5f * unit, 1.0f * unit},
                         -14.0f,
                         color);
        DrawRectangleRoundedLines(Rectangle{iconX + offsetX + 25.0f * unit,
                                            iconY + offsetY + 14.0f * unit,
                                            10.0f * unit,
                                            8.0f * unit},
                                  0.35f,
                                  5,
                                  color);
    };

    drawPistol(1.5f * unit, 1.5f * unit, shadow);
    drawPistol(0.0f, 0.0f, iconColor);

    if (!shootingEnabled) {
        const Vector2 start{iconX - 2.0f * unit, iconY + iconHeight + 1.0f * unit};
        const Vector2 end{iconX + iconWidth + 2.0f * unit, iconY - 1.0f * unit};
        DrawLineEx(Vector2{start.x + 1.5f * unit, start.y + 1.5f * unit},
                   Vector2{end.x + 1.5f * unit, end.y + 1.5f * unit},
                   5.5f * unit,
                   shadow);
        DrawLineEx(start, end, 4.0f * unit, Color{255, 92, 112, 255});
    }
}

bool hasMeaningfulHostedSessionMetadata(const HostedSessionMetadata& metadata) {
    return !metadata.sessionLabel.empty() ||
           !metadata.hostPlayerName.empty() ||
           metadata.hostPeerId != 0u ||
           metadata.levelSlot >= 0 ||
           metadata.levelHash != 0u ||
           metadata.publicJoinPort != 0u ||
           !metadata.studyEventRunId.empty();
}

std::uint16_t roundedLatencyMs(const DiagnosticsModel* diagnosticsModel) {
    if (diagnosticsModel == nullptr) {
        return 0u;
    }

    return static_cast<std::uint16_t>(std::clamp(
        std::lround(diagnosticsModel->localNetworkSettings().latencyMs),
        0l,
        static_cast<long>(std::numeric_limits<std::uint16_t>::max())));
}

std::uint8_t roundedLossPct(const DiagnosticsModel* diagnosticsModel) {
    if (diagnosticsModel == nullptr) {
        return 0u;
    }

    return static_cast<std::uint8_t>(std::clamp(
        std::lround(diagnosticsModel->localNetworkSettings().lossPct),
        0l,
        100l));
}

std::uint32_t generateSessionId(std::uint16_t localPort) {
    static std::atomic<std::uint32_t> counter{0x9E3779B9u};

    const std::uint64_t ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint32_t candidate = static_cast<std::uint32_t>(ticks) ^
                              static_cast<std::uint32_t>(ticks >> 32);
    candidate ^= static_cast<std::uint32_t>(localPort) << 16;
    candidate ^= counter.fetch_add(0x85EBCA6Bu, std::memory_order_relaxed);

    if (candidate == 0u) {
        candidate = 1u;
    }
    return candidate;
}

std::uint64_t secondsToMicros(float dtSeconds) {
    if (dtSeconds <= 0.0f) {
        return 0u;
    }
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(dtSeconds) * 1'000'000.0));
}

std::uint64_t boundedNetworkMicros(float dtSeconds) {
    return std::min(secondsToMicros(dtSeconds), kMaxClientFrameAdvanceUs);
}

float microsToSeconds(std::uint64_t dtUs) {
    return static_cast<float>(dtUs) / 1'000'000.0f;
}

bool applyAuthoritativeLevelIdentity(Arena3D* arena,
                                     sim::MovementEnvironment* environment,
                                     int levelSlot,
                                     std::uint32_t levelHash,
                                     std::string* failureOut) {
    if (arena == nullptr || environment == nullptr) {
        if (failureOut != nullptr) {
            *failureOut = "level_load_failed";
        }
        return false;
    }

    const std::uint32_t expectedHash = makeLevelIdentityHash(levelSlot);
    if (levelHash != expectedHash) {
        if (failureOut != nullptr) {
            *failureOut = "level_identity_mismatch";
        }
        return false;
    }

    if (levelSlot >= 1 && levelSlot <= 9) {
        LevelData::LevelDefinition level;
        if (!LevelData::loadLevel(level, levelSlot)) {
            if (failureOut != nullptr) {
                *failureOut = "level_load_failed";
            }
            return false;
        }
        arena->loadLevel(level);
    } else if (levelSlot >= 0) {
        if (failureOut != nullptr) {
            *failureOut = "level_load_failed";
        }
        return false;
    }

    *environment = arena->buildMovementEnvironment();
    return true;
}

}  // namespace

const char* toString(ClientConnectionState state) {
    switch (state) {
        case ClientConnectionState::Disconnected: return "disconnected";
        case ClientConnectionState::Connecting: return "connecting";
        case ClientConnectionState::Connected: return "connected";
        case ClientConnectionState::Rejected: return "rejected";
        case ClientConnectionState::TimedOut: return "timed_out";
    }
    return "unknown";
}

ClientRuntime::ClientRuntime(ClientConfig config)
    : config_(std::move(config)),
      serverEndpoint_{config_.serverHost.empty() ? "127.0.0.1" : config_.serverHost, config_.serverPort},
      arena_(std::make_unique<Arena3D>()),
      localPlayerVisual_(std::make_unique<Player3D>(std::shared_ptr<Model3DWrapper>{})) {
    if (config_.serverHost.empty()) {
        config_.serverHost = "127.0.0.1";
        serverEndpoint_.host = config_.serverHost;
    }
    if (config_.playerName.empty()) {
        config_.playerName = "player";
    }
    autoAssignSessionId_ = (config_.sessionId == 0u);
    environment_ = arena_->buildMovementEnvironment();
}

ClientRuntime::~ClientRuntime() = default;

bool ClientRuntime::start() {
    shutdown();

    serverEndpoint_ = UdpEndpoint{config_.serverHost, config_.serverPort};
    if (!socket_.bind(UdpEndpoint{config_.bindHost, config_.localPort})) {
        state_ = ClientConnectionState::Rejected;
        statusMessage_ = socket_.lastError();
        return false;
    }

    if (autoAssignSessionId_) {
        config_.sessionId = generateSessionId(socket_.localPort());
    }

    state_ = ClientConnectionState::Connecting;
    statusMessage_ = "connecting to " + config_.serverHost + ":" + std::to_string(config_.serverPort);
    viewYaw_ = 0.0f;
    viewPitch_ = 0.0f;
    viewInitialized_ = false;
    detachedObserverActive_ = false;
    sessionSpectatorObserverLocked_ = false;
    detachedObserverEyePosition_ = sim::Vec3{};
    detachedObserverYaw_ = 0.0f;
    detachedObserverPitch_ = 0.0f;
    spectatorTransitionActive_ = false;
    spectatorTransitionTimer_ = 0.0f;
    arena_ = std::make_unique<Arena3D>();
    localPlayerState_ = sim::PlayerState{};
    syncRuntime_.reset(syncContext(), localPlayerState_);
    replaySubsystem_.reset();
    recordedReplayFrames_.clear();
    localPlayerVisual_->setSimState(localPlayerState_);
    remoteVisuals_.clear();
    combatTraces_.clear();
    perceptionMonitor_.reset();
    studyEventSink_.reset();
    studyEventSeq_ = 0u;
    clientFrameCounter_ = 0u;
    studyPresentation_ = StudyPresentationState{};
    killFeed_.clear();
    lastCombatEventText_.clear();
    localRespawnPending_ = false;
    localDeathStartUs_ = 0u;
    consumedCombatEventCount_ = 0u;
    scoreboardVisible_ = false;
    scoreboardCursorActive_ = false;
    pendingTeamRequest_ = sim::TeamId::None;
    teamMenuVisible_ = false;
    teamMenuSelection_ = sim::TeamId::Attacker;
    authoritativeLocalParticipantSettings_.reset();
    stagedSessionTickRateHz_.reset();
    stagedSessionSnapshotRateHz_.reset();
    lastControlSnapshotPacketSeq_ = 0u;
    latestControlSnapshotReceiveUs_ = 0u;
    hasControlSnapshot_ = false;
    controlRemotePlayerInterpolation_.reset();
    controlRemotePlayers_.clear();
    latestControlSnapshot_ = WorldSnapshot{};
    interpolationEnabled_ = true;
    predictionEnabled_ = true;
    reconciliationStrategy_ = ReconciliationStrategy::Smooth;
    lastCorrection_ = CorrectionRecord{};
    runtimeSettingsTargetId_ = 0u;
    runtimeSettingsVisible_ = false;
    runtimeSettingsOverlay_.resetInteraction();
    keyboardOverlayVisible_ = false;
    keyboardOverlayRestoreCapture_ = false;
    replayOverlayVisible_ = false;
    fovConesVisible_ = false;
    loadSpectatorCheckpoints();

    return sendHello();
}

void ClientRuntime::shutdown() {
    socket_ = UdpSocket{};
    state_ = ClientConnectionState::Disconnected;
    statusMessage_ = "disconnected";
    localPlayerState_ = sim::PlayerState{};
    syncRuntime_.reset(syncContext(), localPlayerState_);
    detachedObserverActive_ = false;
    sessionSpectatorObserverLocked_ = false;
    detachedObserverEyePosition_ = sim::Vec3{};
    detachedObserverYaw_ = 0.0f;
    detachedObserverPitch_ = 0.0f;
    spectatorTransitionActive_ = false;
    spectatorTransitionTimer_ = 0.0f;
    replaySubsystem_.reset();
    recordedReplayFrames_.clear();
    localPlayerVisual_->setSimState(localPlayerState_);
    remoteVisuals_.clear();
    combatTraces_.clear();
    perceptionMonitor_.reset();
    studyEventSink_.reset();
    studyEventSeq_ = 0u;
    clientFrameCounter_ = 0u;
    studyPresentation_ = StudyPresentationState{};
    killFeed_.clear();
    lastCombatEventText_.clear();
    localRespawnPending_ = false;
    localDeathStartUs_ = 0u;
    consumedCombatEventCount_ = 0u;
    uiMode_ = false;
    scoreboardVisible_ = false;
    scoreboardCursorActive_ = false;
    pendingTeamRequest_ = sim::TeamId::None;
    teamMenuVisible_ = false;
    teamMenuSelection_ = sim::TeamId::Attacker;
    authoritativeLocalParticipantSettings_.reset();
    stagedSessionTickRateHz_.reset();
    stagedSessionSnapshotRateHz_.reset();
    lastControlSnapshotPacketSeq_ = 0u;
    latestControlSnapshotReceiveUs_ = 0u;
    hasControlSnapshot_ = false;
    controlRemotePlayerInterpolation_.reset();
    controlRemotePlayers_.clear();
    latestControlSnapshot_ = WorldSnapshot{};
    interpolationEnabled_ = true;
    predictionEnabled_ = true;
    reconciliationStrategy_ = ReconciliationStrategy::Smooth;
    lastCorrection_ = CorrectionRecord{};
    runtimeSettingsTargetId_ = 0u;
    runtimeSettingsVisible_ = false;
    runtimeSettingsOverlay_.resetInteraction();
    keyboardOverlayVisible_ = false;
    keyboardOverlayRestoreCapture_ = false;
    replayOverlayVisible_ = false;
    fovConesVisible_ = false;
}

void ClientRuntime::update(float dtSeconds, const InputHandler3D::InputState* input) {
    const std::uint64_t networkDtUs = boundedNetworkMicros(dtSeconds);
    clockUs_ += networkDtUs;
    ++clientFrameCounter_;
    drainIncomingPackets();
    for (CombatTrace& trace : combatTraces_) {
        trace.beam.update(microsToSeconds(networkDtUs));
    }
    pruneCombatTraces();
    pruneKillFeed();
    if (predictionEnabled_ && hasSnapshot_) {
        syncRuntime_.advanceLocalCorrection(syncContext(), microsToSeconds(networkDtUs));
        localPlayerVisual_->setSimState(localPlayerState_);
    }

    const auto captureReplayFrame = [this, dtSeconds]() {
        if (!replaySubsystem_.timeline().isRecording ||
            state_ != ClientConnectionState::Connected ||
            !hasSnapshot_) {
            return;
        }

        const client::ClientViewState viewState = buildClientViewState();
        client::RecordedReplayFrame frame;
        frame.timestamp = replaySubsystem_.timeline().recordingTimer;
        frame.frame = buildLiveRenderFrame(viewState);
        client::RemotePlayerRenderItem localPlayerItem;
        if (makeLocalReplayPlayerRenderItem(viewState, &localPlayerItem)) {
            frame.localPlayerRenderItem = localPlayerItem;
        }
        recordedReplayFrames_.push_back(std::move(frame));
        replaySubsystem_.advanceRecording(dtSeconds);
    };

    bool keyboardOverlayConsumedInput = false;
    if (input != nullptr) {
        controlBindings_ = input->controlBindings;
        const bool keyboardIconClicked =
            !display::isCursorCaptured() &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(display::mousePosition(),
                                   client::KeyboardHelpOverlay::iconBounds());
        if ((input->toggleKeyboardOverlay || keyboardIconClicked) &&
            !runtimeSettingsVisible_ &&
            !teamMenuVisible_ &&
            !scoreboardCursorActive_) {
            if (keyboardOverlayVisible_) {
                closeKeyboardOverlay();
            } else {
                openKeyboardOverlay();
            }
            keyboardOverlayConsumedInput = true;
        }
    }

    if (keyboardOverlayConsumedInput || keyboardOverlayVisible_) {
        maybeSendIdleKeepalive();
        if (hasSnapshot_) {
            updateInterpolatedRemoteEntities();
        }
        captureReplayFrame();
        return;
    }

    const bool confirmPressed = input != nullptr && input->menuConfirm;
    const bool releaseCapturedMouseWithConfirm =
        confirmPressed && display::isCursorCaptured();
    const bool toggleMouseCaptureWithConfirm =
        input != nullptr &&
        input->menuConfirm &&
        !teamMenuVisible_ &&
        !runtimeSettingsVisible_ &&
        !keyboardOverlayVisible_ &&
        !scoreboardVisible_ &&
        !input->toggleScoreboard;
    const bool toggleRuntimeSettings =
        input != nullptr &&
        (input->toggleUIMode || input->toggleUIPanel);
    if (releaseCapturedMouseWithConfirm) {
        setUiMode(true);
        scoreboardCursorActive_ = false;
    } else if (toggleRuntimeSettings) {
        if (keyboardOverlayVisible_) {
            closeKeyboardOverlay(false);
        }
        runtimeSettingsVisible_ = !runtimeSettingsVisible_;
        setUiMode(runtimeSettingsVisible_);
        if (runtimeSettingsVisible_) {
            teamMenuVisible_ = false;
        }
        scoreboardVisible_ = false;
        scoreboardCursorActive_ = false;
    } else if (toggleMouseCaptureWithConfirm) {
        setUiMode(!uiMode_);
        scoreboardVisible_ = false;
        scoreboardCursorActive_ = false;
    }
    if (input != nullptr && input->toggleInterp &&
        state_ == ClientConnectionState::Connected && peerId_ != 0u &&
        canEditParticipantSyncSettings(peerId_)) {
        const bool requestedInterpolationEnabled = !interpolationEnabled_;
        sendRuntimeParamChangeRequest(RuntimeParamScope::Player,
                                      static_cast<std::int32_t>(peerId_),
                                      runtimeParamKeyForTarget(peerId_, "interpolation_enabled"),
                                      requestedInterpolationEnabled ? 1.0f : 0.0f);
        previewLocalParticipantRuntimeSetting(RuntimeSettingsOverlay::ControlId::Interpolation,
                                              requestedInterpolationEnabled ? 1.0f : 0.0f);
        lastCombatEventText_ = requestedInterpolationEnabled
            ? "Requested interpolation enabled"
            : "Requested interpolation disabled";
    }
    if (input != nullptr && input->togglePrediction &&
        state_ == ClientConnectionState::Connected && peerId_ != 0u &&
        canEditParticipantSyncSettings(peerId_)) {
        const bool requestedPredictionEnabled = !predictionEnabled_;
        sendRuntimeParamChangeRequest(RuntimeParamScope::Player,
                                      static_cast<std::int32_t>(peerId_),
                                      runtimeParamKeyForTarget(peerId_, "prediction_enabled"),
                                      requestedPredictionEnabled ? 1.0f : 0.0f);
        previewLocalParticipantRuntimeSetting(RuntimeSettingsOverlay::ControlId::Prediction,
                                              requestedPredictionEnabled ? 1.0f : 0.0f);
        lastCombatEventText_ = requestedPredictionEnabled
            ? "Requested prediction enabled"
            : "Requested prediction disabled";
    }
    if (input != nullptr && input->toggleEnemyAI &&
        state_ == ClientConnectionState::Connected &&
        !uiMode_ &&
        !teamMenuVisible_) {
        if (isLocalHost()) {
            const bool botsFrozen =
                hasAuthoritativeSessionMetadata_ ? authoritativeSessionMetadata_.botsFrozen : true;
            const bool requestedBotsActive = botsFrozen;
            sendRuntimeParamChangeRequest(RuntimeParamScope::Session,
                                          -1,
                                          "sv.bots_active",
                                          requestedBotsActive ? 1.0f : 0.0f);
            lastCombatEventText_ = requestedBotsActive
                ? "Requested bots active"
                : "Requested bots frozen";
        } else {
            lastCombatEventText_ = "Only the host can toggle bots";
        }
    }
    if (input != nullptr && input->spawnFrozenBotAhead &&
        state_ == ClientConnectionState::Connected &&
        !uiMode_ &&
        !teamMenuVisible_) {
        if (!config_.studyActionsEnabled) {
            lastCombatEventText_ = "Frozen bot spawning is only available in Lab Study";
        } else if (!isLocalHost()) {
            lastCombatEventText_ = "Only the host can spawn frozen study bots";
        } else {
            sendSessionActionRequest(SessionActionKind::SpawnFrozenBotAhead);
            lastCombatEventText_ = "Requested frozen bot spawn";
        }
    }
    if (input != nullptr && !uiMode_ && !teamMenuVisible_) {
        if (input->toggleFocus) {
            studyPresentation_.environmentDimmed = !studyPresentation_.environmentDimmed;
            lastCombatEventText_ = studyPresentation_.environmentDimmed
                ? "Environment clutter dimmed"
                : "Environment clutter restored";
        }
        if (input->toggleAreas) {
            studyPresentation_.areasVisible = !studyPresentation_.areasVisible;
            lastCombatEventText_ = studyPresentation_.areasVisible
                ? "Marked areas shown"
                : "Marked areas hidden";
        }
        if (input->toggleAreaFilterGreen) {
            studyPresentation_.areaFilter =
                studyPresentation_.areaFilter == client::AreaFilterView::GreenOnly
                    ? client::AreaFilterView::All
                    : client::AreaFilterView::GreenOnly;
            lastCombatEventText_ =
                studyPresentation_.areaFilter == client::AreaFilterView::GreenOnly
                    ? "Filtering green areas"
                    : "Showing all areas";
        }
        if (input->toggleAreaFilterRed) {
            studyPresentation_.areaFilter =
                studyPresentation_.areaFilter == client::AreaFilterView::RedOnly
                    ? client::AreaFilterView::All
                    : client::AreaFilterView::RedOnly;
            lastCombatEventText_ =
                studyPresentation_.areaFilter == client::AreaFilterView::RedOnly
                    ? "Filtering red areas"
                    : "Showing all areas";
        }
    }
    applyStudyPresentationState(microsToSeconds(networkDtUs));
    if (input != nullptr) {
        scoreboardVisible_ =
            input->toggleScoreboard && !teamMenuVisible_ && !runtimeSettingsVisible_;
        if (scoreboardVisible_) {
            closeKeyboardOverlay();
        }
    }
    if (shouldShowHostScoreboardAdmin()) {
        if (!uiMode_) {
            scoreboardCursorActive_ = true;
            setUiMode(true);
        }
        handleHostScoreboardAdminClick();
    } else if (scoreboardCursorActive_) {
        scoreboardCursorActive_ = false;
        setUiMode(false);
    }
    if (input != nullptr && input->toggleRecordingOverlay && !runtimeSettingsVisible_) {
        replayOverlayVisible_ = !replayOverlayVisible_;
        lastCombatEventText_ = replayOverlayVisible_
            ? "Replay overlay shown"
            : "Replay overlay hidden";
    }
    if (input != nullptr && input->toggleFovCones && !runtimeSettingsVisible_ && !teamMenuVisible_) {
        fovConesVisible_ = !fovConesVisible_;
        lastCombatEventText_ = fovConesVisible_
            ? "FOV cones shown"
            : "FOV cones hidden";
    }
    if (input != nullptr && input->toggleView && state_ == ClientConnectionState::Connected &&
        !uiMode_ && !teamMenuVisible_) {
        if (detachedObserverActive_) {
            lastCombatEventText_ = "Detached free camera active";
        } else {
            const ControlBindingContracts contracts = controlBindingContracts();
            if (isObservationPaneMode(contracts.paneView.mode)) {
                const int nextTarget = nextEligibleActorId(contracts.eligibleActors.spectatorTargets,
                                                           contracts.paneView.followTargetActorId);
                if (nextTarget >= 0) {
                    sendRuntimeParamChangeRequest(RuntimeParamScope::Player,
                                                  static_cast<std::int32_t>(peerId_),
                                                  runtimeParamKeyForTarget(peerId_, "follow_target_actor_id"),
                                                  static_cast<float>(nextTarget));
                    lastCombatEventText_ = "Following " + actorDisplayLabel(roster_, nextTarget);
                } else {
                    lastCombatEventText_ = "No spectator targets available";
                }
            } else {
                const int nextActor = nextEligibleActorId(contracts.eligibleActors.controlTargets,
                                                          contracts.participantState.control.actorId);
                if (nextActor >= 0) {
                    sendRuntimeParamChangeRequest(RuntimeParamScope::Player,
                                                  static_cast<std::int32_t>(peerId_),
                                                  runtimeParamKeyForTarget(peerId_, "control_actor_id"),
                                                  static_cast<float>(nextActor));
                    lastCombatEventText_ = "Requested control of " +
                                           actorDisplayLabel(roster_, nextActor);
                } else {
                    lastCombatEventText_ = "No local control targets available";
                }
            }
        }
    }
    if (input != nullptr && !uiMode_ && input->switchTeam &&
        state_ == ClientConnectionState::Connected) {
        closeKeyboardOverlay();
        toggleTeamMenu();
    }
    if (input != nullptr && input->toggleSpectator &&
        state_ == ClientConnectionState::Connected && peerId_ != 0u) {
        toggleSpectatorMode();
    }
    if (input != nullptr && detachedObserverActive_) {
        if (input->addSpectatorCheckpoint) {
            SpectatorCamera::Checkpoint checkpoint = detachedObserverCheckpoint();
            checkpoint.transitionDurationSeconds =
                defaultCheckpointTransitionSecondsForCurrentMode();
            currentSpectatorCheckpoint_ = app::CheckpointStore::createCheckpoint(
                spectatorCheckpoints_,
                checkpoint);
            spectatorTransitionActive_ = false;
            saveSpectatorCheckpoints();
            lastCombatEventText_ = "Checkpoint saved";
        }
        if (input->prevSpectatorCheckpoint && !spectatorCheckpoints_.empty()) {
            beginSpectatorCheckpointTransition(
                app::CheckpointStore::cyclePreviousCheckpoint(
                    spectatorCheckpoints_, currentSpectatorCheckpoint_));
        }
        if (input->nextSpectatorCheckpoint && !spectatorCheckpoints_.empty()) {
            beginSpectatorCheckpointTransition(
                app::CheckpointStore::cycleNextCheckpoint(
                    spectatorCheckpoints_, currentSpectatorCheckpoint_));
        }
        if (input->deleteSpectatorCheckpoint && !spectatorCheckpoints_.empty()) {
            currentSpectatorCheckpoint_ = app::CheckpointStore::deleteCheckpoint(
                spectatorCheckpoints_, currentSpectatorCheckpoint_);
            spectatorTransitionActive_ = false;
            if (currentSpectatorCheckpoint_ >= 0 &&
                currentSpectatorCheckpoint_ < static_cast<int>(spectatorCheckpoints_.size())) {
                setDetachedObserverCheckpoint(spectatorCheckpoints_[currentSpectatorCheckpoint_]);
            }
            saveSpectatorCheckpoints();
            lastCombatEventText_ = spectatorCheckpoints_.empty()
                ? "Checkpoint deleted"
                : "Checkpoint deleted, moved to next";
        }
        if (replayOverlayVisible_ && input->decreaseSpectatorTransitionDuration) {
            adjustCurrentSpectatorCheckpointTransition(
                -kSpectatorCheckpointTransitionAdjustmentSeconds);
        }
        if (replayOverlayVisible_ && input->increaseSpectatorTransitionDuration) {
            adjustCurrentSpectatorCheckpointTransition(
                kSpectatorCheckpointTransitionAdjustmentSeconds);
        }
    }
    if (teamMenuVisible_ && input != nullptr) {
        if (input->menuLeft) {
            cycleTeamMenuSelection(-1);
        } else if (input->menuRight) {
            cycleTeamMenuSelection(1);
        }
        if (input->menuConfirm && !releaseCapturedMouseWithConfirm) {
            confirmTeamMenuSelection();
        }
    }

    if (diagnosticsModel_ != nullptr && hostProxy_ != nullptr) {
        refreshDiagnostics();
    }
    if (runtimeSettingsVisible_ && input != nullptr) {
        for (const RuntimeSettingsOverlay::Action& action :
             runtimeSettingsOverlay_.handleMouse(buildRuntimeSettingsOverlayState())) {
            applyRuntimeSettingsAction(action);
        }
    }

    if (input != nullptr && !uiMode_ && !replaySubsystem_.timeline().isPlayback) {
        if (detachedObserverActive_) {
            InputHandler3D::applyLookDelta(*input, &detachedObserverYaw_, &detachedObserverPitch_);
        } else {
            InputHandler3D::applyLookDelta(*input, &viewYaw_, &viewPitch_);
        }
        viewInitialized_ = true;
    }

    if (state_ == ClientConnectionState::Connecting) {
        if (clockUs_ >= config_.connectTimeoutUs) {
            state_ = ClientConnectionState::TimedOut;
            statusMessage_ = "connection timed out";
            captureReplayFrame();
            return;
        }

        if ((clockUs_ - lastHelloSendUs_) >= config_.helloRetryIntervalUs) {
            sendHello();
        }
        captureReplayFrame();
        return;
    }

    if (state_ != ClientConnectionState::Connected) {
        captureReplayFrame();
        return;
    }

    bool startedReplayPlaybackThisFrame = false;
    if (input != nullptr) {
        if (input->toggleRecording) {
            toggleReplayRecording();
        }

        if (input->exportRecording) {
            saveRecordedReplay();
        }

        if (input->togglePlayback) {
            startedReplayPlaybackThisFrame = toggleReplayPlayback();
        }

        if (input->resetPlayback) {
            if (recordedReplayFrames_.empty()) {
                lastCombatEventText_ = "No recording available";
            } else {
                if (!replaySubsystem_.timeline().isPlayback) {
                    replaySubsystem_.startPlayback(recordedReplayFrames_, false);
                }
                if (replaySubsystem_.resetPlayback(recordedReplayFrames_)) {
                    lastCombatEventText_ = "Playback reset";
                }
            }
        }

        if (input->stopReplayPlayback) {
            stopReplayPlayback();
        }

        constexpr int kReplaySeekFrameCount = 500;
        if ((input->stepForward || input->stepBackward) &&
            !recordedReplayFrames_.empty()) {
            if (!replaySubsystem_.timeline().isPlayback) {
                replaySubsystem_.startPlayback(recordedReplayFrames_, false);
            }
            if (replaySubsystem_.timeline().isPlayback) {
                const int direction = input->stepForward ? 1 : -1;
                if (replaySubsystem_.seekBy(recordedReplayFrames_,
                                            direction * kReplaySeekFrameCount)) {
                    lastCombatEventText_ = direction > 0
                        ? "Fast-forwarded replay"
                        : "Rewound replay";
                }
            }
        }
    }

    if (config_.serverSilenceTimeoutUs > 0u &&
        lastServerPacketUs_ > 0u &&
        (clockUs_ - lastServerPacketUs_) > config_.serverSilenceTimeoutUs) {
        state_ = ClientConnectionState::TimedOut;
        statusMessage_ = "server timed out";
        captureReplayFrame();
        return;
    }

    if (localPlayerAwaitingRespawn()) {
        const std::uint64_t elapsedUs =
            localDeathStartUs_ > 0u && clockUs_ > localDeathStartUs_
                ? (clockUs_ - localDeathStartUs_)
                : 0u;
        lastCombatEventText_ = respawnCountdownText(
            kLocalRespawnDelaySeconds - microsToSeconds(elapsedUs));
        maybeSendIdleKeepalive();
        if (hasSnapshot_) {
            updateInterpolatedRemoteEntities();
        }
        captureReplayFrame();
        return;
    }

    if (replaySubsystem_.timeline().isPlayback) {
        maybeSendIdleKeepalive();
        if (hasSnapshot_) {
            updateInterpolatedRemoteEntities();
        }
        if (!startedReplayPlaybackThisFrame) {
            replaySubsystem_.updatePlayback(recordedReplayFrames_, dtSeconds);
        }
        updateReplayPlaybackObserver(microsToSeconds(networkDtUs), input);
        return;
    }

    if (input == nullptr || uiMode_ || teamMenuVisible_) {
        maybeSendIdleKeepalive();
    }

    if (input == nullptr || teamMenuVisible_) {
        if (hasSnapshot_) {
            updateInterpolatedRemoteEntities();
        }
        captureReplayFrame();
        return;
    }

    if (uiMode_) {
        if (hasSnapshot_) {
            updateInterpolatedRemoteEntities();
        }
        captureReplayFrame();
        return;
    }

    if (detachedObserverActive_) {
        updateDetachedObserverCamera(*input, microsToSeconds(networkDtUs));
        updateSpectatorCheckpointTransition(microsToSeconds(networkDtUs));
        maybeSendIdleKeepalive();
        if (hasSnapshot_) {
            updateInterpolatedRemoteEntities();
        }
        captureReplayFrame();
        return;
    }

    const sim::PlayerCommand command =
        makeCommand(*input, microsToSeconds(networkDtUs), ++commandSeq_);
    syncRuntime_.applyLocalPrediction(syncContext(), command, environment_, simConfig_);
    localPlayerVisual_->setSimState(localPlayerState_);
    if (syncRuntime_.shouldPredictFireAttempt(syncContext(), *input)) {
        recordCombatTrace(
            sim::buildRifleHitscan(localPlayerState_, command.yaw, command.pitch, simConfig_),
            false,
            false);
        lastCombatEventText_ = "Shot fired (pending)";
    }
    const bool commandSent = sendCommand(command);
    if (command.has(sim::CommandButton::Fire)) {
        recordClientFirePressed(command, commandSent);
    }
    updateInterpolatedRemoteEntities();
    captureReplayFrame();
}

client::RenderFrame ClientRuntime::buildLiveRenderFrame(
    const client::ClientViewState& viewState) const {
    std::vector<LaserBeam3D> combatTraceBeams;
    combatTraceBeams.reserve(combatTraces_.size());
    for (const CombatTrace& trace : combatTraces_) {
        combatTraceBeams.push_back(trace.beam);
    }
    return clientPresentation_.build(
        client::ClientPresentationInputs{viewState, arena_.get(), &combatTraceBeams});
}

void ClientRuntime::toggleReplayRecording() {
    if (replaySubsystem_.timeline().isRecording) {
        replaySubsystem_.stopRecording();
        replaySubsystem_.stopPlayback();
        lastCombatEventText_ = "Recording stopped";
        return;
    }

    replaySubsystem_.stopPlayback();
    recordedReplayFrames_.clear();
    replaySubsystem_.startRecording();
    lastCombatEventText_ = "Recording started";
}

bool ClientRuntime::toggleReplayPlayback() {
    if (replaySubsystem_.timeline().isRecording) {
        lastCombatEventText_ = "Stop recording with 5 before playback";
        return false;
    }

    if (recordedReplayFrames_.empty()) {
        replaySubsystem_.stopPlayback();
        lastCombatEventText_ = "No recording available";
        return false;
    }

    if (!replaySubsystem_.timeline().isPlayback) {
        if (replaySubsystem_.startPlayback(recordedReplayFrames_, true)) {
            lastCombatEventText_ = "Playback started";
            return true;
        }
        lastCombatEventText_ = "No recording available";
        return false;
    }

    replaySubsystem_.timeline().playbackPlaying =
        !replaySubsystem_.timeline().playbackPlaying;
    lastCombatEventText_ = replaySubsystem_.timeline().playbackPlaying
        ? "Playback resumed"
        : "Playback paused";
    return false;
}

bool ClientRuntime::stopReplayPlayback() {
    if (replaySubsystem_.timeline().isRecording) {
        lastCombatEventText_ = "Stop recording with 5";
        return false;
    }

    if (!replaySubsystem_.timeline().isPlayback) {
        lastCombatEventText_ = recordedReplayFrames_.empty()
            ? "No replay active"
            : "Replay ready";
        return false;
    }

    replaySubsystem_.stopPlayback();
    if (detachedObserverActive_) {
        detachedObserverActive_ = false;
        sessionSpectatorObserverLocked_ = false;
        spectatorTransitionActive_ = false;
    }
    lastCombatEventText_ = "Replay stopped";
    return true;
}

bool ClientRuntime::makeLocalReplayPlayerRenderItem(
    const client::ClientViewState& viewState,
    client::RemotePlayerRenderItem* itemOut) const {
    if (itemOut == nullptr ||
        arena_ == nullptr ||
        localPlayerState_.playerId <= 0 ||
        viewState.localParticipant.state.participation != sim::ParticipationState::Playing) {
        return false;
    }

    client::RemotePlayerRenderItem item;
    item.actorId = localPlayerState_.playerId;
    item.eyePosition = toVector3(localPlayerState_.position);
    item.rootPosition = arena_->playerRenderRootFromEyePosition(localPlayerState_.position);
    item.yawRadians = localPlayerState_.yaw;
    item.pitchRadians = localPlayerState_.pitch;
    item.healthPercent = localPlayerState_.maxHealth > 0.0f
        ? std::clamp(localPlayerState_.health / localPlayerState_.maxHealth, 0.0f, 1.0f)
        : 0.0f;
    item.alive = localPlayerState_.health > 0.0f;
    item.team = viewState.hud.localTeam;
    item.tint = teamTint(item.team);
    item.ghost = false;

    *itemOut = item;
    return true;
}

void ClientRuntime::appendReplaySpectatorLocalPlayer(
    client::RenderFrame* frame,
    const client::RecordedReplayFrame& replayFrame) const {
    if (frame == nullptr || !replayFrame.localPlayerRenderItem.has_value()) {
        return;
    }

    const int localActorId = replayFrame.localPlayerRenderItem->actorId;
    const auto alreadyVisible =
        std::find_if(frame->remotePlayers.begin(),
                     frame->remotePlayers.end(),
                     [localActorId](const client::RemotePlayerRenderItem& player) {
                         return player.actorId == localActorId;
                     });
    if (alreadyVisible == frame->remotePlayers.end()) {
        frame->remotePlayers.push_back(*replayFrame.localPlayerRenderItem);
    }
}

const client::RecordedReplayFrame* ClientRuntime::activeRecordedReplayFrame() const {
    if (!replaySubsystem_.timeline().isPlayback || recordedReplayFrames_.empty()) {
        return nullptr;
    }

    const std::size_t playbackIndex = std::min(
        replaySubsystem_.timeline().playbackIndex,
        recordedReplayFrames_.size() - 1u);
    return &recordedReplayFrames_[playbackIndex];
}

Camera3D ClientRuntime::detachedObserverCamera() const {
    return SpectatorCamera::freeFlyCamera(detachedObserverCheckpoint());
}

client::RenderFrame ClientRuntime::buildRenderFrame(
    const client::ClientViewState& viewState) const {
    client::RenderFrame frame = buildLiveRenderFrame(viewState);
    if (const client::RecordedReplayFrame* replayFrame = activeRecordedReplayFrame();
        replayFrame != nullptr) {
        frame = replayFrame->frame;
        if (detachedObserverActive_) {
            frame.camera = detachedObserverCamera();
            appendReplaySpectatorLocalPlayer(&frame, *replayFrame);
        }
        frame.replay = replayStatusView();
    }
    return frame;
}

void ClientRuntime::render() const {
    ClearBackground(kBackgroundColor);
    const client::ClientViewState viewState = buildClientViewState();
    client::RenderFrame frame = buildRenderFrame(viewState);

    if (!frame.hasSnapshot) {
        drawRuntimeText(frame.waiting.title, 30.0f, 20.0f, 28, SKYBLUE);
        drawRuntimeText(frame.waiting.stateLine, 30.0f, 72.0f, 20, kStatusColor);
        drawRuntimeText(frame.waiting.statusMessage, 30.0f, 108.0f, 20, LIGHTGRAY);
        if (!frame.waiting.hostedSessionLine.empty()) {
            drawRuntimeText(frame.waiting.hostedSessionLine, 30.0f, 144.0f, 20, GOLD);
        }
        const int waitingTextY = frame.waiting.hostedSessionLine.empty() ? 154 : 190;
        drawRuntimeText(frame.waiting.waitingText, 30.0f, static_cast<float>(waitingTextY), 24, RAYWHITE);
        drawRuntimeText(frame.waiting.joinHint, 30.0f, static_cast<float>(waitingTextY + 46), 20, GRAY);
        if (keyboardOverlayVisible_) {
            client::KeyboardHelpOverlay::render(controlBindings_);
        }
        client::KeyboardHelpOverlay::renderIcon(keyboardOverlayVisible_);
        return;
    }

    BeginMode3D(frame.camera);
    arena_->render(frame.arena);
    if (fovConesVisible_) {
        for (const auto& player : frame.remotePlayers) {
            drawFovCone(player);
        }
    }
    for (const LaserBeam3D& trace : frame.combatTraces) {
        trace.render();
    }
    const std::size_t remoteEnemyCount =
        std::min(remoteVisuals_.size(), frame.remoteEnemies.size());
    for (std::size_t index = 0; index < remoteEnemyCount; ++index) {
        if (remoteVisuals_[index] == nullptr) {
            continue;
        }

        remoteVisuals_[index]->render(frame.remoteEnemies[index]);
        remoteVisuals_[index]->renderHealthBar(frame.remoteEnemies[index], frame.camera, 1.0f);
    }
    for (const auto& player : frame.remotePlayers) {
        Player3D remotePlayerVisual(std::shared_ptr<Model3DWrapper>{});
        remotePlayerVisual.render(player);
        remotePlayerVisual.renderHealthBar(player, frame.camera, 1.0f);
    }
    for (const auto& playerGhost : frame.remotePlayerGhosts) {
        Player3D remotePlayerVisual(std::shared_ptr<Model3DWrapper>{});
        remotePlayerVisual.render(playerGhost);
        remotePlayerVisual.renderHealthBar(playerGhost, frame.camera, 1.0f);
    }
    EndMode3D();

    if (!isObservationPaneMode(viewState.pane.state.mode)) {
        DrawLine(Config::SCREEN_WIDTH / 2 - 8, Config::SCREEN_HEIGHT / 2,
                 Config::SCREEN_WIDTH / 2 + 8, Config::SCREEN_HEIGHT / 2, WHITE);
        DrawLine(Config::SCREEN_WIDTH / 2, Config::SCREEN_HEIGHT / 2 - 8,
                 Config::SCREEN_WIDTH / 2, Config::SCREEN_HEIGHT / 2 + 8, WHITE);
    }

    client::HudOverlayRenderer::renderKillFeed(frame.killFeed);
    client::HudOverlayRenderer::renderCompactScore(frame.compactScore);

    if (!frame.hud.lines.empty()) {
        float y = static_cast<float>(Config::SCREEN_HEIGHT - 124);
        for (std::size_t index = 0; index < frame.hud.lines.size(); ++index) {
            const std::string& line = frame.hud.lines[frame.hud.lines.size() - 1u - index];
            const Color color = line.find("Down |") != std::string::npos
                ? ORANGE
                : LIGHTGRAY;
            drawRuntimeText(line, 30.0f, y, 20, color);
            y -= 32.0f;
            if (y < static_cast<float>(Config::SCREEN_HEIGHT - 252)) {
                break;
            }
        }
    }

    client::HudOverlayRenderer::renderScoreboard(frame.scoreboard);
    renderHostScoreboardAdminPanel();

    if (frame.teamMenu.visible) {
        const Rectangle menuRect{
            static_cast<float>(Config::SCREEN_WIDTH - 490),
            90.0f,
            430.0f,
            280.0f
        };
        DrawRectangleRounded(menuRect, 0.08f, 10, Fade(BLACK, 0.8f));
        DrawRectangleRoundedLines(menuRect, 0.08f, 10, PINK);
        drawRuntimeText("Change Team", menuRect.x + 16.0f, menuRect.y + 18.0f, 28, WHITE);
        const std::string currentTeamLine =
            "Current: " + frame.teamMenu.currentTeamLabel;
        drawRuntimeText(currentTeamLine, menuRect.x + 16.0f, menuRect.y + 66.0f, 20, LIGHTGRAY);
        const std::string selectedTeamLine =
            "Selected: " + frame.teamMenu.selectedTeamLabel;
        drawRuntimeText(selectedTeamLine, menuRect.x + 16.0f, menuRect.y + 104.0f, 22, PINK);
        drawRuntimeText("Left/Right changes the target team",
                        menuRect.x + 16.0f,
                        menuRect.y + 148.0f,
                        20,
                        LIGHTGRAY);
        drawRuntimeText("Enter confirms | Y cancels",
                        menuRect.x + 16.0f,
                        menuRect.y + 184.0f,
                        20,
                        LIGHTGRAY);
        drawRuntimeText("Applies immediately and respawns you",
                        menuRect.x + 16.0f,
                        menuRect.y + 220.0f,
                        18,
                        GOLD);
    }
    if (runtimeSettingsVisible_) {
        runtimeSettingsOverlay_.render(buildRuntimeSettingsOverlayState());
    }
    if (frame.replay.overlayVisible && !runtimeSettingsVisible_) {
        drawReplayOverlay(frame.replay);
    }

    if (localPlayerAwaitingRespawn()) {
        const std::uint64_t elapsedUs =
            localDeathStartUs_ > 0u && clockUs_ > localDeathStartUs_
                ? (clockUs_ - localDeathStartUs_)
                : 0u;
        const float respawnRemaining =
            std::max(0.0f, kLocalRespawnDelaySeconds - microsToSeconds(elapsedUs));
        const int centerX = Config::SCREEN_WIDTH / 2;
        const int centerY = Config::SCREEN_HEIGHT / 2;

        DrawRectangle(0,
                      0,
                      Config::SCREEN_WIDTH,
                      Config::SCREEN_HEIGHT,
                      Fade(BLACK, 0.18f));
        drawCenteredOverlayText("YOU WERE KILLED",
                                centerX,
                                centerY - 24,
                                TypographyStyleId::ScoreboardSummary,
                                RED);
        drawCenteredOverlayText(respawnOverlayText(respawnRemaining),
                                centerX,
                                centerY + 20,
                                TypographyStyleId::AppSubtitle,
                                ORANGE);
    }

    const bool keyboardPanelVisible =
        keyboardOverlayVisible_ &&
        !runtimeSettingsVisible_ &&
        !frame.scoreboard.visible &&
        !frame.teamMenu.visible &&
        !scoreboardCursorActive_;
    if (keyboardPanelVisible) {
        client::KeyboardHelpOverlay::render(controlBindings_);
    }
    client::KeyboardHelpOverlay::renderIcon(keyboardOverlayVisible_);
}

ClientConnectionState ClientRuntime::state() const {
    return state_;
}

const std::string& ClientRuntime::statusMessage() const {
    return statusMessage_;
}

const ClientConfig& ClientRuntime::config() const {
    return config_;
}

bool ClientRuntime::hasSnapshot() const {
    return hasSnapshot_;
}

std::uint16_t ClientRuntime::peerId() const {
    return peerId_;
}

std::uint16_t ClientRuntime::localPort() const {
    return socket_.localPort();
}

std::uint32_t ClientRuntime::lastAckedInputSeq() const {
    return lastAckedInputSeq_;
}

bool ClientRuntime::hasAuthoritativeLevelIdentity() const {
    return authoritativeLevelSlot_ >= 0 || authoritativeLevelHash_ != 0u;
}

int ClientRuntime::authoritativeLevelSlot() const {
    return authoritativeLevelSlot_;
}

std::uint32_t ClientRuntime::authoritativeLevelHash() const {
    return authoritativeLevelHash_;
}

bool ClientRuntime::hasAuthoritativeSessionMetadata() const {
    return hasAuthoritativeSessionMetadata_;
}

const HostedSessionMetadata& ClientRuntime::authoritativeSessionMetadata() const {
    return authoritativeSessionMetadata_;
}

std::string ClientRuntime::hostedSessionSummary() const {
    return client::PresentationStateSubsystem::hostedSessionSummary(buildClientViewState());
}

bool ClientRuntime::uiModeActive() const {
    return uiMode_;
}

bool ClientRuntime::interpolationEnabled() const {
    return interpolationEnabled_;
}

bool ClientRuntime::predictionEnabled() const {
    return predictionEnabled_;
}

bool ClientRuntime::fovConesVisible() const {
    return fovConesVisible_;
}

bool ClientRuntime::scoreboardVisible() const {
    return scoreboardVisible_;
}

bool ClientRuntime::teamMenuVisible() const {
    return teamMenuVisible_;
}

sim::TeamId ClientRuntime::teamMenuSelection() const {
    return teamMenuSelection_;
}

const sim::PlayerState& ClientRuntime::localPlayerState() const {
    return localPlayerState_;
}

const std::vector<sim::PlayerState>& ClientRuntime::remotePlayers() const {
    return remotePlayers_;
}

const std::vector<sim::RemoteActorState>& ClientRuntime::remoteEnemies() const {
    return remoteEnemies_;
}

const std::vector<sim::RosterEntry>& ClientRuntime::roster() const {
    return roster_;
}

const sim::TeamScores& ClientRuntime::teamScores() const {
    return teamScores_;
}

std::string ClientRuntime::teamScoreSummary() const {
    return client::PresentationStateSubsystem::teamScoreSummary(buildClientViewState());
}

std::vector<std::string> ClientRuntime::compactHudLines() const {
    return client::PresentationStateSubsystem::compactHudLines(buildClientViewState());
}

std::vector<std::string> ClientRuntime::scoreboardLines() const {
    return client::PresentationStateSubsystem::scoreboardLines(buildClientViewState());
}

const WorldSnapshot* ClientRuntime::latestSnapshot() const {
    return hasSnapshot_ ? &latestSnapshot_ : nullptr;
}

bool ClientRuntime::handleBackAction() {
    if (keyboardOverlayVisible_) {
        closeKeyboardOverlay();
        return true;
    }

    if (detachedObserverActive_ && !sessionSpectatorObserverLocked_) {
        toggleSpectatorMode();
        return true;
    }

    if (teamMenuVisible_) {
        teamMenuVisible_ = false;
        scoreboardVisible_ = false;
        lastCombatEventText_ = "Cancelled team change";
        return true;
    }

    if (runtimeSettingsVisible_) {
        runtimeSettingsVisible_ = false;
        setUiMode(false);
        lastCombatEventText_ = "Closed runtime settings";
        return true;
    }

    if (scoreboardCursorActive_) {
        scoreboardCursorActive_ = false;
        scoreboardVisible_ = false;
        setUiMode(false);
        lastCombatEventText_ = "Closed host scoreboard controls";
        return true;
    }

    if (uiMode_) {
        setUiMode(false);
        lastCombatEventText_ = "Captured mouse";
        return true;
    }

    return false;
}

void ClientRuntime::setUiMode(bool active) {
    uiMode_ = active;
    if (!uiMode_) {
        runtimeSettingsOverlay_.resetInteraction();
    }
    if (!IsWindowReady()) {
        return;
    }
    if (uiMode_) {
        display::enableCursorPreservingPosition();
    } else {
        display::disableCursorForCapture();
    }
}

void ClientRuntime::openKeyboardOverlay() {
    if (keyboardOverlayVisible_) {
        return;
    }

    keyboardOverlayRestoreCapture_ = !uiMode_;
    keyboardOverlayVisible_ = true;
    setUiMode(true);
    scoreboardVisible_ = false;
    scoreboardCursorActive_ = false;
    lastCombatEventText_ = "Keyboard map shown";
}

void ClientRuntime::closeKeyboardOverlay(bool restoreCursorCapture) {
    if (!keyboardOverlayVisible_) {
        return;
    }

    keyboardOverlayVisible_ = false;
    const bool shouldRestoreCapture =
        restoreCursorCapture &&
        keyboardOverlayRestoreCapture_ &&
        !runtimeSettingsVisible_ &&
        !teamMenuVisible_ &&
        !scoreboardCursorActive_;
    keyboardOverlayRestoreCapture_ = false;
    if (shouldRestoreCapture) {
        setUiMode(false);
    }
    lastCombatEventText_ = "Closed keyboard map";
}

const sim::RosterEntry* ClientRuntime::findRosterEntry(int actorId) const {
    const auto it = std::find_if(roster_.begin(),
                                 roster_.end(),
                                 [actorId](const sim::RosterEntry& entry) {
                                     return entry.actorId == actorId;
                                 });
    return it != roster_.end() ? &(*it) : nullptr;
}

bool ClientRuntime::isLocalHost() const {
    const std::uint16_t authoritativeHostPeerId =
        authoritativeSessionMetadata_.hostPeerId != 0u
            ? authoritativeSessionMetadata_.hostPeerId
            : kAuthoritativeHostPeerId;
    return peerId_ != 0u && peerId_ == authoritativeHostPeerId;
}

bool ClientRuntime::shouldShowRuntimeSettingsTarget(std::uint16_t targetId) const {
    if (targetId == 0u) {
        return false;
    }

    if (isLocalHost()) {
        return true;
    }

    return targetId == peerId_;
}

bool ClientRuntime::canEditParticipantSyncSettings(std::uint16_t targetId) const {
    if (state_ != ClientConnectionState::Connected || targetId == 0u) {
        return false;
    }

    const sim::RosterEntry* entry = findRosterEntry(static_cast<int>(targetId));
    if (entry == nullptr || entry->isBot) {
        return false;
    }

    return isLocalHost();
}

bool ClientRuntime::canEditTransportSettings(std::uint16_t targetId) const {
    if (!hasLocalNetworkControls() || state_ != ClientConnectionState::Connected || targetId == 0u) {
        return false;
    }

    if (isBotTransportTargetId(targetId)) {
        return isLocalHost();
    }

    return isLocalHost() || targetId == peerId_;
}

bool ClientRuntime::shouldShowHostScoreboardAdmin() const {
    return scoreboardVisible_ &&
           state_ == ClientConnectionState::Connected &&
           isLocalHost();
}

std::vector<ClientRuntime::HostScoreboardAction> ClientRuntime::hostScoreboardActions() const {
    std::size_t targetCount = 0u;
    for (const sim::RosterEntry& entry : roster_) {
        if (canUseHostScoreboardTarget(entry, peerId_)) {
            ++targetCount;
        }
    }

    const int scoreboardRows = hostScoreboardTeamRowCount(roster_);
    const std::size_t visibleRows = hostScoreboardVisibleTargetRows(targetCount, scoreboardRows);
    const Rectangle panel = hostScoreboardAdminPanelBounds(targetCount, scoreboardRows);
    std::vector<HostScoreboardAction> actions;
    actions.reserve(3u + std::min<std::size_t>(targetCount, 7u) * 4u);
    actions.push_back(HostScoreboardAction{
        HostScoreboardActionKind::ToggleBots,
        0u,
        hostScoreboardBotPlayButtonBounds(panel),
        sim::TeamId::None,
        true
    });
    actions.push_back(HostScoreboardAction{
        HostScoreboardActionKind::ToggleBotPeace,
        0u,
        hostScoreboardBotPeaceButtonBounds(panel),
        sim::TeamId::None,
        true
    });
    actions.push_back(HostScoreboardAction{
        HostScoreboardActionKind::AddBot,
        0u,
        hostScoreboardAddBotButtonBounds(panel),
        sim::TeamId::None,
        true
    });

    std::size_t rowIndex = 0u;
    for (const sim::RosterEntry& entry : roster_) {
        if (!canUseHostScoreboardTarget(entry, peerId_)) {
            continue;
        }
        if (rowIndex >= visibleRows) {
            break;
        }

        const std::uint16_t targetPeerId = static_cast<std::uint16_t>(entry.actorId);
        const Rectangle row = hostScoreboardAdminRowBounds(panel, rowIndex);
        actions.push_back(HostScoreboardAction{
            HostScoreboardActionKind::AssignAttacker,
            targetPeerId,
            hostScoreboardAdminButtonBounds(row, 0u),
            sim::TeamId::Attacker,
            entry.team != sim::TeamId::Attacker
        });
        actions.push_back(HostScoreboardAction{
            HostScoreboardActionKind::AssignDefender,
            targetPeerId,
            hostScoreboardAdminButtonBounds(row, 1u),
            sim::TeamId::Defender,
            entry.team != sim::TeamId::Defender
        });
        actions.push_back(HostScoreboardAction{
            HostScoreboardActionKind::AssignSpectator,
            targetPeerId,
            hostScoreboardAdminButtonBounds(row, 2u),
            sim::TeamId::Spectator,
            !entry.isBot && entry.team != sim::TeamId::Spectator
        });
        actions.push_back(HostScoreboardAction{
            HostScoreboardActionKind::Kick,
            targetPeerId,
            hostScoreboardAdminButtonBounds(row, 3u),
            sim::TeamId::None,
            true
        });
        ++rowIndex;
    }

    return actions;
}

bool ClientRuntime::handleHostScoreboardAdminClick() {
    if (!shouldShowHostScoreboardAdmin() ||
        !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return false;
    }

    const Vector2 mouse = display::mousePosition();
    for (const HostScoreboardAction& action : hostScoreboardActions()) {
        if (!action.enabled || !CheckCollisionPointRec(mouse, action.bounds)) {
            continue;
        }

        switch (action.kind) {
            case HostScoreboardActionKind::AssignAttacker:
            case HostScoreboardActionKind::AssignDefender:
            case HostScoreboardActionKind::AssignSpectator:
                return sendHostTeamAssignment(action.targetPeerId, action.assignedTeam);
            case HostScoreboardActionKind::Kick:
                return sendHostKick(action.targetPeerId);
            case HostScoreboardActionKind::AddBot:
                return sendHostAddBot();
            case HostScoreboardActionKind::ToggleBots:
                return sendHostToggleBots();
            case HostScoreboardActionKind::ToggleBotPeace:
                return sendHostToggleBotPeace();
        }
    }

    return false;
}

void ClientRuntime::renderHostScoreboardAdminPanel() const {
    if (!shouldShowHostScoreboardAdmin()) {
        return;
    }

    std::size_t targetCount = 0u;
    for (const sim::RosterEntry& entry : roster_) {
        if (canUseHostScoreboardTarget(entry, peerId_)) {
            ++targetCount;
        }
    }

    const int scoreboardRows = hostScoreboardTeamRowCount(roster_);
    const std::size_t visibleRows = hostScoreboardVisibleTargetRows(targetCount, scoreboardRows);
    const Rectangle panel = hostScoreboardAdminPanelBounds(targetCount, scoreboardRows);
    const Vector2 mouse = display::mousePosition();
    const std::vector<HostScoreboardAction> actions = hostScoreboardActions();

    DrawRectangleRounded(Rectangle{panel.x, panel.y + 6.0f, panel.width, panel.height},
                         0.06f,
                         10,
                         Fade(BLACK, 0.28f));
    DrawRectangleRounded(panel, 0.06f, 10, Fade(Color{13, 20, 31, 255}, 0.90f));
    DrawRectangleRoundedLines(panel, 0.06f, 10, Fade(Color{88, 184, 255, 255}, 0.72f));

    drawRuntimeShadowedText("HOST CONTROLS",
                            panel.x + 20.0f,
                            panel.y + 20.0f,
                            18,
                            Fade(WHITE, 0.78f));
    drawRuntimeShadowedText("Players & Bots",
                            panel.x + 20.0f,
                            panel.y + 48.0f,
                            24,
                            RAYWHITE);
    for (const HostScoreboardAction& action : actions) {
        if (action.kind != HostScoreboardActionKind::AddBot &&
            action.kind != HostScoreboardActionKind::ToggleBots &&
            action.kind != HostScoreboardActionKind::ToggleBotPeace) {
            continue;
        }
        const int actionKind = static_cast<int>(action.kind);
        const bool botPlayButton = action.kind == HostScoreboardActionKind::ToggleBots;
        const bool botPeaceButton = action.kind == HostScoreboardActionKind::ToggleBotPeace;
        const bool botsFrozen = hasAuthoritativeSessionMetadata_
            ? authoritativeSessionMetadata_.botsFrozen
            : true;
        const bool botsCanShoot = hasAuthoritativeSessionMetadata_
            ? authoritativeSessionMetadata_.botsCanShoot
            : true;
        const Color accent = botPlayButton && !botsFrozen
            ? Color{255, 184, 92, 255}
            : (botPeaceButton && botsCanShoot
                   ? Color{255, 184, 92, 255}
                   : hostScoreboardActionAccent(actionKind));
        const bool hovered = CheckCollisionPointRec(mouse, action.bounds);
        DrawRectangleRounded(action.bounds, 0.16f, 8, Fade(accent, hovered ? 0.82f : 0.62f));
        DrawRectangleRoundedLines(action.bounds, 0.16f, 8, Fade(accent, hovered ? 0.96f : 0.62f));
        if (botPeaceButton) {
            drawHostScoreboardPistolIcon(action.bounds, botsCanShoot, RAYWHITE);
            continue;
        }
        const char* label = botPlayButton
            ? (botsFrozen ? "PLAY" : "PAUSE")
            : hostScoreboardActionLabel(actionKind);
        drawCenteredRuntimeShadowedText(label,
                                        action.bounds.x + action.bounds.width * 0.5f,
                                        action.bounds.y + 9.0f,
                                        14,
                                        RAYWHITE,
                                        Fade(BLACK, 0.48f));
    }
    if (targetCount == 0u) {
        const Rectangle emptyRow = hostScoreboardAdminRowBounds(panel, 0u);
        DrawRectangleRounded(emptyRow, 0.08f, 8, Fade(WHITE, 0.055f));
        DrawRectangleRoundedLines(emptyRow, 0.08f, 8, Fade(WHITE, 0.18f));
        drawRuntimeText("No other players connected",
                        emptyRow.x + 14.0f,
                        emptyRow.y + 15.0f,
                        18,
                        Fade(LIGHTGRAY, 0.82f));
        return;
    }

    std::size_t rowIndex = 0u;
    for (const sim::RosterEntry& entry : roster_) {
        if (!canUseHostScoreboardTarget(entry, peerId_)) {
            continue;
        }
        if (rowIndex >= visibleRows) {
            break;
        }

        const Rectangle row = hostScoreboardAdminRowBounds(panel, rowIndex);
        DrawRectangleRounded(row, 0.08f, 8, Fade(WHITE, 0.055f));
        DrawRectangleRoundedLines(row, 0.08f, 8, Fade(teamTint(entry.team), 0.46f));

        const float firstButtonX = hostScoreboardAdminButtonBounds(row, 0u).x;
        const float labelMaxWidth = std::max(72.0f, firstButtonX - row.x - 16.0f);
        const std::string name =
            fitRuntimeText(actorDisplayLabel(roster_, entry.actorId), 18, labelMaxWidth);
        drawRuntimeShadowedText(name,
                                row.x + 12.0f,
                                row.y + 8.0f,
                                18,
                                RAYWHITE);
        drawRuntimeText(sim::toString(entry.team),
                        row.x + 12.0f,
                        row.y + 30.0f,
                        15,
                        Fade(LIGHTGRAY, 0.82f));

        for (const HostScoreboardAction& action : actions) {
            if (action.targetPeerId != static_cast<std::uint16_t>(entry.actorId)) {
                continue;
            }

            const int actionKind = static_cast<int>(action.kind);
            const Color accent = hostScoreboardActionAccent(actionKind);
            const bool hovered = action.enabled && CheckCollisionPointRec(mouse, action.bounds);
            const Color fill = action.enabled
                ? Fade(accent, hovered ? 0.78f : 0.55f)
                : Fade(WHITE, 0.11f);
            const Color border = action.enabled
                ? Fade(accent, hovered ? 0.95f : 0.58f)
                : Fade(WHITE, 0.18f);
            const Color labelColor = action.enabled ? RAYWHITE : Fade(LIGHTGRAY, 0.48f);

            DrawRectangleRounded(action.bounds, 0.16f, 8, fill);
            DrawRectangleRoundedLines(action.bounds, 0.16f, 8, border);
            drawCenteredRuntimeShadowedText(hostScoreboardActionLabel(actionKind),
                                            action.bounds.x + action.bounds.width * 0.5f,
                                            action.bounds.y + 7.0f,
                                            14,
                                            labelColor,
                                            Fade(BLACK, 0.48f));
        }

        ++rowIndex;
    }

    if (targetCount > visibleRows) {
        const std::string overflowLine =
            "+" + std::to_string(targetCount - visibleRows) + " more";
        drawRuntimeText(overflowLine,
                        panel.x + panel.width - 96.0f,
                        panel.y + panel.height - 24.0f,
                        15,
                        Fade(LIGHTGRAY, 0.72f));
    }
}

bool ClientRuntime::sendHostAddBot() {
    if (!isLocalHost() || state_ != ClientConnectionState::Connected) {
        return false;
    }

    const bool sent = sendRuntimeParamChangeRequest(
        RuntimeParamScope::Session,
        -1,
        "sv.admin_add_bot",
        static_cast<float>(static_cast<std::uint8_t>(sim::TeamId::None)));
    if (sent) {
        lastCombatEventText_ = "Requested bot add";
    }
    return sent;
}

bool ClientRuntime::sendHostToggleBots() {
    if (!isLocalHost() || state_ != ClientConnectionState::Connected) {
        return false;
    }

    const bool botsFrozen =
        hasAuthoritativeSessionMetadata_ ? authoritativeSessionMetadata_.botsFrozen : true;
    const bool requestedBotsActive = botsFrozen;
    const bool sent = sendRuntimeParamChangeRequest(
        RuntimeParamScope::Session,
        -1,
        "sv.bots_active",
        requestedBotsActive ? 1.0f : 0.0f);
    if (sent) {
        lastCombatEventText_ = requestedBotsActive
            ? "Requested bots active"
            : "Requested bots frozen";
    }
    return sent;
}

bool ClientRuntime::sendHostToggleBotPeace() {
    if (!isLocalHost() || state_ != ClientConnectionState::Connected) {
        return false;
    }

    const bool botsCanShoot =
        hasAuthoritativeSessionMetadata_ ? authoritativeSessionMetadata_.botsCanShoot : true;
    const bool requestedBotsCanShoot = !botsCanShoot;
    const bool sent = sendRuntimeParamChangeRequest(
        RuntimeParamScope::Session,
        -1,
        "sv.bots_can_shoot",
        requestedBotsCanShoot ? 1.0f : 0.0f);
    if (sent) {
        lastCombatEventText_ = requestedBotsCanShoot
            ? "Requested bots armed"
            : "Requested bot peace mode";
    }
    return sent;
}

bool ClientRuntime::sendHostTeamAssignment(std::uint16_t targetPeerId, sim::TeamId team) {
    if (!isLocalHost() ||
        state_ != ClientConnectionState::Connected ||
        targetPeerId == 0u ||
        targetPeerId == peerId_) {
        return false;
    }

    const RuntimeParamScope scope = runtimeParamScopeForTargetId(targetPeerId);
    const bool sent = sendRuntimeParamChangeRequest(
        scope,
        static_cast<std::int32_t>(targetPeerId),
        runtimeParamKeyForTarget(targetPeerId, "admin_team"),
        static_cast<float>(static_cast<std::uint8_t>(team)));
    if (sent) {
        lastCombatEventText_ =
            "Requested " + actorDisplayLabel(roster_, targetPeerId) +
            " to " + teamLabel(team);
    }
    return sent;
}

bool ClientRuntime::sendHostKick(std::uint16_t targetPeerId) {
    if (!isLocalHost() ||
        state_ != ClientConnectionState::Connected ||
        targetPeerId == 0u ||
        targetPeerId == peerId_) {
        return false;
    }
    if (findRosterEntry(static_cast<int>(targetPeerId)) == nullptr) {
        return false;
    }

    const RuntimeParamScope scope = runtimeParamScopeForTargetId(targetPeerId);
    const bool sent = sendRuntimeParamChangeRequest(
        scope,
        static_cast<std::int32_t>(targetPeerId),
        runtimeParamKeyForTarget(targetPeerId, "admin_kick"),
        1.0f);
    if (sent) {
        lastCombatEventText_ = "Requested kick for " + actorDisplayLabel(roster_, targetPeerId);
    }
    return sent;
}

void ClientRuntime::syncDiagnosticsAuthoritativeTargetState() {
    if (diagnosticsModel_ == nullptr || runtimeSettingsTargetId_ == 0u) {
        return;
    }

    const sim::RosterEntry* entry = findRosterEntry(static_cast<int>(runtimeSettingsTargetId_));
    if (entry == nullptr) {
        return;
    }

    diagnosticsModel_->syncAuthoritativeLocalNetworkSettings(entry->latencyMs, entry->lossPct);
}

void ClientRuntime::applyAuthoritativeLocalParticipantSettings() {
    if (!hasSnapshot_ || peerId_ == 0u) {
        return;
    }

    auto rosterIt = std::find_if(roster_.begin(),
                                 roster_.end(),
                                 [this](const sim::RosterEntry& entry) {
                                     return entry.actorId == static_cast<int>(peerId_);
                                 });
    if (rosterIt == roster_.end() || rosterIt->isBot) {
        return;
    }

    const ParticipantRuntimeSettingsState nextAuthoritative{
        rosterIt->interpolationEnabled,
        rosterIt->predictionEnabled,
        rosterIt->reconciliationStrategy,
        rosterIt->smoothCorrectionWindowMs};
    const ParticipantRuntimeSettingsState currentLocal{
        interpolationEnabled_,
        predictionEnabled_,
        toRuntimeReconciliationStrategy(reconciliationStrategy_),
        smoothCorrectionWindowMs_};

    const bool pendingLocalOverride =
        authoritativeLocalParticipantSettings_.has_value() &&
        (currentLocal.interpolationEnabled != authoritativeLocalParticipantSettings_->interpolationEnabled ||
         currentLocal.predictionEnabled != authoritativeLocalParticipantSettings_->predictionEnabled ||
         currentLocal.reconciliationStrategy != authoritativeLocalParticipantSettings_->reconciliationStrategy ||
         currentLocal.smoothCorrectionWindowMs !=
             authoritativeLocalParticipantSettings_->smoothCorrectionWindowMs);
    authoritativeLocalParticipantSettings_ = nextAuthoritative;

    const bool authoritativeMatchesCurrentLocal =
        currentLocal.interpolationEnabled == nextAuthoritative.interpolationEnabled &&
        currentLocal.predictionEnabled == nextAuthoritative.predictionEnabled &&
        currentLocal.reconciliationStrategy == nextAuthoritative.reconciliationStrategy &&
        currentLocal.smoothCorrectionWindowMs == nextAuthoritative.smoothCorrectionWindowMs;
    if (!pendingLocalOverride || authoritativeMatchesCurrentLocal) {
        const bool previousPredictionEnabled = predictionEnabled_;
        interpolationEnabled_ = nextAuthoritative.interpolationEnabled;
        predictionEnabled_ = nextAuthoritative.predictionEnabled;
        reconciliationStrategy_ =
            toPredictionReconciliationStrategy(nextAuthoritative.reconciliationStrategy);
        smoothCorrectionWindowMs_ = nextAuthoritative.smoothCorrectionWindowMs;

        if (previousPredictionEnabled != predictionEnabled_) {
            localPlayerState_ = predictionEnabled_
                ? predictionBuffer_.displayState()
                : latestSnapshot_.localPlayerState;
        }
    } else {
        rosterIt->interpolationEnabled = currentLocal.interpolationEnabled;
        rosterIt->predictionEnabled = currentLocal.predictionEnabled;
        rosterIt->reconciliationStrategy = currentLocal.reconciliationStrategy;
        rosterIt->smoothCorrectionWindowMs = currentLocal.smoothCorrectionWindowMs;
    }
}

void ClientRuntime::previewLocalParticipantRuntimeSetting(
    RuntimeSettingsOverlay::ControlId controlId,
    float value) {
    if (peerId_ == 0u) {
        return;
    }

    auto rosterIt = std::find_if(roster_.begin(),
                                 roster_.end(),
                                 [this](const sim::RosterEntry& entry) {
                                     return entry.actorId == static_cast<int>(peerId_);
                                 });

    switch (controlId) {
        case RuntimeSettingsOverlay::ControlId::Interpolation: {
            const bool enabled = value >= 0.5f;
            interpolationEnabled_ = enabled;
            if (rosterIt != roster_.end()) {
                rosterIt->interpolationEnabled = enabled;
            }
            return;
        }
        case RuntimeSettingsOverlay::ControlId::Prediction: {
            const bool enabled = value >= 0.5f;
            const bool previousPredictionEnabled = predictionEnabled_;
            predictionEnabled_ = enabled;
            if (rosterIt != roster_.end()) {
                rosterIt->predictionEnabled = enabled;
            }
            if (previousPredictionEnabled != predictionEnabled_ && hasSnapshot_) {
                localPlayerState_ = predictionEnabled_
                    ? predictionBuffer_.displayState()
                    : latestSnapshot_.localPlayerState;
                localPlayerVisual_->setSimState(localPlayerState_);
            }
            return;
        }
        case RuntimeSettingsOverlay::ControlId::ReconciliationStrategy: {
            const sim::RuntimeReconciliationStrategy strategy =
                value <= static_cast<float>(sim::RuntimeReconciliationStrategy::Snap)
                    ? sim::RuntimeReconciliationStrategy::Snap
                    : sim::RuntimeReconciliationStrategy::Smooth;
            reconciliationStrategy_ = toPredictionReconciliationStrategy(strategy);
            if (rosterIt != roster_.end()) {
                rosterIt->reconciliationStrategy = strategy;
            }
            return;
        }
        case RuntimeSettingsOverlay::ControlId::SmoothWindowMs: {
            smoothCorrectionWindowMs_ = static_cast<std::uint32_t>(std::clamp(
                std::lround(value),
                0l,
                1000l));
            if (rosterIt != roster_.end()) {
                rosterIt->smoothCorrectionWindowMs = smoothCorrectionWindowMs_;
            }
            return;
        }
        default:
            return;
    }
}

void ClientRuntime::syncDiagnosticsTargetPeerId() {
    if (peerId_ == 0u) {
        return;
    }
    if (runtimeSettingsTargetId_ == 0u) {
        runtimeSettingsTargetId_ = peerId_;
    }
    if (diagnosticsModel_ != nullptr && diagnosticsModel_->targetPeerId() == 0u) {
        diagnosticsModel_->setTargetPeerId(runtimeSettingsTargetId_);
        syncDiagnosticsAuthoritativeTargetState();
    }
}

void ClientRuntime::attachProxyDiagnostics(TransportArtifactAdapter* proxy, std::uint16_t targetPeerId) {
    hostProxy_ = proxy;

    if (hostProxy_ == nullptr) {
        diagnosticsModel_.reset();
        return;
    }

    const std::uint16_t resolvedTargetPeerId = targetPeerId != 0u ? targetPeerId : peerId_;
    runtimeSettingsTargetId_ = resolvedTargetPeerId;
    if (!diagnosticsModel_) {
        diagnosticsModel_ = std::make_unique<DiagnosticsModel>(resolvedTargetPeerId);
    } else {
        diagnosticsModel_->setTargetPeerId(resolvedTargetPeerId);
    }

    syncDiagnosticsAuthoritativeTargetState();
    refreshDiagnostics();
}

void ClientRuntime::detachProxyDiagnostics() {
    hostProxy_ = nullptr;
    diagnosticsModel_.reset();
}

void ClientRuntime::setCommandReplayStatus(bool available,
                                           bool recording,
                                           std::size_t eventCount) {
    commandReplayStatusAvailable_ = available;
    commandReplayRecordingActive_ = recording;
    commandReplayEventCount_ = eventCount;
}

bool ClientRuntime::hasLocalNetworkControls() const {
    return diagnosticsModel_ != nullptr && hostProxy_ != nullptr;
}

bool ClientRuntime::hasHostDiagnostics() const {
    return diagnosticsModel_ != nullptr && hostProxy_ != nullptr && isLocalHost();
}

bool ClientRuntime::localNetworkPanelVisible() const {
    return runtimeSettingsVisible_;
}

float ClientRuntime::localNetworkLatencyMs() const {
    return diagnosticsModel_ != nullptr ? diagnosticsModel_->localNetworkSettings().latencyMs : 0.0f;
}

float ClientRuntime::localNetworkLossPct() const {
    return diagnosticsModel_ != nullptr ? diagnosticsModel_->localNetworkSettings().lossPct : 0.0f;
}

void ClientRuntime::setLocalNetworkSettingsForTest(float latencyMs, float lossPct) {
    if (!hasLocalNetworkControls()) {
        return;
    }

    diagnosticsModel_->setLocalLatencyMs(latencyMs);
    diagnosticsModel_->setLocalLossPct(lossPct);
    applyLocalNetworkSettings();
    refreshDiagnostics();
}

RuntimeSettingsOverlay::State ClientRuntime::runtimeSettingsOverlayStateForTest() const {
    return buildRuntimeSettingsOverlayState();
}

std::size_t ClientRuntime::combatTraceCount() const {
    return combatTraces_.size();
}

const std::string& ClientRuntime::lastCombatEventText() const {
    return lastCombatEventText_;
}

void ClientRuntime::setLastCombatEventText(std::string text) {
    lastCombatEventText_ = std::move(text);
}

const DiagnosticsModel* ClientRuntime::diagnosticsModel() const {
    return diagnosticsModel_.get();
}

client::ClientViewState ClientRuntime::clientViewState() const {
    return buildClientViewState();
}

std::vector<sim::RemoteActorState> ClientRuntime::sampleRemoteActorsForPresentation(
    const InterpolationBuffer& buffer,
    const std::vector<sim::RemoteActorState>& newestSnapshot,
    std::uint64_t targetServerTimeUs,
    bool interpolationEnabled) {
    return client::ClientSyncRuntime::sampleRemoteActorsForPresentation(
        buffer,
        newestSnapshot,
        targetServerTimeUs,
        interpolationEnabled);
}

void ClientRuntime::drainIncomingPackets() {
    if (!socket_.isOpen()) {
        return;
    }

    ReceivedDatagram datagram;
    while (true) {
        const ReceiveStatus status = socket_.receive(&datagram);
        if (status == ReceiveStatus::WouldBlock) {
            return;
        }
        if (status == ReceiveStatus::Error) {
            state_ = ClientConnectionState::Rejected;
            statusMessage_ = socket_.lastError();
            return;
        }

        const ParseResult parseResult = deserializePacket(datagram.payload);
        if (!parseResult.ok) {
            continue;
        }

        handlePacket(parseResult.packet);
    }
}

void ClientRuntime::handlePacket(const Packet& packet) {
    switch (packet.header.kind) {
        case PacketKind::Welcome: {
            lastServerPacketUs_ = clockUs_;
            const auto& welcome = std::get<WelcomeMessage>(packet.payload);
            if (welcome.sessionId != config_.sessionId || welcome.assignedPeerId == 0u) {
                return;
            }
            if (peerId_ != 0u && welcome.assignedPeerId != peerId_) {
                return;
            }

            std::string levelFailure;
            if (!applyAuthoritativeLevelIdentity(arena_.get(),
                                                 &environment_,
                                                 welcome.levelSlot,
                                                 welcome.levelHash,
                                                 &levelFailure)) {
                state_ = ClientConnectionState::Rejected;
                statusMessage_ = levelFailure;
                return;
            }

            peerId_ = welcome.assignedPeerId;
            snapshotRateHz_ = welcome.snapshotRateHz;
            authoritativeLevelSlot_ = welcome.levelSlot;
            authoritativeLevelHash_ = welcome.levelHash;
            authoritativeSessionMetadata_ = welcome.sessionMetadata;
            authoritativeSessionMetadata_.levelSlot = welcome.levelSlot;
            authoritativeSessionMetadata_.levelHash = welcome.levelHash;
            hasAuthoritativeSessionMetadata_ = hasMeaningfulHostedSessionMetadata(authoritativeSessionMetadata_);
            syncDiagnosticsTargetPeerId();
            state_ = ClientConnectionState::Connected;
            statusMessage_ = "connected to " + config_.serverHost + ":" + std::to_string(config_.serverPort);
            return;
        }

        case PacketKind::WorldSnapshot: {
            lastServerPacketUs_ = clockUs_;
            if (peerId_ != 0u && packet.header.peerId != peerId_) {
                return;
            }
            if (hasSnapshot_ && !isNewerSequence(packet.header.seq, lastSnapshotPacketSeq_)) {
                return;
            }

            lastSnapshotPacketSeq_ = packet.header.seq;
            applySnapshot(std::get<WorldSnapshot>(packet.payload));
            return;
        }

        case PacketKind::ControlWorldSnapshot: {
            return;
        }

        case PacketKind::Disconnect: {
            lastServerPacketUs_ = clockUs_;
            const auto& disconnect = std::get<DisconnectMessage>(packet.payload);
            state_ = ClientConnectionState::Rejected;
            statusMessage_ = disconnect.reason.empty() ? "server rejected connection" : disconnect.reason;
            return;
        }

        case PacketKind::RuntimeParamSnapshot:
            lastServerPacketUs_ = clockUs_;
            if (diagnosticsModel_ != nullptr &&
                diagnosticsModel_->consumeRuntimeParamSnapshot(
                    std::get<RuntimeParamSnapshot>(packet.payload))) {
                refreshDiagnostics();
            }
            return;

        case PacketKind::RuntimeParamApplyResult:
        {
            lastServerPacketUs_ = clockUs_;
            const RuntimeParamApplyResult& result =
                std::get<RuntimeParamApplyResult>(packet.payload);
            if (result.scope == RuntimeParamScope::Session && result.key == "sv.tickrate") {
                std::uint16_t stagedTickRateHz = 0u;
                if (!result.applied &&
                    result.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
                    tryParseSessionTickRateHzValue(result.value, &stagedTickRateHz)) {
                    stagedSessionTickRateHz_ = stagedTickRateHz;
                    lastCombatEventText_ =
                        "Tick rate staged for the next authoritative tick: " +
                        std::to_string(stagedTickRateHz) + " Hz";
                }
            }
            if (result.scope == RuntimeParamScope::Session && result.key == "sv.snapshot_rate") {
                std::uint16_t stagedSnapshotRateHz = 0u;
                if (!result.applied &&
                    result.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
                    tryParseSessionSnapshotRateHzValue(result.value, &stagedSnapshotRateHz)) {
                    stagedSessionSnapshotRateHz_ = stagedSnapshotRateHz;
                    lastCombatEventText_ =
                        "Snapshot rate staged for the next authoritative tick: " +
                        std::to_string(stagedSnapshotRateHz) + " Hz";
                }
            }
            if (result.scope == RuntimeParamScope::Session && result.key == "sv.bots_active") {
                if (result.applied) {
                    lastCombatEventText_ =
                        result.value >= 0.5f ? "Bots active" : "Bots frozen";
                } else if (result.message == "host_only") {
                    lastCombatEventText_ = "Only the host can toggle bots";
                }
            }
            if (result.scope == RuntimeParamScope::Session && result.key == "sv.bots_can_shoot") {
                if (result.applied) {
                    authoritativeSessionMetadata_.botsCanShoot = result.value >= 0.5f;
                    lastCombatEventText_ =
                        authoritativeSessionMetadata_.botsCanShoot ? "Bots armed" : "Bot peace mode";
                } else if (result.message == "host_only") {
                    lastCombatEventText_ = "Only the host can toggle bot peace mode";
                }
            }
            if (result.scope == RuntimeParamScope::Session && result.key == "sv.event_logging") {
                if (result.applied) {
                    config_.studyEventLoggingEnabled = result.value >= 0.5f;
                    authoritativeSessionMetadata_.studyEventLoggingEnabled =
                        config_.studyEventLoggingEnabled;
                    const std::string_view resultMessageView{result.message};
                    if (config_.studyEventLoggingEnabled &&
                        resultMessageView.substr(0u, kEventLoggingAppliedPrefix.size()) ==
                            kEventLoggingAppliedPrefix) {
                        authoritativeSessionMetadata_.studyEventRunId =
                            telemetry::sanitizeRunId(result.message.substr(
                                kEventLoggingAppliedPrefix.size()));
                    }
                    if (!config_.studyEventLoggingEnabled) {
                        authoritativeSessionMetadata_.studyEventLoggingEnabled = false;
                        perceptionMonitor_.reset();
                    }
                    lastCombatEventText_ = config_.studyEventLoggingEnabled
                        ? "Event logging: " + studyEventLogDirectoryLabel()
                        : "Event logging disabled";
                } else if (result.message == "host_only") {
                    lastCombatEventText_ = "Only the host can toggle event logging";
                }
            }
            if (result.scope == RuntimeParamScope::Session && result.key == "sv.admin_add_bot") {
                if (result.applied) {
                    lastCombatEventText_ = result.targetId > 0
                        ? "Bot added: " + actorDisplayLabel(roster_, result.targetId)
                        : "Bot added";
                } else if (result.message == "host_only") {
                    lastCombatEventText_ = "Only the host can add bots";
                } else {
                    lastCombatEventText_ = "Bot add rejected";
                }
            }
            if (result.scope == RuntimeParamScope::Player &&
                result.targetId > 0 &&
                result.targetId <= static_cast<std::int32_t>(
                    std::numeric_limits<std::uint16_t>::max())) {
                const std::uint16_t targetPeerId =
                    static_cast<std::uint16_t>(result.targetId);
                if (result.key == runtimeParamKeyForTarget(targetPeerId, "admin_team")) {
                    lastCombatEventText_ = result.applied
                        ? "Team updated for " + actorDisplayLabel(roster_, targetPeerId)
                        : "Team update rejected";
                } else if (result.key == runtimeParamKeyForTarget(targetPeerId, "admin_kick")) {
                    lastCombatEventText_ = result.applied
                        ? "Player kicked"
                        : "Kick rejected";
                }
            }
            if (diagnosticsModel_ != nullptr &&
                diagnosticsModel_->consumeRuntimeParamApplyResult(result)) {
                refreshDiagnostics();
            }
            return;
        }

        case PacketKind::SessionActionResult: {
            lastServerPacketUs_ = clockUs_;
            const SessionActionResult& result =
                std::get<SessionActionResult>(packet.payload);
            lastCombatEventText_ = sessionActionStatusText(result);
            return;
        }

        default:
            return;
    }
}

void ClientRuntime::applySnapshot(const WorldSnapshot& snapshot) {
    const float previousHealth = localPlayerState_.health;
    authoritativeSessionMetadata_ = snapshot.sessionMetadata;
    hasAuthoritativeSessionMetadata_ =
        hasMeaningfulHostedSessionMetadata(authoritativeSessionMetadata_);
    if (stagedSessionTickRateHz_.has_value() &&
        snapshot.cadence.authoritativeTickHz == *stagedSessionTickRateHz_) {
        lastCombatEventText_ =
            "Live tick rate now " + std::to_string(snapshot.cadence.authoritativeTickHz) + " Hz";
        stagedSessionTickRateHz_.reset();
    }
    if (stagedSessionSnapshotRateHz_.has_value() &&
        snapshot.cadence.snapshotCadenceHz == *stagedSessionSnapshotRateHz_) {
        lastCombatEventText_ =
            "Live snapshot rate now " + std::to_string(snapshot.cadence.snapshotCadenceHz) + " Hz";
        stagedSessionSnapshotRateHz_.reset();
    }
    lastCorrection_ = syncRuntime_.applySnapshot(syncContext(),
                                                 snapshot,
                                                 environment_,
                                                 simConfig_,
                                                 &config_.preferredTeam,
                                                 reconciliationStrategy_,
                                                 smoothCorrectionWindowMs_);
    syncDiagnosticsTargetPeerId();
    syncDiagnosticsAuthoritativeTargetState();
    applyAuthoritativeLocalParticipantSettings();
    updateLocalDeathState(snapshot, previousHealth);
    localPlayerVisual_->setSimState(localPlayerState_);
    consumeCombatEvents(snapshot);

    latestControlSnapshot_ = snapshot;
    latestControlSnapshot_.remotePlayers = snapshot.controlRemotePlayers;
    latestControlSnapshotReceiveUs_ = clockUs_;
    hasControlSnapshot_ = !snapshot.controlRemotePlayers.empty();
    if (hasControlSnapshot_) {
        controlRemotePlayerInterpolation_.pushSnapshot(
            latestControlSnapshot_.serverTimeUs,
            remoteActorsFromPlayerStates(latestControlSnapshot_.remotePlayers, simConfig_));
    } else {
        controlRemotePlayerInterpolation_.reset();
        controlRemotePlayers_.clear();
    }

    if (!viewInitialized_) {
        viewYaw_ = snapshot.localPlayerState.yaw;
        viewPitch_ = snapshot.localPlayerState.pitch;
        viewInitialized_ = true;
    }

    syncSessionSpectatorObserverState(snapshot);

    updateInterpolatedRemoteEntities();
}

void ClientRuntime::applyControlSnapshot(const ControlWorldSnapshot& snapshot) {
    latestControlSnapshot_ = snapshot.snapshot;
    latestControlSnapshotReceiveUs_ = clockUs_;
    hasControlSnapshot_ = true;
    controlRemotePlayerInterpolation_.pushSnapshot(
        latestControlSnapshot_.serverTimeUs,
        remoteActorsFromPlayerStates(latestControlSnapshot_.remotePlayers, simConfig_));
    updateInterpolatedControlRemotePlayers();
}

void ClientRuntime::updateLocalDeathState(const WorldSnapshot& snapshot, float previousHealth) {
    const bool wasAlive = previousHealth > 0.0f;
    const bool isAlive = snapshot.localPlayerState.health > 0.0f;

    if (!isAlive) {
        predictionBuffer_.reset(snapshot.localPlayerState);
        localPlayerState_ = snapshot.localPlayerState;
        if (wasAlive || !localRespawnPending_) {
            localRespawnPending_ = true;
            localDeathStartUs_ = clockUs_;
            lastCombatEventText_ = "You are down";
        }
        return;
    }

    if (!wasAlive || localRespawnPending_) {
        predictionBuffer_.reset(snapshot.localPlayerState);
        localPlayerState_ = snapshot.localPlayerState;
    }
    localRespawnPending_ = false;
    localDeathStartUs_ = 0u;
}

void ClientRuntime::updateInterpolatedRemoteEntities() {
    syncRuntime_.updateInterpolatedRemoteEntities(syncContext(), simConfig_);
    updateInterpolatedControlRemotePlayers();
    syncRemoteVisuals();
    recordClientPerceptionEvents();
}

void ClientRuntime::updateInterpolatedControlRemotePlayers() {
    if (!hasControlSnapshot_) {
        controlRemotePlayers_.clear();
        return;
    }

    const std::uint64_t estimatedServerTimeUs =
        latestControlSnapshot_.serverTimeUs + (clockUs_ - latestControlSnapshotReceiveUs_);
    const std::uint64_t interpolationDelayUs =
        static_cast<std::uint64_t>(defaultInterpolationDelayMs()) * 1'000u;
    const std::uint64_t targetServerTimeUs =
        estimatedServerTimeUs > interpolationDelayUs
            ? estimatedServerTimeUs - interpolationDelayUs
            : 0u;
    const std::vector<sim::RemoteActorState> interpolatedPlayers =
        client::ClientSyncRuntime::sampleRemoteActorsForPresentation(
            controlRemotePlayerInterpolation_,
            remoteActorsFromPlayerStates(latestControlSnapshot_.remotePlayers, simConfig_),
            targetServerTimeUs,
            interpolationEnabled_);
    controlRemotePlayers_.clear();
    controlRemotePlayers_.reserve(interpolatedPlayers.size());
    for (const auto& playerActor : interpolatedPlayers) {
        controlRemotePlayers_.push_back(
            playerStateFromInterpolatedActor(
                playerActor, latestControlSnapshot_.remotePlayers, simConfig_));
    }
}

bool ClientRuntime::localPlayerAwaitingRespawn() const {
    return hasSnapshot_ && (localRespawnPending_ || localPlayerState_.health <= 0.0f);
}

void ClientRuntime::recordCombatTrace(const sim::HitscanRay& ray, bool authoritative, bool hit) {
    const Vector3 start{ray.origin.x, ray.origin.y, ray.origin.z};
    const Vector3 end{
        ray.origin.x + (ray.direction.x * ray.maxDistance),
        ray.origin.y + (ray.direction.y * ray.maxDistance),
        ray.origin.z + (ray.direction.z * ray.maxDistance)
    };
    combatTraces_.emplace_back(
        LaserBeam3D(
            start,
            end,
            combatTraceColor(authoritative, hit),
            kCombatBeamLifetimeSeconds,
            kCombatBeamThickness,
            false,
            hit),
        clockUs_,
        authoritative,
        hit);
}

void ClientRuntime::consumeCombatEvents(const WorldSnapshot& snapshot) {
    for (const SnapshotEvent& event : snapshot.events) {
        ++consumedCombatEventCount_;
        switch (event.kind) {
            case SnapshotEventKind::WeaponFired:
                recordCombatTrace(
                    sim::HitscanRay{event.origin, event.direction, simConfig_.weaponRange},
                    true,
                    event.hit);
                lastCombatEventText_ = event.hit
                    ? "Authoritative shot confirmed a hit"
                    : "Authoritative shot confirmed a miss";
                break;

            case SnapshotEventKind::ConfirmedHit:
                lastCombatEventText_ = "Hit entity " + std::to_string(event.targetEntityId);
                break;

            case SnapshotEventKind::DamageApplied:
                lastCombatEventText_ = "Damage applied to entity " + std::to_string(event.targetEntityId);
                break;

            case SnapshotEventKind::PlayerRespawned:
                lastCombatEventText_ = "Player respawned";
                break;

            case SnapshotEventKind::PlayerKilled: {
                const sim::RosterEntry* attacker = findRosterEntry(event.sourcePlayerId);
                const sim::RosterEntry* victim = findRosterEntry(event.targetEntityId);

                client::KillFeedEntryView entry;
                entry.attackerLabel = actorDisplayLabel(roster_, event.sourcePlayerId);
                entry.victimLabel = actorDisplayLabel(roster_, event.targetEntityId);
                entry.attackerTeam = attacker != nullptr ? attacker->team : sim::TeamId::None;
                entry.victimTeam = victim != nullptr ? victim->team : sim::TeamId::None;
                entry.attackerIsLocalPlayer =
                    event.sourcePlayerId == static_cast<int>(peerId_);
                entry.victimIsLocalPlayer =
                    event.targetEntityId == static_cast<int>(peerId_);
                killFeed_.insert(killFeed_.begin(), KillFeedItem{entry, clockUs_});
                pruneKillFeed();

                if (entry.victimIsLocalPlayer) {
                    lastCombatEventText_ = entry.attackerLabel + " eliminated you";
                } else if (entry.attackerIsLocalPlayer) {
                    lastCombatEventText_ = "You eliminated " + entry.victimLabel;
                } else {
                    lastCombatEventText_ =
                        entry.attackerLabel + " eliminated " + entry.victimLabel;
                }
                break;
            }
        }
    }
}

void ClientRuntime::pruneCombatTraces() {
    combatTraces_.erase(
        std::remove_if(combatTraces_.begin(),
                       combatTraces_.end(),
                       [this](const CombatTrace& trace) {
                           return clockUs_ > trace.createdAtUs &&
                                  (clockUs_ - trace.createdAtUs) > kCombatTraceLifetimeUs;
                       }),
        combatTraces_.end());
}

void ClientRuntime::pruneKillFeed() {
    killFeed_.erase(
        std::remove_if(killFeed_.begin(),
                       killFeed_.end(),
                       [this](const KillFeedItem& item) {
                           return clockUs_ > item.createdAtUs &&
                                  (clockUs_ - item.createdAtUs) > kKillFeedLifetimeUs;
                       }),
        killFeed_.end());
    if (killFeed_.size() > kMaxKillFeedEntries) {
        killFeed_.resize(kMaxKillFeedEntries);
    }
}

void ClientRuntime::applyStudyPresentationState(float dtSeconds) {
    if (arena_ == nullptr) {
        return;
    }

    const Arena3D::AreaFilter desiredFilter =
        arenaAreaFilterFor(studyPresentation_.areaFilter);
    if (arena_->getAreasVisible() != studyPresentation_.areasVisible) {
        arena_->setAreasVisible(studyPresentation_.areasVisible);
    }
    if (arena_->getAreaFilter() != desiredFilter) {
        arena_->setAreaFilter(desiredFilter);
    }
    arena_->updateAreasFade(dtSeconds);
}

void ClientRuntime::applyRuntimeSettingsAction(const RuntimeSettingsOverlay::Action& action) {
    const std::uint16_t selectedTargetId =
        (runtimeSettingsTargetId_ != 0u && shouldShowRuntimeSettingsTarget(runtimeSettingsTargetId_))
            ? runtimeSettingsTargetId_
            : peerId_;
    if (diagnosticsModel_ != nullptr &&
        selectedTargetId != 0u &&
        diagnosticsModel_->targetPeerId() != selectedTargetId) {
        diagnosticsModel_->setTargetPeerId(selectedTargetId);
        syncDiagnosticsAuthoritativeTargetState();
        refreshDiagnostics();
    }
    const std::string selectedLabel =
        selectedTargetId != 0u ? actorDisplayLabel(roster_, static_cast<int>(selectedTargetId))
                               : std::string("participant");

    switch (action.kind) {
        case RuntimeSettingsOverlay::Action::Kind::None:
            return;
        case RuntimeSettingsOverlay::Action::Kind::Close:
            runtimeSettingsVisible_ = false;
            setUiMode(false);
            return;
        case RuntimeSettingsOverlay::Action::Kind::TargetSelected:
            setRuntimeSettingsTarget(action.targetId);
            return;
        case RuntimeSettingsOverlay::Action::Kind::ToggleChanged:
            switch (action.controlId) {
                case RuntimeSettingsOverlay::ControlId::Interpolation:
                    if (canEditParticipantSyncSettings(selectedTargetId)) {
                        sendRuntimeParamChangeRequest(
                            RuntimeParamScope::Player,
                            static_cast<std::int32_t>(selectedTargetId),
                            runtimeParamKeyForTarget(selectedTargetId, "interpolation_enabled"),
                            action.toggleValue ? 1.0f : 0.0f);
                        if (selectedTargetId == peerId_) {
                            previewLocalParticipantRuntimeSetting(
                                RuntimeSettingsOverlay::ControlId::Interpolation,
                                action.toggleValue ? 1.0f : 0.0f);
                        }
                        lastCombatEventText_ = "Requested " + selectedLabel +
                                               (action.toggleValue ? " interpolation enabled"
                                                                   : " interpolation disabled");
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::Prediction:
                    if (canEditParticipantSyncSettings(selectedTargetId)) {
                        sendRuntimeParamChangeRequest(
                            RuntimeParamScope::Player,
                            static_cast<std::int32_t>(selectedTargetId),
                            runtimeParamKeyForTarget(selectedTargetId, "prediction_enabled"),
                            action.toggleValue ? 1.0f : 0.0f);
                        if (selectedTargetId == peerId_) {
                            previewLocalParticipantRuntimeSetting(
                                RuntimeSettingsOverlay::ControlId::Prediction,
                                action.toggleValue ? 1.0f : 0.0f);
                        }
                        lastCombatEventText_ = "Requested " + selectedLabel +
                                               (action.toggleValue ? " prediction enabled"
                                                                   : " prediction disabled");
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::StudyEventLogging:
                    if (state_ == ClientConnectionState::Connected && isLocalHost()) {
                        sendRuntimeParamChangeRequest(RuntimeParamScope::Session,
                                                      -1,
                                                      "sv.event_logging",
                                                      action.toggleValue ? 1.0f : 0.0f);
                        lastCombatEventText_ = action.toggleValue
                            ? "Requested event logging enabled"
                            : "Requested event logging disabled";
                    }
                    return;
                default:
                    return;
            }
        case RuntimeSettingsOverlay::Action::Kind::ChoiceSelected:
            switch (action.controlId) {
                case RuntimeSettingsOverlay::ControlId::ReconciliationStrategy:
                    if (canEditParticipantSyncSettings(selectedTargetId)) {
                        sendRuntimeParamChangeRequest(
                            RuntimeParamScope::Player,
                            static_cast<std::int32_t>(selectedTargetId),
                            runtimeParamKeyForTarget(selectedTargetId,
                                                     "reconciliation_strategy"),
                            static_cast<float>(action.choiceValue));
                        if (selectedTargetId == peerId_) {
                            previewLocalParticipantRuntimeSetting(
                                RuntimeSettingsOverlay::ControlId::ReconciliationStrategy,
                                static_cast<float>(action.choiceValue));
                        }
                        lastCombatEventText_ = "Requested " + selectedLabel +
                                               " reconciliation strategy update";
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::ShotEvaluationMode:
                    if (state_ == ClientConnectionState::Connected && isLocalHost()) {
                        sendRuntimeParamChangeRequest(RuntimeParamScope::Session,
                                                      -1,
                                                      "sv.shot_mode",
                                                      static_cast<float>(action.choiceValue));
                        lastCombatEventText_ = "Shot evaluation update staged for next tick";
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::TickRate:
                    if (state_ == ClientConnectionState::Connected && isLocalHost()) {
                        sendRuntimeParamChangeRequest(RuntimeParamScope::Session,
                                                      -1,
                                                      "sv.tickrate",
                                                      static_cast<float>(action.choiceValue));
                        lastCombatEventText_ =
                            "Tick rate update staged for the next authoritative tick boundary";
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::SnapshotRate:
                    if (state_ == ClientConnectionState::Connected && isLocalHost()) {
                        sendRuntimeParamChangeRequest(RuntimeParamScope::Session,
                                                      -1,
                                                      "sv.snapshot_rate",
                                                      static_cast<float>(action.choiceValue));
                        lastCombatEventText_ =
                            "Snapshot rate update staged for the next authoritative tick boundary";
                    }
                    return;
                default:
                    return;
            }
        case RuntimeSettingsOverlay::Action::Kind::SliderChanged:
            switch (action.controlId) {
                case RuntimeSettingsOverlay::ControlId::SmoothWindowMs:
                    if (canEditParticipantSyncSettings(selectedTargetId)) {
                        sendRuntimeParamChangeRequest(
                            RuntimeParamScope::Player,
                            static_cast<std::int32_t>(selectedTargetId),
                            runtimeParamKeyForTarget(selectedTargetId,
                                                     "smooth_correction_window_ms"),
                            action.sliderValue);
                        if (selectedTargetId == peerId_) {
                            previewLocalParticipantRuntimeSetting(
                                RuntimeSettingsOverlay::ControlId::SmoothWindowMs,
                                action.sliderValue);
                        }
                        lastCombatEventText_ = "Requested " + selectedLabel +
                                               " smooth correction window update";
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::TargetLatency:
                    if (canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setLocalLatencyMs(action.sliderValue);
                        applyLocalNetworkSettings();
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::TargetLoss:
                    if (canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setLocalLossPct(action.sliderValue);
                        applyLocalNetworkSettings();
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::UpDelay:
                    if (hasHostDiagnostics() && canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setBaseDelayMs(true, action.sliderValue);
                        diagnosticsModel_->applyControl(hostProxy_, true);
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::DownDelay:
                    if (hasHostDiagnostics() && canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setBaseDelayMs(false, action.sliderValue);
                        diagnosticsModel_->applyControl(hostProxy_, false);
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::UpLoss:
                    if (hasHostDiagnostics() && canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setLossPct(true, action.sliderValue);
                        diagnosticsModel_->applyControl(hostProxy_, true);
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::DownLoss:
                    if (hasHostDiagnostics() && canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setLossPct(false, action.sliderValue);
                        diagnosticsModel_->applyControl(hostProxy_, false);
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::UpReorder:
                    if (hasHostDiagnostics() && canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setReorderPct(true, action.sliderValue);
                        diagnosticsModel_->applyControl(hostProxy_, true);
                        refreshDiagnostics();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::DownReorder:
                    if (hasHostDiagnostics() && canEditTransportSettings(selectedTargetId)) {
                        diagnosticsModel_->setReorderPct(false, action.sliderValue);
                        diagnosticsModel_->applyControl(hostProxy_, false);
                        refreshDiagnostics();
                    }
                    return;
                default:
                    return;
            }
    }
}

RuntimeSettingsOverlay::State ClientRuntime::buildRuntimeSettingsOverlayState() const {
    RuntimeSettingsOverlay::State state;
    state.visible = runtimeSettingsVisible_;

    const ShotEvaluationMode activeShotMode = hasAuthoritativeSessionMetadata_
        ? authoritativeSessionMetadata_.shotEvaluationMode
        : ShotEvaluationMode::SeenPosition;
    const std::uint16_t liveTickRateHz =
        hasSnapshot_ && latestSnapshot_.cadence.authoritativeTickHz > 0u
            ? latestSnapshot_.cadence.authoritativeTickHz
            : kDefaultSessionTickRateHz;
    const std::uint16_t displayedTickRateHz =
        stagedSessionTickRateHz_.value_or(liveTickRateHz);
    const std::uint16_t liveSnapshotRateHz =
        hasSnapshot_ && latestSnapshot_.cadence.snapshotCadenceHz > 0u
            ? latestSnapshot_.cadence.snapshotCadenceHz
            : snapshotRateHz_ > 0u
                ? snapshotRateHz_
                : kDefaultHostedSnapshotRateHz;
    const std::uint16_t displayedSnapshotRateHz =
        stagedSessionSnapshotRateHz_.value_or(liveSnapshotRateHz);
    const std::uint16_t hostPeerId =
        authoritativeSessionMetadata_.hostPeerId != 0u
            ? authoritativeSessionMetadata_.hostPeerId
            : kAuthoritativeHostPeerId;
    const std::string hostLabel =
        runtimeSettingsHostLabel(roster_, authoritativeSessionMetadata_, hostPeerId);
    state.subtitle = isLocalHost()
        ? "U closes | HOST | Full participant controls"
        : "U closes | Host: " + hostLabel + " | Your network controls";

    RuntimeSettingsOverlay::ControlState interpolation;
    interpolation.id = RuntimeSettingsOverlay::ControlId::Interpolation;
    interpolation.type = RuntimeSettingsOverlay::ControlType::Toggle;

    std::uint16_t selectedTargetId = runtimeSettingsTargetId_;
    if (selectedTargetId == 0u && peerId_ != 0u) {
        selectedTargetId = peerId_;
    }

    const sim::RosterEntry* selectedEntry = nullptr;
    state.targets.reserve(roster_.size());
    for (const sim::RosterEntry& entry : roster_) {
        if (entry.actorId < 0 || entry.actorId > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
            continue;
        }

        const std::uint16_t rowTargetId = static_cast<std::uint16_t>(entry.actorId);
        if (!shouldShowRuntimeSettingsTarget(rowTargetId)) {
            continue;
        }

        RuntimeSettingsOverlay::TargetRowState row;
        row.targetId = rowTargetId;
        row.selected = row.targetId == selectedTargetId;
        row.editable = canEditParticipantSyncSettings(row.targetId) ||
                       canEditTransportSettings(row.targetId);
        row.label = participantRuntimeLabel(entry,
                                            row.targetId == peerId_,
                                            row.targetId == hostPeerId);
        row.detailLine = participantRuntimeDetail(entry, row.selected, row.editable);
        row.metricsLine = participantRuntimeMetrics(entry);
        if (hostProxy_ != nullptr && transportTargetUsesProxyLink(row.targetId)) {
            row.statsLine = proxyStatsSummary(hostProxy_->peerStats(row.targetId, true));
        }
        state.targets.push_back(std::move(row));

        if (state.targets.back().selected) {
            selectedEntry = &entry;
        }
    }

    if (selectedEntry == nullptr && !state.targets.empty()) {
        selectedTargetId = state.targets.front().targetId;
        state.targets.front().selected = true;
        selectedEntry = findRosterEntry(static_cast<int>(selectedTargetId));
    }

    if (selectedEntry == nullptr) {
        return state;
    }

    const bool selectedIsHumanParticipant = !selectedEntry->isBot;
    const bool canEditSelectedParticipant = selectedIsHumanParticipant &&
        canEditParticipantSyncSettings(selectedTargetId);
    const bool canEditSelectedTransport = canEditTransportSettings(selectedTargetId);
    const bool selectedPredictionEnabled = selectedIsHumanParticipant
        ? selectedEntry->predictionEnabled
        : predictionEnabled_;
    const sim::RuntimeReconciliationStrategy selectedReconciliationStrategy =
        selectedIsHumanParticipant
            ? selectedEntry->reconciliationStrategy
            : toRuntimeReconciliationStrategy(reconciliationStrategy_);
    const std::uint32_t selectedSmoothCorrectionWindowMs = selectedIsHumanParticipant
        ? selectedEntry->smoothCorrectionWindowMs
        : smoothCorrectionWindowMs_;
    const bool canShowSelectedParticipantRuntimeControls =
        selectedIsHumanParticipant && canEditSelectedParticipant;

    if (canShowSelectedParticipantRuntimeControls) {
        interpolation.label = "Interpolation";
        interpolation.description =
            "Smooth remote actors between authoritative snapshots for the selected participant.";
        interpolation.toggleValue = selectedEntry->interpolationEnabled;
        interpolation.enabled = true;
        state.leftControls.push_back(interpolation);

        RuntimeSettingsOverlay::ControlState prediction;
        prediction.id = RuntimeSettingsOverlay::ControlId::Prediction;
        prediction.type = RuntimeSettingsOverlay::ControlType::Toggle;
        prediction.label = "Client-side Prediction";
        prediction.description =
            "Apply local movement immediately before server confirmation for the selected participant.";
        prediction.toggleValue = selectedPredictionEnabled;
        prediction.enabled = true;
        state.leftControls.push_back(prediction);

        RuntimeSettingsOverlay::ControlState reconciliation;
        reconciliation.id = RuntimeSettingsOverlay::ControlId::ReconciliationStrategy;
        reconciliation.type = RuntimeSettingsOverlay::ControlType::Choice;
        reconciliation.label = "Reconciliation Strategy";
        reconciliation.description =
            "Choose how visible prediction corrections are resolved for the selected participant.";
        reconciliation.enabled = selectedPredictionEnabled;
        reconciliation.choices = {
            makeChoiceState("Snap",
                            static_cast<int>(sim::RuntimeReconciliationStrategy::Snap),
                            selectedReconciliationStrategy ==
                                sim::RuntimeReconciliationStrategy::Snap,
                            reconciliation.enabled),
            makeChoiceState("Smooth",
                            static_cast<int>(sim::RuntimeReconciliationStrategy::Smooth),
                            selectedReconciliationStrategy ==
                                sim::RuntimeReconciliationStrategy::Smooth,
                            reconciliation.enabled)
        };
        state.leftControls.push_back(reconciliation);

        RuntimeSettingsOverlay::ControlState smoothWindow;
        smoothWindow.id = RuntimeSettingsOverlay::ControlId::SmoothWindowMs;
        smoothWindow.type = RuntimeSettingsOverlay::ControlType::Slider;
        smoothWindow.label = "Smooth Correction Window";
        smoothWindow.description =
            "How long smooth reconciliation should blend corrections for the selected participant.";
        smoothWindow.enabled =
            selectedPredictionEnabled &&
            selectedReconciliationStrategy == sim::RuntimeReconciliationStrategy::Smooth;
        smoothWindow.sliderMin = 0.0f;
        smoothWindow.sliderMax = 1000.0f;
        smoothWindow.sliderValue = static_cast<float>(selectedSmoothCorrectionWindowMs);
        smoothWindow.valueLabel = runtimeSettingsValueLabel(
            static_cast<float>(selectedSmoothCorrectionWindowMs), "ms");
        state.leftControls.push_back(smoothWindow);
    }

    if (state_ == ClientConnectionState::Connected && isLocalHost()) {
        RuntimeSettingsOverlay::ControlState eventLogging;
        eventLogging.id = RuntimeSettingsOverlay::ControlId::StudyEventLogging;
        eventLogging.type = RuntimeSettingsOverlay::ControlType::Toggle;
        eventLogging.label = "Event Logging";
        eventLogging.description =
            "Host only. Writes JSONL logs to " + studyEventLogDirectoryLabel();
        eventLogging.toggleValue = studyEventLoggingActive();
        eventLogging.valueLabel = studyEventLoggingActive()
            ? studyEventLogDirectoryLabel()
            : "logexports/" + telemetry::currentLocalDateStamp();
        eventLogging.enabled = true;
        state.leftControls.push_back(eventLogging);

        RuntimeSettingsOverlay::ControlState tickRate;
        tickRate.id = RuntimeSettingsOverlay::ControlId::TickRate;
        tickRate.type = RuntimeSettingsOverlay::ControlType::Choice;
        tickRate.label = "Tick Rate";
        tickRate.description =
            stagedSessionTickRateHz_.has_value() && *stagedSessionTickRateHz_ != liveTickRateHz
                ? "Host only. Live " + std::to_string(liveTickRateHz) +
                    " Hz | staged " + std::to_string(*stagedSessionTickRateHz_) +
                    " Hz for the next authoritative tick boundary."
                : "Host only. Live " + std::to_string(liveTickRateHz) +
                    " Hz. Changes apply on the next authoritative tick boundary.";
        tickRate.enabled = true;
        tickRate.choices.reserve(kSessionTickRateChoicesHz.size());
        for (const std::uint16_t tickRateHz : kSessionTickRateChoicesHz) {
            tickRate.choices.push_back(
                makeChoiceState((std::to_string(tickRateHz) + " Hz").c_str(),
                                static_cast<int>(tickRateHz),
                                displayedTickRateHz == tickRateHz,
                                true));
        }
        state.leftControls.push_back(tickRate);

        RuntimeSettingsOverlay::ControlState snapshotRate;
        snapshotRate.id = RuntimeSettingsOverlay::ControlId::SnapshotRate;
        snapshotRate.type = RuntimeSettingsOverlay::ControlType::Choice;
        snapshotRate.label = "Snapshot Rate";
        snapshotRate.description =
            stagedSessionSnapshotRateHz_.has_value() &&
                    *stagedSessionSnapshotRateHz_ != liveSnapshotRateHz
                ? "Host only. Live " + std::to_string(liveSnapshotRateHz) +
                    " Hz | staged " + std::to_string(*stagedSessionSnapshotRateHz_) +
                    " Hz for the next authoritative tick boundary."
                : "Host only. Live " + std::to_string(liveSnapshotRateHz) +
                    " Hz. Changes apply on the next authoritative tick boundary.";
        snapshotRate.enabled = true;
        snapshotRate.choices.reserve(kSessionTickRateChoicesHz.size());
        for (const std::uint16_t snapshotRateHz : kSessionTickRateChoicesHz) {
            snapshotRate.choices.push_back(
                makeChoiceState((std::to_string(snapshotRateHz) + " Hz").c_str(),
                                static_cast<int>(snapshotRateHz),
                                displayedSnapshotRateHz == snapshotRateHz,
                                snapshotRateHz <= displayedTickRateHz));
        }
        state.leftControls.push_back(snapshotRate);

        RuntimeSettingsOverlay::ControlState shotMode;
        shotMode.id = RuntimeSettingsOverlay::ControlId::ShotEvaluationMode;
        shotMode.type = RuntimeSettingsOverlay::ControlType::Choice;
        shotMode.label = "Shot Evaluation Strategy";
        shotMode.description =
            "Host only. The server applies changes on the next authoritative tick.";
        shotMode.enabled = true;
        shotMode.choices = {
            makeChoiceState("Seen Position",
                            static_cast<int>(ShotEvaluationMode::SeenPosition),
                            activeShotMode == ShotEvaluationMode::SeenPosition,
                            true),
            makeChoiceState("Live Position",
                            static_cast<int>(ShotEvaluationMode::LivePosition),
                            activeShotMode == ShotEvaluationMode::LivePosition,
                            true)
        };
        state.leftControls.push_back(shotMode);
    }

    const bool diagnosticsSelected =
        diagnosticsModel_ != nullptr &&
        diagnosticsModel_->targetPeerId() == selectedTargetId;
    const float editorLatency = diagnosticsSelected
        ? diagnosticsModel_->localNetworkSettings().latencyMs
        : static_cast<float>(selectedEntry->latencyMs);
    const float editorLoss = diagnosticsSelected
        ? diagnosticsModel_->localNetworkSettings().lossPct
        : static_cast<float>(selectedEntry->lossPct);

    state.targetEditor.available = true;
    state.targetEditor.title =
        participantRuntimeLabel(*selectedEntry,
                                selectedTargetId == peerId_,
                                selectedTargetId == hostPeerId);
    state.targetEditor.subtitle =
        participantRuntimeDetail(*selectedEntry,
                                 false,
                                 canShowSelectedParticipantRuntimeControls || canEditSelectedTransport) + " | " +
        participantRuntimeMetrics(*selectedEntry);
    const bool showEditableTransportControls =
        hasLocalNetworkControls() && canEditSelectedTransport;

    RuntimeSettingsOverlay::ControlState latency;
    latency.id = RuntimeSettingsOverlay::ControlId::TargetLatency;
    latency.type = RuntimeSettingsOverlay::ControlType::Slider;
    latency.label = "Latency";
    latency.description = "Authoritative runtime latency for the selected participant.";
    latency.visible = showEditableTransportControls;
    latency.enabled = showEditableTransportControls;
    latency.sliderMin = 0.0f;
    latency.sliderMax = 250.0f;
    latency.sliderValue = editorLatency;
    latency.valueLabel = runtimeSettingsValueLabel(editorLatency, "ms");
    state.targetEditor.latency = latency;

    RuntimeSettingsOverlay::ControlState loss;
    loss.id = RuntimeSettingsOverlay::ControlId::TargetLoss;
    loss.type = RuntimeSettingsOverlay::ControlType::Slider;
    loss.label = "Packet Loss";
    loss.description = "Authoritative packet loss percentage for the selected participant.";
    loss.visible = showEditableTransportControls;
    loss.enabled = showEditableTransportControls;
    loss.sliderMin = 0.0f;
    loss.sliderMax = 100.0f;
    loss.sliderValue = editorLoss;
    loss.valueLabel = runtimeSettingsValueLabel(editorLoss, "%");
    state.targetEditor.loss = loss;

    if (showEditableTransportControls && diagnosticsSelected) {
        state.targetEditor.statusLines = diagnosticsModel_->localNetworkSummaryLines();
    } else if (!hasLocalNetworkControls()) {
        state.targetEditor.statusLines.push_back("Proxy-backed runtime controls are not available in this session.");
    } else if (!canEditSelectedTransport) {
        state.targetEditor.statusLines.push_back(
            "Only the host or the selected participant can change these network settings.");
    } else {
        state.targetEditor.statusLines.push_back(
            "Authoritative network values are shown for the selected participant.");
    }

    const bool showAdvanced =
        diagnosticsSelected &&
        hasHostDiagnostics() &&
        canEditSelectedTransport &&
        transportTargetUsesProxyLink(selectedTargetId);
    state.targetEditor.showAdvancedNetworkControls = showAdvanced;

    auto configureLinkControl =
        [](RuntimeSettingsOverlay::ControlState* control,
           RuntimeSettingsOverlay::ControlId id,
           const char* label,
           const char* description,
           float minValue,
           float maxValue,
           float value,
           const char* unit,
           bool enabled) {
            if (control == nullptr) {
                return;
            }
            control->id = id;
            control->type = RuntimeSettingsOverlay::ControlType::Slider;
            control->label = label;
            control->description = description;
            control->enabled = enabled;
            control->sliderMin = minValue;
            control->sliderMax = maxValue;
            control->sliderValue = value;
            control->valueLabel = runtimeSettingsValueLabel(value, unit);
        };

    if (showAdvanced) {
        configureLinkControl(&state.targetEditor.upDelay,
                             RuntimeSettingsOverlay::ControlId::UpDelay,
                             "Up Delay",
                             "Client to server base delay",
                             0.0f,
                             250.0f,
                             diagnosticsModel_->linkConfig(true).baseDelayMs,
                             "ms",
                             true);
        configureLinkControl(&state.targetEditor.downDelay,
                             RuntimeSettingsOverlay::ControlId::DownDelay,
                             "Down Delay",
                             "Server to client base delay",
                             0.0f,
                             250.0f,
                             diagnosticsModel_->linkConfig(false).baseDelayMs,
                             "ms",
                             true);
        configureLinkControl(&state.targetEditor.upLoss,
                             RuntimeSettingsOverlay::ControlId::UpLoss,
                             "Up Loss",
                             "Client to server packet loss",
                             0.0f,
                             100.0f,
                             diagnosticsModel_->linkConfig(true).lossPct,
                             "%",
                             true);
        configureLinkControl(&state.targetEditor.downLoss,
                             RuntimeSettingsOverlay::ControlId::DownLoss,
                             "Down Loss",
                             "Server to client packet loss",
                             0.0f,
                             100.0f,
                             diagnosticsModel_->linkConfig(false).lossPct,
                             "%",
                             true);
        configureLinkControl(&state.targetEditor.upReorder,
                             RuntimeSettingsOverlay::ControlId::UpReorder,
                             "Up Reorder",
                             "Client to server packet reorder chance",
                             0.0f,
                             100.0f,
                             diagnosticsModel_->linkConfig(true).reorderPct,
                             "%",
                             true);
        configureLinkControl(&state.targetEditor.downReorder,
                             RuntimeSettingsOverlay::ControlId::DownReorder,
                             "Down Reorder",
                             "Server to client packet reorder chance",
                             0.0f,
                             100.0f,
                             diagnosticsModel_->linkConfig(false).reorderPct,
                             "%",
                             true);
    } else if (isLocalHost()) {
        state.targetEditor.statusLines.push_back(
            hasHostDiagnostics()
                ? "Advanced proxy controls are only available for human participants routed through the host proxy."
                : "Advanced proxy controls are host-only.");
    }

    return state;
}

void ClientRuntime::setRuntimeSettingsTarget(std::uint16_t targetId) {
    if (targetId == 0u || !shouldShowRuntimeSettingsTarget(targetId)) {
        return;
    }

    runtimeSettingsTargetId_ = targetId;
    if (diagnosticsModel_ != nullptr) {
        diagnosticsModel_->setTargetPeerId(targetId);
        syncDiagnosticsAuthoritativeTargetState();
        refreshDiagnostics();
    }
}

void ClientRuntime::applyLocalNetworkSettings() {
    if (!hasLocalNetworkControls()) {
        return;
    }

    const std::uint16_t targetPeerId = diagnosticsModel_->targetPeerId();
    if (!canEditTransportSettings(targetPeerId)) {
        return;
    }
    if (transportTargetUsesProxyLink(targetPeerId)) {
        hostProxy_->setPeerLinkConfig(targetPeerId, true, diagnosticsModel_->linkConfig(true));
        hostProxy_->setPeerLinkConfig(targetPeerId, false, diagnosticsModel_->linkConfig(false));
    }
    if (state_ != ClientConnectionState::Connected || peerId_ == 0u) {
        return;
    }

    for (const auto& request : diagnosticsModel_->buildLocalNetworkRequests()) {
        sendControlPayload(request);
    }
}

void ClientRuntime::refreshDiagnostics() {
    if (diagnosticsModel_ == nullptr || hostProxy_ == nullptr) {
        return;
    }

    diagnosticsModel_->refreshFromProxy(*hostProxy_);
}

sim::PlayerCommand ClientRuntime::makeCommand(const InputHandler3D::InputState& input,
                                              float dtSeconds,
                                              std::uint32_t seq) {
    sim::PlayerCommand command = syncRuntime_.buildCommand(syncContext(),
                                                           input,
                                                           dtSeconds,
                                                           seq,
                                                           viewYaw_,
                                                           viewPitch_,
                                                           pendingTeamRequest_,
                                                           roundedLatencyMs(diagnosticsModel_.get()),
                                                           roundedLossPct(diagnosticsModel_.get()));
    if (hasControlSnapshot_) {
        sim::CommandTiming controlTiming;
        controlTiming.viewedServerTimeUs = latestControlSnapshot_.serverTimeUs;
        controlTiming.interpolationDelayMs = defaultInterpolationDelayMs();
        command.applyControlTiming(controlTiming);
    }
    pendingTeamRequest_ = sim::TeamId::None;
    return command;
}

sim::PlayerCommand ClientRuntime::makeIdleCommand(std::uint32_t seq) {
    sim::PlayerCommand command = syncRuntime_.buildIdleCommand(syncContext(),
                                                               seq,
                                                               viewYaw_,
                                                               viewPitch_,
                                                               pendingTeamRequest_,
                                                               roundedLatencyMs(diagnosticsModel_.get()),
                                                               roundedLossPct(diagnosticsModel_.get()));
    if (hasControlSnapshot_) {
        sim::CommandTiming controlTiming;
        controlTiming.viewedServerTimeUs = latestControlSnapshot_.serverTimeUs;
        controlTiming.interpolationDelayMs = defaultInterpolationDelayMs();
        command.applyControlTiming(controlTiming);
    }
    pendingTeamRequest_ = sim::TeamId::None;
    return command;
}

sim::TeamId ClientRuntime::localTeam() const {
    const auto it = std::find_if(roster_.begin(),
                                 roster_.end(),
                                 [this](const sim::RosterEntry& entry) {
                                     return entry.actorId == static_cast<int>(peerId_);
                                 });
    if (it != roster_.end()) {
        return it->team;
    }

    return config_.preferredTeam;
}

void ClientRuntime::toggleSpectatorMode() {
    teamMenuVisible_ = false;
    scoreboardVisible_ = false;

    if (replaySubsystem_.timeline().isPlayback) {
        if (detachedObserverActive_) {
            detachedObserverActive_ = false;
            spectatorTransitionActive_ = false;
            lastCombatEventText_ = "Replay spectator mode off";
            return;
        }

        const client::RecordedReplayFrame* replayFrame = activeRecordedReplayFrame();
        if (replayFrame == nullptr || !replayFrame->frame.hasSnapshot) {
            lastCombatEventText_ = "Replay spectator unavailable until playback has a frame";
            return;
        }

        setDetachedObserverCheckpoint(
            SpectatorCamera::checkpointFromCamera(replayFrame->frame.camera));
        detachedObserverActive_ = true;
        sessionSpectatorObserverLocked_ = false;
        spectatorTransitionActive_ = false;
        lastCombatEventText_ = "Replay spectator mode on";
        return;
    }

    if (detachedObserverActive_) {
        if (sessionSpectatorObserverLocked_ || localTeam() == sim::TeamId::Spectator) {
            lastCombatEventText_ = "Session spectator free camera active";
            return;
        }
        detachedObserverActive_ = false;
        spectatorTransitionActive_ = false;
        viewYaw_ = localPlayerState_.yaw;
        viewPitch_ = localPlayerState_.pitch;
        lastCombatEventText_ = "Returned to character";
        return;
    }

    if (localTeam() == sim::TeamId::Spectator) {
        detachedObserverActive_ = true;
        sessionSpectatorObserverLocked_ = true;
        spectatorTransitionActive_ = false;
        detachedObserverEyePosition_ = localPlayerState_.position;
        detachedObserverYaw_ = viewYaw_;
        detachedObserverPitch_ = viewPitch_;
        lastCombatEventText_ = "Session spectator free camera active";
        return;
    }

    if (!hasSnapshot_) {
        lastCombatEventText_ = "Free camera unavailable until the match starts";
        return;
    }

    const sim::ParticipantState participant = localParticipantState();
    if (participant.participation != sim::ParticipationState::Playing ||
        !participant.control.controlsActor()) {
        lastCombatEventText_ = "Free camera requires an active character";
        return;
    }

    detachedObserverActive_ = true;
    spectatorTransitionActive_ = false;
    detachedObserverEyePosition_ = localPlayerState_.position;
    detachedObserverYaw_ = viewYaw_;
    detachedObserverPitch_ = viewPitch_;
    lastCombatEventText_ = "Detached free camera enabled";
}

void ClientRuntime::syncSessionSpectatorObserverState(const WorldSnapshot& snapshot) {
    const bool sessionSpectator =
        snapshot.localParticipantState.participation == sim::ParticipationState::Spectating ||
        snapshot.localParticipantState.team == sim::TeamId::Spectator;

    if (sessionSpectator) {
        if (!sessionSpectatorObserverLocked_) {
            sessionSpectatorObserverLocked_ = true;
            spectatorTransitionActive_ = false;
            if (!detachedObserverActive_) {
                detachedObserverActive_ = true;
                detachedObserverEyePosition_ = snapshot.localPlayerState.position;
                detachedObserverYaw_ = viewYaw_;
                detachedObserverPitch_ = viewPitch_;
            }
            lastCombatEventText_ = "Session spectator free camera active";
        }
        return;
    }

    if (!sessionSpectatorObserverLocked_) {
        return;
    }

    sessionSpectatorObserverLocked_ = false;
    if (detachedObserverActive_) {
        detachedObserverActive_ = false;
        spectatorTransitionActive_ = false;
        viewYaw_ = snapshot.localPlayerState.yaw;
        viewPitch_ = snapshot.localPlayerState.pitch;
    }
}

void ClientRuntime::toggleTeamMenu() {
    if (teamMenuVisible_) {
        teamMenuVisible_ = false;
        lastCombatEventText_ = "Change team cancelled";
        return;
    }

    teamMenuVisible_ = true;
    scoreboardVisible_ = false;
    teamMenuSelection_ = defaultTeamMenuSelection(localTeam());
}

void ClientRuntime::cycleTeamMenuSelection(int delta) {
    teamMenuSelection_ = cycleTeamSelection(teamMenuSelection_, delta);
}

void ClientRuntime::confirmTeamMenuSelection() {
    teamMenuVisible_ = false;
    queueTeamSwitchRequest(teamMenuSelection_);
}

void ClientRuntime::queueTeamSwitchRequest(sim::TeamId desiredTeam) {
    const sim::TeamId currentTeam = localTeam();
    if (desiredTeam == sim::TeamId::Spectator) {
        if (currentTeam == sim::TeamId::Spectator) {
            lastCombatEventText_ = "Already in spectator mode";
            return;
        }

        if (state_ == ClientConnectionState::Connected && peerId_ != 0u) {
            sendTeamChangeRequest(desiredTeam);
        }
        pendingTeamRequest_ = desiredTeam;
        lastCombatEventText_ = "Requested switch to Spectator (applies immediately)";
        return;
    }

    if (!sim::isPlayableTeam(desiredTeam)) {
        return;
    }
    if (sim::isPlayableTeam(currentTeam) && desiredTeam == currentTeam) {
        lastCombatEventText_ = std::string("Already on ") + teamLabel(currentTeam);
        return;
    }

    if (state_ == ClientConnectionState::Connected && peerId_ != 0u) {
        sendTeamChangeRequest(desiredTeam);
    }
    pendingTeamRequest_ = desiredTeam;
    config_.preferredTeam = desiredTeam;
    lastCombatEventText_ =
        std::string("Requested switch to ") + teamLabel(desiredTeam) +
        " (applies immediately and respawns you)";
}

void ClientRuntime::loadSpectatorCheckpoints() {
    spectatorCheckpoints_.clear();
    currentSpectatorCheckpoint_ = -1;

    app::CheckpointLoadResult loaded = checkpointStore_.load();
    if (!loaded.loaded || loaded.checkpoints.empty()) {
        return;
    }

    spectatorCheckpoints_ = std::move(loaded.checkpoints);
    currentSpectatorCheckpoint_ = app::CheckpointStore::initialCheckpointIndex(spectatorCheckpoints_);
    if (loaded.migratedLegacy) {
        saveSpectatorCheckpoints();
    }
}

void ClientRuntime::saveSpectatorCheckpoints() const {
    checkpointStore_.save(spectatorCheckpoints_);
}

SpectatorCamera::Checkpoint ClientRuntime::detachedObserverCheckpoint() const {
    return SpectatorCamera::Checkpoint{
        Vector3{detachedObserverEyePosition_.x,
                detachedObserverEyePosition_.y,
                detachedObserverEyePosition_.z},
        detachedObserverYaw_,
        detachedObserverPitch_,
        currentSpectatorCheckpointTransitionSeconds()
    };
}

void ClientRuntime::setDetachedObserverCheckpoint(const SpectatorCamera::Checkpoint& checkpoint) {
    detachedObserverEyePosition_ = sim::Vec3{
        checkpoint.position.x,
        checkpoint.position.y,
        checkpoint.position.z
    };
    detachedObserverYaw_ = checkpoint.yaw;
    detachedObserverPitch_ = checkpoint.pitch;
}

void ClientRuntime::beginSpectatorCheckpointTransition(int targetIndex) {
    if (!detachedObserverActive_ ||
        targetIndex < 0 ||
        targetIndex >= static_cast<int>(spectatorCheckpoints_.size())) {
        return;
    }

    const int originIndex =
        (currentSpectatorCheckpoint_ >= 0 &&
         currentSpectatorCheckpoint_ < static_cast<int>(spectatorCheckpoints_.size()))
            ? currentSpectatorCheckpoint_
            : -1;

    spectatorTransitionStart_ = detachedObserverCheckpoint();
    spectatorTransitionEnd_ = spectatorCheckpoints_[targetIndex];
    if (originIndex >= 0 &&
        ((originIndex + 1) % static_cast<int>(spectatorCheckpoints_.size())) == targetIndex) {
        spectatorTransitionDuration_ =
            std::max(0.0f, spectatorCheckpoints_[originIndex].transitionDurationSeconds);
    } else {
        spectatorTransitionDuration_ =
            std::max(0.0f, spectatorCheckpoints_[targetIndex].transitionDurationSeconds);
    }
    spectatorTransitionTimer_ = 0.0f;
    spectatorTransitionActive_ = true;
    currentSpectatorCheckpoint_ = targetIndex;
    lastCombatEventText_ =
        "Checkpoint " + std::to_string(targetIndex + 1) +
        "/" + std::to_string(spectatorCheckpoints_.size());
}

void ClientRuntime::updateSpectatorCheckpointTransition(float dtSeconds) {
    if (!detachedObserverActive_ ||
        !spectatorTransitionActive_ ||
        spectatorCheckpoints_.empty()) {
        return;
    }

    spectatorTransitionTimer_ += dtSeconds;
    const float t = std::clamp(
        spectatorTransitionDuration_ > 0.0f
            ? spectatorTransitionTimer_ / spectatorTransitionDuration_
            : 1.0f,
        0.0f,
        1.0f);
    const float eased = t * t * (3.0f - (2.0f * t));

    SpectatorCamera::Checkpoint interpolated;
    interpolated.position = Vector3Lerp(spectatorTransitionStart_.position,
                                        spectatorTransitionEnd_.position,
                                        eased);
    interpolated.yaw = spectatorTransitionStart_.yaw +
                       (spectatorTransitionEnd_.yaw - spectatorTransitionStart_.yaw) * eased;
    interpolated.pitch = spectatorTransitionStart_.pitch +
                         (spectatorTransitionEnd_.pitch - spectatorTransitionStart_.pitch) * eased;
    setDetachedObserverCheckpoint(interpolated);

    if (spectatorTransitionTimer_ >= spectatorTransitionDuration_) {
        spectatorTransitionActive_ = false;
        setDetachedObserverCheckpoint(spectatorTransitionEnd_);
    }
}

float ClientRuntime::currentSpectatorCheckpointTransitionSeconds() const {
    if (currentSpectatorCheckpoint_ < 0 ||
        currentSpectatorCheckpoint_ >= static_cast<int>(spectatorCheckpoints_.size())) {
        return defaultCheckpointTransitionSecondsForCurrentMode();
    }

    return std::max(0.0f,
                    spectatorCheckpoints_[currentSpectatorCheckpoint_].transitionDurationSeconds);
}

float ClientRuntime::defaultCheckpointTransitionSecondsForCurrentMode() const {
    return replaySubsystem_.timeline().isRecording
        ? kRecordingCheckpointTransitionSeconds
        : SpectatorCamera::kDefaultCheckpointTransitionSeconds;
}

void ClientRuntime::adjustCurrentSpectatorCheckpointTransition(float deltaSeconds) {
    if (!detachedObserverActive_ ||
        currentSpectatorCheckpoint_ < 0 ||
        currentSpectatorCheckpoint_ >= static_cast<int>(spectatorCheckpoints_.size()) ||
        spectatorCheckpoints_.size() < 2u) {
        return;
    }

    SpectatorCamera::Checkpoint& checkpoint =
        spectatorCheckpoints_[currentSpectatorCheckpoint_];
    checkpoint.transitionDurationSeconds = std::max(
        0.0f,
        checkpoint.transitionDurationSeconds + deltaSeconds);
    saveSpectatorCheckpoints();
    lastCombatEventText_ = "Next transition " +
                           formatSeconds(checkpoint.transitionDurationSeconds);
}

bool ClientRuntime::maybeSendIdleKeepalive() {
    if (config_.idleKeepaliveIntervalUs == 0u || peerId_ == 0u) {
        return false;
    }

    if (lastCommandSendUs_ != 0u &&
        (clockUs_ - lastCommandSendUs_) < config_.idleKeepaliveIntervalUs) {
        return false;
    }

    const sim::PlayerCommand keepalive = makeIdleCommand(++commandSeq_);
    return sendCommand(keepalive);
}

bool ClientRuntime::studyEventLoggingActive() const {
    if (hasAuthoritativeSessionMetadata_) {
        return authoritativeSessionMetadata_.studyEventLoggingEnabled;
    }
    return config_.studyEventLoggingEnabled;
}

bool ClientRuntime::studyEventLoggingAvailable() const {
    return studyEventLoggingActive() &&
           (!effectiveStudyEventRunId().empty() || config_.sessionId != 0u);
}

std::string ClientRuntime::effectiveStudyEventRunId() const {
    if (!authoritativeSessionMetadata_.studyEventRunId.empty()) {
        return authoritativeSessionMetadata_.studyEventRunId;
    }
    if (!config_.studyEventRunId.empty()) {
        return config_.studyEventRunId;
    }
    return "client_" + std::to_string(config_.sessionId);
}

std::filesystem::path ClientRuntime::effectiveStudyEventLogDirectory() const {
    const std::string runId = telemetry::sanitizeRunId(effectiveStudyEventRunId());
    return config_.studyEventLogDirectory.empty()
        ? telemetry::defaultStudyEventRunDirectory(runId)
        : config_.studyEventLogDirectory;
}

std::string ClientRuntime::studyEventLogDirectoryLabel() const {
    const std::filesystem::path directory =
        std::filesystem::absolute(effectiveStudyEventLogDirectory()).lexically_normal();
    const std::filesystem::path root =
        std::filesystem::absolute(app::applicationRootDirectory()).lexically_normal();
    const std::filesystem::path relative = directory.lexically_relative(root);
    if (!relative.empty()) {
        const auto first = relative.begin();
        if (first != relative.end() && *first != std::filesystem::path("..")) {
            return relative.generic_string();
        }
    }
    return directory.generic_string();
}

bool ClientRuntime::ensureStudyEventSink() {
    if (!studyEventLoggingAvailable()) {
        return false;
    }
    if (studyEventSink_ != nullptr) {
        return true;
    }

    const std::filesystem::path runDirectory = effectiveStudyEventLogDirectory();
    const std::string peerLabel =
        peerId_ != 0u ? std::to_string(peerId_) : std::to_string(config_.sessionId);
    auto writer = std::make_unique<telemetry::JsonlStudyEventWriter>(
        runDirectory / ("events_client_peer_" + peerLabel + ".jsonl"));
    if (!writer->isOpen()) {
        return false;
    }

    studyEventSink_ = std::move(writer);
    return true;
}

telemetry::StudyEventRecord ClientRuntime::makeClientStudyEvent(const std::string& eventName) {
    const std::uint16_t tickHz = latestSnapshot_.cadence.authoritativeTickHz != 0u
        ? latestSnapshot_.cadence.authoritativeTickHz
        : kDefaultSessionTickRateHz;
    const std::uint64_t elapsedSinceSnapshotUs =
        hasSnapshot_ && clockUs_ >= latestSnapshotReceiveUs_
            ? clockUs_ - latestSnapshotReceiveUs_
            : 0u;
    const std::uint64_t estimatedServerTimeUs =
        hasSnapshot_ ? latestSnapshot_.serverTimeUs + elapsedSinceSnapshotUs : 0u;
    const std::uint64_t interpolationDelayUs =
        static_cast<std::uint64_t>(defaultInterpolationDelayMs()) * 1'000u;
    const std::uint64_t renderSampleServerTimeUs =
        estimatedServerTimeUs > interpolationDelayUs
            ? estimatedServerTimeUs - interpolationDelayUs
            : 0u;
    const std::uint64_t estimatedServerTick =
        hasSnapshot_
            ? static_cast<std::uint64_t>(latestSnapshot_.serverTick) +
                  (elapsedSinceSnapshotUs * static_cast<std::uint64_t>(tickHz)) / 1'000'000u
            : 0u;
    const std::uint64_t interpolationDelayTicks =
        (interpolationDelayUs * static_cast<std::uint64_t>(tickHz)) / 1'000'000u;
    const std::uint64_t renderSampleServerTick =
        estimatedServerTick > interpolationDelayTicks
            ? estimatedServerTick - interpolationDelayTicks
            : 0u;

    telemetry::StudyEventRecord record;
    record.add("schema_version", std::int32_t{1})
          .add("event_name", eventName)
          .add("source", "client")
          .add("session_id", telemetry::sanitizeRunId(effectiveStudyEventRunId()))
          .add("event_seq", ++studyEventSeq_)
          .add("client_peer_id", peerId_)
          .add("client_session_id", config_.sessionId)
          .add("client_frame", clientFrameCounter_)
          .add("client_time_us", clockUs_)
          .add("latest_snapshot_server_tick", latestSnapshot_.serverTick)
          .add("latest_snapshot_server_time_us", latestSnapshot_.serverTimeUs)
          .add("estimated_server_tick", estimatedServerTick)
          .add("estimated_server_time_us", estimatedServerTimeUs)
          .add("render_sample_server_tick", renderSampleServerTick)
          .add("render_sample_server_time_us", renderSampleServerTimeUs)
          .add("tick_rate_hz", tickHz)
          .add("snapshot_rate_hz", latestSnapshot_.cadence.snapshotCadenceHz)
          .add("interpolation_enabled", interpolationEnabled_)
          .add("prediction_enabled", predictionEnabled_)
          .add("interpolation_delay_ms", defaultInterpolationDelayMs());
    return record;
}

void ClientRuntime::recordClientPerceptionEvents() {
    if (!studyEventLoggingAvailable()) {
        perceptionMonitor_.reset();
        return;
    }

    const sim::ParticipantState participant = localParticipantState();
    const int perspectiveActorId = participant.control.controlsActor()
        ? participant.control.actorId
        : localPlayerState_.playerId;
    const auto clearVisible = [this, perspectiveActorId]() {
        for (const client::FovVisibilityTransition& transition :
             perceptionMonitor_.clearVisible(perspectiveActorId)) {
            recordClientFovTransition(transition);
        }
    };

    if (state_ != ClientConnectionState::Connected ||
        !hasSnapshot_ ||
        replaySubsystem_.timeline().isPlayback ||
        detachedObserverActive_ ||
        participant.participation != sim::ParticipationState::Playing ||
        !participant.control.controlsActor() ||
        localPlayerState_.health <= 0.0f) {
        clearVisible();
        return;
    }

    sim::PlayerState observer = localPlayerState_;
    observer.playerId = perspectiveActorId;
    observer.yaw = viewYaw_;
    observer.pitch = viewPitch_;

    client::PerceptionFrame frame;
    frame.perspectiveActorId = perspectiveActorId;
    frame.observer = observer;
    frame.subjects = remotePlayers_;
    frame.environment = environment_;
    frame.simConfig = simConfig_;
    frame.config.verticalFovDegrees = kStudyFovVerticalDegrees;
    frame.config.aspectRatio =
        static_cast<float>(Config::SCREEN_WIDTH) / static_cast<float>(Config::SCREEN_HEIGHT);
    frame.config.rangeMeters = kStudyFovRangeMeters;
    frame.config.requireLineOfSight = true;

    for (const client::FovVisibilityTransition& transition : perceptionMonitor_.update(frame)) {
        recordClientFovTransition(transition);
    }
}

void ClientRuntime::recordClientFovTransition(
    const client::FovVisibilityTransition& transition) {
    if (!ensureStudyEventSink()) {
        return;
    }

    const sim::RosterEntry* observerEntry = findRosterEntry(transition.perspectiveActorId);
    const sim::RosterEntry* subjectEntry = findRosterEntry(transition.subjectActorId);
    const float aspectRatio =
        static_cast<float>(Config::SCREEN_WIDTH) / static_cast<float>(Config::SCREEN_HEIGHT);

    telemetry::StudyEventRecord record =
        makeClientStudyEvent(client::toString(transition.kind));
    record.add("perspective_actor_id", transition.perspectiveActorId)
          .add("subject_actor_id", transition.subjectActorId)
          .add("perspective_team_id",
               observerEntry != nullptr ? static_cast<std::int32_t>(observerEntry->team)
                                        : static_cast<std::int32_t>(sim::TeamId::None))
          .add("subject_team_id",
               subjectEntry != nullptr ? static_cast<std::int32_t>(subjectEntry->team)
                                       : static_cast<std::int32_t>(sim::TeamId::None))
          .add("perspective_team",
               observerEntry != nullptr ? sim::toString(observerEntry->team)
                                        : sim::toString(sim::TeamId::None))
          .add("subject_team",
               subjectEntry != nullptr ? sim::toString(subjectEntry->team)
                                       : sim::toString(sim::TeamId::None))
          .add("subject_is_bot", subjectEntry != nullptr && subjectEntry->isBot)
          .add("observer_x", transition.observer.position.x)
          .add("observer_y", transition.observer.position.y)
          .add("observer_z", transition.observer.position.z)
          .add("observer_yaw", transition.observer.yaw)
          .add("observer_pitch", transition.observer.pitch)
          .add("subject_x", transition.subject.position.x)
          .add("subject_y", transition.subject.position.y)
          .add("subject_z", transition.subject.position.z)
          .add("subject_yaw", transition.subject.yaw)
          .add("subject_pitch", transition.subject.pitch)
          .add("subject_center_x", transition.sample.subjectCenter.x)
          .add("subject_center_y", transition.sample.subjectCenter.y)
          .add("subject_center_z", transition.sample.subjectCenter.z)
          .add("distance_m", transition.sample.distanceMeters)
          .add("horizontal_angle_deg", transition.sample.horizontalAngleDegrees)
          .add("vertical_angle_deg", transition.sample.verticalAngleDegrees)
          .add("fov_vertical_deg", kStudyFovVerticalDegrees)
          .add("fov_horizontal_deg", studyFovHorizontalDegrees(aspectRatio))
          .add("fov_range_m", kStudyFovRangeMeters)
          .add("inside_cone", transition.sample.insideCone)
          .add("line_of_sight", transition.sample.lineOfSight)
          .add("visible", transition.sample.visible);
    studyEventSink_->write(record);
}

void ClientRuntime::recordClientFirePressed(const sim::PlayerCommand& command,
                                            bool commandSent) {
    if (!ensureStudyEventSink()) {
        return;
    }

    const sim::ParticipantState participant = localParticipantState();
    const int perspectiveActorId = participant.control.controlsActor()
        ? participant.control.actorId
        : localPlayerState_.playerId;
    const sim::RosterEntry* observerEntry = findRosterEntry(perspectiveActorId);
    telemetry::StudyEventRecord record = makeClientStudyEvent("combat.fire_pressed");
    record.add("perspective_actor_id", perspectiveActorId)
          .add("perspective_team_id",
               observerEntry != nullptr ? static_cast<std::int32_t>(observerEntry->team)
                                        : static_cast<std::int32_t>(sim::TeamId::None))
          .add("perspective_team",
               observerEntry != nullptr ? sim::toString(observerEntry->team)
                                        : sim::toString(sim::TeamId::None))
          .add("command_seq", command.seq)
          .add("command_sent", commandSent)
          .add("command_viewed_server_time_us", command.viewedServerTimeUs)
          .add("command_interp_delay_ms", command.interpDelayMs)
          .add("command_control_viewed_server_time_us", command.controlViewedServerTimeUs)
          .add("command_control_interp_delay_ms", command.controlInterpDelayMs)
          .add("reported_latency_ms", command.reportedLatencyMs)
          .add("reported_loss_pct", std::int32_t{command.reportedLossPct})
          .add("observer_x", localPlayerState_.position.x)
          .add("observer_y", localPlayerState_.position.y)
          .add("observer_z", localPlayerState_.position.z)
          .add("observer_yaw", command.yaw)
          .add("observer_pitch", command.pitch);
    studyEventSink_->write(record);
}

void ClientRuntime::updateDetachedObserverCamera(const InputHandler3D::InputState& input,
                                                 float dtSeconds) {
    const sim::Vec3 forward = sim::forwardFromYaw(detachedObserverYaw_);
    const sim::Vec3 right = sim::rightFromYaw(detachedObserverYaw_);

    sim::Vec3 move{};
    move.x = (forward.x * input.moveInput.y) + (right.x * input.moveInput.x);
    move.z = (forward.z * input.moveInput.y) + (right.z * input.moveInput.x);
    if (input.moveUp) {
        move.y += 1.0f;
    }
    if (input.moveDown) {
        move.y -= 1.0f;
    }

    const float moveLength = std::sqrt((move.x * move.x) + (move.y * move.y) + (move.z * move.z));
    if (moveLength <= 0.0001f) {
        return;
    }

    const float moveSpeed = simConfig_.playerMoveSpeed * kDetachedObserverSpeedMultiplier *
                            (input.fastModifier ? kDetachedObserverFastMultiplier : 1.0f);
    const float moveScale = (moveSpeed * dtSeconds) / moveLength;
    detachedObserverEyePosition_.x += move.x * moveScale;
    detachedObserverEyePosition_.y += move.y * moveScale;
    detachedObserverEyePosition_.z += move.z * moveScale;
}

void ClientRuntime::updateReplayPlaybackObserver(
    float dtSeconds,
    const InputHandler3D::InputState* input) {
    if (!detachedObserverActive_) {
        return;
    }

    const bool transitionWasActive = spectatorTransitionActive_;
    if (transitionWasActive) {
        updateSpectatorCheckpointTransition(dtSeconds);
    }

    if (input == nullptr ||
        transitionWasActive ||
        uiMode_ ||
        runtimeSettingsVisible_ ||
        teamMenuVisible_) {
        return;
    }

    InputHandler3D::applyLookDelta(*input, &detachedObserverYaw_, &detachedObserverPitch_);
    updateDetachedObserverCamera(*input, dtSeconds);
}

sim::PaneViewState ClientRuntime::effectiveLocalPaneView() const {
    sim::PaneViewState paneView = latestSnapshot_.localPaneView;
    if (!detachedObserverActive_) {
        return paneView;
    }

    paneView.mode = sim::PaneViewMode::SpectatorFreeFly;
    paneView.followTargetActorId = -1;
    return paneView;
}

bool ClientRuntime::sendHello() {
    const bool sent = syncRuntime_.sendHello(syncContext(),
                                             config_.protocolVersion,
                                             config_.sessionId,
                                             config_.playerName,
                                             config_.preferredTeam,
                                             socket_,
                                             serverEndpoint_);
    if (!sent) {
        state_ = ClientConnectionState::Rejected;
        statusMessage_ = socket_.lastError();
    }
    return sent;
}

bool ClientRuntime::sendControlPayload(PacketPayload payload) {
    const bool sent = syncRuntime_.sendControlPayload(syncContext(),
                                                      config_.protocolVersion,
                                                      std::move(payload),
                                                      socket_,
                                                      serverEndpoint_);
    if (!sent && peerId_ != 0u) {
        state_ = ClientConnectionState::Rejected;
        statusMessage_ = socket_.lastError();
    }
    return sent;
}

bool ClientRuntime::sendTeamChangeRequest(sim::TeamId desiredTeam) {
    return sendControlPayload(TeamChangeRequest{desiredTeam});
}

bool ClientRuntime::sendSessionActionRequest(SessionActionKind kind) {
    return sendControlPayload(SessionActionRequest{kind});
}

bool ClientRuntime::sendRuntimeParamChangeRequest(RuntimeParamScope scope,
                                                  std::int32_t targetId,
                                                  std::string key,
                                                  float value) {
    return sendControlPayload(RuntimeParamChangeRequest{
        scope,
        targetId,
        std::move(key),
        value
    });
}

bool ClientRuntime::sendCommand(const sim::PlayerCommand& command) {
    const bool sent = syncRuntime_.sendCommand(syncContext(),
                                               config_.protocolVersion,
                                               command,
                                               socket_,
                                               serverEndpoint_);
    if (!sent && peerId_ != 0u) {
        state_ = ClientConnectionState::Rejected;
        statusMessage_ = socket_.lastError();
    }
    return sent;
}

void ClientRuntime::syncRemoteVisuals() {
    if (remoteVisuals_.size() < remoteEnemies_.size()) {
        remoteVisuals_.reserve(remoteEnemies_.size());
        while (remoteVisuals_.size() < remoteEnemies_.size()) {
            remoteVisuals_.push_back(std::make_unique<Enemy3D>(std::shared_ptr<Model3DWrapper>{}));
        }
    } else if (remoteVisuals_.size() > remoteEnemies_.size()) {
        remoteVisuals_.resize(remoteEnemies_.size());
    }

    for (std::size_t index = 0; index < remoteEnemies_.size(); ++index) {
        const auto& actor = remoteEnemies_[index];
        auto& visual = remoteVisuals_[index];
        visual->setColor(kRemoteEnemyColor);
        visual->setSimState(actor);
    }
}

std::uint32_t ClientRuntime::defaultInterpolationDelayMs() const {
    return syncRuntime_.defaultInterpolationDelayMs(syncContext());
}

bool ClientRuntime::saveRecordedReplay() {
    if (recordedReplayFrames_.empty()) {
        lastCombatEventText_ = "No recording available";
        return false;
    }

    client::ReplayRecording recording;
    recording.metadata.title = authoritativeSessionMetadata_.sessionLabel.empty()
        ? "Session Replay"
        : authoritativeSessionMetadata_.sessionLabel;
    recording.metadata.sourceLabel = hostedSessionSummary();
    recording.metadata.levelSlot = authoritativeLevelSlot_;
    recording.metadata.levelHash = authoritativeLevelHash_;
    recording.frames = recordedReplayFrames_;

    app::ReplayArchive archive;
    std::filesystem::path savedPath;
    std::string error;
    if (!archive.save(recording, &savedPath, &error)) {
        lastCombatEventText_ = error.empty() ? "Replay export failed" : error;
        return false;
    }

    lastCombatEventText_ = "Replay saved: " + savedPath.filename().string();
    return true;
}

client::ReplayStatusView ClientRuntime::replayStatusView() const {
    client::ReplayStatusView replay = replaySubsystem_.statusView(recordedReplayFrames_.size());
    const bool localReplayStatusAvailable =
        replaySubsystem_.timeline().isRecording ||
        replaySubsystem_.timeline().isPlayback ||
        !recordedReplayFrames_.empty();
    if (commandReplayStatusAvailable_ && !localReplayStatusAvailable) {
        replay.recordingActive = commandReplayRecordingActive_;
        if (commandReplayRecordingActive_) {
            replay.statusLine = commandReplayEventCount_ == 0u
                ? "Command Recording"
                : "Command Recording (" + std::to_string(commandReplayEventCount_) + " events)";
        } else if (commandReplayEventCount_ > 0u) {
            replay.statusLine =
                "Command replay ready (" + std::to_string(commandReplayEventCount_) + " events)";
        } else {
            replay.statusLine = "Command recorder ready";
        }
    }
    replay.overlayVisible = replayOverlayVisible_;
    replay.checkpoint.detachedCameraActive = detachedObserverActive_;
    replay.checkpoint.activeIndex = currentSpectatorCheckpoint_;
    replay.checkpoint.checkpointCount = spectatorCheckpoints_.size();
    replay.checkpoint.transitionToNextSeconds = currentSpectatorCheckpointTransitionSeconds();
    replay.checkpoint.transitionEditable =
        detachedObserverActive_ &&
        currentSpectatorCheckpoint_ >= 0 &&
        currentSpectatorCheckpoint_ < static_cast<int>(spectatorCheckpoints_.size()) &&
        spectatorCheckpoints_.size() > 1u;
    return replay;
}

client::PresentationStateInputs ClientRuntime::presentationStateInputs() const {
    const sim::ParticipantState participantState = localParticipantState();
    const sim::PaneViewState paneState = effectiveLocalPaneView();
    std::vector<client::KillFeedEntryView> killFeedEntries;
    killFeedEntries.reserve(killFeed_.size());
    for (const KillFeedItem& item : killFeed_) {
        killFeedEntries.push_back(item.view);
    }
    return client::PresentationStateInputs{
        authoritativeSessionMetadata_,
        roster_,
        teamScores_,
        localPlayerState_,
        remotePlayers_,
        controlRemotePlayers_,
        remoteEnemies_,
        diagnosticsModel_.get(),
        peerId_,
        config_.preferredTeam,
        state_ == ClientConnectionState::Connected,
        hasSnapshot_,
        std::string(toString(state_)),
        statusMessage_,
        detachedObserverActive_ ? detachedObserverYaw_ : viewYaw_,
        detachedObserverActive_ ? detachedObserverPitch_ : viewPitch_,
        hasLocalNetworkControls(),
        hasHostDiagnostics(),
        runtimeSettingsVisible_,
        teamMenuVisible_,
        teamMenuSelection_,
        uiMode_,
        scoreboardVisible_,
        lastCombatEventText_,
        replayStatusView(),
        latestSnapshot_.cadence,
        latestSnapshot_.authoritativeTime,
        lastAckedInputSeq_,
        toViewStrategy(reconciliationStrategy_),
        toViewCorrectionMode(lastCorrection_.mode),
        lastCorrection_.magnitude,
        lastCorrection_.replayedCommandCount,
        static_cast<std::uint32_t>(predictionBuffer_.pendingCommandCount()),
        smoothCorrectionWindowMs_,
        participantState,
        paneState,
        {},
        {},
        currentSpectatorCheckpoint_,
        spectatorCheckpoints_.size(),
        detachedObserverActive_,
        detachedObserverActive_,
        detachedObserverActive_ ? detachedObserverEyePosition_ : sim::Vec3{},
        studyPresentation_.environmentDimmed,
        studyPresentation_.environmentDimmed ? kStudyEnvironmentDimFactor : 0.0f,
        studyPresentation_.areasVisible,
        studyPresentation_.areaFilter,
        std::move(killFeedEntries)
    };
}

client::ClientViewState ClientRuntime::buildClientViewState() const {
    return presentationStateSubsystem_.build(presentationStateInputs());
}

client::ClientSyncContext ClientRuntime::syncContext() {
    return client::ClientSyncContext{
        clockUs_,
        lastHelloSendUs_,
        lastServerPacketUs_,
        lastCommandSendUs_,
        packetSeq_,
        commandSeq_,
        lastSnapshotPacketSeq_,
        lastAckedInputSeq_,
        peerId_,
        snapshotRateHz_,
        authoritativeLevelSlot_,
        authoritativeLevelHash_,
        hasAuthoritativeSessionMetadata_,
        authoritativeSessionMetadata_,
        latestSnapshotReceiveUs_,
        hasSnapshot_,
        interpolationEnabled_,
        predictionEnabled_,
        remotePlayerInterpolation_,
        remoteEnemyInterpolation_,
        predictionBuffer_,
        localPlayerState_,
        remotePlayers_,
        remoteEnemies_,
        roster_,
        teamScores_,
        latestSnapshot_
    };
}

client::ClientSyncContext ClientRuntime::syncContext() const {
    ClientRuntime& self = const_cast<ClientRuntime&>(*this);
    return self.syncContext();
}

}  // namespace net
