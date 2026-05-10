#include "app/CheckpointStore.hpp"
#include "net/ClientRuntime.hpp"
#include "net/DiagnosticsModel.hpp"
#include "net/ProxyRuntime.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/ServerRuntime.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "LevelData.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

float absoluteDifference(float lhs, float rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

const sim::RosterEntry& requireRosterEntry(const std::vector<sim::RosterEntry>& roster, int actorId) {
    for (const auto& entry : roster) {
        if (entry.actorId == actorId) {
            return entry;
        }
    }
    throw std::runtime_error("roster entry missing from client snapshot");
}

int requireBotActorId(const std::vector<sim::RosterEntry>& roster, sim::TeamId team) {
    for (const auto& entry : roster) {
        if (entry.isBot && entry.team == team) {
            return entry.actorId;
        }
    }
    throw std::runtime_error("bot roster entry missing from client snapshot");
}

const net::RuntimeParamChangeRequest* findRuntimeParamRequest(
    const std::vector<net::RuntimeParamChangeRequest>& requests,
    const std::string& key) {
    for (const auto& request : requests) {
        if (request.key == key) {
            return &request;
        }
    }
    return nullptr;
}

const RuntimeSettingsOverlay::ControlState* findOverlayControl(
    const RuntimeSettingsOverlay::State& state,
    RuntimeSettingsOverlay::ControlId controlId) {
    for (const auto& control : state.leftControls) {
        if (control.id == controlId) {
            return &control;
        }
    }
    return nullptr;
}

bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool containsEligibleActor(const std::vector<sim::EligibleActor>& actors, int actorId) {
    for (const auto& actor : actors) {
        if (actor.actorId == actorId) {
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

    throw std::runtime_error("failed to locate repository root for client runtime characterization tests");
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

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
#ifdef _WIN32
        char* existing = nullptr;
        std::size_t existingLength = 0u;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
            std::free(existing);
        }
#else
        const char* existing = std::getenv(name_.c_str());
        if (existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
        }
#endif
        set(value);
    }

    ~ScopedEnvVar() {
        if (hadOriginal_) {
            set(originalValue_);
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    bool hadOriginal_{false};
    std::string originalValue_{};
};

class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~ScopedDirectoryCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

private:
    std::filesystem::path path_;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(original_);
    }

private:
    std::filesystem::path original_;
};

std::filesystem::path makeUniqueTempDirectory(const std::string& prefix) {
    const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        (prefix + "-" + std::to_string(uniqueId));
    std::filesystem::create_directories(path);
    return path;
}

enum class HarnessMode {
    AcceptClient,
    RejectUnsupportedVersion,
    ManualWelcome
};

class LoopbackServerHarness {
public:
    explicit LoopbackServerHarness(HarnessMode mode, net::ServerConfig config = {})
        : mode_(mode),
          server_(config) {}

    bool start() {
        return socket_.bind(net::UdpEndpoint{"127.0.0.1", 0});
    }

    std::uint16_t port() const {
        return socket_.localPort();
    }

    const std::vector<net::TeamChangeRequest>& observedTeamChangeRequests() const {
        return teamChangeRequests_;
    }

    const std::vector<net::RuntimeParamChangeRequest>& observedRuntimeParamRequests() const {
        return runtimeParamRequests_;
    }

    const std::vector<net::SessionActionRequest>& observedSessionActionRequests() const {
        return sessionActionRequests_;
    }

    bool hasClientEndpoint() const {
        return hasClientEndpoint_;
    }

    void step(float dtSeconds) {
        nowUs_ += dtSeconds > 0.0f
            ? static_cast<std::uint64_t>(dtSeconds * 1'000'000.0f)
            : 0u;
        receiveIncoming();
        if (acceptedPeerId_ != 0u) {
            server_.tickOnce(nowUs_);
            flushServerPackets();
        }
    }

    void sendWelcome(std::uint32_t sessionId,
                     std::uint16_t assignedPeerId,
                     std::uint16_t snapshotRateHz = 20u,
                     int levelSlot = -1,
                     std::uint32_t levelHash = 0u,
                     net::HostedSessionMetadata sessionMetadata = {}) {
        net::Packet response;
        response.header.peerId = assignedPeerId;
        response.header.channel = net::Channel::Control;
        response.header.seq = nextControlSeq_++;
        response.header.kind = net::PacketKind::Welcome;
        net::WelcomeMessage welcome{
            sessionId,
            assignedPeerId,
            snapshotRateHz,
            levelSlot,
            levelHash
        };
        sessionMetadata.levelSlot = levelSlot;
        sessionMetadata.levelHash = levelHash;
        welcome.sessionMetadata = sessionMetadata;
        response.payload = welcome;
        sendPacket(response);
    }

    std::uint16_t addRemoteClient(std::uint32_t sessionId, const std::string& playerName) {
        net::WelcomeMessage welcome;
        std::string rejectReason;
        const bool accepted = server_.acceptClient(
            net::HelloMessage{sessionId, 0u, playerName},
            nowUs_,
            &welcome,
            &rejectReason);
        expect(accepted, "server runtime should accept the synthetic remote client");
        return welcome.assignedPeerId;
    }

    bool applyControlPayloadForPeer(std::uint16_t peerId, const net::PacketPayload& payload) {
        return server_.handleControlPayload(peerId, payload, nowUs_);
    }

    void clearPendingServerPackets() {
        server_.takePendingPackets();
    }

    void flushPendingServerPacketsForTest() {
        flushServerPackets();
    }

    void addAuthoritativeRosterEntry(int actorId,
                                     sim::TeamId team,
                                     bool isBot,
                                     std::uint16_t kills,
                                     std::uint16_t deaths,
                                     bool alive,
                                     const std::string& displayName = {},
                                     std::uint16_t assists = 0u,
                                     std::uint16_t latencyMs = 0u,
                                     std::uint8_t lossPct = 0u) {
        sim::ensureRosterEntry(&server_.worldState(), actorId, team, isBot);
        sim::RosterEntry* entry = sim::findRosterEntry(&server_.worldState(), actorId);
        expect(entry != nullptr, "test harness should be able to create roster entries");
        entry->team = team;
        entry->isBot = isBot;
        entry->kills = kills;
        entry->deaths = deaths;
        entry->assists = assists;
        entry->alive = alive;
        entry->latencyMs = latencyMs;
        entry->lossPct = lossPct;
        entry->displayName = displayName;
    }

    void setAuthoritativeTeamScores(std::uint16_t attackers, std::uint16_t defenders) {
        server_.worldState().teamScores.attackers = attackers;
        server_.worldState().teamScores.defenders = defenders;
    }

    void setFirstEnemyEntityId(int entityId) {
        expect(!server_.worldState().enemies.empty(), "server world should contain an enemy for snapshot tests");
        server_.worldState().enemies.front().entityId = entityId;
    }

    void setAuthoritativePlayerState(int playerId,
                                     const sim::Vec3& position,
                                     float yaw = 0.0f,
                                     float pitch = 0.0f) {
        sim::PlayerState* player = sim::findPlayer(&server_.worldState(), playerId);
        expect(player != nullptr, "test harness should be able to update an authoritative player");
        player->position = position;
        player->yaw = yaw;
        player->pitch = pitch;
    }

    void setAuthoritativePlayerHealth(int playerId, float health) {
        sim::PlayerState* player = sim::findPlayer(&server_.worldState(), playerId);
        expect(player != nullptr, "test harness should be able to update authoritative health");
        player->health = health;
        player->maxHealth = std::max(player->maxHealth, health);
        if (sim::RosterEntry* entry = sim::findRosterEntry(&server_.worldState(), playerId)) {
            entry->alive = health > 0.0f;
        }
    }

    void setControlGhostPlayerState(std::uint16_t peerId,
                                    const sim::Vec3& position,
                                    float yaw = 0.0f,
                                    float pitch = 0.0f) {
        auto& sessions =
            const_cast<std::vector<net::ClientSession>&>(server_.sessions());
        const auto it = std::find_if(sessions.begin(),
                                     sessions.end(),
                                     [peerId](const net::ClientSession& session) {
                                         return session.peerId == peerId;
                                     });
        expect(it != sessions.end(), "test harness should be able to update a control ghost player state");

        sim::PlayerState* authoritativePlayer =
            sim::findPlayer(&server_.worldState(), static_cast<int>(peerId));
        expect(authoritativePlayer != nullptr,
               "test harness should only update control ghosts for authoritative players");

        it->controlPlayerState = *authoritativePlayer;
        it->controlPlayerState.position = position;
        it->controlPlayerState.yaw = yaw;
        it->controlPlayerState.pitch = pitch;
        it->hasControlPlayerState = true;
        it->lastAppliedControlSeq = std::max(it->lastAppliedControlSeq, 1u);
    }

    void sendDisconnectForTest(const std::string& reason) {
        sendDisconnect(reason);
    }

private:
    void receiveIncoming() {
        net::ReceivedDatagram datagram;
        while (true) {
            const net::ReceiveStatus status = socket_.receive(&datagram);
            if (status == net::ReceiveStatus::WouldBlock) {
                return;
            }
            if (status == net::ReceiveStatus::Error) {
                throw std::runtime_error("server socket receive failed");
            }

            clientEndpoint_ = datagram.sender;
            hasClientEndpoint_ = true;

            const net::ParseResult parseResult = net::deserializePacket(datagram.payload);
            if (!parseResult.ok) {
                if (mode_ == HarnessMode::RejectUnsupportedVersion &&
                    parseResult.error == net::ParseError::UnsupportedVersion) {
                    sendDisconnect("unsupported_protocol_version");
                }
                continue;
            }

            switch (parseResult.packet.header.kind) {
                case net::PacketKind::Hello: {
                    if (mode_ == HarnessMode::RejectUnsupportedVersion) {
                        sendDisconnect("unsupported_protocol_version");
                        break;
                    }
                    if (mode_ == HarnessMode::ManualWelcome) {
                        break;
                    }
                    const auto& hello = std::get<net::HelloMessage>(parseResult.packet.payload);
                    net::WelcomeMessage welcome;
                    std::string rejectReason;
                    expect(server_.acceptClient(hello, nowUs_, &welcome, &rejectReason),
                           "test server should accept the hello packet");
                    acceptedPeerId_ = welcome.assignedPeerId;
                    sendWelcome(welcome.sessionId,
                                welcome.assignedPeerId,
                                welcome.snapshotRateHz,
                                welcome.levelSlot,
                                welcome.levelHash,
                                welcome.sessionMetadata);
                    flushServerPackets();
                    break;
                }

                case net::PacketKind::CommandBundle: {
                    const auto& commands = std::get<net::CommandBundle>(parseResult.packet.payload);
                    server_.enqueueCommandBundle(parseResult.packet.header.peerId, commands, nowUs_);
                    break;
                }

                case net::PacketKind::TeamChangeRequest:
                    expect(server_.handleControlPayload(parseResult.packet.header.peerId,
                                                        parseResult.packet.payload,
                                                        nowUs_),
                           "server runtime should accept client team-change control packets");
                    teamChangeRequests_.push_back(
                        std::get<net::TeamChangeRequest>(parseResult.packet.payload));
                    break;

                case net::PacketKind::RuntimeParamChangeRequest:
                    expect(server_.handleControlPayload(parseResult.packet.header.peerId,
                                                        parseResult.packet.payload,
                                                        nowUs_),
                           "server runtime should accept client runtime-parameter control packets");
                    runtimeParamRequests_.push_back(
                        std::get<net::RuntimeParamChangeRequest>(parseResult.packet.payload));
                    break;

                case net::PacketKind::SessionActionRequest:
                    server_.handleControlPayload(parseResult.packet.header.peerId,
                                                 parseResult.packet.payload,
                                                 nowUs_);
                    sessionActionRequests_.push_back(
                        std::get<net::SessionActionRequest>(parseResult.packet.payload));
                    break;

                default:
                    break;
            }
        }
    }

    void flushServerPackets() {
        if (!hasClientEndpoint_) {
            return;
        }

        const auto packets = server_.takePendingPackets();
        for (const auto& packet : packets) {
            sendPacket(packet);
        }
    }

    void sendDisconnect(const std::string& reason) {
        net::Packet packet;
        packet.header.channel = net::Channel::Control;
        packet.header.seq = nextControlSeq_++;
        packet.header.kind = net::PacketKind::Disconnect;
        packet.payload = net::DisconnectMessage{1u, reason};
        sendPacket(packet);
    }

    void sendPacket(const net::Packet& packet) {
        expect(hasClientEndpoint_, "client endpoint must be known before sending");
        expect(socket_.sendTo(clientEndpoint_, net::serializePacket(packet)),
               "server harness should be able to send packets");
    }

    HarnessMode mode_{HarnessMode::AcceptClient};
    net::UdpSocket socket_{};
    net::ServerRuntime server_{};
    net::UdpEndpoint clientEndpoint_{};
    bool hasClientEndpoint_{false};
    std::uint64_t nowUs_{1'000'000u};
    std::uint32_t nextControlSeq_{1u};
    std::uint16_t acceptedPeerId_{0u};
    std::vector<net::TeamChangeRequest> teamChangeRequests_{};
    std::vector<net::RuntimeParamChangeRequest> runtimeParamRequests_{};
    std::vector<net::SessionActionRequest> sessionActionRequests_{};
};

struct ClientSyncSemanticsFixture {
    std::uint64_t clockUs{123'456u};
    std::uint64_t lastHelloSendUs{0u};
    std::uint64_t lastServerPacketUs{0u};
    std::uint64_t lastCommandSendUs{0u};
    std::uint32_t packetSeq{11u};
    std::uint32_t commandSeq{4u};
    std::uint32_t lastSnapshotPacketSeq{9u};
    std::uint32_t lastAckedInputSeq{2u};
    std::uint16_t peerId{7u};
    std::uint16_t snapshotRateHz{60u};
    int authoritativeLevelSlot{-1};
    std::uint32_t authoritativeLevelHash{0u};
    bool hasAuthoritativeSessionMetadata{false};
    net::HostedSessionMetadata authoritativeSessionMetadata{};
    std::uint64_t latestSnapshotReceiveUs{100'000u};
    bool hasSnapshot{true};
    bool interpolationEnabled{true};
    bool predictionEnabled{true};
    net::InterpolationBuffer remotePlayerInterpolation{};
    net::InterpolationBuffer remoteEnemyInterpolation{};
    net::PredictionBuffer predictionBuffer{};
    sim::PlayerState localPlayerState{};
    std::vector<sim::PlayerState> remotePlayers{};
    std::vector<sim::RemoteActorState> remoteEnemies{};
    std::vector<sim::RosterEntry> roster{};
    sim::TeamScores teamScores{};
    net::WorldSnapshot latestSnapshot{};

    explicit ClientSyncSemanticsFixture(std::uint16_t assignedPeerId = 7u)
        : peerId(assignedPeerId) {
        localPlayerState.playerId = static_cast<int>(assignedPeerId);
        localPlayerState.position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
        predictionBuffer.reset(localPlayerState);
        latestSnapshot.localPlayerState = localPlayerState;
        latestSnapshot.serverTimeUs = 7'654u;
    }

    client::ClientSyncContext context() {
        return client::ClientSyncContext{
            clockUs,
            lastHelloSendUs,
            lastServerPacketUs,
            lastCommandSendUs,
            packetSeq,
            commandSeq,
            lastSnapshotPacketSeq,
            lastAckedInputSeq,
            peerId,
            snapshotRateHz,
            authoritativeLevelSlot,
            authoritativeLevelHash,
            hasAuthoritativeSessionMetadata,
            authoritativeSessionMetadata,
            latestSnapshotReceiveUs,
            hasSnapshot,
            interpolationEnabled,
            predictionEnabled,
            remotePlayerInterpolation,
            remoteEnemyInterpolation,
            predictionBuffer,
            localPlayerState,
            remotePlayers,
            remoteEnemies,
            roster,
            teamScores,
            latestSnapshot
        };
    }
};

void pumpLoopbackFrame(net::ClientRuntime* client,
                       LoopbackServerHarness* server,
                       float dtSeconds = 1.0f / 60.0f) {
    expect(client != nullptr, "client pointer is required");
    expect(server != nullptr, "server pointer is required");

    server->step(dtSeconds);
    client->update(dtSeconds, nullptr);
    server->step(0.0f);
    client->update(0.0f, nullptr);
}

void pumpUntilConnected(net::ClientRuntime* client, LoopbackServerHarness* server) {
    expect(client != nullptr, "client pointer is required");
    expect(server != nullptr, "server pointer is required");

    constexpr int kMaxConnectionFrames = 360;
    for (int attempt = 0; attempt < kMaxConnectionFrames; ++attempt) {
        pumpLoopbackFrame(client, server);
        if (client->state() == net::ClientConnectionState::Connected && client->hasSnapshot()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("client did not connect within the allotted frames");
}

void pumpUntilConnectedViaProxy(net::ClientRuntime* client,
                                LoopbackServerHarness* server,
                                net::TransportArtifactAdapter* proxy) {
    expect(client != nullptr, "client pointer is required");
    expect(server != nullptr, "server pointer is required");
    expect(proxy != nullptr, "proxy pointer is required");

    std::uint64_t nowUs = 1'000'000u;
    constexpr int kMaxProxyConnectionFrames = 480;
    for (int attempt = 0; attempt < kMaxProxyConnectionFrames; ++attempt) {
        proxy->tick(nowUs);
        server->step(1.0f / 60.0f);
        nowUs += 16'667u;
        proxy->tick(nowUs);
        client->update(1.0f / 60.0f, nullptr);
        proxy->tick(nowUs);
        server->step(0.0f);
        proxy->tick(nowUs);
        client->update(0.0f, nullptr);
        if (client->state() == net::ClientConnectionState::Connected && client->hasSnapshot()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("client did not connect through proxy within the allotted frames");
}

void stepProxyBackedFrame(LoopbackServerHarness* server,
                          net::TransportArtifactAdapter* proxy,
                          net::ClientRuntime* client,
                          float dtSeconds,
                          std::uint64_t* nowUs) {
    expect(server != nullptr, "server pointer is required");
    expect(proxy != nullptr, "proxy pointer is required");
    expect(client != nullptr, "client pointer is required");
    expect(nowUs != nullptr, "proxy-backed frame time is required");

    proxy->tick(*nowUs);
    server->step(dtSeconds);
    *nowUs += static_cast<std::uint64_t>(dtSeconds * 1'000'000.0f);
    proxy->tick(*nowUs);
    client->update(dtSeconds, nullptr);
    proxy->tick(*nowUs);
    server->step(0.0f);
    proxy->tick(*nowUs);
    client->update(0.0f, nullptr);
}

void waitForClientEndpoint(net::ClientRuntime* client, LoopbackServerHarness* server) {
    expect(client != nullptr, "client pointer is required");
    expect(server != nullptr, "server pointer is required");

    for (int attempt = 0; attempt < 120; ++attempt) {
        pumpLoopbackFrame(client, server);
        if (server->hasClientEndpoint()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("manual welcome test did not observe the client endpoint");
}

void waitForClientState(net::ClientRuntime* client,
                        LoopbackServerHarness* server,
                        net::ClientConnectionState expectedState,
                        const std::string& context) {
    expect(client != nullptr, "client pointer is required");
    expect(server != nullptr, "server pointer is required");

    for (int attempt = 0; attempt < 120; ++attempt) {
        pumpLoopbackFrame(client, server);
        if (client->state() == expectedState) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    throw std::runtime_error(context);
}

bool localClientHasTeam(const net::ClientRuntime& client, sim::TeamId expectedTeam) {
    if (client.peerId() == 0u) {
        return false;
    }

    for (const auto& entry : client.roster()) {
        if (entry.actorId == static_cast<int>(client.peerId())) {
            return entry.team == expectedTeam;
        }
    }

    return false;
}

void waitForLocalTeam(net::ClientRuntime* client,
                      LoopbackServerHarness* server,
                      sim::TeamId expectedTeam,
                      const std::string& context) {
    expect(client != nullptr, "client pointer is required");
    expect(server != nullptr, "server pointer is required");

    for (int attempt = 0; attempt < 120; ++attempt) {
        pumpLoopbackFrame(client, server);
        if (localClientHasTeam(*client, expectedTeam)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    throw std::runtime_error(context);
}

void ensureTestLevelExists(int slot) {
    LevelData::Level level("Network Test Level " + std::to_string(slot));
    level.obstacles.push_back(LevelData::Obstacle{
        static_cast<float>(slot),
        -static_cast<float>(slot),
        4.0f,
        4.0f,
        2.5f,
        Color{100, 150, 220, 255}
    });
    expect(LevelData::saveLevel(level, slot),
           "test level fixture should save successfully");
}

void testClientJoinsUsingConfiguredAddressAndPort() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "localhost";
    config.serverPort = server.port();
    config.playerName = "alpha";
    config.sessionId = 1001u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start its local UDP socket");

    pumpUntilConnected(&client, &server);

    expect(client.state() == net::ClientConnectionState::Connected,
           "client should connect over the configured address and port");
    expect(client.peerId() == 1u, "server should assign stable peer id 1 to the first client");
    expect(client.hasSnapshot(), "client should receive an authoritative snapshot after joining");
}

void testClientLearnsAuthoritativeLevelIdentityFromWelcome() {
    ensureTestLevelExists(4);

    net::ServerConfig serverConfig;
    serverConfig.levelSlot = 4;
    serverConfig.levelHash = net::makeLevelIdentityHash(4);

    LoopbackServerHarness server(HarnessMode::AcceptClient, serverConfig);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "level-aware";
    config.sessionId = 1234u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start its local UDP socket");

    bool connected = false;
    for (int attempt = 0; attempt < 120; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.state() == net::ClientConnectionState::Connected) {
            connected = true;
            break;
        }
    }

    expect(connected, "client should reach connected state after the welcome arrives");
    expect(client.hasAuthoritativeLevelIdentity(),
           "client should expose the authoritative level identity after the welcome packet");
    expect(client.authoritativeLevelSlot() == 4,
           "client should preserve the authoritative level slot from the welcome packet");
    expect(client.authoritativeLevelHash() == net::makeLevelIdentityHash(4),
           "client should preserve the authoritative level hash from the welcome packet");
}

void testClientExposesAuthoritativeHostedSessionMetadataAfterConnect() {
    ensureTestLevelExists(4);

    LoopbackServerHarness server(HarnessMode::ManualWelcome);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "joiner";
    config.sessionId = 5555u;
    config.connectTimeoutUs = 2'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start its local UDP socket");

    waitForClientEndpoint(&client, &server);

    net::HostedSessionMetadata metadata;
    metadata.sessionLabel = "Authority Match";
    metadata.hostPlayerName = "host-player";
    metadata.hostPeerId = 7u;
    metadata.publicJoinPort = 41000u;
    metadata.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    server.sendWelcome(client.config().sessionId,
                       1u,
                       20u,
                       4,
                       net::makeLevelIdentityHash(4),
                       metadata);

    waitForClientState(&client,
                       &server,
                       net::ClientConnectionState::Connected,
                       "client did not reach connected state after metadata welcome");

    expect(client.hasAuthoritativeSessionMetadata(),
           "client should store authoritative hosted session metadata after the welcome packet");
    expect(client.authoritativeSessionMetadata().sessionLabel == "Authority Match",
           "client should preserve the authoritative session label from the welcome packet");
    expect(client.authoritativeSessionMetadata().hostPlayerName == "host-player",
           "client should preserve the authoritative host player name from the welcome packet");
    expect(client.authoritativeSessionMetadata().hostPeerId == 7u,
           "client should preserve the authoritative host peer id from the welcome packet");
    expect(client.authoritativeSessionMetadata().publicJoinPort == 41000u,
           "client should preserve the authoritative public join port from the welcome packet");
    expect(client.authoritativeSessionMetadata().shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "client should preserve the authoritative shot-evaluation rule from the welcome packet");
}

void testClientHostedSessionSummaryUsesAuthoritativeMetadata() {
    ensureTestLevelExists(5);

    LoopbackServerHarness server(HarnessMode::ManualWelcome);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "local-joiner";
    config.sessionId = 5656u;
    config.connectTimeoutUs = 2'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start its local UDP socket");

    waitForClientEndpoint(&client, &server);

    net::HostedSessionMetadata metadata;
    metadata.sessionLabel = "Remote Metadata";
    metadata.hostPlayerName = "remote-host";
    metadata.publicJoinPort = 42000u;
    metadata.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    server.sendWelcome(client.config().sessionId,
                       1u,
                       20u,
                       5,
                       net::makeLevelIdentityHash(5),
                       metadata);

    waitForClientState(&client,
                       &server,
                       net::ClientConnectionState::Connected,
                       "client did not connect before hosted session summary assertion");

    const std::string hostedSummary = client.hostedSessionSummary();
    expect(hostedSummary.find("Session: Remote Metadata") != std::string::npos,
           "client-hosted status text should use the authoritative remote session label");
    expect(hostedSummary.find("Rule: Seen Position") != std::string::npos,
           "client-hosted status text should use the authoritative shot-evaluation rule");
    expect(hostedSummary.find("Bots: Frozen") != std::string::npos,
           "client-hosted status text should expose the authoritative bot-director state");
    expect(hostedSummary.find("Join Port 42000") != std::string::npos,
           "client-hosted status text should surface the authoritative public join port");
    expect(hostedSummary.find("local-joiner") == std::string::npos,
           "client-hosted status text should not fall back to the local joiner name when authoritative metadata exists");
}

void testHostToggleEnemyAIHotkeyEmitsSessionBotControlRequest() {
    ensureTestLevelExists(5);

    net::ServerConfig serverConfig;
    serverConfig.defenderBotCount = 1u;
    LoopbackServerHarness server(HarnessMode::AcceptClient, serverConfig);
    expect(server.start(), "server harness should bind loopback for the bot hotkey test");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "host-hotkey";
    config.sessionId = 5657u;
    config.connectTimeoutUs = 2'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for the bot hotkey test");

    pumpUntilConnected(&client, &server);
    expect(client.hasAuthoritativeSessionMetadata() &&
               client.authoritativeSessionMetadata().botsFrozen,
           "bot hotkey regression requires the hosted session to start frozen");

    InputHandler3D::InputState input;
    input.toggleEnemyAI = true;
    client.update(1.0f / 60.0f, &input);

    const net::RuntimeParamChangeRequest* request = nullptr;
    for (int attempt = 0; attempt < 10 && request == nullptr; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        request = findRuntimeParamRequest(server.observedRuntimeParamRequests(), "sv.bots_active");
    }
    expect(request != nullptr &&
               request->scope == net::RuntimeParamScope::Session &&
               request->targetId == -1 &&
               request->value == 1.0f,
           "the legacy enemy-AI hotkey should emit a host session request that activates frozen bots");
}

void testClientRejectsInvalidAuthoritativeLevelSlot() {
    LoopbackServerHarness server(HarnessMode::ManualWelcome);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "bad-slot";
    config.sessionId = 3333u;
    config.connectTimeoutUs = 2'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start its local UDP socket");

    waitForClientEndpoint(&client, &server);

    server.sendWelcome(client.config().sessionId,
                       1u,
                       20u,
                       99,
                       net::makeLevelIdentityHash(99));

    bool rejected = false;
    for (int attempt = 0; attempt < 180; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.state() == net::ClientConnectionState::Rejected) {
            rejected = true;
            break;
        }
    }

    expect(rejected,
           "client should reject a welcome for a level slot that cannot be loaded");
    expect(client.statusMessage().find("level_load_failed") != std::string::npos,
           "client should surface an explicit level load failure reason");
}

void testClientRejectsAuthoritativeLevelHashMismatch() {
    LoopbackServerHarness server(HarnessMode::ManualWelcome);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "bad-hash";
    config.sessionId = 4444u;
    config.connectTimeoutUs = 2'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start its local UDP socket");

    waitForClientEndpoint(&client, &server);

    server.sendWelcome(client.config().sessionId, 1u, 20u, -1, 123u);

    bool rejected = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.state() == net::ClientConnectionState::Rejected) {
            rejected = true;
            break;
        }
    }

    expect(rejected,
           "client should reject a welcome whose level hash does not match the authoritative slot identity");
    expect(client.statusMessage().find("level_identity_mismatch") != std::string::npos,
           "client should surface an explicit level identity mismatch reason");
}

void testClientAutoAssignsUniqueSessionIdsPerStart() {
    net::ClientConfig firstConfig;
    firstConfig.serverPort = 45991u;

    net::ClientConfig secondConfig;
    secondConfig.serverPort = 45992u;

    net::ClientRuntime first(firstConfig);
    net::ClientRuntime second(secondConfig);

    expect(first.start(), "first client should bind and start without an explicit session id");
    expect(second.start(), "second client should bind and start without an explicit session id");

    expect(first.config().sessionId != 0u,
           "first client should auto-assign a non-zero session id");
    expect(second.config().sessionId != 0u,
           "second client should auto-assign a non-zero session id");
    expect(first.config().sessionId != second.config().sessionId,
           "fresh client starts should not reuse the same session id");
}

void testClientRejectsIncompatibleProtocolVersion() {
    LoopbackServerHarness server(HarnessMode::RejectUnsupportedVersion);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.sessionId = 2002u;
    config.protocolVersion = static_cast<std::uint16_t>(net::kProtocolVersion + 1u);

    net::ClientRuntime client(config);
    expect(client.start(), "client should start even when using a bad version");

    waitForClientEndpoint(&client, &server);
    waitForClientState(&client,
                       &server,
                       net::ClientConnectionState::Rejected,
                       "server should reject an incompatible protocol version explicitly");
    expect(client.statusMessage().find("unsupported_protocol_version") != std::string::npos,
           "client should surface the explicit protocol rejection reason");
}

void testClientConsumesAuthoritativeSnapshotsWithoutFakeNetworkPath() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "bravo";
    config.sessionId = 3003u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    const sim::Vec3 spawn = client.localPlayerState().position;
    InputHandler3D::InputState input;
    input.moveInput.y = 1.0f;

    for (int attempt = 0; attempt < 12; ++attempt) {
        client.update(1.0f / 60.0f, &input);
        server.step(1.0f / 60.0f);
        client.update(0.0f, nullptr);
    }

    expect(client.lastAckedInputSeq() > 0u,
           "authoritative snapshots should acknowledge command input sequences");
    expect(client.localPlayerState().position.z < spawn.z,
           "authoritative snapshots should move the player using the real network path");
    expect(!client.remoteEnemies().empty(),
           "authoritative snapshots should provide renderable remote enemies");
}

void testClientKeepsRemotePlayersSeparateFromRemoteEnemies() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "split";
    config.sessionId = 7007u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    const std::uint16_t remotePeerId = server.addRemoteClient(8008u, "remote");
    server.setFirstEnemyEntityId(5);

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (!client.remotePlayers().empty() && !client.remoteEnemies().empty()) {
            break;
        }
    }

    const net::WorldSnapshot* latestSnapshot = client.latestSnapshot();
    expect(latestSnapshot != nullptr, "client should keep the latest authoritative snapshot");
    expect(latestSnapshot->remotePlayers.size() == 1u,
           "snapshot payload should preserve remote players separately");
    expect(!latestSnapshot->remoteEnemies.empty(),
           "snapshot payload should preserve remote enemies separately");
    expect(client.remotePlayers().size() == 1u,
           "client runtime should keep remote players in their own interpolation path");
    expect(!client.remoteEnemies().empty(),
           "client runtime should keep remote enemies in their own interpolation path");
    expect(client.remotePlayers().front().playerId == static_cast<int>(remotePeerId),
           "client runtime should identify remote players by the dedicated player list");
    expect(client.remoteEnemies().front().entityId == 5,
           "client runtime should identify remote enemies by the dedicated enemy list, not entity-id heuristics");
}

void testServerPublishedControlGhostsExposeGhostPlayersWithoutScoreboardDuplication() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", server.port()};
    proxyConfig.defaultDownstream.baseDelayMs = 180.0f;
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "proxy should start");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = proxy.clientListenPort();
    config.playerName = "ghost-watch";
    config.sessionId = 7011u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnectedViaProxy(&client, &server, &proxy);

    const std::uint16_t remotePeerId = server.addRemoteClient(8011u, "remote-ghost");
    std::uint64_t nowUs = 5'000'000u;
    client::ClientViewState viewState;
    bool sawGhostTracks = false;
    for (int frame = 0; frame < 120; ++frame) {
        const float manipulatedX = 6.0f + static_cast<float>(frame) * 0.55f;
        const float controlX = manipulatedX + 2.0f;
        server.setAuthoritativePlayerState(static_cast<int>(remotePeerId),
                                           sim::Vec3{manipulatedX, Config::PLAYER_EYE_HEIGHT, -4.0f},
                                           0.35f,
                                           0.0f);
        server.setControlGhostPlayerState(remotePeerId,
                                          sim::Vec3{controlX, Config::PLAYER_EYE_HEIGHT, -4.0f},
                                          0.35f,
                                          0.0f);
        stepProxyBackedFrame(&server, &proxy, &client, 1.0f / 60.0f, &nowUs);
        viewState = client.clientViewState();
        if (viewState.remotePlayers.size() == 1u &&
            viewState.remotePlayerGhosts.size() == 1u) {
            sawGhostTracks = true;
            break;
        }
    }

    expect(sawGhostTracks,
           "proxied clients should expose both manipulated and server-published ghost player render tracks");
    expect(viewState.remotePlayerGhosts.front().ghost,
           "the published control track should be marked as a ghost render item");
    expect(viewState.remotePlayerGhosts.front().eyePosition.x >
               viewState.remotePlayers.front().eyePosition.x + 1.0f,
           "the clean control ghost should lead the manipulated player when downstream delay is applied");
    expect(viewState.scoreboardSections.size() == 2u &&
               viewState.scoreboardSections[0].entries.size() == 1u &&
               viewState.scoreboardSections[1].entries.size() == 1u &&
               viewState.scoreboardSections[1].entries[0].identity == "remote-ghost",
           "ghost-control entities should not create duplicate scoreboard rows");
}

void testClientStoresAuthoritativeRosterAndTeamTotals() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    const std::uint16_t hostPeerId = server.addRemoteClient(8101u, "host");
    server.clearPendingServerPackets();

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "joiner";
    config.sessionId = 8102u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() == 2u) {
            break;
        }
    }

    expect(client.peerId() == 2u,
           "the real client should join as the second participant when the host is pre-registered");
    expect(client.roster().size() == 2u,
           "client runtime should store the full authoritative roster from snapshots");
    expect(client.teamScores().attackers == 0u && client.teamScores().defenders == 0u,
           "client runtime should store authoritative team totals from snapshots");
    expect(requireRosterEntry(client.roster(), hostPeerId).team == sim::TeamId::Attacker,
           "client runtime should preserve the host attacker assignment");
    expect(requireRosterEntry(client.roster(), client.peerId()).team == sim::TeamId::Defender,
           "client runtime should preserve the joiner defender assignment");
    expect(client.latestSnapshot() != nullptr && client.latestSnapshot()->roster.size() == client.roster().size(),
           "latest authoritative snapshot should expose the same roster state the client stores");
}

void testClientAppliesHostEditedParticipantRuntimeAndSessionSettingsFromSnapshots() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    const std::uint16_t hostPeerId = server.addRemoteClient(8201u, "host");
    server.clearPendingServerPackets();

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "joiner";
    config.sessionId = 8202u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() == 2u &&
            client.hasAuthoritativeSessionMetadata() &&
            client.authoritativeSessionMetadata().hostPeerId == hostPeerId) {
            break;
        }
    }

    expect(client.peerId() == 2u,
           "the joiner should still connect as the second participant in the host-edit sync test");
    expect(client.hasAuthoritativeSessionMetadata() &&
               client.authoritativeSessionMetadata().hostPeerId == hostPeerId,
           "the joiner should learn the authoritative host peer id from replicated session metadata");

    const std::uint16_t joinerPeerId = client.peerId();
    expect(server.applyControlPayloadForPeer(
               hostPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(joinerPeerId),
                   net::runtimeParamKeyForTarget(joinerPeerId, "interpolation_enabled"),
                   0.0f}}),
           "a synthetic host should be able to disable interpolation for the affected client");
    expect(server.applyControlPayloadForPeer(
               hostPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(joinerPeerId),
                   net::runtimeParamKeyForTarget(joinerPeerId, "prediction_enabled"),
                   0.0f}}),
           "a synthetic host should be able to disable prediction for the affected client");
    expect(server.applyControlPayloadForPeer(
               hostPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(joinerPeerId),
                   net::runtimeParamKeyForTarget(joinerPeerId, "reconciliation_strategy"),
                   static_cast<float>(sim::RuntimeReconciliationStrategy::Snap)}}),
           "a synthetic host should be able to change the affected client's reconciliation strategy");
    expect(server.applyControlPayloadForPeer(
               hostPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(joinerPeerId),
                   net::runtimeParamKeyForTarget(joinerPeerId, "smooth_correction_window_ms"),
                   80.0f}}),
           "a synthetic host should be able to change the affected client's smooth correction window");
    expect(server.applyControlPayloadForPeer(
               hostPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.shot_mode",
                   static_cast<float>(static_cast<std::uint8_t>(net::ShotEvaluationMode::LivePosition))}}),
           "a synthetic host should be able to stage an authoritative shot-rule update");

    for (int attempt = 0; attempt < 60; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        const sim::RosterEntry& localEntry = requireRosterEntry(client.roster(), joinerPeerId);
        if (!client.interpolationEnabled() &&
            !client.predictionEnabled() &&
            localEntry.reconciliationStrategy == sim::RuntimeReconciliationStrategy::Snap &&
            localEntry.smoothCorrectionWindowMs == 80u &&
            client.authoritativeSessionMetadata().shotEvaluationMode ==
                net::ShotEvaluationMode::LivePosition) {
            break;
        }
    }

    const sim::RosterEntry& localEntry = requireRosterEntry(client.roster(), joinerPeerId);
    expect(!client.interpolationEnabled() && !localEntry.interpolationEnabled,
           "host-edited interpolation should converge on the affected client's runtime and roster state");
    expect(!client.predictionEnabled() && !localEntry.predictionEnabled,
           "host-edited prediction should converge on the affected client's runtime and roster state");
    expect(localEntry.reconciliationStrategy == sim::RuntimeReconciliationStrategy::Snap,
           "host-edited reconciliation strategy should appear in the affected client's authoritative roster view");
    expect(localEntry.smoothCorrectionWindowMs == 80u,
           "host-edited smooth correction windows should appear in the affected client's authoritative roster view");
    expect(client.authoritativeSessionMetadata().shotEvaluationMode ==
               net::ShotEvaluationMode::LivePosition,
           "host-edited shot evaluation rules should refresh from authoritative snapshots on affected clients");
}

void testGuestRuntimeOverlayOnlyShowsEditableSettings() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    server.addRemoteClient(8301u, "host");
    server.clearPendingServerPackets();

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "guest-ui";
    config.sessionId = 8302u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");
    pumpUntilConnected(&client, &server);

    net::ProxyRuntime proxy;
    client.attachProxyDiagnostics(&proxy, client.peerId());

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() == 2u) {
            break;
        }
    }

    const RuntimeSettingsOverlay::State overlayState = client.runtimeSettingsOverlayStateForTest();
    expect(overlayState.targets.size() == 1u &&
               overlayState.targets.front().targetId == client.peerId(),
           "guest runtime settings should only list the local participant as an editable target");
    expect(overlayState.subtitle.find("Host:") != std::string::npos,
           "guest runtime settings should still identify the host in the header");
    expect(findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::ShotEvaluationMode) == nullptr,
           "guest runtime settings should hide host-only shot evaluation controls");
    expect(findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::Interpolation) == nullptr &&
               findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::Prediction) == nullptr &&
               findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::ReconciliationStrategy) == nullptr &&
               findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::SmoothWindowMs) == nullptr &&
               findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::TickRate) == nullptr &&
               findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::StudyEventLogging) == nullptr &&
               overlayState.leftControls.empty() &&
               overlayState.targetEditor.latency.visible &&
               overlayState.targetEditor.loss.visible,
           "guest runtime settings should hide all host-managed sync controls and keep only the guest transport controls visible");
}

void testHostRuntimeOverlayShowsAllParticipantSettings() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "host-ui";
    config.sessionId = 8401u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");
    pumpUntilConnected(&client, &server);

    server.addRemoteClient(8402u, "guest");

    net::ProxyRuntime proxy;
    client.attachProxyDiagnostics(&proxy, client.peerId());

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() == 2u) {
            break;
        }
    }

    expect(server.applyControlPayloadForPeer(
               client.peerId(),
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.tickrate",
                   120.0f}}),
           "a synthetic host should be able to stage a tick-rate change through the shared control path");

    for (int attempt = 0; attempt < 10; ++attempt) {
        pumpLoopbackFrame(&client, &server);
    }

    const RuntimeSettingsOverlay::State overlayState = client.runtimeSettingsOverlayStateForTest();
    const RuntimeSettingsOverlay::ControlState* tickRateControl =
        findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::TickRate);
    const RuntimeSettingsOverlay::ControlState* snapshotRateControl =
        findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::SnapshotRate);
    const RuntimeSettingsOverlay::ControlState* eventLoggingControl =
        findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::StudyEventLogging);
    expect(overlayState.targets.size() == 2u,
           "host runtime settings should list every participant target");
    expect(tickRateControl != nullptr,
           "host runtime settings should show the host-only tick-rate control");
    expect(snapshotRateControl != nullptr,
           "host runtime settings should show the host-only snapshot-rate control");
    expect(eventLoggingControl != nullptr &&
               eventLoggingControl->type == RuntimeSettingsOverlay::ControlType::Toggle &&
               !eventLoggingControl->toggleValue,
           "host runtime settings should expose event logging as an off-by-default session toggle");
    expect(tickRateControl != nullptr &&
               tickRateControl->description.find("Live 120 Hz") != std::string::npos &&
               tickRateControl->description.find("staged") == std::string::npos,
           "host runtime settings should reconcile to the new live tick rate after the server applies the change");
    expect(tickRateControl != nullptr &&
               tickRateControl->type == RuntimeSettingsOverlay::ControlType::Choice &&
               std::any_of(tickRateControl->choices.begin(),
                           tickRateControl->choices.end(),
                           [](const RuntimeSettingsOverlay::ChoiceState& choice) {
                               return choice.value == 120 && choice.selected;
                           }),
           "host runtime settings should highlight the staged tick-rate choice");
    expect(snapshotRateControl != nullptr &&
               snapshotRateControl->type == RuntimeSettingsOverlay::ControlType::Choice &&
               std::any_of(snapshotRateControl->choices.begin(),
                           snapshotRateControl->choices.end(),
                           [](const RuntimeSettingsOverlay::ChoiceState& choice) {
                               return choice.value == 240 && !choice.enabled;
                           }),
           "host runtime settings should prevent snapshot choices above the live authoritative tick rate");
    expect(findOverlayControl(overlayState, RuntimeSettingsOverlay::ControlId::ShotEvaluationMode) != nullptr,
           "host runtime settings should show the host-only shot evaluation control");
    expect(overlayState.targetEditor.latency.visible &&
               overlayState.targetEditor.loss.visible,
           "host runtime settings should keep transport controls visible for the selected participant");
}

void testTickRateFeedbackReportsStagedThenLiveCadence() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "tickrate-feedback";
    config.sessionId = 8403u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for tick-rate feedback coverage");
    pumpUntilConnected(&client, &server);

    expect(server.applyControlPayloadForPeer(
               client.peerId(),
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.tickrate",
                   120.0f}}),
           "the synthetic host should be able to stage a live tick-rate change");
    server.flushPendingServerPacketsForTest();
    client.update(1.0f / 60.0f, nullptr);
    bool sawStagedMessage =
        client.lastCombatEventText() == "Tick rate staged for the next authoritative tick: 120 Hz";
    bool sawLiveMessage = client.lastCombatEventText() == "Live tick rate now 120 Hz";
    for (int attempt = 0; attempt < 3 && !sawStagedMessage && !sawLiveMessage; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.lastCombatEventText() ==
            "Tick rate staged for the next authoritative tick: 120 Hz") {
            sawStagedMessage = true;
            break;
        }
        if (client.lastCombatEventText() == "Live tick rate now 120 Hz") {
            sawLiveMessage = true;
            break;
        }
    }

    const std::string stagedTickFeedback = client.lastCombatEventText();
    expect(sawStagedMessage || sawLiveMessage,
           "client feedback should report the staged or live next-tick tick-rate update (actual: " +
               stagedTickFeedback + ")");
    if (sawStagedMessage) {
        const RuntimeSettingsOverlay::State stagedOverlayState = client.runtimeSettingsOverlayStateForTest();
        const RuntimeSettingsOverlay::ControlState* stagedTickRateControl =
            findOverlayControl(stagedOverlayState, RuntimeSettingsOverlay::ControlId::TickRate);
        expect(stagedTickRateControl != nullptr &&
                   stagedTickRateControl->description.find("Live 60 Hz | staged 120 Hz") !=
                       std::string::npos,
               "runtime settings should show both the live and staged authoritative tick rates before the boundary");
    }

    for (int attempt = 0; attempt < 60 && !sawLiveMessage; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.lastCombatEventText() == "Live tick rate now 120 Hz") {
            sawLiveMessage = true;
            break;
        }
    }

    expect(sawLiveMessage,
           "client feedback should confirm when the staged authoritative tick rate becomes live");
}

void testSnapshotRateFeedbackReportsStagedThenLiveCadence() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "snapshot-rate-feedback";
    config.sessionId = 8404u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for snapshot-rate feedback coverage");
    pumpUntilConnected(&client, &server);

    expect(server.applyControlPayloadForPeer(
               client.peerId(),
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.snapshot_rate",
                   30.0f}}),
           "the synthetic host should be able to stage a live snapshot-rate change");
    server.flushPendingServerPacketsForTest();
    client.update(1.0f / 60.0f, nullptr);
    bool sawStagedMessage =
        client.lastCombatEventText() == "Snapshot rate staged for the next authoritative tick: 30 Hz";
    bool sawLiveMessage = client.lastCombatEventText() == "Live snapshot rate now 30 Hz";
    for (int attempt = 0; attempt < 3 && !sawStagedMessage && !sawLiveMessage; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.lastCombatEventText() ==
            "Snapshot rate staged for the next authoritative tick: 30 Hz") {
            sawStagedMessage = true;
            break;
        }
        if (client.lastCombatEventText() == "Live snapshot rate now 30 Hz") {
            sawLiveMessage = true;
            break;
        }
    }

    const std::string stagedSnapshotFeedback = client.lastCombatEventText();
    expect(sawStagedMessage || sawLiveMessage,
           "client feedback should report the staged or live next-tick snapshot-rate update (actual: " +
               stagedSnapshotFeedback + ")");
    if (sawStagedMessage) {
        const RuntimeSettingsOverlay::State stagedOverlayState = client.runtimeSettingsOverlayStateForTest();
        const RuntimeSettingsOverlay::ControlState* stagedSnapshotRateControl =
            findOverlayControl(stagedOverlayState, RuntimeSettingsOverlay::ControlId::SnapshotRate);
        expect(stagedSnapshotRateControl != nullptr &&
                   stagedSnapshotRateControl->description.find("Live 20 Hz | staged 30 Hz") !=
                       std::string::npos,
               "runtime settings should show both the live and staged snapshot rates before the boundary");
    }

    for (int attempt = 0; attempt < 60 && !sawLiveMessage; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.lastCombatEventText() == "Live snapshot rate now 30 Hz") {
            sawLiveMessage = true;
            break;
        }
    }

    expect(sawLiveMessage,
           "client feedback should confirm when the staged snapshot rate becomes live");
}

void testConnectedClientIgnoresConflictingWelcomePeerReassignment() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "delta";
    config.sessionId = 5005u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    const std::uint16_t stablePeerId = client.peerId();
    const std::uint16_t conflictingPeerId = stablePeerId == 1u ? 2u : 1u;

    server.sendWelcome(client.config().sessionId, conflictingPeerId);
    client.update(0.0f, nullptr);

    expect(client.state() == net::ClientConnectionState::Connected,
           "client should stay connected after a conflicting welcome");
    expect(client.peerId() == stablePeerId,
           "client should ignore a later welcome that attempts to reassign its peer id");
}

void testConnectedIdleClientSendsKeepaliveCommands() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "idle";
    config.sessionId = 6006u;
    config.idleKeepaliveIntervalUs = 100'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    const std::uint32_t baselineAck = client.lastAckedInputSeq();
    for (int frame = 0; frame < 90; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    expect(client.lastAckedInputSeq() > baselineAck,
           "an idle connected client should still send keepalive-equivalent commands that get acknowledged");
}

void testJoinClientTogglesUiModeWithoutHostDiagnostics() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "join-ui";
    config.sessionId = 6100u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(!client.hasHostDiagnostics(),
           "plain join clients should not require host diagnostics to toggle ui mode");
    expect(!client.uiModeActive(),
           "connected join clients should start with ui mode disabled");

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleUIMode = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.uiModeActive(),
           "the dedicated ui-mode input should enable ui mode for join clients without diagnostics");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.uiModeActive(),
           "the dedicated ui-mode input should disable ui mode for join clients without diagnostics");
}

void testJoinClientTogglesReplayOverlay() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "join-replay";
    config.sessionId = 6125u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(!client.clientViewState().replay.overlayVisible,
           "replay overlay should start hidden for connected clients");

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleRecordingOverlay = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.clientViewState().replay.overlayVisible,
           "pressing O should surface the replay overlay on the shared client runtime");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.clientViewState().replay.overlayVisible,
           "pressing O again should hide the replay overlay on the shared client runtime");
}

void testFovConeToggleIsClientLocalPresentationState() {
    net::ClientRuntime client;
    expect(!client.fovConesVisible(),
           "FOV cones should start hidden for a plain client runtime");

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleFovCones = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.fovConesVisible() &&
               client.lastCombatEventText().find("FOV cones shown") != std::string::npos,
           "C should toggle the local FOV cone overlay on without requiring a server round trip");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.fovConesVisible() &&
               client.lastCombatEventText().find("FOV cones hidden") != std::string::npos,
           "C should toggle the local FOV cone overlay off on a second press");
}

void testJoinClientReplayHotkeysDriveSharedReplayState() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "join-replay-hotkeys";
    config.sessionId = 6130u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState recordInput;
    recordInput.toggleRecording = true;
    client.update(1.0f / 60.0f, &recordInput);
    expect(client.clientViewState().replay.recordingActive,
           "pressing 5 should start recording on the shared client runtime");

    for (int frame = 0; frame < 4; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    InputHandler3D::InputState playbackInput;
    playbackInput.togglePlayback = true;
    client.update(1.0f / 60.0f, &playbackInput);
    client::ReplayStatusView replayStatus = client.clientViewState().replay;
    expect(replayStatus.recordingActive && !replayStatus.playbackActive,
           "pressing 6 while recording should not silently leave recording mode");
    expect(client.lastCombatEventText().find("Stop recording with 5") != std::string::npos,
           "pressing 6 while recording should direct the user to stop with 5 first");

    client.update(1.0f / 60.0f, &recordInput);
    client::ReplayStatusView stoppedReplayStatus = client.clientViewState().replay;
    expect(!stoppedReplayStatus.recordingActive && !stoppedReplayStatus.playbackActive,
           "pressing 5 again should stop recording and return to live gameplay instead of replaying");
    expect(stoppedReplayStatus.statusLine.find("Replay ready") != std::string::npos,
           "stopping recording should keep the last capture ready for explicit 6 playback");

    client.update(1.0f / 60.0f, &playbackInput);

    replayStatus = client.clientViewState().replay;
    expect(replayStatus.playbackActive &&
               replayStatus.statusLine.find("Playback (playing)") != std::string::npos,
           "pressing 6 should enter shared replay playback after a capture is available");

    InputHandler3D::InputState resetReplayInput;
    resetReplayInput.resetPlayback = true;
    client.update(1.0f / 60.0f, &resetReplayInput);
    replayStatus = client.clientViewState().replay;
    expect(replayStatus.playbackActive &&
               replayStatus.statusLine.find("Playback (paused) @1/") != std::string::npos,
           "pressing 0 during playback should reset to the first replay frame without exiting replay mode");
    expect(client.lastCombatEventText().find("Playback reset") != std::string::npos,
           "pressing 0 should tell the user that replay playback reset");

    InputHandler3D::InputState exitReplayInput;
    exitReplayInput.stopReplayPlayback = true;
    client.update(1.0f / 60.0f, &exitReplayInput);
    replayStatus = client.clientViewState().replay;
    expect(!replayStatus.recordingActive &&
               !replayStatus.playbackActive &&
               replayStatus.statusLine.find("Replay ready") != std::string::npos,
           "pressing Backspace during playback should exit replay mode while keeping the last recording ready");
    expect(client.lastCombatEventText().find("Replay stopped") != std::string::npos,
           "pressing Backspace should tell the user that replay playback stopped");

    client.update(1.0f / 60.0f, &playbackInput);
    replayStatus = client.clientViewState().replay;
    expect(replayStatus.playbackActive &&
               replayStatus.statusLine.find("Playback (playing)") != std::string::npos,
           "pressing 6 after Backspace should restart playback of the last completed recording");

    client.update(1.0f / 60.0f, &playbackInput);
    replayStatus = client.clientViewState().replay;
    expect(replayStatus.playbackActive &&
               replayStatus.statusLine.find("Playback (paused)") != std::string::npos,
           "pressing 6 during playback should pause the current recording replay");

    client.update(1.0f / 60.0f, &playbackInput);
    replayStatus = client.clientViewState().replay;
    expect(replayStatus.playbackActive &&
               replayStatus.statusLine.find("Playback (playing)") != std::string::npos,
           "pressing 6 again should resume the current recording replay");
}

void testReplayPlaybackSupportsDetachedSpectatorCameraAndCheckpoints() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "playback-spectator";
    config.sessionId = 6131u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState recordInput;
    recordInput.toggleRecording = true;
    client.update(1.0f / 60.0f, &recordInput);
    for (int frame = 0; frame < 5; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }
    client.update(1.0f / 60.0f, &recordInput);

    InputHandler3D::InputState playbackInput;
    playbackInput.togglePlayback = true;
    client.update(1.0f / 60.0f, &playbackInput);

    const client::RenderFrame playbackFrame =
        net::ClientRuntimeTestAccess::renderFrame(client);
    expect(playbackFrame.replay.playbackActive && playbackFrame.hasSnapshot,
           "recorded gameplay playback should expose a renderable replay frame before spectator mode");

    InputHandler3D::InputState spectatorInput;
    spectatorInput.toggleSpectator = true;
    client.update(1.0f / 60.0f, &spectatorInput);
    expect(client.lastCombatEventText().find("Replay spectator mode on") != std::string::npos,
           "toggling spectator during gameplay replay playback should enter replay spectator mode");

    client::ClientViewState spectatorViewState = client.clientViewState();
    expect(spectatorViewState.pane.state.mode == sim::PaneViewMode::SpectatorFreeFly &&
               spectatorViewState.replay.checkpoint.detachedCameraActive,
           "gameplay replay spectator mode should reuse the detached observer pane and checkpoint overlay state");

    const client::RenderFrame seededSpectatorFrame =
        net::ClientRuntimeTestAccess::renderFrame(client);
    const Vector3 seededCameraPosition = seededSpectatorFrame.camera.position;
    const int localActorId = client.localPlayerState().playerId;
    const bool localPlayerVisibleInReplaySpectator =
        std::any_of(seededSpectatorFrame.remotePlayers.begin(),
                    seededSpectatorFrame.remotePlayers.end(),
                    [localActorId](const client::RemotePlayerRenderItem& player) {
                        return player.actorId == localActorId;
                    });
    expect(localPlayerVisibleInReplaySpectator,
           "replay spectator mode should render the recorded local character body");

    InputHandler3D::InputState roamInput;
    roamInput.moveUp = true;
    roamInput.moveInput.y = 1.0f;
    roamInput.fastModifier = true;
    roamInput.lookDelta = Vector2{24.0f, -12.0f};
    client.update(1.0f / 60.0f, &roamInput);

    const client::RenderFrame movedSpectatorFrame =
        net::ClientRuntimeTestAccess::renderFrame(client);
    expect(absoluteDifference(movedSpectatorFrame.camera.position.x, seededCameraPosition.x) > 0.0001f ||
               absoluteDifference(movedSpectatorFrame.camera.position.y, seededCameraPosition.y) > 0.0001f ||
               absoluteDifference(movedSpectatorFrame.camera.position.z, seededCameraPosition.z) > 0.0001f,
           "replay spectator mode should let the detached camera move while recorded playback remains active");
    expect(movedSpectatorFrame.replay.playbackActive,
           "moving the replay spectator camera should keep gameplay replay playback active");

    const std::size_t baselineCheckpointCount =
        spectatorViewState.pane.checkpointCount;
    InputHandler3D::InputState addCheckpointInput;
    addCheckpointInput.addSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &addCheckpointInput);
    spectatorViewState = client.clientViewState();
    expect(spectatorViewState.pane.checkpointCount == baselineCheckpointCount + 1u &&
               spectatorViewState.replay.checkpoint.detachedCameraActive,
           "gameplay replay spectator mode should save checkpoints through the shared detached observer flow");

    InputHandler3D::InputState deleteCheckpointInput;
    deleteCheckpointInput.deleteSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &deleteCheckpointInput);
    spectatorViewState = client.clientViewState();
    expect(spectatorViewState.pane.checkpointCount == baselineCheckpointCount,
           "gameplay replay spectator checkpoint deletion should use the same shared checkpoint helpers");
}

void testGameplayConfirmReleasesMouseWithoutOpeningRuntimeSettings() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "enter-ui";
    config.sessionId = 6150u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(!client.uiModeActive(),
           "gameplay confirm test requires gameplay mode to start with the cursor captured");
    expect(!client.teamMenuVisible(),
           "gameplay confirm test requires the explicit team menu to stay closed");

    InputHandler3D::InputState confirmInput;
    confirmInput.menuConfirm = true;
    client.update(1.0f / 60.0f, &confirmInput);

    expect(client.uiModeActive(),
           "pressing Enter during gameplay should still release the mouse");
    expect(!client.localNetworkPanelVisible(),
           "pressing Enter during gameplay should not open the runtime settings overlay");
    expect(!client.teamMenuVisible(),
           "pressing Enter during gameplay should not reroute through the explicit team menu");

    client.update(1.0f / 60.0f, &confirmInput);
    expect(!client.uiModeActive(),
           "pressing Enter again while no explicit menu is open should recapture the mouse");
    expect(!client.localNetworkPanelVisible(),
           "repeated Enter presses should still avoid surfacing the runtime settings overlay");
}

void testHostDiagnosticsUsesSameUiModeToggle() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "host-ui";
    config.sessionId = 6200u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::ProxyRuntime proxy;
    client.attachProxyDiagnostics(&proxy, client.peerId());
    expect(client.hasHostDiagnostics(),
           "host diagnostics should be attachable without changing the generic ui mode path");

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleUIMode = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.uiModeActive(),
           "host diagnostics clients should use the same dedicated ui-mode toggle");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.uiModeActive(),
           "host diagnostics clients should use the same dedicated ui-mode toggle to leave ui mode");
}

void testHostDiagnosticsDefaultsToAssignedPeerAfterConnect() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "host-target";
    config.sessionId = 6250u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    net::ProxyRuntime proxy;
    client.attachProxyDiagnostics(&proxy);
    expect(client.diagnosticsModel() != nullptr && client.diagnosticsModel()->targetPeerId() == 0u,
           "pre-connect host diagnostics should start untargeted until the server assigns a peer id");

    pumpUntilConnected(&client, &server);

    expect(client.peerId() != 0u,
           "connected host diagnostics test should receive an authoritative peer id");
    expect(client.diagnosticsModel() != nullptr &&
               client.diagnosticsModel()->targetPeerId() == client.peerId(),
           "host diagnostics should retarget themselves to the assigned local peer after connect");
}

void testUiModeKeepsClientConnectedAndSnapshotsAdvancing() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "ui-stable";
    config.sessionId = 6300u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    const net::WorldSnapshot* baselineSnapshot = client.latestSnapshot();
    expect(baselineSnapshot != nullptr,
           "client should have an authoritative snapshot before toggling ui mode");
    const std::uint32_t baselineServerTick = baselineSnapshot->serverTick;

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleUIMode = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.uiModeActive(),
           "ui mode should become active before the stability run");

    for (int frame = 0; frame < 30; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    const net::WorldSnapshot* advancedSnapshot = client.latestSnapshot();
    expect(client.state() == net::ClientConnectionState::Connected,
           "ui mode should not disconnect the client");
    expect(advancedSnapshot != nullptr && advancedSnapshot->serverTick > baselineServerTick,
           "ui mode should not stop authoritative snapshot advancement");
}

void testConnectedClientOpensLocalNetworkPanelAndAdjustsSettingsWithoutDisconnect() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "local-net";
    config.sessionId = 6350u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::UdpSocket proxyServer;
    expect(proxyServer.bind({"127.0.0.1", 0u}), "proxy server socket should bind");
    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", proxyServer.localPort()};
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "local network proxy should start");

    client.attachProxyDiagnostics(&proxy, client.peerId());
    expect(client.hasLocalNetworkControls(),
           "connected clients with a local proxy should expose player-facing local network controls");
    expect(!client.localNetworkPanelVisible(),
           "local network panel should start hidden");

    InputHandler3D::InputState panelInput;
    panelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &panelInput);
    expect(client.localNetworkPanelVisible(),
           "the runtime settings shortcut should open the centered runtime settings menu");
    expect(client.uiModeActive(),
           "opening runtime settings through the panel shortcut should enter runtime ui mode");

    client.setLocalNetworkSettingsForTest(120.0f, 5.0f);
    expect(client.localNetworkLatencyMs() == 120.0f &&
               client.localNetworkLossPct() == 5.0f,
           "player-facing local network settings should preserve the requested latency and loss values");
    expect(client.diagnosticsModel() != nullptr &&
               client.diagnosticsModel()->linkConfig(true).baseDelayMs == 120.0f &&
               client.diagnosticsModel()->linkConfig(false).lossPct == 5.0f,
           "player-facing local network settings should mirror onto the proxy-backed diagnostics model");

    for (int frame = 0; frame < 30; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    const auto& runtimeRequests = server.observedRuntimeParamRequests();
    const net::RuntimeParamChangeRequest* latencyRequest =
        findRuntimeParamRequest(runtimeRequests, "net.player[1].latency_ms");
    const net::RuntimeParamChangeRequest* lossRequest =
        findRuntimeParamRequest(runtimeRequests, "net.player[1].loss_pct");
    expect(latencyRequest != nullptr &&
               latencyRequest->scope == net::RuntimeParamScope::Player &&
               latencyRequest->targetId == 1 &&
               latencyRequest->value == 120.0f,
           "player-facing local network settings should emit a player-scoped latency control request");
    expect(lossRequest != nullptr &&
               lossRequest->scope == net::RuntimeParamScope::Player &&
               lossRequest->targetId == 1 &&
               lossRequest->value == 5.0f,
           "player-facing local network settings should emit a player-scoped loss control request");
    expect(client.diagnosticsModel() != nullptr &&
               client.diagnosticsModel()->hasAuthoritativeLocalNetworkSettings(),
           "player-facing local network settings should consume authoritative apply feedback once the server responds");
    expect(client.diagnosticsModel()->authoritativeLocalNetworkSettings().latencyMs == 120.0f &&
               client.diagnosticsModel()->authoritativeLocalNetworkSettings().lossPct == 5.0f,
           "player-facing local network settings should reconcile the diagnostics model to the authoritative applied values");
    expect(latencyRequest->value == client.diagnosticsModel()->localNetworkSettings().latencyMs &&
               lossRequest->value == client.diagnosticsModel()->localNetworkSettings().lossPct,
           "player-facing local network settings should keep the live panel values aligned with the authoritative request path");
    const auto consoleLines = client.diagnosticsModel()->summaryLines();
    const auto panelLines = client.diagnosticsModel()->localNetworkSummaryLines();
    expect(contains(consoleLines, "Requested Latency 120ms | Loss 5%") &&
               contains(panelLines, "Requested Latency 120ms | Loss 5%"),
           "client diagnostics should keep console and local-network panel requested values synchronized");
    expect(contains(consoleLines, "Applied Latency 120ms | Loss 5%") &&
               contains(panelLines, "Applied Latency 120ms | Loss 5%"),
           "client diagnostics should keep console and local-network panel effective values synchronized");
    expect(contains(consoleLines, "Status applied | Boundary next_tick") &&
               contains(panelLines, "Status applied | Boundary next_tick"),
           "client diagnostics should surface the same staged-apply feedback on both study control surfaces");

    const client::ClientViewState diagnosticsViewState = client.clientViewState();
    expect(contains(diagnosticsViewState.diagnostics.summaryLines,
                    "Applied Latency 120ms | Loss 5%") &&
               contains(diagnosticsViewState.diagnostics.localNetworkSummaryLines,
                        "Applied Latency 120ms | Loss 5%"),
           "typed client diagnostics view state should keep console and panel summaries aligned");
    expect(client.state() == net::ClientConnectionState::Connected,
           "adjusting local latency and loss through the player-facing panel should not disconnect the client");
}

void testConnectedClientTargetsBotRuntimeParamsThroughSharedControlPath() {
    net::ServerConfig serverConfig;
    serverConfig.attackerBotCount = 1u;
    serverConfig.defenderBotCount = 1u;

    LoopbackServerHarness server(HarnessMode::AcceptClient, serverConfig);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "bot-operator";
    config.sessionId = 6351u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::UdpSocket proxyServer;
    expect(proxyServer.bind({"127.0.0.1", 0u}), "proxy server socket should bind");
    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", proxyServer.localPort()};
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "local network proxy should start");

    const int defenderBotId = requireBotActorId(client.roster(), sim::TeamId::Defender);
    client.attachProxyDiagnostics(&proxy, static_cast<std::uint16_t>(defenderBotId));
    expect(client.diagnosticsModel() != nullptr &&
               client.diagnosticsModel()->targetPeerId() == static_cast<std::uint16_t>(defenderBotId) &&
               client.diagnosticsModel()->targetScope() == net::RuntimeParamScope::Bot,
           "bot-targeted diagnostics should preserve the authoritative bot target scope");

    client.setLocalNetworkSettingsForTest(90.0f, 8.0f);
    for (int frame = 0; frame < 30; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    const auto& runtimeRequests = server.observedRuntimeParamRequests();
    const std::string latencyKey = "net.bot[" + std::to_string(defenderBotId) + "].latency_ms";
    const std::string lossKey = "net.bot[" + std::to_string(defenderBotId) + "].loss_pct";
    const net::RuntimeParamChangeRequest* latencyRequest =
        findRuntimeParamRequest(runtimeRequests, latencyKey);
    const net::RuntimeParamChangeRequest* lossRequest =
        findRuntimeParamRequest(runtimeRequests, lossKey);
    expect(latencyRequest != nullptr &&
               latencyRequest->scope == net::RuntimeParamScope::Bot &&
               latencyRequest->targetId == defenderBotId &&
               latencyRequest->value == 90.0f,
           "bot-targeted local network settings should emit a bot-scoped latency control request");
    expect(lossRequest != nullptr &&
               lossRequest->scope == net::RuntimeParamScope::Bot &&
               lossRequest->targetId == defenderBotId &&
               lossRequest->value == 8.0f,
           "bot-targeted local network settings should emit a bot-scoped loss control request");
    expect(client.diagnosticsModel()->hasAuthoritativeLocalNetworkSettings() &&
               client.diagnosticsModel()->authoritativeLocalNetworkSettings().latencyMs == 90.0f &&
               client.diagnosticsModel()->authoritativeLocalNetworkSettings().lossPct == 8.0f,
           "bot-targeted local network settings should reconcile through authoritative apply feedback");
    expect(contains(client.diagnosticsModel()->localNetworkSummaryLines(),
                    std::string("Applies to bot ") + std::to_string(defenderBotId)),
           "bot-targeted diagnostics should identify the authoritative bot target in the panel summary");
}

void testHostDiagnosticsRemainDistinctFromLocalNetworkPanel() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "host-local-net";
    config.sessionId = 6375u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::UdpSocket proxyServer;
    expect(proxyServer.bind({"127.0.0.1", 0u}), "proxy server socket should bind");
    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", proxyServer.localPort()};
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "diagnostics proxy should start");

    client.attachProxyDiagnostics(&proxy, client.peerId());
    expect(client.hasHostDiagnostics(),
           "host-local clients should still expose the host diagnostics surface");
    expect(client.hasLocalNetworkControls(),
           "host-local clients should also expose the player-facing local network controls");

    InputHandler3D::InputState panelInput;
    panelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &panelInput);
    expect(client.localNetworkPanelVisible(),
           "the panel shortcut should open the unified runtime settings menu");
    expect(client.uiModeActive(),
           "the unified runtime settings menu should use the same ui-mode visibility flag");

    InputHandler3D::InputState uiInput;
    uiInput.toggleUIMode = true;
    client.update(1.0f / 60.0f, &uiInput);
    expect(!client.uiModeActive(),
           "the dedicated ui-mode input should close the unified runtime settings menu");
    expect(!client.localNetworkPanelVisible(),
           "the unified runtime settings menu should close no matter which shortcut opened it");
}

void testConnectedClientTogglesInterpolationWithoutDisconnect() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "interp-toggle";
    config.sessionId = 6385u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(client.interpolationEnabled(),
           "connected multiplayer clients should start with interpolation enabled");

    const net::WorldSnapshot* baselineSnapshot = client.latestSnapshot();
    expect(baselineSnapshot != nullptr,
           "client should have an authoritative snapshot before toggling interpolation");
    const std::uint32_t baselineServerTick = baselineSnapshot->serverTick;

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleInterp = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.interpolationEnabled(),
           "I should toggle multiplayer interpolation off locally");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.interpolationEnabled(),
           "I should toggle multiplayer interpolation back on locally");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.interpolationEnabled(),
           "multiplayer interpolation should continue toggling reliably on repeated local input");

    for (int frame = 0; frame < 30; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    const net::WorldSnapshot* advancedSnapshot = client.latestSnapshot();
    expect(client.state() == net::ClientConnectionState::Connected,
           "toggling multiplayer interpolation should not disconnect the client");
    expect(advancedSnapshot != nullptr && advancedSnapshot->serverTick > baselineServerTick,
           "toggling multiplayer interpolation should not stop authoritative snapshot advancement");
}

void testConnectedClientTogglesPredictionWithoutDisconnect() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "predict-toggle";
    config.sessionId = 6395u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(client.predictionEnabled(),
           "connected multiplayer clients should start with prediction enabled");

    const net::WorldSnapshot* baselineSnapshot = client.latestSnapshot();
    expect(baselineSnapshot != nullptr,
           "client should have an authoritative snapshot before toggling prediction");
    const std::uint32_t baselineServerTick = baselineSnapshot->serverTick;

    InputHandler3D::InputState toggleInput;
    toggleInput.togglePrediction = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.predictionEnabled(),
           "K should toggle multiplayer prediction off locally");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(client.predictionEnabled(),
           "K should toggle multiplayer prediction back on locally");

    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.predictionEnabled(),
           "multiplayer prediction should continue toggling reliably on repeated local input");

    for (int frame = 0; frame < 30; ++frame) {
        pumpLoopbackFrame(&client, &server);
    }

    const net::WorldSnapshot* advancedSnapshot = client.latestSnapshot();
    expect(client.state() == net::ClientConnectionState::Connected,
           "toggling multiplayer prediction should not disconnect the client");
    expect(advancedSnapshot != nullptr && advancedSnapshot->serverTick > baselineServerTick,
           "toggling multiplayer prediction should not stop authoritative snapshot advancement");
}

void testJoinedClientPredictionDisabledSuppressesImmediateLocalFeedback() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "predict-off";
    config.sessionId = 6398u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);
    expect(client.hasSnapshot(),
           "joined client prediction-off test requires an authoritative snapshot");

    InputHandler3D::InputState toggleInput;
    toggleInput.togglePrediction = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.predictionEnabled(),
           "joined client should allow prediction to be disabled locally");

    const sim::PlayerState baselineState = client.localPlayerState();
    const sim::Vec3 baselineLookDirection = client.clientViewState().camera.lookDirection;
    const std::size_t baselineTraceCount = client.combatTraceCount();
    const std::string baselineCombatEventText = client.lastCombatEventText();

    InputHandler3D::InputState gameplayInput;
    gameplayInput.moveInput.y = 1.0f;
    gameplayInput.lookDelta = Vector2{45.0f, -30.0f};
    gameplayInput.jumpPressed = true;
    gameplayInput.firePressed = true;
    client.update(1.0f / 60.0f, &gameplayInput);

    expect(absoluteDifference(client.localPlayerState().position.x, baselineState.position.x) < 0.0001f &&
               absoluteDifference(client.localPlayerState().position.y, baselineState.position.y) < 0.0001f &&
               absoluteDifference(client.localPlayerState().position.z, baselineState.position.z) < 0.0001f,
           "joined clients should not apply immediate movement or jump prediction while prediction is disabled");
    expect(absoluteDifference(client.clientViewState().camera.lookDirection.x, baselineLookDirection.x) > 0.0001f ||
               absoluteDifference(client.clientViewState().camera.lookDirection.y, baselineLookDirection.y) > 0.0001f ||
               absoluteDifference(client.clientViewState().camera.lookDirection.z, baselineLookDirection.z) > 0.0001f,
           "joined clients should still allow immediate local camera turning while prediction is disabled");
    expect(client.combatTraceCount() == baselineTraceCount,
           "joined clients should not render immediate local fire traces while prediction is disabled");
    expect(client.lastCombatEventText() == baselineCombatEventText,
           "joined clients should not emit immediate pending-fire text while prediction is disabled");
}

void testClientSyncRuntimeKeepsHostAndJoinPredictionSemanticsAligned() {
    client::ClientSyncRuntime syncRuntime;
    ClientSyncSemanticsFixture hostFixture(7u);
    ClientSyncSemanticsFixture joinFixture(19u);

    InputHandler3D::InputState input;
    input.moveInput = Vector2{0.5f, -1.0f};
    input.jumpPressed = true;
    input.firePressed = true;

    const sim::MovementEnvironment environment;
    const sim::SimConfig simConfig;
    const float dtSeconds = 1.0f / 60.0f;
    const float viewYaw = 1.5f;
    const float viewPitch = -0.25f;

    const sim::PlayerCommand hostCommand = syncRuntime.buildCommand(hostFixture.context(),
                                                                    input,
                                                                    dtSeconds,
                                                                    12u,
                                                                    viewYaw,
                                                                    viewPitch,
                                                                    sim::TeamId::Defender,
                                                                    42u,
                                                                    7u);
    const sim::PlayerCommand joinCommand = syncRuntime.buildCommand(joinFixture.context(),
                                                                    input,
                                                                    dtSeconds,
                                                                    12u,
                                                                    viewYaw,
                                                                    viewPitch,
                                                                    sim::TeamId::Defender,
                                                                    42u,
                                                                    7u);

    expect(hostCommand.moveX == joinCommand.moveX &&
               hostCommand.moveY == joinCommand.moveY &&
               hostCommand.buttons == joinCommand.buttons &&
               hostCommand.yaw == joinCommand.yaw &&
               hostCommand.pitch == joinCommand.pitch &&
               hostCommand.viewedServerTimeUs == joinCommand.viewedServerTimeUs &&
               hostCommand.interpDelayMs == joinCommand.interpDelayMs,
           "host-style and join-style sync contexts should shape identical gameplay commands");

    hostFixture.predictionEnabled = false;
    joinFixture.predictionEnabled = false;
    syncRuntime.applyLocalPrediction(hostFixture.context(), hostCommand, environment, simConfig);
    syncRuntime.applyLocalPrediction(joinFixture.context(), joinCommand, environment, simConfig);
    expect(hostFixture.predictionBuffer.pendingCommandCount() == 0u &&
               joinFixture.predictionBuffer.pendingCommandCount() == 0u,
           "host and joined sync contexts should both suppress pending prediction queues when prediction is disabled");
    expect(absoluteDifference(hostFixture.localPlayerState.position.z,
                              hostFixture.latestSnapshot.localPlayerState.position.z) < 0.0001f &&
               absoluteDifference(joinFixture.localPlayerState.position.z,
                                  joinFixture.latestSnapshot.localPlayerState.position.z) < 0.0001f,
           "host and joined sync contexts should both preserve authoritative local state when prediction is disabled");
    expect(!syncRuntime.shouldPredictFireAttempt(hostFixture.context(), input) &&
               !syncRuntime.shouldPredictFireAttempt(joinFixture.context(), input),
           "host and joined sync contexts should both suppress immediate fire prediction when disabled");

    hostFixture.predictionEnabled = true;
    joinFixture.predictionEnabled = true;
    syncRuntime.applyLocalPrediction(hostFixture.context(), hostCommand, environment, simConfig);
    syncRuntime.applyLocalPrediction(joinFixture.context(), joinCommand, environment, simConfig);
    expect(hostFixture.predictionBuffer.pendingCommandCount() == 1u &&
               joinFixture.predictionBuffer.pendingCommandCount() == 1u,
           "host and joined sync contexts should both enqueue pending commands when prediction is enabled");
    expect(syncRuntime.shouldPredictFireAttempt(hostFixture.context(), input) &&
               syncRuntime.shouldPredictFireAttempt(joinFixture.context(), input),
           "host and joined sync contexts should both preserve immediate fire prediction when enabled");
    expect(absoluteDifference(hostFixture.localPlayerState.position.x, joinFixture.localPlayerState.position.x) < 0.0001f &&
               absoluteDifference(hostFixture.localPlayerState.position.y, joinFixture.localPlayerState.position.y) < 0.0001f &&
               absoluteDifference(hostFixture.localPlayerState.position.z, joinFixture.localPlayerState.position.z) < 0.0001f,
           "host and joined sync contexts should replay the same predicted local state under identical inputs");
}

void testLocalTogglesPreserveExplicitDisconnectHandling() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "disconnect-toggle";
    config.sessionId = 6405u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::UdpSocket proxyServer;
    expect(proxyServer.bind({"127.0.0.1", 0u}), "proxy server socket should bind");
    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", proxyServer.localPort()};
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "disconnect diagnostics proxy should start");
    client.attachProxyDiagnostics(&proxy, client.peerId());

    server.addAuthoritativeRosterEntry(101,
                                       sim::TeamId::Defender,
                                       true,
                                       3u,
                                       1u,
                                       false,
                                       "Defense Bot",
                                       2u,
                                       44u,
                                       7u);
    server.setAuthoritativeTeamScores(4u, 2u);
    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() >= 2u && client.teamScores().attackers == 4u) {
            break;
        }
    }

    InputHandler3D::InputState panelInput;
    panelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &panelInput);

    InputHandler3D::InputState interpInput;
    interpInput.toggleInterp = true;
    client.update(1.0f / 60.0f, &interpInput);

    InputHandler3D::InputState predictionInput;
    predictionInput.togglePrediction = true;
    client.update(1.0f / 60.0f, &predictionInput);

    InputHandler3D::InputState closePanelInput;
    closePanelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &closePanelInput);

    InputHandler3D::InputState scoreboardInput;
    scoreboardInput.toggleScoreboard = true;
    client.update(1.0f / 60.0f, &scoreboardInput);

    expect(!client.localNetworkPanelVisible() &&
               !client.interpolationEnabled() &&
               !client.predictionEnabled() &&
               client.scoreboardVisible(),
           "disconnect regression requires runtime settings toggles and the scoreboard overlay to be active first");

    server.sendDisconnectForTest("server shutting down");
    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.state() == net::ClientConnectionState::Rejected) {
            break;
        }
    }

    expect(client.state() == net::ClientConnectionState::Rejected,
           "explicit disconnect should still transition the client into the rejected state after local-only toggles");
    expect(client.statusMessage().find("server shutting down") != std::string::npos,
           "explicit disconnect should preserve the disconnect reason after local-only toggles");
    expect(client.teamScores().attackers == 4u && client.teamScores().defenders == 2u,
           "explicit disconnect should preserve the last authoritative team score after local-only toggles");
    expect(client.roster().size() >= 2u,
           "explicit disconnect should preserve the last authoritative roster after local-only toggles");
}

void testLocalTogglesPreserveTimeoutHandling() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "timeout-toggle";
    config.sessionId = 6410u;
    config.serverSilenceTimeoutUs = 120'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::UdpSocket proxyServer;
    expect(proxyServer.bind({"127.0.0.1", 0u}), "proxy server socket should bind");
    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", proxyServer.localPort()};
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "timeout diagnostics proxy should start");
    client.attachProxyDiagnostics(&proxy, client.peerId());

    server.addAuthoritativeRosterEntry(101,
                                       sim::TeamId::Defender,
                                       true,
                                       3u,
                                       1u,
                                       false,
                                       "Defense Bot",
                                       2u,
                                       44u,
                                       7u);
    server.setAuthoritativeTeamScores(4u, 2u);
    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() >= 2u && client.teamScores().attackers == 4u) {
            break;
        }
    }

    InputHandler3D::InputState panelInput;
    panelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &panelInput);

    InputHandler3D::InputState interpInput;
    interpInput.toggleInterp = true;
    client.update(1.0f / 60.0f, &interpInput);

    InputHandler3D::InputState predictionInput;
    predictionInput.togglePrediction = true;
    client.update(1.0f / 60.0f, &predictionInput);

    InputHandler3D::InputState closePanelInput;
    closePanelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &closePanelInput);

    InputHandler3D::InputState scoreboardInput;
    scoreboardInput.toggleScoreboard = true;
    client.update(1.0f / 60.0f, &scoreboardInput);

    expect(!client.localNetworkPanelVisible() &&
               !client.interpolationEnabled() &&
               !client.predictionEnabled() &&
               client.scoreboardVisible(),
           "timeout regression requires runtime settings toggles and the scoreboard overlay to be active first");

    for (int attempt = 0; attempt < 30; ++attempt) {
        client.update(1.0f / 60.0f, nullptr);
        if (client.state() == net::ClientConnectionState::TimedOut) {
            break;
        }
    }

    expect(client.state() == net::ClientConnectionState::TimedOut,
           "server silence should still time out the client after local-only toggles");
    expect(client.statusMessage().find("timed out") != std::string::npos,
           "timeout handling should preserve the timeout reason after local-only toggles");
    expect(client.teamScores().attackers == 4u && client.teamScores().defenders == 2u,
           "timeout handling should preserve the last authoritative team score after local-only toggles");
    expect(client.roster().size() >= 2u,
           "timeout handling should preserve the last authoritative roster after local-only toggles");
}

void testScoreboardFollowsHeldTabInput() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "score-toggle";
    config.sessionId = 6400u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(!client.scoreboardVisible(),
           "scoreboard should start hidden");
    expect(!client.uiModeActive(),
           "scoreboard should start with the gameplay cursor captured");

    InputHandler3D::InputState showInput;
    showInput.toggleScoreboard = true;
    client.update(1.0f / 60.0f, &showInput);
    expect(client.scoreboardVisible(),
           "holding the scoreboard input should show the full overlay");
    expect(client.uiModeActive(),
           "the host scoreboard should temporarily release the cursor for admin controls");

    InputHandler3D::InputState hideInput;
    client.update(1.0f / 60.0f, &hideInput);
    expect(!client.scoreboardVisible(),
           "releasing the scoreboard input should hide the overlay again");
    expect(!client.uiModeActive(),
           "releasing the host scoreboard should restore captured gameplay input");
}

void testChangeTeamOpensExplicitMenuInsteadOfImmediateSwitch() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "team-open";
    config.sessionId = 6450u;
    config.preferredTeam = sim::TeamId::Attacker;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    expect(localClientHasTeam(client, sim::TeamId::Attacker),
           "explicit team-menu test requires the client to start on the attacker roster");

    InputHandler3D::InputState toggleInput;
    toggleInput.switchTeam = true;
    client.update(1.0f / 60.0f, &toggleInput);

    expect(client.teamMenuVisible(),
           "switch-team input should open an explicit change-team menu");
    expect(client.teamMenuSelection() == sim::TeamId::Defender,
           "change-team menu should default to the opposite playable team");
    expect(localClientHasTeam(client, sim::TeamId::Attacker),
           "opening the change-team menu should not immediately change the authoritative team");
}

void testChangeTeamConfirmationRequestsSelectedTeamAndRespawnText() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "team-confirm";
    config.sessionId = 6451u;
    config.preferredTeam = sim::TeamId::Attacker;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState openInput;
    openInput.switchTeam = true;
    client.update(1.0f / 60.0f, &openInput);
    expect(client.teamMenuVisible(),
           "change-team confirmation test requires the explicit menu to open first");

    InputHandler3D::InputState confirmInput;
    confirmInput.menuConfirm = true;
    client.update(1.0f / 60.0f, &confirmInput);

    expect(!client.teamMenuVisible(),
           "confirming a team choice should close the explicit change-team menu");
    expect(client.lastCombatEventText().find("Requested switch to Defenders") != std::string::npos,
           "confirming a team choice should surface the selected target team immediately");
    expect(client.lastCombatEventText().find("respawns you") != std::string::npos,
           "confirming a team choice should warn that the switch respawns immediately");

    waitForLocalTeam(&client,
                     &server,
                     sim::TeamId::Defender,
                     "confirming the change-team menu should send an authoritative defender request");
    expect(!server.observedTeamChangeRequests().empty() &&
               server.observedTeamChangeRequests().back().requestedTeam == sim::TeamId::Defender,
           "confirming the change-team menu should emit an explicit control-plane defender request");
}

void testChangeTeamCancelLeavesTeamUnchangedAndRestoresNormalPlayPresentation() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "team-cancel";
    config.sessionId = 6452u;
    config.preferredTeam = sim::TeamId::Attacker;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState openInput;
    openInput.switchTeam = true;
    client.update(1.0f / 60.0f, &openInput);
    expect(client.teamMenuVisible(),
           "change-team cancel test requires the explicit menu to open first");

    InputHandler3D::InputState cancelInput;
    cancelInput.switchTeam = true;
    client.update(1.0f / 60.0f, &cancelInput);

    expect(!client.teamMenuVisible(),
           "pressing the change-team key again should cancel the explicit menu");
    expect(client.lastCombatEventText().find("cancelled") != std::string::npos,
           "cancelling the explicit change-team menu should provide immediate feedback");

    for (int attempt = 0; attempt < 15; ++attempt) {
        pumpLoopbackFrame(&client, &server);
    }

    expect(localClientHasTeam(client, sim::TeamId::Attacker),
           "cancelling the explicit change-team menu should leave the authoritative team unchanged");
}

void testChangeTeamMenuCanRequestSpectatorMode() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "team-spectator";
    config.sessionId = 6453u;
    config.preferredTeam = sim::TeamId::Attacker;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState openInput;
    openInput.switchTeam = true;
    client.update(1.0f / 60.0f, &openInput);
    expect(client.teamMenuVisible(),
           "spectator-selection test requires the explicit team menu to open first");
    expect(client.teamMenuSelection() == sim::TeamId::Defender,
           "change-team menu should still default to the opposite playable team");

    InputHandler3D::InputState cycleInput;
    cycleInput.menuRight = true;
    client.update(1.0f / 60.0f, &cycleInput);
    expect(client.teamMenuSelection() == sim::TeamId::Spectator,
           "cycling the change-team menu should include spectator as a selectable target");

    InputHandler3D::InputState confirmInput;
    confirmInput.menuConfirm = true;
    client.update(1.0f / 60.0f, &confirmInput);

    expect(!client.teamMenuVisible(),
           "confirming spectator selection should close the explicit team menu");
    expect(client.lastCombatEventText().find("Spectator") != std::string::npos,
           "confirming spectator selection should surface spectator feedback immediately");

    waitForLocalTeam(&client,
                     &server,
                     sim::TeamId::Spectator,
                     "confirming spectator selection should send an authoritative spectator request");
    expect(!server.observedTeamChangeRequests().empty() &&
               server.observedTeamChangeRequests().back().requestedTeam == sim::TeamId::Spectator,
           "confirming spectator selection should emit an explicit spectator control-plane request");
}

void testJoiningAsSessionSpectatorStartsInFreeFlyMode() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "join-spectator";
    config.sessionId = 6458u;
    config.preferredTeam = sim::TeamId::Spectator;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);
    waitForLocalTeam(&client,
                     &server,
                     sim::TeamId::Spectator,
                     "joining with spectator selected should preserve the authoritative spectator team");

    const client::ClientViewState baselineViewState = client.clientViewState();
    expect(baselineViewState.pane.state.mode == sim::PaneViewMode::SpectatorFreeFly &&
               baselineViewState.pane.observationContext == client::ObservationContextView::SessionSpectator,
           "joining as spectator should start in the session spectator free-fly camera instead of player movement mode");

    const sim::Vec3 baselinePlayerPosition = client.localPlayerState().position;
    const sim::Vec3 baselineCameraPosition = baselineViewState.camera.eyePosition;

    InputHandler3D::InputState ascendInput;
    ascendInput.jumpPressed = true;
    ascendInput.moveUp = true;
    client.update(1.0f / 60.0f, &ascendInput);

    const client::ClientViewState ascendedViewState = client.clientViewState();
    expect(absoluteDifference(ascendedViewState.camera.eyePosition.y, baselineCameraPosition.y) > 0.0001f,
           "session spectator space input should fly the camera upward instead of performing a grounded jump");
    expect(absoluteDifference(client.localPlayerState().position.x, baselinePlayerPosition.x) < 0.0001f &&
               absoluteDifference(client.localPlayerState().position.y, baselinePlayerPosition.y) < 0.0001f &&
               absoluteDifference(client.localPlayerState().position.z, baselinePlayerPosition.z) < 0.0001f,
           "session spectator movement should not move an authoritative player body");

    InputHandler3D::InputState toggleInput;
    toggleInput.toggleSpectator = true;
    client.update(1.0f / 60.0f, &toggleInput);
    const client::ClientViewState toggledViewState = client.clientViewState();
    expect(toggledViewState.pane.state.mode == sim::PaneViewMode::SpectatorFreeFly &&
               toggledViewState.pane.observationContext == client::ObservationContextView::SessionSpectator,
           "session spectators should remain in free-fly mode instead of toggling back into player-style control");
}

void testToggleSpectatorUsesDetachedFreeCameraWithoutLeavingTheMatch() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "toggle-spectator";
    config.sessionId = 6454u;
    config.preferredTeam = sim::TeamId::Attacker;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);
    expect(localClientHasTeam(client, sim::TeamId::Attacker),
           "detached free-camera test requires the client to start on a playable team");

    const sim::Vec3 baselinePlayerPosition = client.localPlayerState().position;
    const client::ClientViewState baselineViewState = client.clientViewState();

    InputHandler3D::InputState enterSpectatorInput;
    enterSpectatorInput.toggleSpectator = true;
    client.update(1.0f / 60.0f, &enterSpectatorInput);
    expect(client.lastCombatEventText().find("free camera") != std::string::npos ||
               client.lastCombatEventText().find("Free camera") != std::string::npos,
           "entering detached observer mode should surface immediate free-camera feedback");

    client::ClientViewState detachedViewState = client.clientViewState();
    expect(detachedViewState.pane.state.mode == sim::PaneViewMode::SpectatorFreeFly &&
               detachedViewState.pane.observationContext == client::ObservationContextView::PaneLocalObservation,
           "detached observer mode should expose a pane-local spectator free-fly camera without changing participation");
    const std::size_t baselineCheckpointCount = detachedViewState.pane.checkpointCount;
    expect(std::any_of(detachedViewState.remotePlayers.begin(),
                       detachedViewState.remotePlayers.end(),
                       [&client](const client::RemotePlayerView& view) {
                           return view.actorId == static_cast<int>(client.peerId());
                       }),
           "detached observer mode should render the local player's parked body in the world");
    expect(server.observedTeamChangeRequests().empty(),
           "entering detached observer mode should not emit a session spectator team-change request");

    InputHandler3D::InputState roamInput;
    roamInput.moveInput.y = 1.0f;
    roamInput.moveUp = true;
    roamInput.fastModifier = true;
    roamInput.lookDelta = Vector2{30.0f, -18.0f};
    client.update(1.0f / 60.0f, &roamInput);

    detachedViewState = client.clientViewState();
    expect(absoluteDifference(detachedViewState.camera.eyePosition.x, baselineViewState.camera.eyePosition.x) > 0.0001f ||
               absoluteDifference(detachedViewState.camera.eyePosition.y, baselineViewState.camera.eyePosition.y) > 0.0001f ||
               absoluteDifference(detachedViewState.camera.eyePosition.z, baselineViewState.camera.eyePosition.z) > 0.0001f,
           "detached observer mode should let the camera move independently of the local player body");
    expect(absoluteDifference(client.localPlayerState().position.x, baselinePlayerPosition.x) < 0.0001f &&
               absoluteDifference(client.localPlayerState().position.y, baselinePlayerPosition.y) < 0.0001f &&
               absoluteDifference(client.localPlayerState().position.z, baselinePlayerPosition.z) < 0.0001f,
           "detached observer movement should not move the authoritative local player immediately");

    const sim::Vec3 checkpointA = detachedViewState.camera.eyePosition;
    InputHandler3D::InputState addCheckpointInput;
    addCheckpointInput.addSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &addCheckpointInput);
    detachedViewState = client.clientViewState();
    expect(detachedViewState.pane.checkpointCount == baselineCheckpointCount + 1u,
           "saving a detached observer checkpoint should increase the active checkpoint count");

    InputHandler3D::InputState roamToSecondCheckpoint;
    roamToSecondCheckpoint.moveInput.x = 1.0f;
    roamToSecondCheckpoint.moveDown = true;
    roamToSecondCheckpoint.fastModifier = true;
    client.update(1.0f / 60.0f, &roamToSecondCheckpoint);
    const sim::Vec3 checkpointB = client.clientViewState().camera.eyePosition;

    client.update(1.0f / 60.0f, &addCheckpointInput);
    detachedViewState = client.clientViewState();
    expect(detachedViewState.pane.checkpointCount == baselineCheckpointCount + 2u,
           "saving a second detached observer checkpoint should keep both captured viewpoints");

    InputHandler3D::InputState previousCheckpointInput;
    previousCheckpointInput.prevSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &previousCheckpointInput);
    InputHandler3D::InputState idleDetachedInput;
    for (int attempt = 0; attempt < 30; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, &idleDetachedInput);
    }

    detachedViewState = client.clientViewState();
    expect(absoluteDifference(detachedViewState.camera.eyePosition.x, checkpointA.x) < 0.15f &&
               absoluteDifference(detachedViewState.camera.eyePosition.y, checkpointA.y) < 0.15f &&
               absoluteDifference(detachedViewState.camera.eyePosition.z, checkpointA.z) < 0.15f,
           "cycling detached observer checkpoints should transition the camera back to the saved viewpoint");

    InputHandler3D::InputState showReplayOverlayInput;
    showReplayOverlayInput.toggleRecordingOverlay = true;
    client.update(1.0f / 60.0f, &showReplayOverlayInput);
    detachedViewState = client.clientViewState();
    expect(detachedViewState.replay.overlayVisible &&
               detachedViewState.replay.checkpoint.detachedCameraActive &&
               detachedViewState.replay.checkpoint.activeIndex == detachedViewState.pane.activeCheckpointIndex &&
               detachedViewState.replay.checkpoint.checkpointCount == detachedViewState.pane.checkpointCount,
           "recording overlay should expose the active detached-camera checkpoint indicator");
    const float baselineTransitionSeconds =
        detachedViewState.replay.checkpoint.transitionToNextSeconds;

    InputHandler3D::InputState increaseTransitionInput;
    increaseTransitionInput.increaseSpectatorTransitionDuration = true;
    client.update(1.0f / 60.0f, &increaseTransitionInput);
    detachedViewState = client.clientViewState();
    const float increasedTransitionSeconds =
        detachedViewState.replay.checkpoint.transitionToNextSeconds;
    expect(detachedViewState.replay.checkpoint.transitionEditable &&
               absoluteDifference(increasedTransitionSeconds,
                                  baselineTransitionSeconds + 0.25f) < 0.0001f,
           "period should increase the current checkpoint's transition-to-next duration by 0.25 seconds");

    InputHandler3D::InputState decreaseTransitionInput;
    decreaseTransitionInput.decreaseSpectatorTransitionDuration = true;
    client.update(1.0f / 60.0f, &decreaseTransitionInput);
    detachedViewState = client.clientViewState();
    expect(absoluteDifference(detachedViewState.replay.checkpoint.transitionToNextSeconds,
                              baselineTransitionSeconds) < 0.0001f,
           "comma should decrease the current checkpoint's transition-to-next duration by 0.25 seconds");

    client.update(1.0f / 60.0f, &increaseTransitionInput);
    detachedViewState = client.clientViewState();
    expect(absoluteDifference(detachedViewState.replay.checkpoint.transitionToNextSeconds,
                              increasedTransitionSeconds) < 0.0001f,
           "restoring the longer checkpoint transition should keep the edited duration visible in the overlay");

    InputHandler3D::InputState nextCheckpointInput;
    nextCheckpointInput.nextSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &nextCheckpointInput);
    for (int attempt = 0; attempt < 10; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, &idleDetachedInput);
    }
    detachedViewState = client.clientViewState();
    expect(absoluteDifference(detachedViewState.camera.eyePosition.x, checkpointB.x) > 0.15f ||
               absoluteDifference(detachedViewState.camera.eyePosition.y, checkpointB.y) > 0.15f ||
               absoluteDifference(detachedViewState.camera.eyePosition.z, checkpointB.z) > 0.15f,
           "increased checkpoint transition durations should keep the camera in-flight partway through the move");
    for (int attempt = 0; attempt < 60; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, &idleDetachedInput);
    }
    detachedViewState = client.clientViewState();
    expect(absoluteDifference(detachedViewState.camera.eyePosition.x, checkpointB.x) < 0.15f &&
               absoluteDifference(detachedViewState.camera.eyePosition.y, checkpointB.y) < 0.15f &&
               absoluteDifference(detachedViewState.camera.eyePosition.z, checkpointB.z) < 0.15f,
           "checkpoint transition timing edits should still converge on the selected next checkpoint");

    InputHandler3D::InputState deleteCheckpointInput;
    deleteCheckpointInput.deleteSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &deleteCheckpointInput);
    client.update(1.0f / 60.0f, &deleteCheckpointInput);
    detachedViewState = client.clientViewState();
    expect(detachedViewState.pane.checkpointCount == baselineCheckpointCount,
           "deleting the added detached observer checkpoints should restore the prior checkpoint count");
    expect(absoluteDifference(checkpointA.x, checkpointB.x) > 0.0001f ||
               absoluteDifference(checkpointA.y, checkpointB.y) > 0.0001f ||
               absoluteDifference(checkpointA.z, checkpointB.z) > 0.0001f,
           "detached observer checkpoint regression test requires two distinct saved viewpoints");

    InputHandler3D::InputState exitSpectatorInput;
    exitSpectatorInput.toggleSpectator = true;
    client.update(1.0f / 60.0f, &exitSpectatorInput);
    expect(client.lastCombatEventText().find("Returned") != std::string::npos,
           "leaving detached observer mode should surface a return-to-character message");

    const client::ClientViewState restoredViewState = client.clientViewState();
    expect(restoredViewState.pane.state.mode == sim::PaneViewMode::PlayerControlled,
           "leaving detached observer mode should restore the player-controlled pane");
    expect(localClientHasTeam(client, sim::TeamId::Attacker) &&
               server.observedTeamChangeRequests().empty(),
           "detached observer mode should preserve the active match team and avoid team-change packets entirely");
}

void testRecordingCheckpointDefaultUsesOneSecondTransitionAndPersists() {
    const std::filesystem::path tempRoot = makeUniqueTempDirectory("netcodesim-clientruntime-home");
    ScopedDirectoryCleanup cleanup(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar dataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath currentPath(tempRoot / "repo");

    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback for recording-checkpoint coverage");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "record-checkpoints";
    config.sessionId = 6450u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for recording-checkpoint coverage");
    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState spectatorInput;
    spectatorInput.toggleSpectator = true;
    client.update(1.0f / 60.0f, &spectatorInput);

    InputHandler3D::InputState addCheckpointInput;
    addCheckpointInput.addSpectatorCheckpoint = true;
    client.update(1.0f / 60.0f, &addCheckpointInput);

    const client::ClientViewState baselineViewState = client.clientViewState();
    expect(absoluteDifference(
               baselineViewState.replay.checkpoint.transitionToNextSeconds,
               SpectatorCamera::kDefaultCheckpointTransitionSeconds) < 0.0001f,
           "non-recording detached-observer checkpoints should keep the general spectator transition default");

    app::CheckpointStore checkpointStore(app::CheckpointCollection::Latency);
    const app::CheckpointLoadResult baselineLoad = checkpointStore.load();
    expect(baselineLoad.loaded &&
               baselineLoad.checkpoints.size() == 1u &&
               absoluteDifference(baselineLoad.checkpoints.front().transitionDurationSeconds,
                                  SpectatorCamera::kDefaultCheckpointTransitionSeconds) < 0.0001f,
           "non-recording detached-observer checkpoints should persist the general spectator transition default");

    InputHandler3D::InputState recordInput;
    recordInput.toggleRecording = true;
    client.update(1.0f / 60.0f, &recordInput);

    InputHandler3D::InputState moveInput;
    moveInput.moveInput.x = 1.0f;
    moveInput.fastModifier = true;
    client.update(1.0f / 60.0f, &moveInput);
    client.update(1.0f / 60.0f, &addCheckpointInput);

    const client::ClientViewState recordingViewState = client.clientViewState();
    expect(recordingViewState.replay.recordingActive &&
               recordingViewState.pane.checkpointCount == 2u &&
               absoluteDifference(recordingViewState.replay.checkpoint.transitionToNextSeconds, 1.0f) <
                   0.0001f,
           "detached-observer checkpoints saved while recording should default to a one-second transition");

    const app::CheckpointLoadResult recordingLoad = checkpointStore.load();
    expect(recordingLoad.loaded &&
               recordingLoad.checkpoints.size() == 2u &&
               absoluteDifference(recordingLoad.checkpoints.back().transitionDurationSeconds, 1.0f) <
                   0.0001f,
           "recording-time detached-observer checkpoints should persist the one-second transition default immediately");
}

void testCompactHudAndScoreboardReflectLatestAuthoritativeSnapshot() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "score-data";
    config.sessionId = 6500u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    net::UdpSocket proxyServer;
    expect(proxyServer.bind({"127.0.0.1", 0u}), "proxy server socket should bind");
    net::ProxyConfig proxyConfig;
    proxyConfig.serverEndpoint = {"127.0.0.1", proxyServer.localPort()};
    net::ProxyRuntime proxy(proxyConfig);
    expect(proxy.start(), "compact-hud diagnostics proxy should start");
    client.attachProxyDiagnostics(&proxy, client.peerId());

    server.addAuthoritativeRosterEntry(101,
                                       sim::TeamId::Defender,
                                       true,
                                       3u,
                                       1u,
                                       false,
                                       "Defense Bot",
                                       2u,
                                       44u,
                                       7u);
    server.setAuthoritativeTeamScores(4u, 2u);

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() >= 2u && client.teamScores().attackers == 4u) {
            break;
        }
    }

    InputHandler3D::InputState panelInput;
    panelInput.toggleUIPanel = true;
    client.update(1.0f / 60.0f, &panelInput);
    expect(client.localNetworkPanelVisible(),
           "compact-hud test requires the intentional runtime settings surface to remain available");

    expect(client.teamScoreSummary().find("Attackers 4") != std::string::npos,
           "compact team score text should reflect the latest attacker total");
    expect(client.teamScoreSummary().find("Defenders 2") != std::string::npos,
           "compact team score text should reflect the latest defender total");

    const client::ClientViewState viewState = client.clientViewState();
    expect(!viewState.scoreboardVisible &&
               viewState.diagnostics.localNetworkPanelVisible &&
               viewState.compactScore.available &&
               viewState.compactScore.attackerScore == 4u &&
               viewState.compactScore.defenderScore == 2u &&
               viewState.attackerScore == 4u &&
               viewState.defenderScore == 2u,
           "client view state should preserve runtime settings visibility and structured compact-score totals without forcing the scoreboard overlay open");
    expect(viewState.scoreboardSections.size() == 2u &&
               viewState.scoreboardSections[0].team == sim::TeamId::Attacker &&
               viewState.scoreboardSections[1].team == sim::TeamId::Defender,
           "client view state should keep typed attacker and defender scoreboard sections");

    const std::vector<std::string> hudLines = client.compactHudLines();
    expect(std::none_of(hudLines.begin(),
                        hudLines.end(),
                        [](const std::string& line) {
                            return line.find("Team Kills") != std::string::npos;
                        }),
           "compact score data should move into typed overlay state instead of remaining the first generic hud string");
    for (const auto& line : hudLines) {
        expect(line.find("Peer") == std::string::npos,
               "compact hud should not reintroduce peer-id diagnostics by default");
        expect(line.find("Port") == std::string::npos,
               "compact hud should not reintroduce local-port diagnostics by default");
        expect(line.find("Ack") == std::string::npos,
               "compact hud should not reintroduce ack counters by default");
        expect(line.find("Latency") == std::string::npos,
               "compact hud should keep local-network diagnostics behind the explicit panel");
        expect(line.find("Loss") == std::string::npos,
               "compact hud should keep local-network loss diagnostics behind the explicit panel");
    }

    const std::vector<std::string> lines = client.scoreboardLines();
    bool sawBotRow = false;
    bool sawHumanRow = false;
    bool sawAttackerSection = false;
    bool sawDefenderSection = false;
    for (const auto& line : lines) {
        if (line == "Attackers:") {
            sawAttackerSection = true;
        }
        if (line == "Defenders:") {
            sawDefenderSection = true;
        }
        if (line.find("Defense Bot") != std::string::npos &&
            line.find("K/D 3/1") != std::string::npos &&
            line.find("Ping --") != std::string::npos &&
            line.find("Down") != std::string::npos &&
            line.find("K/D/A") == std::string::npos &&
            line.find("Loss") == std::string::npos &&
            line.find("Lat") == std::string::npos) {
            sawBotRow = true;
        }
        if (line.find("score-data") != std::string::npos &&
            line.find("K/D 0/0") != std::string::npos &&
            line.find("Ping 0ms") != std::string::npos &&
            line.find("Alive") != std::string::npos &&
            line.find("Loss") == std::string::npos &&
            line.find("Lat") == std::string::npos &&
            line.find("[P") == std::string::npos) {
            sawHumanRow = true;
        }
    }

    expect(sawAttackerSection && sawDefenderSection,
           "scoreboard should remain grouped into attacker and defender sections");
    expect(sawBotRow,
           "scoreboard rows should keep only compact kills, deaths, ping, and alive-state details");
    expect(sawHumanRow,
           "scoreboard rows should still include the local human player without old diagnostics-heavy fields");
    expect(viewState.scoreboardSections[0].entries.size() == 1u &&
               viewState.scoreboardSections[0].entries[0].isLocalPlayer &&
               viewState.scoreboardSections[1].entries.size() == 1u &&
               viewState.scoreboardSections[1].entries[0].isBot &&
               viewState.scoreboardSections[1].entries[0].rowLabel.find("Ping --") != std::string::npos,
           "client view state should preserve typed human and bot scoreboard entry semantics");
}

void testLabStudyShortcutDispatchesFrozenBotActionAndPublishesRosterUpdate() {
    net::ServerConfig serverConfig;
    serverConfig.studyActionsEnabled = true;
    LoopbackServerHarness server(HarnessMode::AcceptClient, serverConfig);
    expect(server.start(), "server harness should bind loopback for frozen-bot shortcut coverage");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "study-host";
    config.sessionId = 6503u;
    config.preferredTeam = sim::TeamId::Attacker;
    config.studyActionsEnabled = true;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for frozen-bot shortcut coverage");
    pumpUntilConnected(&client, &server);

    server.setAuthoritativePlayerState(static_cast<int>(client.peerId()),
                                       sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 6.0f},
                                       0.0f,
                                       0.0f);

    InputHandler3D::InputState spawnInput;
    spawnInput.spawnFrozenBotAhead = true;
    client.update(1.0f / 60.0f, &spawnInput);

    bool sawSpawnedBot = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        sawSpawnedBot = std::any_of(client.roster().begin(),
                                    client.roster().end(),
                                    [](const sim::RosterEntry& entry) {
                                        return entry.isBot &&
                                               entry.displayName.find("Frozen BOT ") == 0;
                                    });
        if (sawSpawnedBot) {
            break;
        }
    }

    expect(server.observedSessionActionRequests().size() == 1u &&
               server.observedSessionActionRequests().front().kind ==
                   net::SessionActionKind::SpawnFrozenBotAhead,
           "lab-study shortcut presses should dispatch the dedicated session-action request instead of overloading runtime params");
    expect(sawSpawnedBot,
           "successful frozen-bot study actions should surface the new authoritative bot in the client roster");
    const auto spawnedBotIt = std::find_if(client.roster().begin(),
                                           client.roster().end(),
                                           [](const sim::RosterEntry& entry) {
                                               return entry.isBot &&
                                                      entry.displayName.find("Frozen BOT ") == 0;
                                           });
    expect(spawnedBotIt != client.roster().end() &&
               spawnedBotIt->team == sim::TeamId::Defender &&
               spawnedBotIt->alive,
           "spawned frozen study bots should appear as live opposing-team bot roster entries on the client");
    expect(client.lastCombatEventText().find("Spawned frozen bot") != std::string::npos,
           "client feedback should confirm the authoritative frozen-bot study action result");
}

void testFrozenBotShortcutRemainsLocalWhenStudyActionsAreDisabled() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback for study-action gating coverage");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "non-study";
    config.sessionId = 6504u;
    config.studyActionsEnabled = false;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for study-action gating coverage");
    pumpUntilConnected(&client, &server);

    InputHandler3D::InputState spawnInput;
    spawnInput.spawnFrozenBotAhead = true;
    client.update(1.0f / 60.0f, &spawnInput);

    expect(server.observedSessionActionRequests().empty(),
           "non-study runtimes should reject the frozen-bot shortcut locally without sending a session-action packet");
    expect(client.lastCombatEventText() ==
               "Frozen bot spawning is only available in Lab Study",
           "non-study runtimes should explain locally why the frozen-bot shortcut is unavailable");
}

void testFrozenBotShortcutRemainsLocalForJoinClients() {
    net::ServerConfig serverConfig;
    serverConfig.studyActionsEnabled = true;
    LoopbackServerHarness server(HarnessMode::AcceptClient, serverConfig);
    expect(server.start(), "server harness should bind loopback for host-only study-action coverage");
    server.addRemoteClient(6505u, "existing-host");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "study-guest";
    config.sessionId = 6506u;
    config.preferredTeam = sim::TeamId::Defender;
    config.studyActionsEnabled = true;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for host-only study-action coverage");
    pumpUntilConnected(&client, &server);

    expect(client.peerId() != 1u,
           "host-only study-action coverage requires the local client to join as a non-host participant");

    InputHandler3D::InputState spawnInput;
    spawnInput.spawnFrozenBotAhead = true;
    client.update(1.0f / 60.0f, &spawnInput);

    expect(server.observedSessionActionRequests().empty(),
           "join clients should reject the frozen-bot shortcut locally without sending a host-only session-action packet");
    expect(client.lastCombatEventText() == "Only the host can spawn frozen study bots",
           "join clients should explain locally that the frozen-bot shortcut is host-only");
}

void testStudyPresentationTogglesRemainClientLocal() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback for study-presentation toggles");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "study-controls";
    config.sessionId = 6501u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for study-presentation toggle coverage");

    pumpUntilConnected(&client, &server);
    const sim::TeamScores baselineScores = client.teamScores();
    const int baselineActorId = client.localPlayerState().playerId;

    InputHandler3D::InputState dimInput;
    dimInput.toggleFocus = true;
    client.update(1.0f / 60.0f, &dimInput);
    client::ClientViewState viewState = client.clientViewState();
    expect(viewState.studyPresentation.environmentDimmed &&
               absoluteDifference(viewState.studyPresentation.environmentDimFactor, 1.0f) < 0.001f,
           "F1 should toggle client-local environment dimming through structured study presentation state");

    InputHandler3D::InputState areasInput;
    areasInput.toggleAreas = true;
    client.update(1.0f / 60.0f, &areasInput);
    viewState = client.clientViewState();
    expect(!viewState.studyPresentation.areasVisible,
           "F2 should hide marked areas without requiring a server-authoritative state change");

    InputHandler3D::InputState greenFilterInput;
    greenFilterInput.toggleAreaFilterGreen = true;
    client.update(1.0f / 60.0f, &greenFilterInput);
    viewState = client.clientViewState();
    expect(viewState.studyPresentation.areaFilter == client::AreaFilterView::GreenOnly,
           "F3 should switch the client-local area filter to green-only");

    InputHandler3D::InputState redFilterInput;
    redFilterInput.toggleAreaFilterRed = true;
    client.update(1.0f / 60.0f, &redFilterInput);
    client.update(1.0f / 60.0f, nullptr);
    viewState = client.clientViewState();
    expect(viewState.studyPresentation.areaFilter == client::AreaFilterView::RedOnly &&
               viewState.studyPresentation.environmentDimmed &&
               !viewState.studyPresentation.areasVisible,
           "study-presentation state should persist across later frames after successive local toggles");
    expect(client.teamScores().attackers == baselineScores.attackers &&
               client.teamScores().defenders == baselineScores.defenders &&
               client.localPlayerState().playerId == baselineActorId,
           "client-local study presentation toggles should not mutate authoritative gameplay state");
}

void testKillFeedUsesAuthoritativePlayerKilledEvents() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback for kill-feed coverage");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "kill-feed";
    config.sessionId = 6502u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for kill-feed coverage");

    pumpUntilConnected(&client, &server);

    const std::uint16_t targetPeer = server.addRemoteClient(6602u, "target-player");
    server.addAuthoritativeRosterEntry(static_cast<int>(client.peerId()),
                                       sim::TeamId::Attacker,
                                       false,
                                       0u,
                                       0u,
                                       true,
                                       "kill-feed");
    server.addAuthoritativeRosterEntry(static_cast<int>(targetPeer),
                                       sim::TeamId::Defender,
                                       false,
                                       0u,
                                       0u,
                                       true,
                                       "target-player");
    server.setAuthoritativePlayerState(static_cast<int>(client.peerId()),
                                       sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f},
                                       0.0f,
                                       0.0f);
    server.setAuthoritativePlayerState(static_cast<int>(targetPeer),
                                       sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f},
                                       0.0f,
                                       0.0f);
    server.setAuthoritativePlayerHealth(static_cast<int>(targetPeer), Config::SHOOT_DAMAGE);

    for (int attempt = 0; attempt < 12; ++attempt) {
        pumpLoopbackFrame(&client, &server);
    }

    InputHandler3D::InputState fireInput;
    fireInput.firePressed = true;
    client.update(1.0f / 60.0f, &fireInput);

    bool sawKillFeed = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (!client.clientViewState().killFeed.empty()) {
            sawKillFeed = true;
            break;
        }
    }

    expect(sawKillFeed, "authoritative player kills should surface through the client kill feed");
    const client::ClientViewState killFeedViewState = client.clientViewState();
    const client::KillFeedEntryView& entry = killFeedViewState.killFeed.front();
    expect(entry.attackerLabel == "kill-feed" &&
               entry.victimLabel == "target-player" &&
               entry.attackerTeam == sim::TeamId::Attacker &&
               entry.victimTeam == sim::TeamId::Defender &&
               entry.attackerIsLocalPlayer &&
               !entry.victimIsLocalPlayer,
           "kill feed rows should come from authoritative PlayerKilled events with resolved labels and team identity");
    expect(client.lastCombatEventText().find("You eliminated") != std::string::npos,
           "the latest combat event text should reflect the resolved local kill result");

    for (int attempt = 0; attempt < 320; ++attempt) {
        pumpLoopbackFrame(&client, &server);
    }
    expect(client.clientViewState().killFeed.empty(),
           "kill-feed entries should expire after a bounded recent-history window");
}

void testControlBindingContractsKeepParticipantOwnershipSeparateFromPaneFollowTargets() {
    net::ControlBindingContracts contracts;
    contracts.participantState.presence = sim::SessionPresence::Connected;
    contracts.participantState.team = sim::TeamId::Attacker;
    contracts.participantState.participation = sim::ParticipationState::Playing;
    contracts.participantState.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};
    contracts.paneView.slot = sim::PaneSlot::Right;
    contracts.paneView.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    contracts.paneView.focused = false;
    contracts.paneView.followTargetActorId = 22;
    contracts.eligibleActors.controlTargets.push_back(
        sim::EligibleActor{7, sim::TeamId::Attacker, false, true, "local-human"});
    contracts.eligibleActors.spectatorTargets.push_back(
        sim::EligibleActor{22, sim::TeamId::Defender, true, true, "bot-target"});

    expect(contracts.participantState.control.controlsActor() &&
               contracts.participantState.control.actorId == 7,
           "control-binding contracts should preserve explicit participant ownership");
    expect(contracts.paneView.mode == sim::PaneViewMode::SpectatorFollowThirdPerson &&
               contracts.paneView.followTargetActorId == 22,
           "control-binding contracts should preserve explicit pane follow targets separately from actor ownership");
    expect(contracts.participantState.control.actorId != contracts.paneView.followTargetActorId,
           "control-binding contracts should keep controlled actors distinct from pane observation targets");
    expect(containsEligibleActor(contracts.eligibleActors.controlTargets, 7) &&
               !containsEligibleActor(contracts.eligibleActors.controlTargets, 22) &&
               containsEligibleActor(contracts.eligibleActors.spectatorTargets, 22),
           "eligible-actor contracts should keep local-control and spectator-target sets separate");
}

void testClientRuntimeEligibleActorContractsRejectRemoteHumanControlTargets() {
    LoopbackServerHarness server(HarnessMode::AcceptClient);
    expect(server.start(), "server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "contract-check";
    config.sessionId = 6501u;
    config.preferredTeam = sim::TeamId::Attacker;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start");

    pumpUntilConnected(&client, &server);

    const std::uint16_t remoteHumanPeer = server.addRemoteClient(6502u, "remote-human");
    const int botActorId = 101;

    server.addAuthoritativeRosterEntry(static_cast<int>(remoteHumanPeer),
                                       sim::TeamId::Defender,
                                       false,
                                       1u,
                                       0u,
                                       true,
                                       "remote-human");
    server.addAuthoritativeRosterEntry(botActorId,
                                       sim::TeamId::Defender,
                                       true,
                                       2u,
                                       1u,
                                       true,
                                       "defender-bot");

    for (int attempt = 0; attempt < 30; ++attempt) {
        pumpLoopbackFrame(&client, &server);
        if (client.roster().size() >= 3u) {
            break;
        }
    }

    const net::ControlBindingContracts contracts = client.controlBindingContracts();

    expect(contracts.participantState.control.controlsActor() &&
               contracts.participantState.control.actorId == static_cast<int>(client.peerId()) &&
               contracts.paneView.mode == sim::PaneViewMode::PlayerControlled,
           "client runtime should expose participant control ownership and pane view state through one explicit contract");
    expect(containsEligibleActor(contracts.eligibleActors.controlTargets,
                                 contracts.participantState.control.actorId),
           "eligible control actors should include the locally controlled authoritative actor");
    expect(containsEligibleActor(contracts.eligibleActors.controlTargets, botActorId),
           "eligible control actors should include authoritative bots for local switching in study-safe runtimes");
    expect(!containsEligibleActor(contracts.eligibleActors.controlTargets,
                                  static_cast<int>(remoteHumanPeer)),
           "eligible control actors should reject remote-human multiplayer ownership");
    expect(containsEligibleActor(contracts.eligibleActors.spectatorTargets, botActorId) &&
               containsEligibleActor(contracts.eligibleActors.spectatorTargets,
                                     static_cast<int>(remoteHumanPeer)),
           "spectator target contracts should remain distinct from local-control eligibility");
}

void testClientRuntimePresentationTextUsesSharedTypographyService() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string source = readTextFile(repoRoot / "src/ClientRuntime.cpp");

    expect(source.find("TypographyService") != std::string::npos,
           "client runtime should depend on the shared typography service for HUD and waiting text");
    expect(source.find("drawRuntimeText(") != std::string::npos,
           "client runtime should route HUD and waiting presentation through the shared runtime typography helper");
    expect(countOccurrences(source, "DrawText(") == 0u,
           "client runtime presentation should no longer use raw DrawText calls for migrated HUD and overlay text");
    expect(countOccurrences(source, "MeasureText(") == 0u,
           "client runtime presentation should no longer use raw MeasureText calls for migrated HUD and overlay text");
}

void testHostScoreboardAdminControlsReleaseCursorAndUseAuthoritativeRequests() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string source = readTextFile(repoRoot / "src/ClientRuntime.cpp");
    const std::string header = readTextFile(repoRoot / "include/net/ClientRuntime.hpp");

    expect(source.find("renderHostScoreboardAdminPanel()") != std::string::npos,
           "host scoreboard controls should render from the in-game scoreboard path");
    expect(source.find("scoreboardCursorActive_ = true") != std::string::npos &&
               source.find("setUiMode(true)") != std::string::npos,
           "host scoreboard controls should temporarily release the mouse cursor while Tab is held");
    expect(source.find("runtimeParamKeyForTarget(targetPeerId, \"admin_team\")") != std::string::npos &&
               source.find("runtimeParamKeyForTarget(targetPeerId, \"admin_kick\")") != std::string::npos,
           "host scoreboard actions should use authoritative runtime-param control requests");
    expect(source.find("runtimeParamScopeForTargetId(targetPeerId)") != std::string::npos,
           "host scoreboard team assignment should route human and bot targets through the matching scope");
    expect(source.find("\"sv.admin_add_bot\"") != std::string::npos &&
               source.find("HostScoreboardActionKind::AddBot") != std::string::npos,
           "host scoreboard controls should expose an add-bot action");
    expect(source.find("\"sv.bots_active\"") != std::string::npos &&
               source.find("HostScoreboardActionKind::ToggleBots") != std::string::npos &&
               header.find("sendHostToggleBots") != std::string::npos,
           "host scoreboard controls should expose a bot play/pause action backed by the bot director");
    expect(source.find("\"sv.bots_can_shoot\"") != std::string::npos &&
               source.find("HostScoreboardActionKind::ToggleBotPeace") != std::string::npos &&
               header.find("sendHostToggleBotPeace") != std::string::npos,
           "host scoreboard controls should expose a Peace-mode toggle backed by the bot shooting policy");
    expect(source.find("drawHostScoreboardPistolIcon") != std::string::npos &&
               source.find("DrawLineEx(start, end") != std::string::npos,
           "host scoreboard bot shooting toggle should render as a weapon icon with a crossed-out disabled state");
    expect(header.find("HostScoreboardActionKind") != std::string::npos &&
               header.find("scoreboardCursorActive_") != std::string::npos,
           "client runtime should keep explicit state for host-only scoreboard action handling");
}

}  // namespace

int main() {
    try {
        const std::filesystem::path dataRoot =
            makeUniqueTempDirectory("netcodesim-clientruntime-data-root");
        ScopedDirectoryCleanup dataRootCleanup(dataRoot);
        ScopedEnvVar dataRootEnv("NETCODESIM_DATA_ROOT", dataRoot.string());

        testClientAutoAssignsUniqueSessionIdsPerStart();
        testClientJoinsUsingConfiguredAddressAndPort();
        testClientLearnsAuthoritativeLevelIdentityFromWelcome();
        testClientExposesAuthoritativeHostedSessionMetadataAfterConnect();
        testClientHostedSessionSummaryUsesAuthoritativeMetadata();
        testHostToggleEnemyAIHotkeyEmitsSessionBotControlRequest();
        testClientRejectsInvalidAuthoritativeLevelSlot();
        testClientRejectsAuthoritativeLevelHashMismatch();
        testClientRejectsIncompatibleProtocolVersion();
        testClientConsumesAuthoritativeSnapshotsWithoutFakeNetworkPath();
        testClientKeepsRemotePlayersSeparateFromRemoteEnemies();
        testServerPublishedControlGhostsExposeGhostPlayersWithoutScoreboardDuplication();
        testClientStoresAuthoritativeRosterAndTeamTotals();
        testClientAppliesHostEditedParticipantRuntimeAndSessionSettingsFromSnapshots();
        testGuestRuntimeOverlayOnlyShowsEditableSettings();
        testHostRuntimeOverlayShowsAllParticipantSettings();
        testTickRateFeedbackReportsStagedThenLiveCadence();
        testSnapshotRateFeedbackReportsStagedThenLiveCadence();
        testConnectedClientIgnoresConflictingWelcomePeerReassignment();
        testConnectedIdleClientSendsKeepaliveCommands();
        testJoinClientTogglesUiModeWithoutHostDiagnostics();
        testJoinClientTogglesReplayOverlay();
        testFovConeToggleIsClientLocalPresentationState();
        testJoinClientReplayHotkeysDriveSharedReplayState();
        testReplayPlaybackSupportsDetachedSpectatorCameraAndCheckpoints();
        testGameplayConfirmReleasesMouseWithoutOpeningRuntimeSettings();
        testHostDiagnosticsUsesSameUiModeToggle();
        testHostDiagnosticsDefaultsToAssignedPeerAfterConnect();
        testUiModeKeepsClientConnectedAndSnapshotsAdvancing();
        testConnectedClientOpensLocalNetworkPanelAndAdjustsSettingsWithoutDisconnect();
        testConnectedClientTargetsBotRuntimeParamsThroughSharedControlPath();
        testHostDiagnosticsRemainDistinctFromLocalNetworkPanel();
        testConnectedClientTogglesInterpolationWithoutDisconnect();
        testConnectedClientTogglesPredictionWithoutDisconnect();
        testJoinedClientPredictionDisabledSuppressesImmediateLocalFeedback();
        testClientSyncRuntimeKeepsHostAndJoinPredictionSemanticsAligned();
        testLocalTogglesPreserveExplicitDisconnectHandling();
        testLocalTogglesPreserveTimeoutHandling();
        testScoreboardFollowsHeldTabInput();
        testChangeTeamOpensExplicitMenuInsteadOfImmediateSwitch();
        testChangeTeamConfirmationRequestsSelectedTeamAndRespawnText();
        testChangeTeamCancelLeavesTeamUnchangedAndRestoresNormalPlayPresentation();
        testChangeTeamMenuCanRequestSpectatorMode();
        testJoiningAsSessionSpectatorStartsInFreeFlyMode();
        testToggleSpectatorUsesDetachedFreeCameraWithoutLeavingTheMatch();
        testRecordingCheckpointDefaultUsesOneSecondTransitionAndPersists();
        testCompactHudAndScoreboardReflectLatestAuthoritativeSnapshot();
        testLabStudyShortcutDispatchesFrozenBotActionAndPublishesRosterUpdate();
        testFrozenBotShortcutRemainsLocalWhenStudyActionsAreDisabled();
        testFrozenBotShortcutRemainsLocalForJoinClients();
        testStudyPresentationTogglesRemainClientLocal();
        testKillFeedUsesAuthoritativePlayerKilledEvents();
        testControlBindingContractsKeepParticipantOwnershipSeparateFromPaneFollowTargets();
        testClientRuntimeEligibleActorContractsRejectRemoteHumanControlTargets();
        testClientRuntimePresentationTextUsesSharedTypographyService();
        testHostScoreboardAdminControlsReleaseCursorAndUseAuthoritativeRequests();
        std::cout << "ClientRuntimeJoinTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ClientRuntimeJoinTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
