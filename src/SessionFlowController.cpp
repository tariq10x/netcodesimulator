#include "net/SessionFlowController.hpp"

#include "app/SessionComposer.hpp"
#include "app/UserDataPaths.hpp"
#include "replay/ReplayArchive.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace net {
namespace {

constexpr std::uint64_t kHostedNetworkStepUs = 16'667u;
constexpr std::uint64_t kMaxHostedFrameAdvanceUs = 100'000u;

bool hasComposedRuntime(const ClientRuntime* client,
                        const TransportArtifactAdapter* proxy,
                        const ServerRuntime* hostedServer,
                        const UdpSocket& hostedServerSocket,
                        const UdpSocket& discoverySocket) {
    return client != nullptr || proxy != nullptr || hostedServer != nullptr ||
           hostedServerSocket.isOpen() || discoverySocket.isOpen();
}

std::uint64_t secondsToMicros(float dtSeconds) {
    if (dtSeconds <= 0.0f) {
        return 0u;
    }
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(dtSeconds) * 1'000'000.0));
}

Packet buildDisconnectPacket(std::uint16_t peerId,
                             std::uint16_t protocolVersion,
                             const std::string& reason) {
    Packet packet;
    packet.header.version = protocolVersion;
    packet.header.peerId = peerId;
    packet.header.channel = Channel::Control;
    packet.header.kind = PacketKind::Disconnect;
    packet.payload = DisconnectMessage{1u, reason};
    return packet;
}

Packet buildWelcomePacket(std::uint16_t protocolVersion,
                          const WelcomeMessage& welcome) {
    Packet packet;
    packet.header.version = protocolVersion;
    packet.header.peerId = welcome.assignedPeerId;
    packet.header.channel = Channel::Control;
    packet.header.kind = PacketKind::Welcome;
    packet.payload = welcome;
    return packet;
}

std::string sanitizeFilenamePart(std::string value) {
    if (value.empty()) {
        return "session";
    }
    for (char& ch : value) {
        const bool allowed =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!allowed) {
            ch = '_';
        }
    }
    return value;
}

std::string timestampForFilename(std::uint64_t unixSeconds) {
    const std::time_t timestamp = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &timestamp);
#else
    if (const std::tm* local = std::localtime(&timestamp); local != nullptr) {
        tm = *local;
    }
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return stream.str();
}

std::filesystem::path commandReplayDirectory() {
    std::filesystem::path directory = app::userDataDirectory() / "replays";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return directory;
}

std::filesystem::path uniqueCommandReplayPath(const replay::ReplayDemo& demo) {
    const std::uint64_t created =
        demo.header.recordedAtUnixSeconds != 0u
            ? demo.header.recordedAtUnixSeconds
            : static_cast<std::uint64_t>(
                  std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    const std::string title =
        demo.header.title.empty() ? "command_replay" : demo.header.title;
    const std::string base =
        "command_" + timestampForFilename(created) + "_" + sanitizeFilenamePart(title);
    const std::filesystem::path directory = commandReplayDirectory();
    std::filesystem::path candidate =
        directory / (base + replay::ReplayArchive::kCommandReplayExtension);
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
        candidate = directory / (base + "_" + std::to_string(suffix) +
                                 replay::ReplayArchive::kCommandReplayExtension);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return directory / (base + "_latest" + replay::ReplayArchive::kCommandReplayExtension);
}

bool saveHostedCommandReplay(const ServerRuntime* server,
                             std::filesystem::path* savedPathOut,
                             std::string* errorOut) {
    if (server == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = "Command replay export is only available on the host";
        }
        return false;
    }

    const replay::ReplayDemo& demo = server->commandReplayDemo();
    if (demo.commandEvents.empty() && demo.keyframes.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "No command replay available";
        }
        return false;
    }

    const std::filesystem::path path = uniqueCommandReplayPath(demo);
    replay::ReplayArchive archive;
    if (!archive.save(demo, path, errorOut)) {
        return false;
    }
    if (savedPathOut != nullptr) {
        *savedPathOut = path;
    }
    return true;
}

}  // namespace

const char* toString(SessionFlowState state) {
    switch (state) {
        case SessionFlowState::Idle: return "idle";
        case SessionFlowState::StartingServer: return "starting_server";
        case SessionFlowState::StartingProxy: return "starting_proxy";
        case SessionFlowState::StartingClient: return "starting_client";
        case SessionFlowState::Connecting: return "connecting";
        case SessionFlowState::Running: return "running";
        case SessionFlowState::Failed: return "failed";
        case SessionFlowState::Ended: return "ended";
    }
    return "unknown";
}

SessionFlowController::SessionFlowController(SessionLaunchConfig config)
    : config_(std::move(config)) {}

SessionFlowController::SessionFlowController(SessionRuntimeComposition composition)
    : config_(composition.config) {
    adoptComposition(std::move(composition));
}

SessionFlowController::~SessionFlowController() {
    if (state_ != SessionFlowState::Ended) {
        shutdown();
    }
}

bool SessionFlowController::start() {
    const bool hasRuntimeComposition =
        hasComposedRuntime(client_.get(),
                           proxy_.get(),
                           hostedServer_.get(),
                           hostedServerSocket_,
                           discoverySocket_);
    if (!hasRuntimeComposition) {
        shutdown("idle");
    }
    state_ = SessionFlowState::Idle;
    statusMessage_ = "starting session";
    if (!hasRuntimeComposition) {
        startupSequence_.clear();
    }
    clockUs_ = 0u;
    hostedNetworkAccumulatorUs_ = 0u;
    serverAccumulatorUs_ = 0u;

    if (!hasRuntimeComposition) {
        app::SessionComposer composer(config_);
        app::SessionComposer::Result composition = composer.compose();
        if (!composition.ok) {
            fail(composition.error);
            return false;
        }
        adoptComposition(std::move(composition.composition));
    }

    state_ = SessionFlowState::Connecting;
    statusMessage_ = client_ != nullptr ? client_->statusMessage() : "connecting";
    return true;
}

void SessionFlowController::update(float dtSeconds, const InputHandler3D::InputState* input) {
    if (state_ == SessionFlowState::Failed || state_ == SessionFlowState::Ended || client_ == nullptr) {
        return;
    }

    const std::uint64_t dtUs = secondsToMicros(dtSeconds);
    if (config_.mode == SessionLaunchMode::Host) {
        advanceHostedNetworking(dtUs);
        pumpHostedDiscoveryResponder();
    } else if (proxy_ != nullptr) {
        clockUs_ += std::min(dtUs, kMaxHostedFrameAdvanceUs);
        proxy_->tick(clockUs_);
    }

    const bool running = state_ == SessionFlowState::Running;
    bool sessionShouldEnd = false;
    InputHandler3D::InputState filteredInput{};
    const InputHandler3D::InputState* forwardedInput = nullptr;
    if (running && input != nullptr) {
        filteredInput = *input;
        filteredInput.exportRecording = false;
        if (input->quit) {
            if (client_->handleBackAction()) {
                filteredInput.quit = false;
            } else {
                sessionShouldEnd = true;
                filteredInput.quit = false;
            }
        }
        forwardedInput = &filteredInput;
    }

    bool commandReplayToggled = false;
    bool commandReplayRecording = false;
    if (running && input != nullptr && input->toggleRecording) {
        if (hostedServer_ != nullptr) {
            const bool enableCommandRecording = !hostedServer_->commandReplayRecordingEnabled();
            hostedServer_->setCommandReplayRecordingEnabled(enableCommandRecording);
            commandReplayToggled = true;
            commandReplayRecording = enableCommandRecording;
        }
    }

    if (hostedServer_ != nullptr) {
        client_->setCommandReplayStatus(true,
                                        hostedServer_->commandReplayRecordingEnabled(),
                                        hostedServer_->commandReplayDemo().commandEvents.size());
    } else {
        client_->setCommandReplayStatus(false, false, 0u);
    }

    client_->update(dtSeconds, forwardedInput);

    if (config_.mode == SessionLaunchMode::Host) {
        pumpHostedNetworkingAtCurrentTime();
        client_->update(0.0f, nullptr);
    } else if (proxy_ != nullptr) {
        proxy_->tick(clockUs_);
        client_->update(0.0f, nullptr);
    }

    if (hostedServer_ != nullptr) {
        client_->setCommandReplayStatus(true,
                                        hostedServer_->commandReplayRecordingEnabled(),
                                        hostedServer_->commandReplayDemo().commandEvents.size());
    }

    if (commandReplayToggled) {
        client_->setLastCombatEventText(commandReplayRecording
            ? "Recording started"
            : "Recording stopped");
    }
    if (running && input != nullptr && input->exportRecording) {
        if (hostedServer_ != nullptr) {
            std::filesystem::path savedPath;
            std::string error;
            if (saveHostedCommandReplay(hostedServer_.get(), &savedPath, &error)) {
                client_->setLastCombatEventText(
                    "Command replay saved: " + savedPath.filename().string());
            } else {
                client_->setLastCombatEventText(
                    error.empty() ? "Command replay export failed" : error);
            }
        } else {
            client_->setLastCombatEventText("Command replay export is host-only");
        }
    }

    updateStateFromClient(sessionShouldEnd);
}

void SessionFlowController::render() const {
    if (client_ != nullptr) {
        client_->render();
    }
}

void SessionFlowController::shutdown(const std::string& reason) {
    if (client_ != nullptr) {
        client_->shutdown();
    }
    client_.reset();

    if (proxy_ != nullptr) {
        proxy_->stop();
    }
    proxy_.reset();
    discoverySocket_ = UdpSocket{};
    hostedServer_.reset();
    hostedServerSocket_ = UdpSocket{};
    hostedNetworkAccumulatorUs_ = 0u;
    serverAccumulatorUs_ = 0u;

    if (reason != "idle") {
        state_ = SessionFlowState::Ended;
        statusMessage_ = reason;
    }
}

SessionFlowState SessionFlowController::state() const {
    return state_;
}

const std::string& SessionFlowController::statusMessage() const {
    return statusMessage_;
}

const SessionLaunchConfig& SessionFlowController::config() const {
    return config_;
}

HostedSessionMetadata SessionFlowController::hostedSessionMetadata() const {
    return makeHostedSessionMetadata(config_);
}

const std::vector<std::string>& SessionFlowController::startupSequence() const {
    return startupSequence_;
}

bool SessionFlowController::shouldReturnToMainMenu() const {
    return state_ == SessionFlowState::Ended;
}

ProxyStats SessionFlowController::aggregateProxyStats(bool upstream) const {
    if (proxy_ == nullptr) {
        return ProxyStats{};
    }
    return proxy_->aggregateStats(upstream);
}

std::size_t SessionFlowController::hostedSessionCount() const {
    return hostedServer_ != nullptr ? hostedServer_->sessions().size() : 0u;
}

void SessionFlowController::bindPrimaryLocalParticipant(std::uint16_t participantId,
                                                        int actorId,
                                                        const std::string& label) {
    splitScreenController_.bindPrimaryLocalParticipant(participantId, actorId, label);
    syncSplitScreenConfig();
}

void SessionFlowController::requestRightLocalParticipant() {
    splitScreenController_.requestRightLocalParticipant();
    syncSplitScreenConfig();
}

bool SessionFlowController::bindRightLocalParticipant(std::uint16_t participantId,
                                                      int actorId,
                                                      const std::string& label) {
    const bool bound =
        splitScreenController_.bindRightLocalParticipant(participantId, actorId, label);
    syncSplitScreenConfig();
    return bound;
}

void SessionFlowController::bindRightObservation(const client::PaneBinding& binding) {
    splitScreenController_.bindRightObservation(binding);
    syncSplitScreenConfig();
}

void SessionFlowController::setFocusedPane(sim::PaneSlot slot) {
    splitScreenController_.setFocusedSlot(slot);
}

void SessionFlowController::disableSplitScreen() {
    splitScreenController_.disableSplitScreen();
    syncSplitScreenConfig();
}

const app::SplitScreenSessionController& SessionFlowController::splitScreenController() const {
    return splitScreenController_;
}

ClientRuntime* SessionFlowController::clientRuntime() {
    return client_.get();
}

const ClientRuntime* SessionFlowController::clientRuntime() const {
    return client_.get();
}

const ServerRuntime* SessionFlowController::hostedServer() const {
    return hostedServer_.get();
}

void SessionFlowController::adoptComposition(SessionRuntimeComposition composition) {
    config_ = std::move(composition.config);
    startupSequence_ = std::move(composition.startupSequence_);
    discoverySocket_ = std::move(composition.discoverySocket_);
    hostedServerSocket_ = std::move(composition.hostedServerSocket_);
    hostedServer_ = std::move(composition.hostedServer_);
    proxy_ = std::move(composition.proxy_);
    client_ = std::move(composition.client_);
}

void SessionFlowController::fail(const std::string& reason) {
    if (client_ != nullptr) {
        client_->shutdown();
    }
    client_.reset();

    if (proxy_ != nullptr) {
        proxy_->stop();
    }
    proxy_.reset();
    discoverySocket_ = UdpSocket{};
    hostedServer_.reset();
    hostedServerSocket_ = UdpSocket{};
    hostedNetworkAccumulatorUs_ = 0u;
    state_ = SessionFlowState::Failed;
    statusMessage_ = reason.empty() ? "session launch failed" : reason;
}

void SessionFlowController::syncSplitScreenConfig() {
    config_.localParticipantCount = splitScreenController_.activeLocalParticipantCount();
}

void SessionFlowController::advanceHostedNetworking(std::uint64_t dtUs) {
    if (hostedServer_ == nullptr || proxy_ == nullptr) {
        return;
    }

    hostedNetworkAccumulatorUs_ += std::min(dtUs, kMaxHostedFrameAdvanceUs);
    while (hostedNetworkAccumulatorUs_ >= kHostedNetworkStepUs) {
        clockUs_ += kHostedNetworkStepUs;
        proxy_->tick(clockUs_);
        pumpHostedServerIngress();
        tickHostedServer(kHostedNetworkStepUs, clockUs_);
        proxy_->tick(clockUs_);
        hostedNetworkAccumulatorUs_ -= kHostedNetworkStepUs;
    }
}

void SessionFlowController::pumpHostedDiscoveryResponder() {
    if (!discoverySocket_.isOpen() || config_.mode != SessionLaunchMode::Host) {
        return;
    }

    ReceivedDatagram datagram;
    while (true) {
        const ReceiveStatus status = discoverySocket_.receive(&datagram);
        if (status == ReceiveStatus::WouldBlock || status == ReceiveStatus::Error) {
            return;
        }

        const SessionDiscoveryQueryParseResult query =
            deserializeSessionDiscoveryQuery(datagram.payload);
        if (!query.ok) {
            continue;
        }

        const std::string advertisedJoinHost =
            config_.proxyClientListenHost.empty() ? "127.0.0.1" : config_.proxyClientListenHost;
        const SessionAdvertisement advertisement =
            makeSessionAdvertisement(config_,
                                     advertisedJoinHost,
                                     static_cast<std::uint16_t>(hostedSessionCount()),
                                     config_.protocolVersion);
        discoverySocket_.sendTo(datagram.sender,
                                serializeSessionAdvertisement(advertisement));
    }
}

void SessionFlowController::pumpHostedNetworkingAtCurrentTime() {
    if (proxy_ == nullptr || hostedServer_ == nullptr) {
        return;
    }

    proxy_->tick(clockUs_);
    pumpHostedServerIngress();
    proxy_->tick(clockUs_);
}

void SessionFlowController::pumpHostedServerIngress() {
    if (hostedServer_ == nullptr || !hostedServerSocket_.isOpen() || proxy_ == nullptr) {
        return;
    }

    ReceivedDatagram datagram;
    while (true) {
        const ReceiveStatus status = hostedServerSocket_.receive(&datagram);
        if (status == ReceiveStatus::WouldBlock) {
            return;
        }
        if (status == ReceiveStatus::Error) {
            fail(hostedServerSocket_.lastError());
            return;
        }

        const ParseResult parsed = deserializePacket(datagram.payload);
        if (!parsed.ok) {
            continue;
        }

        const Packet& packet = parsed.packet;
        if (packet.header.version != config_.protocolVersion) {
            sendHostedServerPacket(buildDisconnectPacket(packet.header.peerId,
                                                        config_.protocolVersion,
                                                        "unsupported_protocol_version"));
            continue;
        }

        switch (packet.header.kind) {
            case PacketKind::Hello: {
                const auto& hello = std::get<HelloMessage>(packet.payload);
                WelcomeMessage welcome;
                std::string rejectReason;
                if (hostedServer_->acceptClient(hello, clockUs_, &welcome, &rejectReason)) {
                    welcome.sessionMetadata = hostedSessionMetadata();
                    welcome.levelSlot = welcome.sessionMetadata.levelSlot;
                    welcome.levelHash = welcome.sessionMetadata.levelHash;
                    sendHostedServerPacket(buildWelcomePacket(config_.protocolVersion, welcome));
                    flushHostedServerPackets();
                } else {
                    sendHostedServerPacket(buildDisconnectPacket(0u,
                                                                config_.protocolVersion,
                                                                rejectReason.empty() ? "server_reject" : rejectReason));
                }
                break;
            }

            case PacketKind::CommandBundle: {
                hostedServer_->enqueueCommandBundle(packet.header.peerId,
                                                    std::get<CommandBundle>(packet.payload),
                                                    clockUs_);
                break;
            }

            case PacketKind::ControlCommandBundle: {
                hostedServer_->enqueueControlCommandBundle(
                    packet.header.peerId,
                    std::get<ControlCommandBundle>(packet.payload),
                    clockUs_);
                break;
            }

            case PacketKind::Disconnect: {
                hostedServer_->disconnectClient(packet.header.peerId,
                                                std::get<DisconnectMessage>(packet.payload).reason);
                break;
            }

            default:
                if (packet.header.channel == Channel::Control &&
                    hostedServer_->handleControlPayload(packet.header.peerId,
                                                       packet.payload,
                                                       clockUs_)) {
                    flushHostedServerPackets();
                }
                break;
        }
    }
}

void SessionFlowController::flushHostedServerPackets() {
    if (hostedServer_ == nullptr) {
        return;
    }

    const auto packets = hostedServer_->takePendingPackets();
    for (const auto& packet : packets) {
        sendHostedServerPacket(packet);
    }
}

void SessionFlowController::tickHostedServer(std::uint64_t dtUs, std::uint64_t nowUs) {
    if (hostedServer_ == nullptr) {
        return;
    }

    serverAccumulatorUs_ += dtUs;
    const std::uint64_t tickStepUs = serverTickIntervalUs();
    if (tickStepUs == 0u) {
        hostedServer_->tickOnce(nowUs);
        flushHostedServerPackets();
        return;
    }

    while (serverAccumulatorUs_ >= tickStepUs) {
        hostedServer_->tickOnce(nowUs);
        flushHostedServerPackets();
        serverAccumulatorUs_ -= tickStepUs;
    }
}

void SessionFlowController::sendHostedServerPacket(const Packet& packet) {
    if (!hostedServerSocket_.isOpen() || proxy_ == nullptr) {
        return;
    }

    if (!hostedServerSocket_.sendTo({config_.proxyServerListenHost, proxy_->serverListenPort()},
                                    serializePacket(packet))) {
        fail(hostedServerSocket_.lastError());
    }
}

void SessionFlowController::updateStateFromClient(bool sessionShouldEnd) {
    if (client_ == nullptr) {
        return;
    }

    if (sessionShouldEnd) {
        shutdown("session ended");
        return;
    }

    if (client_->state() == ClientConnectionState::Connected &&
        client_->hasAuthoritativeLevelIdentity() &&
        client_->hasSnapshot()) {
        state_ = SessionFlowState::Running;
        statusMessage_ = "session running";
        return;
    }

    if (client_->state() == ClientConnectionState::Rejected ||
        client_->state() == ClientConnectionState::TimedOut) {
        const std::string failure = client_->statusMessage();
        if (state_ == SessionFlowState::Running) {
            shutdown(failure.empty() ? "session ended" : failure);
        } else {
            fail(failure.empty() ? "connection failed" : failure);
        }
        return;
    }

    statusMessage_ = client_->statusMessage();
}

std::uint64_t SessionFlowController::serverTickIntervalUs() const {
    if (hostedServer_ == nullptr) {
        return 0u;
    }

    return hostedServer_->tickIntervalUs();
}

}  // namespace net
