#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "LevelData.hpp"
#include "client/ReplaySubsystem.hpp"
#include "net/ProxyRuntime.hpp"
#include "net/SessionDiscovery.hpp"
#include "net/SessionFlowController.hpp"
#include "telemetry/StudyEventLog.hpp"

namespace app {

inline std::string makeStudyEventRunId(const net::SessionLaunchConfig& config) {
    std::ostringstream stream;
    stream << "study_" << telemetry::currentLocalTimestampStamp()
           << "_level_" << config.levelSlot
           << "_port_" << config.publicJoinPort;
    return telemetry::sanitizeRunId(stream.str());
}

inline bool buildHostedMovementEnvironment(const net::SessionLaunchConfig& config,
                                           sim::MovementEnvironment* environmentOut,
                                           std::string* errorOut,
                                           std::vector<sim::Vec3>* authoredBotSpawnsOut = nullptr) {
    if (environmentOut == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = "level_load_failed";
        }
        return false;
    }

    *environmentOut = sim::MovementEnvironment{};
    if (authoredBotSpawnsOut != nullptr) {
        authoredBotSpawnsOut->clear();
    }
    if (config.levelSlot < 0) {
        return true;
    }
    if (config.levelSlot < 1 || config.levelSlot > 9) {
        if (errorOut != nullptr) {
            *errorOut = "level_load_failed";
        }
        return false;
    }

    LevelData::LevelDefinition level;
    if (!LevelData::loadLevel(level, config.levelSlot)) {
        if (errorOut != nullptr) {
            *errorOut = "level_load_failed";
        }
        return false;
    }

    environmentOut->collisionBoxes.reserve(level.obstacles.size());
    for (const auto& obstacle : level.obstacles) {
        environmentOut->collisionBoxes.push_back(
            sim::CollisionBox{
                sim::Vec3{obstacle.x, obstacle.height * 0.5f, obstacle.z},
                sim::Vec3{obstacle.width * 0.5f, obstacle.height * 0.5f, obstacle.depth * 0.5f}});
    }
    if (authoredBotSpawnsOut != nullptr) {
        authoredBotSpawnsOut->reserve(level.enemies.size());
        for (const auto& spawn : level.enemies) {
            authoredBotSpawnsOut->push_back(sim::Vec3{spawn.x, 0.0f, spawn.z});
        }
    }
    return true;
}

class SessionComposer {
public:
    struct Result {
        bool ok{false};
        net::SessionRuntimeComposition composition{};
        client::ReplayPaneLayout replayLayout{};
        std::vector<client::ReplayPaneBindingChange> replayPaneBindingTimeline{};
        std::string error{};
    };

    explicit SessionComposer(net::SessionLaunchConfig config = {})
        : config_(std::move(config)) {}

    const net::SessionLaunchConfig& launchConfig() const {
        return config_;
    }

    net::HostedSessionMetadata hostedSessionMetadata() const {
        net::SessionLaunchConfig normalized = config_;
        net::normalizeSessionLaunchConfig(&normalized);
        return net::makeHostedSessionMetadata(normalized);
    }

    Result compose() const {
        Result result;
        result.composition.config = config_;
        net::SessionLaunchConfig& config = result.composition.config;
        net::normalizeSessionLaunchConfig(&config);
        if (config.studyOptions.enableEventLogging && config.studyEventRunId.empty()) {
            config.studyEventRunId = makeStudyEventRunId(config);
        }
        if (config.studyOptions.enableEventLogging && config.studyEventLogDirectory.empty()) {
            config.studyEventLogDirectory =
                telemetry::defaultStudyEventRunDirectory(config.studyEventRunId);
        }

        auto fail = [&result](const std::string& reason) -> Result {
            result.ok = false;
            result.error = reason.empty() ? "session launch failed" : reason;
            return std::move(result);
        };

        if (config.entryPoint == net::SessionEntryPoint::Replay) {
            result.replayLayout =
                client::ReplaySubsystem::defaultReplayPaneLayout(config.localParticipantCount);
            result.replayPaneBindingTimeline =
                client::ReplaySubsystem::defaultPaneBindingTimeline(config.localParticipantCount);
            result.ok = true;
            return result;
        }

        if (!net::isRuntimeEntryPoint(config.entryPoint)) {
            result.ok = true;
            return result;
        }

        if (config.startLocalServer) {
            sim::MovementEnvironment hostedEnvironment;
            std::vector<sim::Vec3> authoredBotSpawns;
            std::string environmentError;
            if (!buildHostedMovementEnvironment(config,
                                                &hostedEnvironment,
                                                &environmentError,
                                                &authoredBotSpawns)) {
                return fail(environmentError);
            }

            auto bindServer = [&](std::uint16_t port) {
                return result.composition.hostedServerSocket_.bind({config.serverListenHost, port});
            };

            if (config.serverListenPort == 0u) {
                bool bound = false;
                for (int attempt = 0; attempt < 16; ++attempt) {
                    if (!bindServer(0u)) {
                        return fail(result.composition.hostedServerSocket_.lastError());
                    }

                    const std::uint16_t actualPort =
                        result.composition.hostedServerSocket_.localPort();
                    const bool collidesWithPublicPort = actualPort == config.proxyClientListenPort;
                    const bool collidesWithExplicitProxyServerPort =
                        config.proxyServerListenPort != 0u &&
                        actualPort == config.proxyServerListenPort;
                    if (!collidesWithPublicPort && !collidesWithExplicitProxyServerPort) {
                        config.serverListenPort = actualPort;
                        bound = true;
                        break;
                    }

                    result.composition.hostedServerSocket_ = net::UdpSocket{};
                }

                if (!bound) {
                    return fail("failed to allocate an internal server port distinct from the public join port");
                }
            } else if (!bindServer(config.serverListenPort)) {
                return fail(result.composition.hostedServerSocket_.lastError());
            }

            config.serverListenPort = result.composition.hostedServerSocket_.localPort();
            config.proxyUpstreamServerPort = config.serverListenPort;

            net::ServerConfig serverConfig;
            serverConfig.listenPort = config.serverListenPort;
            serverConfig.tickRateHz = config.tickRateHz;
            serverConfig.snapshotRateHz = config.snapshotRateHz;
            serverConfig.maxPlayers = config.maxHumanPlayers;
            serverConfig.maxHumanPlayers = config.maxHumanPlayers;
            serverConfig.attackerBotCount = config.attackerBotCount;
            serverConfig.defenderBotCount = config.defenderBotCount;
            serverConfig.authoredBotSpawns = std::move(authoredBotSpawns);
            serverConfig.authoredBotTeamBias = sim::isPlayableTeam(config.preferredTeam)
                ? config.preferredTeam
                : sim::TeamId::None;
            serverConfig.suppressDefaultEnemiesForAuthoredLevel =
                config.levelSlot >= 1 && config.levelSlot <= 9;
            serverConfig.sessionLabel = config.sessionLabel;
            serverConfig.hostPlayerName = config.playerName.empty() ? "player" : config.playerName;
            serverConfig.publicJoinPort = config.publicJoinPort;
            serverConfig.studyActionsEnabled =
                config.surface == net::SessionProductSurface::LabStudy;
            serverConfig.studyEventLoggingEnabled = config.studyOptions.enableEventLogging;
            serverConfig.studyEventRunId = config.studyEventRunId;
            serverConfig.studyEventLogDirectory = config.studyEventLogDirectory;
            serverConfig.levelSlot = config.levelSlot;
            serverConfig.levelHash = config.levelHash;
            serverConfig.shotEvaluationMode = config.shotEvaluationMode;
            serverConfig.visualizationMode = config.visualizationMode;
            serverConfig.characterProfileName = config.characterProfileName;
            serverConfig.characterAppearance = config.characterAppearance;
            result.composition.hostedServer_ =
                std::make_unique<net::ServerRuntime>(serverConfig, sim::SimConfig{}, hostedEnvironment);
            result.composition.startupSequence_.push_back("server");
        }

        if (config.startLocalProxy) {
            net::ProxyConfig proxyConfig;
            proxyConfig.clientListenEndpoint = {config.proxyClientListenHost, config.proxyClientListenPort};
            proxyConfig.serverEndpoint = {config.proxyUpstreamServerHost, config.proxyUpstreamServerPort};
            proxyConfig.serverListenEndpoint = {config.proxyServerListenHost, config.proxyServerListenPort};
            proxyConfig.defaultUpstream = config.proxyUpstreamLink;
            proxyConfig.defaultDownstream = config.proxyDownstreamLink;

            auto proxy = std::make_unique<net::ProxyRuntime>(proxyConfig);
            if (!proxy->start()) {
                return fail(proxy->lastError());
            }

            config.proxyClientListenPort = proxy->clientListenPort();
            config.proxyServerListenPort = proxy->serverListenPort();
            if (config.mode == net::SessionLaunchMode::Host) {
                config.publicJoinPort = config.proxyClientListenPort;
                config.clientConnectHost =
                    config.proxyClientListenHost.empty() ? "127.0.0.1" : config.proxyClientListenHost;
                config.clientConnectPort = config.proxyClientListenPort;
            } else {
                config.clientConnectHost =
                    config.proxyClientListenHost.empty() ? "127.0.0.1" : config.proxyClientListenHost;
                config.clientConnectPort = config.proxyClientListenPort;
            }

            result.composition.proxy_ = std::move(proxy);
            result.composition.startupSequence_.push_back("proxy");
        }

        if (net::isHostedRuntimeEntryPoint(config.entryPoint) && config.discoveryPort != 0u) {
            result.composition.discoverySocket_ = net::UdpSocket{};
            result.composition.discoverySocket_.bind({"0.0.0.0", config.discoveryPort});
        }

        net::ClientConfig clientConfig;
        clientConfig.serverHost = config.clientConnectHost;
        clientConfig.serverPort = config.clientConnectPort;
        clientConfig.localPort = config.clientLocalPort;
        clientConfig.playerName = config.playerName;
        clientConfig.preferredTeam = config.preferredTeam;
        clientConfig.sessionId = config.clientSessionId;
        clientConfig.protocolVersion = config.protocolVersion;
        clientConfig.connectTimeoutUs = config.clientConnectTimeoutUs;
        clientConfig.helloRetryIntervalUs = config.clientHelloRetryIntervalUs;
        clientConfig.serverSilenceTimeoutUs = config.clientServerSilenceTimeoutUs;
        clientConfig.studyActionsEnabled =
            config.surface == net::SessionProductSurface::LabStudy;
        clientConfig.studyEventLoggingEnabled = config.studyOptions.enableEventLogging;
        clientConfig.studyEventRunId = config.studyEventRunId;
        clientConfig.studyEventLogDirectory = config.studyEventLogDirectory;

        auto client = std::make_unique<net::ClientRuntime>(clientConfig);
        if (result.composition.proxy_ != nullptr) {
            client->attachProxyDiagnostics(result.composition.proxy_.get(), 0u);
        }

        if (!client->start()) {
            return fail(client->statusMessage());
        }

        result.composition.client_ = std::move(client);
        result.composition.startupSequence_.push_back("client");
        result.ok = true;
        return result;
    }

private:
    net::SessionLaunchConfig config_{};
};

}  // namespace app
