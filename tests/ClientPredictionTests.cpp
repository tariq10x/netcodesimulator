#include "Config3D.hpp"
#include "client/ClientSyncRuntime.hpp"
#include "net/ClientRuntime.hpp"
#include "net/PredictionBuffer.hpp"
#include "net/ServerRuntime.hpp"
#include "sim/SimulationRules.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

float absoluteDifference(float lhs, float rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

struct ClientSyncFixture {
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

    ClientSyncFixture() {
        localPlayerState.playerId = 7;
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

net::Packet receivePacket(net::UdpSocket& socket) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        net::ReceivedDatagram datagram;
        const net::ReceiveStatus status = socket.receive(&datagram);
        if (status == net::ReceiveStatus::Error) {
            throw std::runtime_error("loopback socket receive failed");
        }
        if (status == net::ReceiveStatus::WouldBlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const net::ParseResult parseResult = net::deserializePacket(datagram.payload);
        if (!parseResult.ok) {
            throw std::runtime_error("failed to parse loopback packet");
        }
        return parseResult.packet;
    }

    throw std::runtime_error("timed out waiting for loopback packet");
}

class DelayedServerHarness {
public:
    explicit DelayedServerHarness(std::uint64_t outboundDelayUs)
        : outboundDelayUs_(outboundDelayUs) {}

    bool start() {
        return socket_.bind(net::UdpEndpoint{"127.0.0.1", 0});
    }

    std::uint16_t port() const {
        return socket_.localPort();
    }

    void step(float dtSeconds) {
        nowUs_ += dtSeconds > 0.0f
            ? static_cast<std::uint64_t>(dtSeconds * 1'000'000.0f)
            : 0u;
        receiveIncoming();
        if (acceptedPeerId_ != 0u) {
            server_.tickOnce(nowUs_);
            queueServerPackets();
        }
        flushScheduledPackets();
    }

private:
    struct ScheduledPacket {
        std::uint64_t deliverAtUs{0u};
        net::Packet packet{};
    };

    void receiveIncoming() {
        net::ReceivedDatagram datagram;
        while (true) {
            const net::ReceiveStatus status = socket_.receive(&datagram);
            if (status == net::ReceiveStatus::WouldBlock) {
                return;
            }
            if (status == net::ReceiveStatus::Error) {
                throw std::runtime_error("delayed server socket receive failed");
            }

            clientEndpoint_ = datagram.sender;
            hasClientEndpoint_ = true;

            const net::ParseResult parseResult = net::deserializePacket(datagram.payload);
            if (!parseResult.ok) {
                continue;
            }

            switch (parseResult.packet.header.kind) {
                case net::PacketKind::Hello: {
                    if (acceptedPeerId_ != 0u) {
                        break;
                    }

                    const auto& hello = std::get<net::HelloMessage>(parseResult.packet.payload);
                    net::WelcomeMessage welcome;
                    expect(server_.acceptClient(hello, nowUs_, &welcome, nullptr),
                           "delayed harness should accept the client");
                    acceptedPeerId_ = welcome.assignedPeerId;

                    net::Packet packet;
                    packet.header.peerId = acceptedPeerId_;
                    packet.header.channel = net::Channel::Control;
                    packet.header.seq = nextControlSeq_++;
                    packet.header.kind = net::PacketKind::Welcome;
                    packet.payload = welcome;
                    queuePacket(packet);
                    queueServerPackets();
                    break;
                }

                case net::PacketKind::CommandBundle: {
                    const auto& commands = std::get<net::CommandBundle>(parseResult.packet.payload);
                    expect(server_.enqueueCommandBundle(parseResult.packet.header.peerId, commands, nowUs_),
                           "delayed harness should accept command packets");
                    break;
                }

                default:
                    break;
            }
        }
    }

    void queueServerPackets() {
        const auto packets = server_.takePendingPackets();
        for (const auto& packet : packets) {
            queuePacket(packet);
        }
    }

    void queuePacket(const net::Packet& packet) {
        scheduledPackets_.push_back(ScheduledPacket{nowUs_ + outboundDelayUs_, packet});
    }

    void flushScheduledPackets() {
        if (!hasClientEndpoint_) {
            return;
        }

        auto it = scheduledPackets_.begin();
        while (it != scheduledPackets_.end()) {
            if (it->deliverAtUs > nowUs_) {
                ++it;
                continue;
            }

            expect(socket_.sendTo(clientEndpoint_, net::serializePacket(it->packet)),
                   "delayed harness should forward queued packets");
            it = scheduledPackets_.erase(it);
        }
    }

    std::uint64_t outboundDelayUs_{0u};
    net::UdpSocket socket_{};
    net::ServerRuntime server_{};
    net::UdpEndpoint clientEndpoint_{};
    bool hasClientEndpoint_{false};
    std::uint64_t nowUs_{1'000'000u};
    std::uint32_t nextControlSeq_{1u};
    std::uint16_t acceptedPeerId_{0u};
    std::vector<ScheduledPacket> scheduledPackets_{};
};

void testReplayOfPendingCommandsMatchesExpectedAuthoritativeCorrection() {
    net::PredictionBuffer buffer;
    sim::MovementEnvironment environment;
    sim::PlayerState start;
    start.position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    buffer.reset(start);

    sim::PlayerCommand first;
    first.seq = 1u;
    first.dtSeconds = 0.1f;
    first.yaw = 0.35f;
    first.pitch = -0.2f;
    first.buttons = sim::commandButtonBit(sim::CommandButton::Jump) |
                    sim::commandButtonBit(sim::CommandButton::Fire);

    sim::PlayerCommand second;
    second.seq = 2u;
    second.dtSeconds = 0.1f;
    second.moveY = 1.0f;
    second.yaw = 0.8f;
    second.pitch = 0.15f;

    buffer.recordCommand(first, environment);
    buffer.recordCommand(second, environment);

    sim::PlayerState authoritative = sim::applyPlayerCommand(start, first, environment);
    authoritative.position.z += 0.2f;
    const auto result = buffer.reconcile(authoritative, 1u, environment);

    const sim::PlayerState expected = sim::applyPlayerCommand(authoritative, second, environment);
    expect(result.mode != net::ReconciliationMode::None,
           "reconciliation should detect a non-zero correction after replay");
    expect(result.replayedCommandCount == 1u,
           "reconciliation should report the count of unacknowledged commands replayed after acknowledgement");
    expect(absoluteDifference(buffer.predictedState().position.x, expected.position.x) < 0.0001f,
           "replaying pending commands should rebuild the expected corrected x position");
    expect(absoluteDifference(buffer.predictedState().position.y, expected.position.y) < 0.0001f,
           "replaying pending commands should rebuild the expected corrected y position");
    expect(absoluteDifference(buffer.predictedState().position.z, expected.position.z) < 0.0001f,
           "replaying pending commands should rebuild the expected predicted position");
    expect(absoluteDifference(buffer.predictedState().yaw, expected.yaw) < 0.0001f &&
               absoluteDifference(buffer.predictedState().pitch, expected.pitch) < 0.0001f,
           "replaying pending commands should rebuild corrected aim state alongside position");
    expect(absoluteDifference(buffer.predictedState().weaponCooldownRemaining,
                              expected.weaponCooldownRemaining) < 0.0001f &&
               buffer.predictedState().jumpsUsed == expected.jumpsUsed &&
               buffer.predictedState().grounded == expected.grounded,
           "replaying pending commands should rebuild jump and weapon-cooldown state alongside movement");
    expect(buffer.pendingCommandCount() == 1u,
           "acked commands should be dropped from the pending prediction buffer");
}

void testPredictionBuffersTrackPendingCommandsPerParticipant() {
    net::PredictionBufferMap buffers;
    sim::MovementEnvironment environment;

    sim::PlayerState firstStart;
    firstStart.playerId = 11;
    firstStart.position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    sim::PlayerState secondStart = firstStart;
    secondStart.playerId = 12;
    secondStart.position.x = 3.0f;

    buffers.resetParticipant(11u, firstStart);
    buffers.resetParticipant(12u, secondStart);

    sim::PlayerCommand firstCommand;
    firstCommand.seq = 1u;
    firstCommand.dtSeconds = 0.1f;
    firstCommand.moveY = 1.0f;

    sim::PlayerCommand secondCommand = firstCommand;
    secondCommand.seq = 2u;

    sim::PlayerCommand otherParticipantCommand = firstCommand;
    otherParticipantCommand.seq = 7u;
    otherParticipantCommand.moveX = 1.0f;

    buffers.recordCommand(11u, firstCommand, environment);
    buffers.recordCommand(11u, secondCommand, environment);
    buffers.recordCommand(12u, otherParticipantCommand, environment);

    expect(buffers.pendingCommandCount(11u) == 2u &&
               buffers.pendingCommandCount(12u) == 1u,
           "prediction buffer ownership should keep sequenced pending commands isolated per local participant");

    sim::PlayerState firstAuthoritative = sim::applyPlayerCommand(firstStart, firstCommand, environment);
    firstAuthoritative.position.z += 0.2f;
    buffers.reconcile(11u, firstAuthoritative, 1u, environment);
    expect(buffers.pendingCommandCount(11u) == 1u &&
               buffers.pendingCommandCount(12u) == 1u,
           "acked commands should be dropped promptly from only the reconciled participant buffer");
    expect(buffers.lastAckedInputSeq(11u) == 1u &&
               buffers.lastAckedInputSeq(12u) == 0u,
           "acknowledgement tracking should stay scoped to the participant that was reconciled");
    expect(buffers.correctionHistory(11u).size() == 1u &&
               buffers.correctionHistory(12u).empty(),
           "reconciliation history should only be recorded for the participant whose pending queue was replayed");
}

void testGameplayInputFramePreservesCommandShaping() {
    InputHandler3D::InputState input;
    input.moveInput = Vector2{0.5f, -1.0f};
    input.jumpPressed = true;
    input.firePressed = true;
    input.togglePrediction = true;
    input.toggleUIMode = true;

    const InputHandler3D::InputFrame frame = InputHandler3D::toInputFrame(input);
    expect(frame.moveX == 0.5f && frame.moveY == -1.0f,
           "InputFrame should preserve raw gameplay movement axes");
    expect(frame.jumpPressed && frame.firePressed,
           "InputFrame should preserve gameplay button edges");

    const InputHandler3D::InputFrame predictionFrame =
        InputHandler3D::toPredictionFrame(input, true);
    expect(predictionFrame.moveX == frame.moveX &&
               predictionFrame.moveY == frame.moveY &&
               predictionFrame.jumpPressed == frame.jumpPressed &&
               predictionFrame.firePressed == frame.firePressed,
           "prediction-enabled shaping should preserve gameplay axes and buttons");

    const InputHandler3D::InputFrame disabledPredictionFrame =
        InputHandler3D::toPredictionFrame(input, false);
    expect(disabledPredictionFrame.moveX == 0.0f &&
               disabledPredictionFrame.moveY == 0.0f &&
               !disabledPredictionFrame.jumpPressed &&
               !disabledPredictionFrame.firePressed,
           "prediction-disabled shaping should zero immediate gameplay prediction inputs");

    const sim::PlayerCommand command = InputHandler3D::toPlayerCommand(
        frame,
        1.0f / 60.0f,
        9u,
        1.5f,
        -0.25f,
        1234u,
        50u);
    expect(command.moveX == 0.5f && command.moveY == -1.0f,
           "PlayerCommand shaping should consume the gameplay-only InputFrame axes");
    expect(command.has(sim::CommandButton::Jump) && command.has(sim::CommandButton::Fire),
           "PlayerCommand shaping should preserve gameplay buttons through the InputFrame seam");
    expect(command.interpDelayMs == 50u && command.viewedServerTimeUs == 1234u,
           "PlayerCommand shaping should preserve timing metadata after the InputFrame split");
}

void testClientSyncRuntimeBuildsAndDispatchesCommandsAndControlPackets() {
    client::ClientSyncRuntime syncRuntime;
    ClientSyncFixture fixture;
    fixture.snapshotRateHz = 0u;
    fixture.latestSnapshot.serverTimeUs = 9'999u;

    InputHandler3D::InputState input;
    input.moveInput = Vector2{0.5f, -1.0f};
    input.jumpPressed = true;
    input.firePressed = true;

    const sim::PlayerCommand command = syncRuntime.buildCommand(fixture.context(),
                                                                input,
                                                                1.0f / 60.0f,
                                                                12u,
                                                                1.5f,
                                                                -0.25f,
                                                                sim::TeamId::Defender,
                                                                42u,
                                                                7u);
    expect(command.moveX == 0.5f && command.moveY == -1.0f,
           "ClientSyncRuntime should preserve gameplay axes when shaping commands");
    expect(command.has(sim::CommandButton::Jump) && command.has(sim::CommandButton::Fire),
           "ClientSyncRuntime should preserve gameplay buttons when shaping commands");
    expect(command.viewedServerTimeUs == 9'999u && command.interpDelayMs == 100u,
           "ClientSyncRuntime should source viewed authority time and interpolation delay from sync context");
    expect(command.requestedTeam == sim::TeamId::Defender &&
               command.reportedLatencyMs == 42u &&
               command.reportedLossPct == 7u,
           "ClientSyncRuntime should carry team and transport diagnostics onto gameplay commands");

    sim::MovementEnvironment environment;
    sim::SimConfig simConfig;
    const sim::PlayerState authoritativeState = fixture.latestSnapshot.localPlayerState;
    fixture.predictionEnabled = false;
    syncRuntime.applyLocalPrediction(fixture.context(), command, environment, simConfig);
    expect(fixture.predictionBuffer.pendingCommandCount() == 0u,
           "ClientSyncRuntime should not enqueue local prediction commands when prediction is disabled");
    expect(fixture.localPlayerState.position.x == authoritativeState.position.x &&
               fixture.localPlayerState.position.y == authoritativeState.position.y &&
               fixture.localPlayerState.position.z == authoritativeState.position.z,
           "ClientSyncRuntime should keep the authoritative local player state unchanged when prediction is disabled");
    expect(!syncRuntime.shouldPredictFireAttempt(fixture.context(), command, authoritativeState),
           "ClientSyncRuntime should suppress immediate fire prediction when prediction is disabled");

    fixture.predictionEnabled = true;
    syncRuntime.applyLocalPrediction(fixture.context(), command, environment, simConfig);
    const sim::PlayerState predictedState =
        sim::applyPlayerCommand(authoritativeState, command, environment, simConfig);
    expect(fixture.predictionBuffer.pendingCommandCount() == 1u,
           "ClientSyncRuntime should enqueue pending local commands when prediction is enabled");
    expect(absoluteDifference(fixture.localPlayerState.position.x, predictedState.position.x) < 0.0001f &&
               absoluteDifference(fixture.localPlayerState.position.y, predictedState.position.y) < 0.0001f &&
               absoluteDifference(fixture.localPlayerState.position.z, predictedState.position.z) < 0.0001f,
           "ClientSyncRuntime should apply the predicted local player state when prediction is enabled");
    expect(syncRuntime.shouldPredictFireAttempt(fixture.context(), command, authoritativeState),
           "ClientSyncRuntime should preserve immediate fire prediction when prediction is enabled");
    sim::PlayerState cooldownState = authoritativeState;
    cooldownState.weaponCooldownRemaining = 0.25f;
    expect(!syncRuntime.shouldPredictFireAttempt(fixture.context(), command, cooldownState),
           "ClientSyncRuntime should suppress immediate fire prediction while the weapon is cooling down");

    net::UdpSocket sender;
    net::UdpSocket receiver;
    expect(sender.bind(net::UdpEndpoint{"127.0.0.1", 0}),
           "command sender socket should bind loopback");
    expect(receiver.bind(net::UdpEndpoint{"127.0.0.1", 0}),
           "command receiver socket should bind loopback");
    const net::UdpEndpoint receiverEndpoint{"127.0.0.1", receiver.localPort()};

    expect(syncRuntime.sendControlPayload(fixture.context(),
                                          net::kProtocolVersion,
                                          net::RuntimeParamChangeRequest{
                                              net::RuntimeParamScope::Player,
                                              7,
                                              "net.player[7].latency_ms",
                                              18.0f
                                          },
                                          sender,
                                          receiverEndpoint),
           "ClientSyncRuntime should dispatch control packets once the peer is assigned");
    const net::Packet controlPacket = receivePacket(receiver);
    expect(controlPacket.header.kind == net::PacketKind::RuntimeParamChangeRequest,
           "control dispatch should preserve the runtime-parameter packet kind");
    expect(controlPacket.header.peerId == fixture.peerId &&
               controlPacket.header.ack == fixture.lastSnapshotPacketSeq,
           "control dispatch should preserve peer identity and latest snapshot acknowledgement");

    expect(syncRuntime.sendCommand(fixture.context(),
                                   net::kProtocolVersion,
                                   command,
                                   sender,
                                   receiverEndpoint),
           "ClientSyncRuntime should dispatch gameplay command packets");
    const net::Packet commandPacket = receivePacket(receiver);
    expect(commandPacket.header.kind == net::PacketKind::CommandBundle,
           "gameplay dispatch should preserve the command bundle packet kind");
    const auto& bundle = std::get<net::CommandBundle>(commandPacket.payload);
    expect(bundle.commands.size() == 1u && bundle.commands.front().seq == 12u,
           "gameplay dispatch should preserve the shaped command payload");
    expect(fixture.lastCommandSendUs == fixture.clockUs,
           "successful gameplay dispatch should stamp the last command send time");
}

void testSmallCorrectionsSmoothAndLargeCorrectionsSnap() {
    net::PredictionBuffer buffer;
    sim::MovementEnvironment environment;
    sim::PlayerState start;
    start.position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    buffer.reset(start);

    sim::PlayerCommand command;
    command.seq = 1u;
    command.dtSeconds = 0.1f;
    command.moveY = 1.0f;
    command.yaw = 0.4f;
    command.pitch = -0.1f;
    buffer.recordCommand(command, environment);

    const sim::PlayerState preCorrectionDisplay = buffer.displayState();
    sim::PlayerState smallCorrection = buffer.predictedState();
    smallCorrection.position.z += 0.1f;
    smallCorrection.yaw -= 0.1f;
    smallCorrection.pitch += 0.05f;
    const auto smoothResult = buffer.reconcile(smallCorrection,
                                               1u,
                                               environment,
                                               {},
                                               net::ReconciliationStrategy::Smooth,
                                               250u);
    expect(smoothResult.mode == net::ReconciliationMode::Smooth,
           "small prediction errors should smooth instead of snapping");
    expect(smoothResult.replayedCommandCount == 0u,
           "smooth reconciliation should report zero replayed commands after all pending inputs are acknowledged");
    expect(absoluteDifference(buffer.displayState().position.z, buffer.predictedState().position.z) > 0.0001f,
           "smoothed display position should lag behind the exact replayed prediction");
    buffer.advanceDisplay(0.125f);
    expect(absoluteDifference(buffer.displayState().position.z,
                              (preCorrectionDisplay.position.z + smallCorrection.position.z) * 0.5f) < 0.01f,
           "smooth reconciliation should reach the midpoint of the fixed 250 ms window after 125 ms");
    expect(absoluteDifference(buffer.displayState().yaw,
                              (preCorrectionDisplay.yaw + smallCorrection.yaw) * 0.5f) < 0.01f,
           "smooth reconciliation should blend aim state over the same fixed 250 ms window");
    buffer.advanceDisplay(0.125f);
    expect(absoluteDifference(buffer.displayState().position.z, buffer.predictedState().position.z) < 0.0001f,
           "smooth reconciliation should converge to the corrected state after the full 250 ms window");
    expect(absoluteDifference(buffer.displayState().yaw, buffer.predictedState().yaw) < 0.0001f,
           "smooth reconciliation should converge corrected aim state after the full 250 ms window");

    buffer.reset(start);
    buffer.recordCommand(command, environment);
    sim::PlayerState largeCorrection = start;
    largeCorrection.position.z = 8.0f;
    const auto snapResult = buffer.reconcile(largeCorrection,
                                             1u,
                                             environment,
                                             {},
                                             net::ReconciliationStrategy::Smooth,
                                             250u);
    expect(snapResult.mode == net::ReconciliationMode::Snap,
           "large prediction errors should snap to the replayed state");
    expect(buffer.displayState().position.z == buffer.predictedState().position.z,
           "snap corrections should match the replayed prediction exactly");
}

void testMultipleLocalParticipantsKeepIndependentCorrectionHistories() {
    net::PredictionBufferMap buffers;
    sim::MovementEnvironment environment;

    sim::PlayerState leftStart;
    leftStart.playerId = 21;
    leftStart.position = sim::Vec3{-2.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    sim::PlayerState rightStart = leftStart;
    rightStart.playerId = 22;
    rightStart.position.x = 2.0f;

    buffers.resetParticipant(21u, leftStart);
    buffers.resetParticipant(22u, rightStart);

    sim::PlayerCommand leftCommand;
    leftCommand.seq = 3u;
    leftCommand.dtSeconds = 0.1f;
    leftCommand.moveY = 1.0f;

    sim::PlayerCommand rightCommand = leftCommand;
    rightCommand.seq = 4u;
    rightCommand.moveX = 1.0f;

    buffers.recordCommand(21u, leftCommand, environment);
    buffers.recordCommand(22u, rightCommand, environment);

    sim::PlayerState leftCorrection = leftStart;
    leftCorrection.position.z = 4.55f;
    sim::PlayerState rightCorrection = rightStart;
    rightCorrection.position.x = 4.5f;

    buffers.reconcile(21u, leftCorrection, 3u, environment);
    expect(buffers.correctionHistory(21u).size() == 1u &&
               buffers.correctionHistory(22u).empty(),
           "reconciling one local participant should not append correction history for another participant");
    expect(buffers.pendingCommandCount(21u) == 0u &&
               buffers.pendingCommandCount(22u) == 1u,
           "replaying one participant's acknowledged commands should leave the other participant's pending queue untouched");

    buffers.reconcile(22u, rightCorrection, 4u, environment);
    expect(buffers.correctionHistory(21u).size() == 1u &&
               buffers.correctionHistory(22u).size() == 1u,
           "correction history should be recorded independently for each local participant buffer");
    expect(buffers.lastAckedInputSeq(21u) == 3u &&
               buffers.lastAckedInputSeq(22u) == 4u,
           "acknowledgement tracking should stay scoped to each local participant after both buffers reconcile");
}

void testPredictionStaysImmediateUnderLatencyAndLaterConverges() {
    DelayedServerHarness server(250'000u);
    expect(server.start(), "delayed server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "predictor";
    config.sessionId = 4004u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for prediction test");

    for (int attempt = 0; attempt < 360; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, nullptr);
        if (client.state() == net::ClientConnectionState::Connected && client.hasSnapshot()) {
            break;
        }
    }

    expect(client.state() == net::ClientConnectionState::Connected && client.hasSnapshot(),
           "client should connect before the latency prediction test begins");

    const float authoritativeStartZ = client.latestSnapshot()->localPlayerState.position.z;
    InputHandler3D::InputState input;
    input.moveInput.y = 1.0f;

    client.update(1.0f / 60.0f, &input);
    expect(client.localPlayerState().position.z < authoritativeStartZ,
           "local movement should advance immediately from prediction before the delayed snapshot arrives");

    for (int attempt = 0; attempt < 90; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, nullptr);
        if (client.lastAckedInputSeq() > 0u) {
            break;
        }
    }

    expect(client.lastAckedInputSeq() > 0u,
           "delayed authoritative snapshots should eventually acknowledge the predicted command");
    expect(absoluteDifference(client.localPlayerState().position.z,
                              client.latestSnapshot()->localPlayerState.position.z) < 0.5f,
           "predicted and authoritative positions should converge after reconciliation");
}

void testPredictionToggleSwitchesBetweenPredictedAndAuthoritativeOnlyMovement() {
    DelayedServerHarness server(250'000u);
    expect(server.start(), "delayed server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "predict-toggle";
    config.sessionId = 4005u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for prediction toggle test");

    for (int attempt = 0; attempt < 180; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, nullptr);
        if (client.state() == net::ClientConnectionState::Connected && client.hasSnapshot()) {
            break;
        }
    }

    expect(client.state() == net::ClientConnectionState::Connected,
           "client should connect before the prediction toggle test begins");
    expect(client.predictionEnabled(),
           "multiplayer prediction should start enabled");

    const float authoritativeStartZ = client.latestSnapshot()->localPlayerState.position.z;
    InputHandler3D::InputState moveInput;
    moveInput.moveInput.y = 1.0f;
    client.update(1.0f / 60.0f, &moveInput);
    expect(client.localPlayerState().position.z < authoritativeStartZ,
           "enabled prediction should move the local player immediately before authoritative correction arrives");

    InputHandler3D::InputState toggleInput;
    toggleInput.togglePrediction = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.predictionEnabled(),
           "prediction toggle should disable local movement prediction");

    const float authoritativeAfterToggle = client.latestSnapshot()->localPlayerState.position.z;
    client.update(1.0f / 60.0f, &moveInput);
    expect(absoluteDifference(client.localPlayerState().position.z, authoritativeAfterToggle) < 0.0001f,
           "disabled prediction should keep local movement on the latest authoritative state until new snapshots arrive");

    for (int attempt = 0; attempt < 90; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, nullptr);
        if (client.lastAckedInputSeq() > 0u) {
            break;
        }
    }

    expect(client.state() == net::ClientConnectionState::Connected,
           "toggling prediction should not disconnect the client");
}

void testPredictionDisabledSuppressesImmediateLocalPredictionFeedback() {
    DelayedServerHarness server(250'000u);
    expect(server.start(), "delayed server harness should bind loopback");

    net::ClientConfig config;
    config.serverHost = "127.0.0.1";
    config.serverPort = server.port();
    config.playerName = "predict-fire";
    config.sessionId = 4006u;
    config.serverSilenceTimeoutUs = 10'000'000u;

    net::ClientRuntime client(config);
    expect(client.start(), "client should start for immediate-fire feedback test");

    for (int attempt = 0; attempt < 180; ++attempt) {
        server.step(1.0f / 60.0f);
        client.update(1.0f / 60.0f, nullptr);
        if (client.state() == net::ClientConnectionState::Connected && client.hasSnapshot()) {
            break;
        }
    }

    expect(client.state() == net::ClientConnectionState::Connected,
           "client should connect before disabling prediction for fire feedback");

    InputHandler3D::InputState toggleInput;
    toggleInput.togglePrediction = true;
    client.update(1.0f / 60.0f, &toggleInput);
    expect(!client.predictionEnabled(),
           "prediction should be disabled before testing immediate local fire feedback");

    const std::size_t baselineTraceCount = client.combatTraceCount();
    const sim::PlayerState baselineState = client.localPlayerState();
    const sim::Vec3 baselineLookDirection = client.clientViewState().camera.lookDirection;
    const std::string baselineCombatEventText = client.lastCombatEventText();
    InputHandler3D::InputState fireInput;
    fireInput.moveInput.y = 1.0f;
    fireInput.lookDelta = Vector2{50.0f, -35.0f};
    fireInput.jumpPressed = true;
    fireInput.firePressed = true;
    client.update(1.0f / 60.0f, &fireInput);

    expect(client.localPlayerState().position.x == baselineState.position.x &&
               client.localPlayerState().position.y == baselineState.position.y &&
               client.localPlayerState().position.z == baselineState.position.z,
           "disabling prediction should keep movement and jump on the last authoritative local player state");
    expect(absoluteDifference(client.clientViewState().camera.lookDirection.x,
                              baselineLookDirection.x) > 0.0001f ||
               absoluteDifference(client.clientViewState().camera.lookDirection.y,
                                  baselineLookDirection.y) > 0.0001f ||
               absoluteDifference(client.clientViewState().camera.lookDirection.z,
                                  baselineLookDirection.z) > 0.0001f,
           "disabling prediction should still allow immediate local camera turning");
    expect(client.combatTraceCount() == baselineTraceCount,
           "disabling prediction should suppress immediate local combat traces until authoritative fire arrives");
    expect(client.lastCombatEventText() == baselineCombatEventText,
           "disabling prediction should suppress immediate readable local shot feedback");
}

}  // namespace

int main() {
    try {
        testReplayOfPendingCommandsMatchesExpectedAuthoritativeCorrection();
        testPredictionBuffersTrackPendingCommandsPerParticipant();
        testGameplayInputFramePreservesCommandShaping();
        testClientSyncRuntimeBuildsAndDispatchesCommandsAndControlPackets();
        testSmallCorrectionsSmoothAndLargeCorrectionsSnap();
        testMultipleLocalParticipantsKeepIndependentCorrectionHistories();
        testPredictionStaysImmediateUnderLatencyAndLaterConverges();
        testPredictionToggleSwitchesBetweenPredictedAndAuthoritativeOnlyMovement();
        testPredictionDisabledSuppressesImmediateLocalPredictionFeedback();
        std::cout << "ClientPredictionTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ClientPredictionTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
