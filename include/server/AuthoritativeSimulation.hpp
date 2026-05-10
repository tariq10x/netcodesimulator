#pragma once

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "net/LagCompensation.hpp"
#include "net/Protocol.hpp"
#include "server/BotDirector.hpp"
#include "server/BotCommandSource.hpp"
#include "server/ServerGateway.hpp"
#include "sim/WorldState.hpp"

namespace replay {
class CommandReplayRecorder;
}

namespace net {
namespace server {

enum class CombatStudyEventKind : std::uint8_t {
    ShotResolved = 0,
    PlayerKilled = 1
};

struct CombatStudyEvent {
    CombatStudyEventKind kind{CombatStudyEventKind::ShotResolved};
    int shooterActorId{0};
    int targetActorId{-1};
    std::uint32_t commandSeq{0u};
    std::uint64_t commandViewedServerTimeUs{0u};
    std::uint32_t commandInterpolationDelayMs{0u};
    sim::Vec3 shotOrigin{};
    sim::Vec3 shotDirection{0.0f, 0.0f, -1.0f};
    bool hit{false};
    bool blockedByGeometry{false};
    float hitDistance{0.0f};
    float damage{0.0f};
    float targetHealthBefore{0.0f};
    float targetHealthAfter{0.0f};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
};

struct StepContext {
    float dtSeconds{0.0f};
    float respawnDelaySeconds{0.0f};
    float spawnProtectionSeconds{0.0f};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    sim::TimingCadence cadence{};
    sim::AuthoritativeTime authoritativeTime{};
};

class AuthoritativeSimulation {
public:
    explicit AuthoritativeSimulation(BotCommandSource botCommandSource = {},
                                     BotDirectorConfig botDirectorConfig = {});

    void setBotsFrozen(bool frozen);
    bool botsFrozen() const;
    void setBotShootingEnabled(bool enabled);
    bool botShootingEnabled() const;
    void setBotFrozenPassive(int actorId, bool frozenPassive);
    void setBotSpawnPosition(int actorId, const sim::Vec3& spawnPosition);

    bool applyTeamChangeRequest(sim::WorldState& worldState,
                                ClientSession& session,
                                const TeamChangeRequest& request,
                                const sim::SimConfig& simConfig,
                                std::vector<SnapshotEvent>* outEvents) const;
    bool applyRuntimeParamChangeRequest(sim::WorldState& worldState,
                                        std::vector<ClientSession>& sessions,
                                        ClientSession& session,
                                        const RuntimeParamChangeRequest& request,
                                        RuntimeParamApplyResult* resultOut);

    void stepWorld(sim::WorldState& worldState,
                   std::vector<ClientSession>& sessions,
                   float dtSeconds,
                   const sim::SimConfig& simConfig,
                   float respawnDelaySeconds,
                   float spawnProtectionSeconds,
                   ShotEvaluationMode shotEvaluationMode,
                   LagCompensationHistory& lagCompensation,
                   std::uint64_t currentServerTimeUs,
                   std::vector<SnapshotEvent>* outEvents,
                   std::vector<CombatStudyEvent>* outStudyEvents = nullptr,
                   replay::CommandReplayRecorder* replayRecorder = nullptr,
                   std::uint32_t serverTick = 0u);

    void stepWorld(sim::WorldState& worldState,
                   std::vector<ClientSession>& sessions,
                   const StepContext& context,
                   const sim::SimConfig& simConfig,
                   LagCompensationHistory& lagCompensation,
                   std::vector<SnapshotEvent>* outEvents,
                   std::vector<CombatStudyEvent>* outStudyEvents = nullptr,
                   replay::CommandReplayRecorder* replayRecorder = nullptr) {
        worldState.cadence = context.cadence;
        worldState.authoritativeTime = context.authoritativeTime;
        stepWorld(worldState,
                  sessions,
                  context.dtSeconds,
                  simConfig,
                  context.respawnDelaySeconds,
                  context.spawnProtectionSeconds,
                  context.shotEvaluationMode,
                  lagCompensation,
                  context.authoritativeTime.serverTimeUs,
                  outEvents,
                  outStudyEvents,
                  replayRecorder,
                  context.authoritativeTime.serverTick);
    }

    std::vector<sim::PlayerState> botControlPlayers(const sim::WorldState& worldState) const;

private:
    struct PendingBotCommand {
        sim::PlayerCommand command{};
        std::uint64_t deliverAtUs{0u};
        std::uint64_t order{0u};
    };

    struct BotRuntimeState {
        ProxyLinkConfig config{};
        std::mt19937 rng{std::random_device{}()};
        std::vector<PendingBotCommand> scheduled{};
        std::uint64_t orderCounter{0u};
        std::uint32_t replayCommandSeq{0u};
        sim::PlayerState controlPlayerState{};
        bool hasControlPlayerState{false};
        std::uint32_t lastControlSeq{0u};
        bool frozenPassive{false};
        int targetActorId{-1};
        float visibleTargetSeconds{0.0f};
        float shotCooldownRemaining{0.0f};
        float movementDecisionRemaining{0.0f};
        float lateralMove{0.0f};
        float forwardBias{0.0f};
        sim::Vec3 spawnPosition{};
        bool hasSpawnPosition{false};
        float respawnTimerSeconds{0.0f};
    };

    void applyQueuedCommands(sim::WorldState& worldState,
                             const std::vector<ClientSession>& sessions,
                             ClientSession& session,
                             float dtSeconds,
                             const sim::SimConfig& simConfig,
                             ShotEvaluationMode shotEvaluationMode,
                             LagCompensationHistory& lagCompensation,
                             std::uint64_t currentServerTimeUs,
                             std::vector<SnapshotEvent>* outEvents,
                             std::vector<CombatStudyEvent>* outStudyEvents,
                             replay::CommandReplayRecorder* replayRecorder,
                             std::uint32_t serverTick) const;
    void applyBotCommands(sim::WorldState& worldState,
                          const std::vector<ClientSession>& sessions,
                          float dtSeconds,
                          const sim::SimConfig& simConfig,
                          float respawnDelaySeconds,
                          ShotEvaluationMode shotEvaluationMode,
                          LagCompensationHistory& lagCompensation,
                          std::uint64_t currentServerTimeUs,
                          std::vector<SnapshotEvent>* outEvents,
                          std::vector<CombatStudyEvent>* outStudyEvents,
                          replay::CommandReplayRecorder* replayRecorder,
                          std::uint32_t serverTick);
    void processFireCommand(sim::WorldState& worldState,
                            const std::vector<ClientSession>& sessions,
                            int shooterActorId,
                            const sim::PlayerState& shooterState,
                            const sim::PlayerCommand& command,
                            const std::vector<sim::HitscanTarget>& evaluationTargets,
                            const sim::SimConfig& simConfig,
                            std::uint64_t currentServerTimeUs,
                            ShotEvaluationMode shotEvaluationMode,
                            std::vector<SnapshotEvent>* outEvents,
                            std::vector<CombatStudyEvent>* outStudyEvents) const;
    static void resetBotRuntimeState(BotRuntimeState& state);
    static void resetBotCombatState(BotRuntimeState& state);
    static void resetBotControlState(BotRuntimeState& state);
    static void syncBotControlStateFromAuthoritative(BotRuntimeState& state,
                                                     const sim::PlayerState& authoritativePlayer,
                                                     int actorId);
    static void syncBotControlVitalsFromAuthoritative(BotRuntimeState& state,
                                                      const sim::PlayerState& authoritativePlayer);

    BotCommandSource botCommandSource_{};
    BotDirectorConfig botDirectorConfig_{};
    bool botsFrozen_{true};
    bool botShootingEnabled_{true};
    std::unordered_map<int, BotRuntimeState> botRuntimeStates_{};
};

}  // namespace server
}  // namespace net
