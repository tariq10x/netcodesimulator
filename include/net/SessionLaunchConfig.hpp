#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

#include "net/Protocol.hpp"

namespace net {

enum class SessionLaunchMode : std::uint8_t {
    None = 0,
    Host = 1,
    Join = 2
};

enum class SessionProductSurface : std::uint8_t {
    None = 0,
    Multiplayer = 1,
    LabStudy = 2,
    Replay = 3,
    LevelEditor = 4
};

enum class SessionEntryPoint : std::uint8_t {
    None = 0,
    Host = 1,
    Join = 2,
    LabStudy = 3,
    Replay = 4,
    LevelEditor = 5
};

constexpr std::uint16_t kDefaultServerPort = 41000u;
constexpr std::uint16_t kDefaultProxyClientPort = kDefaultServerPort;
constexpr std::uint16_t kDefaultProxyServerPort = 0u;
constexpr std::uint16_t kDefaultSessionDiscoveryPort = 41001u;
constexpr std::uint16_t kDefaultHostedBotCount = 0u;
constexpr std::uint16_t kMaxHostedBotCount = 16u;
constexpr std::uint16_t kMaxHostedHumanPlayerCap = 16u;
constexpr std::uint16_t kDefaultHostedHumanPlayerCap = 2u;
constexpr std::uint16_t kDefaultHostedSnapshotRateHz = 20u;
constexpr std::uint8_t kDefaultLocalParticipantCount = 1u;

struct SessionStudyOptions {
    bool enablePredictionToggle{false};
    bool enableShotStrategyToggle{false};
    bool enableReplayCapture{false};
    bool enableEventLogging{false};
};

inline std::uint32_t makeLevelIdentityHash(int levelSlot) {
    std::uint32_t hash = 2166136261u;
    const std::uint32_t slotBits = static_cast<std::uint32_t>(levelSlot);
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>((slotBits >> shift) & 0xFFu);
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

struct SessionLaunchConfig {
    SessionLaunchMode mode{SessionLaunchMode::None};
    SessionProductSurface surface{SessionProductSurface::Multiplayer};
    SessionEntryPoint entryPoint{SessionEntryPoint::None};
    std::string sessionLabel{};
    int levelSlot{-1};
    std::uint32_t levelHash{0u};
    std::string playerName{"player"};
    std::uint8_t localParticipantCount{kDefaultLocalParticipantCount};
    sim::TeamId preferredTeam{sim::TeamId::None};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    SessionVisualizationMode visualizationMode{SessionVisualizationMode::Diagnostic};
    std::uint16_t tickRateHz{kDefaultSessionTickRateHz};
    std::uint16_t snapshotRateHz{kDefaultHostedSnapshotRateHz};
    SessionStudyOptions studyOptions{};
    std::string studyEventRunId{};
    std::filesystem::path studyEventLogDirectory{};
    std::uint16_t publicJoinPort{kDefaultServerPort};
    std::uint16_t discoveryPort{kDefaultSessionDiscoveryPort};
    std::uint16_t maxHumanPlayers{kDefaultHostedHumanPlayerCap};
    std::uint16_t attackerBotCount{kDefaultHostedBotCount};
    std::uint16_t defenderBotCount{kDefaultHostedBotCount};
    std::uint16_t protocolVersion{kProtocolVersion};
    std::uint64_t clientConnectTimeoutUs{3'000'000u};
    std::uint64_t clientHelloRetryIntervalUs{250'000u};
    std::uint64_t clientServerSilenceTimeoutUs{5'000'000u};
    std::uint32_t clientSessionId{0u};

    bool startLocalServer{false};
    std::string serverListenHost{"127.0.0.1"};
    std::uint16_t serverListenPort{0u};

    bool startLocalProxy{false};
    std::string proxyClientListenHost{"127.0.0.1"};
    std::uint16_t proxyClientListenPort{kDefaultProxyClientPort};
    std::string proxyServerListenHost{"127.0.0.1"};
    std::uint16_t proxyServerListenPort{kDefaultProxyServerPort};
    std::string proxyUpstreamServerHost{"127.0.0.1"};
    std::uint16_t proxyUpstreamServerPort{0u};
    ProxyLinkConfig proxyUpstreamLink{};
    ProxyLinkConfig proxyDownstreamLink{};

    std::string clientConnectHost{"127.0.0.1"};
    std::uint16_t clientConnectPort{kDefaultServerPort};
    std::uint16_t clientLocalPort{0};
};

inline bool isSupportedSessionTickRateChoice(std::uint16_t tickRateHz) {
    return std::find(kSessionTickRateChoicesHz.begin(),
                     kSessionTickRateChoicesHz.end(),
                     tickRateHz) != kSessionTickRateChoicesHz.end();
}

inline bool isRuntimeEntryPoint(SessionEntryPoint entryPoint) {
    return entryPoint == SessionEntryPoint::Host ||
           entryPoint == SessionEntryPoint::Join ||
           entryPoint == SessionEntryPoint::LabStudy;
}

inline bool isHostedRuntimeEntryPoint(SessionEntryPoint entryPoint) {
    return entryPoint == SessionEntryPoint::Host ||
           entryPoint == SessionEntryPoint::LabStudy;
}

inline void normalizeSessionLaunchConfig(SessionLaunchConfig* config) {
    if (config == nullptr) {
        return;
    }

    if (config->entryPoint == SessionEntryPoint::None) {
        if (config->mode == SessionLaunchMode::Host) {
            config->entryPoint = SessionEntryPoint::Host;
        } else if (config->mode == SessionLaunchMode::Join) {
            config->entryPoint = SessionEntryPoint::Join;
        }
    }

    if (config->surface == SessionProductSurface::None) {
        switch (config->entryPoint) {
            case SessionEntryPoint::LabStudy:
                config->surface = SessionProductSurface::LabStudy;
                break;
            case SessionEntryPoint::Replay:
                config->surface = SessionProductSurface::Replay;
                break;
            case SessionEntryPoint::LevelEditor:
                config->surface = SessionProductSurface::LevelEditor;
                break;
            case SessionEntryPoint::Host:
            case SessionEntryPoint::Join:
            case SessionEntryPoint::None:
                config->surface = SessionProductSurface::Multiplayer;
                break;
        }
    }

    if (config->playerName.empty()) {
        config->playerName = "player";
    }
    if (config->localParticipantCount == 0u) {
        config->localParticipantCount = kDefaultLocalParticipantCount;
    }
    if (!isSupportedSessionTickRateChoice(config->tickRateHz)) {
        config->tickRateHz = kDefaultSessionTickRateHz;
    }
    if (!isSupportedSessionTickRateChoice(config->snapshotRateHz)) {
        config->snapshotRateHz = kDefaultHostedSnapshotRateHz;
    }
    if (config->snapshotRateHz > config->tickRateHz) {
        config->snapshotRateHz = config->tickRateHz;
    }
    if (config->maxHumanPlayers == 0u) {
        config->maxHumanPlayers = kDefaultHostedHumanPlayerCap;
    }
    if (config->maxHumanPlayers > kMaxHostedHumanPlayerCap) {
        config->maxHumanPlayers = kMaxHostedHumanPlayerCap;
    }
    if (config->maxHumanPlayers < config->localParticipantCount) {
        config->maxHumanPlayers = config->localParticipantCount;
    }
    if (config->levelHash == 0u && config->levelSlot >= 0) {
        config->levelHash = makeLevelIdentityHash(config->levelSlot);
    }

    switch (config->entryPoint) {
        case SessionEntryPoint::Host:
            config->mode = SessionLaunchMode::Host;
            config->startLocalServer = true;
            config->startLocalProxy = true;
            break;
        case SessionEntryPoint::Join:
            config->mode = SessionLaunchMode::Join;
            config->startLocalServer = false;
            config->startLocalProxy = true;
            break;
        case SessionEntryPoint::LabStudy:
            config->mode = SessionLaunchMode::Host;
            config->surface = SessionProductSurface::LabStudy;
            config->startLocalServer = true;
            config->startLocalProxy = true;
            break;
        case SessionEntryPoint::Replay:
            config->mode = SessionLaunchMode::None;
            config->surface = SessionProductSurface::Replay;
            config->startLocalServer = false;
            config->startLocalProxy = false;
            break;
        case SessionEntryPoint::LevelEditor:
            config->mode = SessionLaunchMode::None;
            config->surface = SessionProductSurface::LevelEditor;
            config->startLocalServer = false;
            config->startLocalProxy = false;
            break;
        case SessionEntryPoint::None:
            break;
    }
}

inline bool parseSessionPort(const std::string& text, std::uint16_t* portOut) {
    if (portOut == nullptr || text.empty()) {
        return false;
    }

    unsigned int value = 0u;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10u + static_cast<unsigned int>(ch - '0');
        if (value > 65535u) {
            return false;
        }
    }

    if (value == 0u) {
        return false;
    }

    *portOut = static_cast<std::uint16_t>(value);
    return true;
}

inline bool parseSessionBotCount(const std::string& text, std::uint16_t* countOut) {
    if (countOut == nullptr || text.empty()) {
        return false;
    }

    unsigned int value = 0u;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10u + static_cast<unsigned int>(ch - '0');
        if (value > kMaxHostedBotCount) {
            return false;
        }
    }

    *countOut = static_cast<std::uint16_t>(value);
    return true;
}

inline bool parseSessionHumanPlayerCap(const std::string& text, std::uint16_t* countOut) {
    if (countOut == nullptr || text.empty()) {
        return false;
    }

    unsigned int value = 0u;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10u + static_cast<unsigned int>(ch - '0');
        if (value > kMaxHostedHumanPlayerCap) {
            return false;
        }
    }

    if (value == 0u) {
        return false;
    }

    *countOut = static_cast<std::uint16_t>(value);
    return true;
}

inline bool isValidSessionHost(const std::string& host) {
    if (host.empty()) {
        return false;
    }

    for (char ch : host) {
        const bool isAlphaNum = (ch >= 'a' && ch <= 'z') ||
                                (ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9');
        const bool isAllowedPunct = ch == '.' || ch == '-' || ch == '_';
        if (!isAlphaNum && !isAllowedPunct) {
            return false;
        }
    }
    return true;
}

inline SessionLaunchConfig makeHostSessionLaunchConfig(int levelSlot,
                                                       const std::string& playerName,
                                                       std::uint16_t publicPort = kDefaultServerPort,
                                                       std::uint16_t attackerBotCount = kDefaultHostedBotCount,
                                                       std::uint16_t defenderBotCount = kDefaultHostedBotCount,
                                                       std::uint16_t serverPort = 0u,
                                                       std::uint16_t proxyServerPort = kDefaultProxyServerPort,
                                                       std::uint16_t protocolVersion = kProtocolVersion,
                                                       sim::TeamId preferredTeam = sim::TeamId::Attacker,
                                                       std::uint8_t localParticipantCount =
                                                           kDefaultLocalParticipantCount,
                                                       ShotEvaluationMode shotEvaluationMode =
                                                           ShotEvaluationMode::SeenPosition) {
    SessionLaunchConfig config;
    config.surface = SessionProductSurface::Multiplayer;
    config.entryPoint = SessionEntryPoint::Host;
    config.mode = SessionLaunchMode::Host;
    config.levelSlot = levelSlot;
    config.levelHash = makeLevelIdentityHash(levelSlot);
    config.playerName = playerName.empty() ? "player" : playerName;
    config.localParticipantCount =
        localParticipantCount == 0u ? kDefaultLocalParticipantCount : localParticipantCount;
    config.preferredTeam = (preferredTeam == sim::TeamId::Spectator ||
                            sim::isPlayableTeam(preferredTeam))
        ? preferredTeam
        : sim::TeamId::Attacker;
    config.shotEvaluationMode = shotEvaluationMode;
    config.publicJoinPort = publicPort;
    config.attackerBotCount = attackerBotCount;
    config.defenderBotCount = defenderBotCount;
    config.protocolVersion = protocolVersion;
    config.startLocalServer = true;
    config.startLocalProxy = true;
    config.serverListenPort = serverPort;
    config.proxyClientListenPort = publicPort;
    config.proxyServerListenPort = proxyServerPort;
    config.proxyUpstreamServerPort = serverPort;
    config.clientConnectHost = "127.0.0.1";
    config.clientConnectPort = publicPort;
    normalizeSessionLaunchConfig(&config);
    return config;
}

inline SessionLaunchConfig makeJoinSessionLaunchConfig(const std::string& host,
                                                       std::uint16_t port,
                                                       const std::string& playerName,
                                                       std::uint16_t protocolVersion = kProtocolVersion,
                                                       sim::TeamId preferredTeam = sim::TeamId::Defender,
                                                       std::uint8_t localParticipantCount =
                                                           kDefaultLocalParticipantCount,
                                                       ShotEvaluationMode shotEvaluationMode =
                                                           ShotEvaluationMode::SeenPosition) {
    SessionLaunchConfig config;
    const std::string resolvedHost = host.empty() ? "127.0.0.1" : host;
    config.surface = SessionProductSurface::Multiplayer;
    config.entryPoint = SessionEntryPoint::Join;
    config.mode = SessionLaunchMode::Join;
    config.playerName = playerName.empty() ? "player" : playerName;
    config.localParticipantCount =
        localParticipantCount == 0u ? kDefaultLocalParticipantCount : localParticipantCount;
    config.preferredTeam = (preferredTeam == sim::TeamId::Spectator ||
                            sim::isPlayableTeam(preferredTeam))
        ? preferredTeam
        : sim::TeamId::Defender;
    config.shotEvaluationMode = shotEvaluationMode;
    config.protocolVersion = protocolVersion;
    config.publicJoinPort = port;
    config.startLocalServer = false;
    config.startLocalProxy = true;
    config.proxyClientListenPort = 0u;
    config.proxyServerListenPort = 0u;
    config.proxyUpstreamServerHost = resolvedHost;
    config.proxyUpstreamServerPort = port;
    config.clientConnectHost = resolvedHost;
    config.clientConnectPort = port;
    normalizeSessionLaunchConfig(&config);
    return config;
}

inline SessionLaunchConfig makeStudySessionLaunchConfig(
    int levelSlot,
    const std::string& playerName = "player",
    std::uint16_t publicPort = kDefaultServerPort,
    std::uint16_t attackerBotCount = 1u,
    std::uint16_t defenderBotCount = 1u,
    ShotEvaluationMode shotEvaluationMode = ShotEvaluationMode::SeenPosition,
    std::uint8_t localParticipantCount = kDefaultLocalParticipantCount) {
    SessionLaunchConfig config =
        makeHostSessionLaunchConfig(levelSlot,
                                    playerName,
                                    publicPort,
                                    attackerBotCount,
                                    defenderBotCount,
                                    0u,
                                    kDefaultProxyServerPort,
                                    kProtocolVersion,
                                    sim::TeamId::Attacker,
                                    localParticipantCount,
                                    shotEvaluationMode);
    config.surface = SessionProductSurface::LabStudy;
    config.entryPoint = SessionEntryPoint::LabStudy;
    config.sessionLabel = "Lab Study";
    config.maxHumanPlayers = config.localParticipantCount;
    config.studyOptions.enablePredictionToggle = true;
    config.studyOptions.enableShotStrategyToggle = true;
    config.studyOptions.enableReplayCapture = true;
    normalizeSessionLaunchConfig(&config);
    return config;
}

inline SessionLaunchConfig makeReplaySessionLaunchConfig(
    int levelSlot,
    std::uint8_t localParticipantCount = kDefaultLocalParticipantCount) {
    SessionLaunchConfig config;
    config.surface = SessionProductSurface::Replay;
    config.entryPoint = SessionEntryPoint::Replay;
    config.levelSlot = levelSlot;
    config.localParticipantCount =
        localParticipantCount == 0u ? kDefaultLocalParticipantCount : localParticipantCount;
    normalizeSessionLaunchConfig(&config);
    return config;
}

inline SessionLaunchConfig makeEditorSessionLaunchConfig(int levelSlot) {
    SessionLaunchConfig config;
    config.surface = SessionProductSurface::LevelEditor;
    config.entryPoint = SessionEntryPoint::LevelEditor;
    config.levelSlot = levelSlot;
    normalizeSessionLaunchConfig(&config);
    return config;
}

inline HostedSessionMetadata makeHostedSessionMetadata(const SessionLaunchConfig& config) {
    SessionLaunchConfig normalized = config;
    normalizeSessionLaunchConfig(&normalized);

    HostedSessionMetadata metadata;
    metadata.sessionLabel = normalized.sessionLabel;
    metadata.hostPlayerName = normalized.playerName;
    metadata.levelSlot = normalized.levelSlot;
    metadata.levelHash = normalized.levelHash;
    metadata.publicJoinPort = normalized.publicJoinPort;
    metadata.maxHumanPlayers = normalized.maxHumanPlayers;
    metadata.shotEvaluationMode = normalized.shotEvaluationMode;
    metadata.visualizationMode = normalized.visualizationMode;
    metadata.botsFrozen = true;
    metadata.botsCanShoot = true;
    metadata.studyEventLoggingEnabled = normalized.studyOptions.enableEventLogging;
    metadata.studyEventRunId = normalized.studyEventRunId;
    return metadata;
}

}  // namespace net
