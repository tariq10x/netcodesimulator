#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "net/LagCompensation.hpp"
#include "net/Protocol.hpp"
#include "replay/ReplayRecorder.hpp"
#include "server/AuthoritativeSimulation.hpp"
#include "server/BotDirector.hpp"
#include "server/ServerGateway.hpp"
#include "sim/WorldState.hpp"
#include "telemetry/StudyEventLog.hpp"

namespace net {

struct ServerConfig {
    std::uint16_t listenPort{41000};
    std::uint16_t tickRateHz{60};
    std::uint16_t snapshotRateHz{20};
    std::uint16_t maxPlayers{2};
    std::uint16_t maxHumanPlayers{0};
    std::uint16_t attackerBotCount{0};
    std::uint16_t defenderBotCount{0};
    std::vector<sim::Vec3> authoredBotSpawns{};
    sim::TeamId authoredBotTeamBias{sim::TeamId::None};
    bool suppressDefaultEnemiesForAuthoredLevel{false};
    std::uint32_t maxRewindMs{500};
    std::uint64_t clientTimeoutUs{5'000'000};
    float respawnDelaySeconds{5.0f};
    float spawnProtectionSeconds{0.0f};
    std::string sessionLabel{};
    std::string hostPlayerName{"player"};
    std::uint16_t publicJoinPort{0};
    bool studyActionsEnabled{false};
    bool studyEventLoggingEnabled{false};
    std::string studyEventRunId{};
    std::filesystem::path studyEventLogDirectory{};
    int levelSlot{-1};
    std::uint32_t levelHash{0u};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    SessionVisualizationMode visualizationMode{SessionVisualizationMode::Diagnostic};
    server::BotDirectorConfig botDirector{};
};

class ServerRuntime {
public:
    explicit ServerRuntime(const ServerConfig& config = {},
                           const sim::SimConfig& simConfig = {},
                           const sim::MovementEnvironment& environment = {});

    bool acceptClient(const HelloMessage& hello,
                      std::uint64_t nowUs,
                      WelcomeMessage* welcomeOut = nullptr,
                      std::string* rejectReasonOut = nullptr);
    bool disconnectClient(std::uint16_t peerId, const std::string& reason = {});
    bool handleControlPayload(std::uint16_t peerId,
                              const PacketPayload& payload,
                              std::uint64_t nowUs);
    bool enqueueCommandBundle(std::uint16_t peerId,
                              const CommandBundle& bundle,
                              std::uint64_t nowUs);
    bool enqueueControlCommandBundle(std::uint16_t peerId,
                                     const ControlCommandBundle& bundle,
                                     std::uint64_t nowUs);

    void tickOnce(std::uint64_t nowUs);
    std::vector<Packet> takePendingPackets();

    const ServerConfig& config() const;
    std::uint64_t tickIntervalUs() const;
    std::uint64_t snapshotIntervalUs() const;
    const sim::WorldState& worldState() const;
    sim::WorldState& worldState();
    const std::vector<ClientSession>& sessions() const;
    const ClientSession* findSession(std::uint16_t peerId) const;
    void setCommandReplayRecordingEnabled(bool enabled);
    bool commandReplayRecordingEnabled() const;
    const replay::ReplayDemo& commandReplayDemo() const;

private:
    ClientSession* findSessionMutable(std::uint16_t peerId);
    bool handleTeamChangeRequest(ClientSession& session, const TeamChangeRequest& request);
    bool shouldAcceptGameplayCommand(const ClientSession& session,
                                     const sim::PlayerCommand& command) const;
    bool shouldAcceptControlGameplayCommand(const ClientSession& session,
                                            const sim::PlayerCommand& command) const;
    bool handleShotEvaluationModeChange(ClientSession& session,
                                        const RuntimeParamChangeRequest& request,
                                        RuntimeParamApplyResult* resultOut);
    bool handleSessionTickRateChange(ClientSession& session,
                                     const RuntimeParamChangeRequest& request,
                                     RuntimeParamApplyResult* resultOut);
    bool handleSessionSnapshotRateChange(ClientSession& session,
                                         const RuntimeParamChangeRequest& request,
                                         RuntimeParamApplyResult* resultOut);
    bool handleStudyEventLoggingChange(ClientSession& session,
                                       const RuntimeParamChangeRequest& request,
                                       RuntimeParamApplyResult* resultOut);
    bool handleSessionVisualizationModeChange(ClientSession& session,
                                              const RuntimeParamChangeRequest& request,
                                              RuntimeParamApplyResult* resultOut);
    bool handleBotDirectorActiveChange(ClientSession& session,
                                       const RuntimeParamChangeRequest& request,
                                       RuntimeParamApplyResult* resultOut);
    bool handleBotShootingEnabledChange(ClientSession& session,
                                        const RuntimeParamChangeRequest& request,
                                        RuntimeParamApplyResult* resultOut);
    bool handleHostAdminAddBot(ClientSession& session,
                               const RuntimeParamChangeRequest& request,
                               RuntimeParamApplyResult* resultOut);
    bool handleHostAdminTeamChange(ClientSession& session,
                                   const RuntimeParamChangeRequest& request,
                                   RuntimeParamApplyResult* resultOut);
    bool handleHostAdminKick(ClientSession& session,
                             const RuntimeParamChangeRequest& request,
                             RuntimeParamApplyResult* resultOut);
    bool handleSessionAction(ClientSession& session,
                             const SessionActionRequest& request,
                             SessionActionResult* resultOut);
    bool spawnFrozenBotAhead(ClientSession& session, SessionActionResult* resultOut);
    void applyTickRateHz(std::uint16_t tickRateHz);
    void applySnapshotRateHz(std::uint16_t snapshotRateHz);
    void syncControlGhostStates(float dtSeconds);
    void syncControlPlayerStateFromAuthoritative(ClientSession& session);
    sim::TeamId* spectatorReturnTeam(std::uint16_t peerId);
    void refreshAuthoritativeWorldStateMetadata();
    HostedSessionMetadata hostedSessionMetadata() const;
    void spawnConfiguredBots();
    bool ensureStudyEventSink();
    telemetry::StudyEventRecord makeServerStudyEvent(const std::string& eventName);
    void recordCombatStudyEvent(const server::CombatStudyEvent& event);

    struct PendingShotEvaluationChange {
        bool active{false};
        ShotEvaluationMode mode{ShotEvaluationMode::SeenPosition};
    };

    ServerConfig config_{};
    sim::SimConfig simConfig_{};
    sim::WorldState worldState_{};
    LagCompensationHistory lagCompensation_{};
    server::ServerGateway gateway_{};
    server::AuthoritativeSimulation authoritativeSimulation_{};
    std::uint64_t tickIntervalUs_{0};
    std::uint64_t snapshotIntervalUs_{0};
    std::uint64_t snapshotAccumulatorUs_{0};
    std::uint32_t serverTick_{0};
    std::uint64_t currentServerTimeUs_{0};
    PendingShotEvaluationChange pendingShotEvaluationChange_{};
    std::optional<std::uint16_t> pendingSessionTickRateHz_{};
    std::optional<std::uint16_t> pendingSessionSnapshotRateHz_{};
    std::vector<sim::TeamId> spectatorReturnTeams_{};
    std::unique_ptr<telemetry::StudyEventSink> studyEventSink_{};
    std::uint64_t studyEventSeq_{0u};
    replay::CommandReplayRecorder commandReplayRecorder_{};
};

}  // namespace net
