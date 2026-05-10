#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/SimulationDefaults.hpp"

namespace sim {

struct Vec2 {
    float x{0.0f};
    float y{0.0f};
};

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct CollisionBox {
    Vec3 center{};
    Vec3 halfSize{};
};

struct MovementEnvironment {
    float arenaHalfSize{defaults::kArenaHalfSize};
    std::vector<CollisionBox> collisionBoxes{};
};

enum class CommandButton : std::uint32_t {
    Jump = 1u << 0,
    Fire = 1u << 1
};

constexpr std::uint32_t commandButtonBit(CommandButton button) {
    return static_cast<std::uint32_t>(button);
}

inline bool hasButton(std::uint32_t buttons, CommandButton button) {
    return (buttons & commandButtonBit(button)) != 0u;
}

struct TimingCadence {
    std::uint16_t authoritativeTickHz{60u};
    std::uint16_t snapshotCadenceHz{20u};
    std::uint16_t commandCadenceHz{60u};
};

struct AuthoritativeTime {
    std::uint32_t serverTick{0u};
    std::uint64_t serverTimeUs{0u};
    std::uint64_t viewedServerTimeUs{0u};
};

enum class StagedApplyBoundary : std::uint8_t {
    NextTick = 0,
    NextSnapshot = 1,
    NextSessionRestart = 2
};

struct CommandTiming {
    std::uint64_t viewedServerTimeUs{0u};
    std::uint32_t interpolationDelayMs{0u};
};

struct SimConfig {
    float playerEyeHeight{defaults::kPlayerEyeHeight};
    float playerMoveSpeed{defaults::kPlayerSpeed};
    float playerJumpVelocity{defaults::kPlayerJumpVelocity};
    float playerGravity{defaults::kPlayerGravity};
    int playerMaxJumps{defaults::kPlayerMaxJumps};
    float playerRadius{defaults::kPlayerCollisionRadius};
    float playerCollisionHeight{defaults::kPlayerCollisionHeight};
    float minPitch{defaults::kMinPitch};
    float maxPitch{defaults::kMaxPitch};
    float playerMaxHealth{defaults::kPlayerMaxHealth};
    float enemyRadius{defaults::kEnemyRadius};
    float enemyMaxHealth{defaults::kEnemyMaxHealth};
    float weaponCooldownSeconds{defaults::kWeaponCooldownSeconds};
    float weaponDamage{defaults::kWeaponDamage};
    float weaponRange{defaults::kWeaponRange};
    float arenaHalfSize{defaults::kArenaHalfSize};
};

struct PlayerState {
    int playerId{0};
    Vec3 position{0.0f, defaults::kPlayerEyeHeight, 0.0f};
    Vec3 velocity{};
    float yaw{0.0f};
    float pitch{0.0f};
    float health{defaults::kPlayerMaxHealth};
    float maxHealth{defaults::kPlayerMaxHealth};
    float weaponCooldownRemaining{0.0f};
    int jumpsUsed{0};
    bool grounded{true};
};

struct RemoteActorState {
    int entityId{0};
    Vec3 position{};
    Vec3 velocity{};
    float yaw{0.0f};
    float pitch{0.0f};
    float health{defaults::kEnemyMaxHealth};
    float radius{defaults::kEnemyRadius};
    bool alive{true};
};

enum class TeamId : std::uint8_t {
    None = 0,
    Attacker = 1,
    Defender = 2,
    Spectator = 3
};

inline bool isPlayableTeam(TeamId team) {
    return team == TeamId::Attacker || team == TeamId::Defender;
}

inline TeamId opposingTeam(TeamId team) {
    switch (team) {
        case TeamId::Attacker:
            return TeamId::Defender;
        case TeamId::Defender:
            return TeamId::Attacker;
        case TeamId::Spectator:
        case TeamId::None:
            return TeamId::None;
    }
    return TeamId::None;
}

inline const char* toString(TeamId team) {
    switch (team) {
        case TeamId::Attacker:
            return "Attackers";
        case TeamId::Defender:
            return "Defenders";
        case TeamId::Spectator:
            return "Spectator";
        case TeamId::None:
            return "Unassigned";
    }
    return "Unassigned";
}

enum class SessionPresence : std::uint8_t {
    Connecting = 0,
    Connected = 1,
    Disconnected = 2
};

inline const char* toString(SessionPresence presence) {
    switch (presence) {
        case SessionPresence::Connecting:
            return "connecting";
        case SessionPresence::Connected:
            return "connected";
        case SessionPresence::Disconnected:
            return "disconnected";
    }
    return "disconnected";
}

enum class ParticipationState : std::uint8_t {
    TeamSelection = 0,
    Playing = 1,
    Spectating = 2
};

inline const char* toString(ParticipationState participation) {
    switch (participation) {
        case ParticipationState::TeamSelection:
            return "team_selection";
        case ParticipationState::Playing:
            return "playing";
        case ParticipationState::Spectating:
            return "spectating";
    }
    return "team_selection";
}

enum class ControlBindingKind : std::uint8_t {
    None = 0,
    Actor = 1
};

struct ControlBinding {
    ControlBindingKind kind{ControlBindingKind::None};
    int actorId{-1};

    bool controlsActor() const {
        return kind == ControlBindingKind::Actor && actorId >= 0;
    }
};

struct ParticipantState {
    SessionPresence presence{SessionPresence::Connected};
    TeamId team{TeamId::None};
    ParticipationState participation{ParticipationState::TeamSelection};
    ControlBinding control{};
};

enum class PaneSlot : std::uint8_t {
    Left = 0,
    Right = 1
};

enum class PaneViewMode : std::uint8_t {
    PlayerControlled = 0,
    SpectatorFreeFly = 1,
    SpectatorFollowFirstPerson = 2,
    SpectatorFollowThirdPerson = 3,
    ReplayCamera = 4
};

inline const char* toString(PaneViewMode mode) {
    switch (mode) {
        case PaneViewMode::PlayerControlled:
            return "player_controlled";
        case PaneViewMode::SpectatorFreeFly:
            return "spectator_free_fly";
        case PaneViewMode::SpectatorFollowFirstPerson:
            return "spectator_follow_first_person";
        case PaneViewMode::SpectatorFollowThirdPerson:
            return "spectator_follow_third_person";
        case PaneViewMode::ReplayCamera:
            return "replay_camera";
    }
    return "player_controlled";
}

struct PaneViewState {
    PaneSlot slot{PaneSlot::Left};
    PaneViewMode mode{PaneViewMode::PlayerControlled};
    bool focused{true};
    int followTargetActorId{-1};
};

enum class RuntimeReconciliationStrategy : std::uint8_t {
    Snap = 0,
    Smooth = 1
};

inline const char* toString(RuntimeReconciliationStrategy strategy) {
    switch (strategy) {
        case RuntimeReconciliationStrategy::Snap:
            return "snap";
        case RuntimeReconciliationStrategy::Smooth:
            return "smooth";
    }
    return "smooth";
}

struct RosterEntry {
    int actorId{0};
    TeamId team{TeamId::None};
    SessionPresence sessionPresence{SessionPresence::Connected};
    ParticipationState participation{ParticipationState::TeamSelection};
    ControlBinding control{};
    bool isBot{false};
    std::uint16_t kills{0};
    std::uint16_t deaths{0};
    std::uint16_t assists{0};
    bool alive{true};
    bool interpolationEnabled{true};
    bool predictionEnabled{true};
    RuntimeReconciliationStrategy reconciliationStrategy{RuntimeReconciliationStrategy::Smooth};
    std::uint32_t smoothCorrectionWindowMs{250u};
    std::uint16_t latencyMs{0};
    std::uint8_t lossPct{0};
    std::string displayName{};
};

struct EligibleActor {
    int actorId{0};
    TeamId team{TeamId::None};
    bool isBot{false};
    bool alive{false};
    std::string displayName{};
};

inline EligibleActor eligibleActorFromRosterEntry(const RosterEntry& entry) {
    EligibleActor actor;
    actor.actorId = entry.actorId;
    actor.team = entry.team;
    actor.isBot = entry.isBot;
    actor.alive = entry.alive;
    actor.displayName = entry.displayName;
    return actor;
}

inline bool isActivePlayableRosterEntry(const RosterEntry& entry) {
    return entry.sessionPresence == SessionPresence::Connected &&
           isPlayableTeam(entry.team);
}

inline bool isLocalControlEligible(const RosterEntry& entry, int localActorId) {
    return isActivePlayableRosterEntry(entry) &&
           (entry.isBot || entry.actorId == localActorId);
}

inline bool isSpectatorTargetEligible(const RosterEntry& entry) {
    return isActivePlayableRosterEntry(entry);
}

struct TeamScores {
    std::uint16_t attackers{0};
    std::uint16_t defenders{0};
};

struct UserCmd {
    float dtSeconds{0.0f};
    float moveX{0.0f};
    float moveY{0.0f};
    float yaw{0.0f};
    float pitch{0.0f};
    std::uint32_t buttons{0u};

    bool has(CommandButton button) const {
        return hasButton(buttons, button);
    }
};

struct PlayerCommand {
    std::uint32_t seq{0};
    float dtSeconds{0.0f};
    float moveX{0.0f};
    float moveY{0.0f};
    float yaw{0.0f};
    float pitch{0.0f};
    std::uint32_t buttons{0u};
    std::uint64_t viewedServerTimeUs{0};
    std::uint32_t interpDelayMs{0};
    std::uint64_t controlViewedServerTimeUs{0};
    std::uint32_t controlInterpDelayMs{0};
    TeamId requestedTeam{TeamId::None};
    std::uint16_t reportedLatencyMs{0};
    std::uint8_t reportedLossPct{0};

    UserCmd toUserCmd() const {
        UserCmd command;
        command.dtSeconds = dtSeconds;
        command.moveX = moveX;
        command.moveY = moveY;
        command.yaw = yaw;
        command.pitch = pitch;
        command.buttons = buttons;
        return command;
    }

    void applyUserCmd(const UserCmd& userCmd) {
        dtSeconds = userCmd.dtSeconds;
        moveX = userCmd.moveX;
        moveY = userCmd.moveY;
        yaw = userCmd.yaw;
        pitch = userCmd.pitch;
        buttons = userCmd.buttons;
    }

    CommandTiming toCommandTiming() const {
        CommandTiming timing;
        timing.viewedServerTimeUs = viewedServerTimeUs;
        timing.interpolationDelayMs = interpDelayMs;
        return timing;
    }

    void applyCommandTiming(const CommandTiming& timing) {
        viewedServerTimeUs = timing.viewedServerTimeUs;
        interpDelayMs = timing.interpolationDelayMs;
    }

    CommandTiming toControlTiming() const {
        CommandTiming timing;
        timing.viewedServerTimeUs = controlViewedServerTimeUs;
        timing.interpolationDelayMs = controlInterpDelayMs;
        return timing;
    }

    void applyControlTiming(const CommandTiming& timing) {
        controlViewedServerTimeUs = timing.viewedServerTimeUs;
        controlInterpDelayMs = timing.interpolationDelayMs;
    }

    bool hasControlTiming() const {
        return controlViewedServerTimeUs > 0u || controlInterpDelayMs > 0u;
    }

    static PlayerCommand fromUserCmd(std::uint32_t sequence,
                                     const UserCmd& userCmd,
                                     const CommandTiming& timing = {},
                                     TeamId requestedTeamValue = TeamId::None,
                                     std::uint16_t reportedLatencyMsValue = 0u,
                                     std::uint8_t reportedLossPctValue = 0u,
                                     const CommandTiming& controlTiming = {}) {
        PlayerCommand command;
        command.seq = sequence;
        command.applyUserCmd(userCmd);
        command.applyCommandTiming(timing);
        command.applyControlTiming(controlTiming);
        command.requestedTeam = requestedTeamValue;
        command.reportedLatencyMs = reportedLatencyMsValue;
        command.reportedLossPct = reportedLossPctValue;
        return command;
    }

    bool has(CommandButton button) const {
        return hasButton(buttons, button);
    }
};

struct HitscanRay {
    Vec3 origin{};
    Vec3 direction{0.0f, 0.0f, -1.0f};
    float maxDistance{defaults::kWeaponRange};
};

struct HitscanTarget {
    int entityId{0};
    Vec3 center{};
    float radius{defaults::kEnemyRadius};
    bool alive{true};
};

struct FireResult {
    bool fired{false};
    bool hit{false};
    int hitEntityId{-1};
    float hitDistance{0.0f};
    Vec3 hitPoint{};
};

}  // namespace sim
