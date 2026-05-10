#include "Config3D.hpp"
#include "client/ClientViewState.hpp"
#include "net/Protocol.hpp"
#include "net/SessionLaunchConfig.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

net::ReplicationSnapshot makeReplicationSnapshotContract() {
    net::ReplicationSnapshot snapshot;
    snapshot.serverTick = 1200;
    snapshot.serverTimeUs = 1712233445000ULL;
    snapshot.ackedInputSeq = 142;
    snapshot.cadence.authoritativeTickHz = 60u;
    snapshot.cadence.snapshotCadenceHz = 20u;
    snapshot.cadence.commandCadenceHz = 60u;
    snapshot.authoritativeTime.serverTick = snapshot.serverTick;
    snapshot.authoritativeTime.serverTimeUs = snapshot.serverTimeUs;
    snapshot.authoritativeTime.viewedServerTimeUs = 1712233444800ULL;
    snapshot.localParticipantState.presence = sim::SessionPresence::Connected;
    snapshot.localParticipantState.team = sim::TeamId::Attacker;
    snapshot.localParticipantState.participation = sim::ParticipationState::Playing;
    snapshot.localParticipantState.control =
        sim::ControlBinding{sim::ControlBindingKind::Actor, 2};
    snapshot.localPaneView.slot = sim::PaneSlot::Left;
    snapshot.localPaneView.mode = sim::PaneViewMode::PlayerControlled;
    snapshot.localPaneView.focused = true;
    snapshot.localPaneView.followTargetActorId = 2;
    snapshot.localPlayerState.playerId = 2;
    snapshot.localPlayerState.position = {5.1f, 1.7f, -2.4f};
    snapshot.localPlayerState.velocity = {0.0f, 0.0f, 0.0f};
    snapshot.localPlayerState.yaw = 0.1f;
    snapshot.localPlayerState.pitch = -0.02f;
    snapshot.localPlayerState.health = 100.0f;
    snapshot.localPlayerState.maxHealth = 100.0f;
    snapshot.localPlayerState.grounded = true;

    sim::PlayerState remotePlayer;
    remotePlayer.playerId = 7;
    remotePlayer.position = {10.2f, 1.7f, -5.0f};
    remotePlayer.velocity = {1.0f, 0.0f, 0.0f};
    remotePlayer.yaw = 1.57f;
    remotePlayer.pitch = 0.0f;
    remotePlayer.health = 95.0f;
    remotePlayer.maxHealth = 100.0f;
    remotePlayer.grounded = true;
    snapshot.remotePlayers.push_back(remotePlayer);
    remotePlayer.position.x += 2.5f;
    remotePlayer.position.z -= 1.0f;
    snapshot.controlRemotePlayers.push_back(remotePlayer);

    sim::RemoteActorState remoteEnemy;
    remoteEnemy.entityId = 5;
    remoteEnemy.position = {4.0f, 0.0f, -9.0f};
    remoteEnemy.velocity = {0.0f, 0.0f, 0.5f};
    remoteEnemy.yaw = 0.0f;
    remoteEnemy.pitch = 0.0f;
    remoteEnemy.health = 100.0f;
    remoteEnemy.radius = Config::ENEMY_RADIUS;
    remoteEnemy.alive = true;
    snapshot.remoteEnemies.push_back(remoteEnemy);

    return snapshot;
}

net::SessionSummary makeSessionSummaryContract(
    net::ShotEvaluationMode shotEvaluationMode = net::ShotEvaluationMode::SeenPosition) {
    net::SessionSummary summary;
    summary.sessionMetadata.sessionLabel = "Player LAN Match";
    summary.sessionMetadata.hostPlayerName = "host-player";
    summary.sessionMetadata.levelSlot = 4;
    summary.sessionMetadata.levelHash = net::makeLevelIdentityHash(4);
    summary.sessionMetadata.publicJoinPort = 41000u;
    summary.sessionMetadata.maxHumanPlayers = 2u;
    summary.sessionMetadata.shotEvaluationMode = shotEvaluationMode;
    summary.sessionMetadata.botsFrozen = true;
    summary.sessionMetadata.botsCanShoot = false;
    summary.sessionMetadata.studyEventLoggingEnabled = true;
    summary.sessionMetadata.studyEventRunId = "protocol-run";

    sim::RosterEntry playerRosterEntry;
    playerRosterEntry.actorId = 2;
    playerRosterEntry.team = sim::TeamId::Attacker;
    playerRosterEntry.sessionPresence = sim::SessionPresence::Connected;
    playerRosterEntry.participation = sim::ParticipationState::Playing;
    playerRosterEntry.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 2};
    playerRosterEntry.isBot = false;
    playerRosterEntry.kills = 1;
    playerRosterEntry.deaths = 0;
    playerRosterEntry.assists = 2;
    playerRosterEntry.alive = true;
    playerRosterEntry.latencyMs = 35u;
    playerRosterEntry.lossPct = 4u;
    playerRosterEntry.displayName = "host-player";
    summary.roster.push_back(playerRosterEntry);

    sim::RosterEntry botRosterEntry;
    botRosterEntry.actorId = 101;
    botRosterEntry.team = sim::TeamId::Defender;
    botRosterEntry.sessionPresence = sim::SessionPresence::Connected;
    botRosterEntry.participation = sim::ParticipationState::Playing;
    botRosterEntry.control = sim::ControlBinding{sim::ControlBindingKind::None, -1};
    botRosterEntry.isBot = true;
    botRosterEntry.kills = 3;
    botRosterEntry.deaths = 2;
    botRosterEntry.assists = 1;
    botRosterEntry.alive = false;
    botRosterEntry.displayName = "Defense Bot";
    summary.roster.push_back(botRosterEntry);

    summary.teamScores.attackers = 4;
    summary.teamScores.defenders = 2;

    return summary;
}

net::GameplayEventBatch makeGameplayEventBatchContract() {
    net::GameplayEventBatch batch;
    net::GameplayEvent event;
    event.kind = net::SnapshotEventKind::ConfirmedHit;
    event.sourcePlayerId = 2;
    event.targetEntityId = 7;
    batch.events.push_back(event);

    net::GameplayEvent killEvent;
    killEvent.kind = net::SnapshotEventKind::PlayerKilled;
    killEvent.sourcePlayerId = 2;
    killEvent.targetEntityId = 7;
    killEvent.origin = {5.1f, 1.7f, -2.4f};
    killEvent.direction = {0.0f, 0.0f, -1.0f};
    killEvent.hit = true;
    batch.events.push_back(killEvent);

    return batch;
}

net::Packet makeSnapshotPacket(
    net::ShotEvaluationMode shotEvaluationMode = net::ShotEvaluationMode::SeenPosition) {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Snapshot;
    packet.header.seq = 9001;
    packet.header.ack = 8970;
    packet.header.ackBits = 0xFFFFFFFFu;
    packet.header.kind = net::PacketKind::WorldSnapshot;

    const net::ReplicationSnapshot replication = makeReplicationSnapshotContract();
    const net::SessionSummary summary = makeSessionSummaryContract(shotEvaluationMode);
    const net::GameplayEventBatch gameplayEvents = makeGameplayEventBatchContract();

    packet.payload = net::WorldSnapshot::fromContracts(replication, summary, gameplayEvents);
    return packet;
}

net::Packet makeWelcomePacket(
    net::ShotEvaluationMode shotEvaluationMode = net::ShotEvaluationMode::SeenPosition) {
    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(4, "host-player", 41000u);
    config.sessionLabel = "Player LAN Match";
    config.shotEvaluationMode = shotEvaluationMode;
    config.studyOptions.enableEventLogging = true;
    config.studyEventRunId = "welcome-run";

    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 44;
    packet.header.kind = net::PacketKind::Welcome;
    net::WelcomeMessage welcome{17u, 2u, 20u, 4, net::makeLevelIdentityHash(4)};
    welcome.cadence.authoritativeTickHz = 60u;
    welcome.cadence.snapshotCadenceHz = welcome.snapshotRateHz;
    welcome.cadence.commandCadenceHz = 60u;
    welcome.participantState.presence = sim::SessionPresence::Connected;
    welcome.participantState.team = sim::TeamId::Attacker;
    welcome.participantState.participation = sim::ParticipationState::Playing;
    welcome.participantState.control =
        sim::ControlBinding{sim::ControlBindingKind::Actor, 2};
    welcome.paneView.slot = sim::PaneSlot::Right;
    welcome.paneView.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    welcome.paneView.focused = false;
    welcome.paneView.followTargetActorId = 22;
    welcome.authoritativeTime.serverTick = 1200u;
    welcome.authoritativeTime.serverTimeUs = 1712233445000ULL;
    welcome.authoritativeTime.viewedServerTimeUs = 1712233444800ULL;
    welcome.sessionMetadata = net::makeHostedSessionMetadata(config);
    packet.payload = welcome;
    return packet;
}

net::Packet makeCommandBundlePacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Command;
    packet.header.seq = 46;
    packet.header.ack = 44;
    packet.header.kind = net::PacketKind::CommandBundle;

    sim::UserCmd userCmd;
    userCmd.dtSeconds = 1.0f / 60.0f;
    userCmd.moveX = -0.5f;
    userCmd.moveY = 1.0f;
    userCmd.yaw = 0.25f;
    userCmd.pitch = -0.1f;
    userCmd.buttons = sim::commandButtonBit(sim::CommandButton::Jump) |
                      sim::commandButtonBit(sim::CommandButton::Fire);

    sim::CommandTiming timing;
    timing.viewedServerTimeUs = 1712233445000ULL;
    timing.interpolationDelayMs = 50u;
    sim::CommandTiming controlTiming;
    controlTiming.viewedServerTimeUs = 1712233449000ULL;
    controlTiming.interpolationDelayMs = 33u;

    net::CommandBundle bundle;
    bundle.commands.push_back(sim::PlayerCommand::fromUserCmd(77u,
                                                              userCmd,
                                                              timing,
                                                              sim::TeamId::Attacker,
                                                              35u,
                                                              4u,
                                                              controlTiming));

    packet.payload = bundle;
    return packet;
}

net::Packet makeControlCommandBundlePacket() {
    net::Packet packet = makeCommandBundlePacket();
    packet.header.kind = net::PacketKind::ControlCommandBundle;
    packet.payload = net::ControlCommandBundle{
        std::get<net::CommandBundle>(makeCommandBundlePacket().payload).commands
    };
    return packet;
}

net::Packet makeHelloPacket() {
    net::Packet packet;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 17;
    packet.header.kind = net::PacketKind::Hello;
    packet.payload = net::HelloMessage{88u, 0u, "joiner", sim::TeamId::Defender};
    return packet;
}

net::Packet makeTeamChangePacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 45;
    packet.header.ack = 44;
    packet.header.kind = net::PacketKind::TeamChangeRequest;
    packet.payload = net::TeamChangeRequest{sim::TeamId::Attacker};
    return packet;
}

net::Packet makeRuntimeParamChangePacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 52;
    packet.header.ack = 45;
    packet.header.kind = net::PacketKind::RuntimeParamChangeRequest;
    packet.payload = net::RuntimeParamChangeRequest{
        net::RuntimeParamScope::Player,
        2,
        "net.player[2].latency_ms",
        85.0f
    };
    return packet;
}

net::Packet makeRuntimeParamSnapshotPacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 53;
    packet.header.ack = 52;
    packet.header.kind = net::PacketKind::RuntimeParamSnapshot;
    packet.payload = net::RuntimeParamSnapshot{
        net::RuntimeParamScope::Session,
        -1,
        "sv.tickrate",
        60.0f
    };
    return packet;
}

net::Packet makeRuntimeParamApplyResultPacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 54;
    packet.header.ack = 53;
    packet.header.kind = net::PacketKind::RuntimeParamApplyResult;
    packet.payload = net::RuntimeParamApplyResult{
        net::RuntimeParamScope::Bot,
        1,
        "net.bot[1].loss_pct",
        10.0f,
        true,
        sim::StagedApplyBoundary::NextSnapshot,
        "applied immediately"
    };
    return packet;
}

net::Packet makeSessionActionRequestPacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 55;
    packet.header.ack = 54;
    packet.header.kind = net::PacketKind::SessionActionRequest;
    packet.payload = net::SessionActionRequest{
        net::SessionActionKind::SpawnFrozenBotAhead
    };
    return packet;
}

net::Packet makeSessionActionResultPacket() {
    net::Packet packet;
    packet.header.peerId = 2;
    packet.header.channel = net::Channel::Control;
    packet.header.seq = 56;
    packet.header.ack = 55;
    packet.header.kind = net::PacketKind::SessionActionResult;
    packet.payload = net::SessionActionResult{
        net::SessionActionKind::SpawnFrozenBotAhead,
        true,
        101,
        "spawned"
    };
    return packet;
}

net::Packet makeProxyControlPacket() {
    net::Packet packet;
    packet.header.peerId = 9;
    packet.header.channel = net::Channel::ProxyControl;
    packet.header.seq = 61;
    packet.header.ack = 54;
    packet.header.kind = net::PacketKind::ProxyControl;
    packet.payload = net::ProxyControl{
        2u,
        true,
        net::ProxyLinkConfig{45.0f, 5.0f, 2.5f, 0.5f, 1.0f, 77u}
    };
    return packet;
}

net::Packet makeProxyStatsPacket() {
    net::Packet packet;
    packet.header.peerId = 9;
    packet.header.channel = net::Channel::ProxyStats;
    packet.header.seq = 62;
    packet.header.ack = 61;
    packet.header.kind = net::PacketKind::ProxyStats;
    packet.payload = net::ProxyStats{120u, 110u, 4u, 2u, 1u, 3u};
    return packet;
}

void testPacketHeaderRoundTrip() {
    net::PacketHeader header;
    header.peerId = 12;
    header.channel = net::Channel::Command;
    header.seq = 42;
    header.ack = 39;
    header.ackBits = 0x12345678u;
    header.kind = net::PacketKind::CommandBundle;

    const net::ByteBuffer bytes = net::serializeHeader(header);
    net::PacketHeader decoded;
    std::size_t consumed = 0;
    const net::ParseError error = net::deserializeHeader(bytes, &decoded, &consumed);

    expect(error == net::ParseError::None, "header should deserialize successfully");
    expect(consumed == bytes.size(), "header decode should consume all bytes");
    expect(header == decoded, "header should round-trip exactly");
}

void testWorldSnapshotRoundTrip() {
    const net::ReplicationSnapshot expectedReplication = makeReplicationSnapshotContract();
    const net::SessionSummary expectedSummary = makeSessionSummaryContract();
    const net::GameplayEventBatch expectedGameplayEvents = makeGameplayEventBatchContract();
    const net::Packet packet = makeSnapshotPacket();
    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "snapshot packet should deserialize");
    expect(decoded.packet == packet, "snapshot packet should round-trip exactly");
    const auto& decodedSnapshot = std::get<net::WorldSnapshot>(decoded.packet.payload);
    expect(decodedSnapshot.replication() == expectedReplication,
           "snapshot packet should preserve the time-sensitive replication contract");
    expect(decodedSnapshot.summary() == expectedSummary,
           "snapshot packet should preserve the slower-changing session summary contract");
    expect(decodedSnapshot.gameplayEvents() == expectedGameplayEvents,
           "snapshot packet should preserve the discrete gameplay-event batch contract");
    expect(decodedSnapshot.remotePlayers.size() == 1u,
           "snapshot packet should preserve remote player entries separately");
    expect(decodedSnapshot.controlRemotePlayers.size() == 1u,
           "snapshot packet should preserve control remote player entries separately");
    expect(decodedSnapshot.remoteEnemies.size() == 1u,
           "snapshot packet should preserve remote enemy entries separately");
    expect(decodedSnapshot.roster.size() == 2u,
           "snapshot packet should preserve roster entries");
    expect(decodedSnapshot.roster.front().team == sim::TeamId::Attacker,
           "snapshot packet should preserve roster team identity");
    expect(decodedSnapshot.roster.back().isBot,
           "snapshot packet should preserve the bot flag");
    expect(decodedSnapshot.roster.back().kills == 3u &&
               decodedSnapshot.roster.back().deaths == 2u,
           "snapshot packet should preserve per-entry kills and deaths");
    expect(decodedSnapshot.roster.front().assists == 2u &&
               decodedSnapshot.roster.front().latencyMs == 35u &&
               decodedSnapshot.roster.front().lossPct == 4u,
           "snapshot packet should preserve assists and per-player network metrics");
    expect(decodedSnapshot.roster.front().displayName == "host-player" &&
               decodedSnapshot.roster.back().displayName == "Defense Bot",
           "snapshot packet should preserve roster display names");
    expect(decodedSnapshot.teamScores.attackers == 4u &&
               decodedSnapshot.teamScores.defenders == 2u,
           "snapshot packet should preserve team score fields");
    expect(decodedSnapshot.cadence.authoritativeTickHz == 60u &&
               decodedSnapshot.cadence.snapshotCadenceHz == 20u &&
               decodedSnapshot.cadence.commandCadenceHz == 60u,
           "snapshot packet should preserve shared cadence vocabulary");
    expect(decodedSnapshot.authoritativeTime.serverTick == 1200u &&
               decodedSnapshot.authoritativeTime.serverTimeUs == 1712233445000ULL &&
               decodedSnapshot.authoritativeTime.viewedServerTimeUs == 1712233444800ULL,
           "snapshot packet should preserve explicit authoritative timing references");
    expect(decodedSnapshot.localParticipantState.team == sim::TeamId::Attacker &&
               decodedSnapshot.localParticipantState.participation == sim::ParticipationState::Playing &&
               decodedSnapshot.localParticipantState.control.actorId == 2,
           "snapshot packet should preserve typed local participant identity and control-binding state");
    expect(decodedSnapshot.localPaneView.slot == sim::PaneSlot::Left &&
               decodedSnapshot.localPaneView.mode == sim::PaneViewMode::PlayerControlled &&
               decodedSnapshot.localPaneView.followTargetActorId == 2,
           "snapshot packet should preserve typed pane-view state without camera inference");
    expect(decodedSnapshot.sessionMetadata.sessionLabel == "Player LAN Match" &&
               decodedSnapshot.sessionMetadata.publicJoinPort == 41000u,
           "snapshot packet should preserve hosted session metadata");
    expect(decodedSnapshot.sessionMetadata.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "snapshot packet should preserve the authoritative shot-evaluation mode");
    expect(decodedSnapshot.sessionMetadata.botsFrozen,
           "snapshot packet should preserve the hosted bot-director freeze state");
    expect(!decodedSnapshot.sessionMetadata.botsCanShoot,
           "snapshot packet should preserve the hosted bot shooting policy");
    expect(decodedSnapshot.sessionMetadata.studyEventLoggingEnabled &&
               decodedSnapshot.sessionMetadata.studyEventRunId == "protocol-run",
           "snapshot packet should preserve hosted study event logging metadata");

    const net::ByteBuffer reserialized = net::serializePacket(decoded.packet);
    expect(bytes == reserialized, "snapshot serialization should be deterministic");
}

void testControlCommandBundleRoundTrip() {
    const net::Packet packet = makeControlCommandBundlePacket();
    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "control command bundle should deserialize");
    expect(decoded.packet == packet, "control command bundle should round-trip exactly");
    expect(decoded.packet.header.kind == net::PacketKind::ControlCommandBundle,
           "control command bundles should preserve their explicit packet kind");
}

void testControlWorldSnapshotRoundTrip() {
    net::Packet packet;
    packet.header.peerId = 1u;
    packet.header.channel = net::Channel::Snapshot;
    packet.header.seq = 9u;
    packet.header.kind = net::PacketKind::ControlWorldSnapshot;
    packet.payload = net::ControlWorldSnapshot{
        std::get<net::WorldSnapshot>(makeSnapshotPacket().payload)
    };

    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "control snapshot packet should deserialize");
    expect(decoded.packet.header.kind == net::PacketKind::ControlWorldSnapshot,
           "control snapshot packets should preserve their explicit packet kind");
    expect(decoded.packet.header.peerId == packet.header.peerId &&
               decoded.packet.header.channel == packet.header.channel &&
               decoded.packet.header.seq == packet.header.seq,
           "control snapshot packets should preserve their routing header fields");
    expect(std::get<net::ControlWorldSnapshot>(decoded.packet.payload) ==
               std::get<net::ControlWorldSnapshot>(packet.payload),
           "control snapshot packets should preserve the wrapped snapshot payload");
}

void testWelcomeRoundTripPreservesLevelIdentity() {
    const net::Packet packet = makeWelcomePacket();
    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "welcome packet should deserialize");
    expect(decoded.packet == packet, "welcome packet should round-trip exactly");

    const auto& welcome = std::get<net::WelcomeMessage>(decoded.packet.payload);
    expect(welcome.levelSlot == 4, "welcome packet should preserve the selected level slot");
    expect(welcome.levelHash == net::makeLevelIdentityHash(4),
           "welcome packet should preserve the selected level hash");
    expect(welcome.sessionMetadata.sessionLabel == "Player LAN Match",
           "welcome packet should preserve the hosted session label");
    expect(welcome.sessionMetadata.hostPlayerName == "host-player",
           "welcome packet should preserve the host player name");
    expect(welcome.sessionMetadata.publicJoinPort == 41000u,
           "welcome packet should preserve the public join port");
    expect(welcome.sessionMetadata.maxHumanPlayers == 2u,
           "welcome packet should preserve the fixed hosted human-player cap");
    expect(welcome.sessionMetadata.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "welcome packet should preserve the authoritative shot-evaluation mode");
    expect(welcome.sessionMetadata.botsFrozen,
           "welcome packet should preserve the hosted bot-director freeze state");
    expect(welcome.sessionMetadata.botsCanShoot,
           "welcome packet should preserve the hosted bot shooting policy");
    expect(welcome.sessionMetadata.studyEventLoggingEnabled &&
               !welcome.sessionMetadata.studyEventRunId.empty(),
           "welcome packet should preserve study event logging metadata");
    expect(welcome.cadence.authoritativeTickHz == 60u &&
               welcome.cadence.snapshotCadenceHz == 20u &&
               welcome.cadence.commandCadenceHz == 60u,
           "welcome packet should preserve the shared cadence vocabulary");
    expect(welcome.participantState.team == sim::TeamId::Attacker &&
               welcome.participantState.participation == sim::ParticipationState::Playing &&
               welcome.participantState.control.actorId == 2,
           "welcome packet should preserve typed participant state");
    expect(welcome.paneView.slot == sim::PaneSlot::Right &&
               welcome.paneView.mode == sim::PaneViewMode::SpectatorFollowThirdPerson &&
               welcome.paneView.followTargetActorId == 22,
           "welcome packet should preserve typed pane-view semantics");
    expect(welcome.authoritativeTime.serverTick == 1200u &&
               welcome.authoritativeTime.serverTimeUs == 1712233445000ULL &&
               welcome.authoritativeTime.viewedServerTimeUs == 1712233444800ULL,
           "welcome packet should preserve authoritative timing references");
}

void testCommandBundleRoundTripPreservesGameplayAndTimingAdapters() {
    const net::Packet packet = makeCommandBundlePacket();
    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "command-bundle packet should deserialize");
    expect(decoded.packet == packet, "command-bundle packet should round-trip exactly");

    const auto& bundle = std::get<net::CommandBundle>(decoded.packet.payload);
    expect(bundle.commands.size() == 1u,
           "command-bundle packets should preserve gameplay command counts");

    const sim::PlayerCommand& command = bundle.commands.front();
    const sim::UserCmd userCmd = command.toUserCmd();
    expect(userCmd.dtSeconds == 1.0f / 60.0f &&
               userCmd.moveX == -0.5f &&
               userCmd.moveY == 1.0f &&
               userCmd.yaw == 0.25f &&
               userCmd.pitch == -0.1f,
           "command-bundle packets should preserve gameplay-only user command fields");
    expect(userCmd.has(sim::CommandButton::Jump) &&
               userCmd.has(sim::CommandButton::Fire),
           "command-bundle packets should preserve gameplay button state through the UserCmd adapter");

    const sim::CommandTiming timing = command.toCommandTiming();
    expect(timing.viewedServerTimeUs == 1712233445000ULL &&
               timing.interpolationDelayMs == 50u,
           "command-bundle packets should preserve authoritative timing semantics");
    const sim::CommandTiming controlTiming = command.toControlTiming();
    expect(controlTiming.viewedServerTimeUs == 1712233449000ULL &&
               controlTiming.interpolationDelayMs == 33u,
           "command-bundle packets should preserve clean-control ghost timing semantics");
    expect(command.requestedTeam == sim::TeamId::Attacker &&
               command.reportedLatencyMs == 35u &&
               command.reportedLossPct == 4u,
           "command-bundle packets should preserve control and transport metrics outside the gameplay-only UserCmd");

    const sim::PlayerCommand rebuilt = sim::PlayerCommand::fromUserCmd(command.seq,
                                                                       userCmd,
                                                                       timing,
                                                                       command.requestedTeam,
                                                                       command.reportedLatencyMs,
                                                                       command.reportedLossPct,
                                                                       controlTiming);
    expect(rebuilt.seq == command.seq &&
               rebuilt.requestedTeam == command.requestedTeam &&
               rebuilt.reportedLatencyMs == command.reportedLatencyMs &&
               rebuilt.reportedLossPct == command.reportedLossPct &&
               rebuilt.toControlTiming().viewedServerTimeUs ==
                   command.toControlTiming().viewedServerTimeUs &&
               rebuilt.toControlTiming().interpolationDelayMs ==
                   command.toControlTiming().interpolationDelayMs &&
               rebuilt.toUserCmd().buttons == command.toUserCmd().buttons,
           "transitional PlayerCommand adapters should reconstruct the current command shape without behavior loss");
}

void testHelloRoundTripPreservesRequestedTeam() {
    const net::Packet packet = makeHelloPacket();
    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "hello packet should deserialize");
    expect(decoded.packet == packet, "hello packet should round-trip exactly");

    const auto& hello = std::get<net::HelloMessage>(decoded.packet.payload);
    expect(hello.requestedTeam == sim::TeamId::Defender,
           "hello packets should preserve the requested join team");
}

void testTeamChangeRequestRoundTrip() {
    const net::Packet packet = makeTeamChangePacket();
    const net::ByteBuffer bytes = net::serializePacket(packet);
    const net::ParseResult decoded = net::deserializePacket(bytes);

    expect(decoded.ok, "team-change packets should deserialize");
    expect(decoded.packet == packet, "team-change packets should round-trip exactly");

    const auto& request = std::get<net::TeamChangeRequest>(decoded.packet.payload);
    expect(request.requestedTeam == sim::TeamId::Attacker,
           "team-change packets should preserve the selected target team");

    const net::ByteBuffer reserialized = net::serializePacket(decoded.packet);
    expect(bytes == reserialized, "team-change serialization should be deterministic");
}

void testRuntimeParamMessagesRoundTripDeterministically() {
    {
        const net::Packet packet = makeRuntimeParamChangePacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "runtime-param change packets should deserialize");
        expect(decoded.packet == packet, "runtime-param change packets should round-trip exactly");
        const auto& request = std::get<net::RuntimeParamChangeRequest>(decoded.packet.payload);
        expect(request.scope == net::RuntimeParamScope::Player && request.targetId == 2,
               "runtime-param change packets should preserve their target scope");
        expect(request.key == "net.player[2].latency_ms" && request.value == 85.0f,
               "runtime-param change packets should preserve key and numeric value");
        expect(bytes == net::serializePacket(decoded.packet),
               "runtime-param change serialization should be deterministic");
    }

    {
        const net::Packet packet = makeRuntimeParamSnapshotPacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "runtime-param snapshot packets should deserialize");
        expect(decoded.packet == packet, "runtime-param snapshot packets should round-trip exactly");
        const auto& snapshot = std::get<net::RuntimeParamSnapshot>(decoded.packet.payload);
        expect(snapshot.scope == net::RuntimeParamScope::Session && snapshot.targetId == -1,
               "runtime-param snapshot packets should preserve their scope identity");
        expect(snapshot.key == "sv.tickrate" && snapshot.value == 60.0f,
               "runtime-param snapshot packets should preserve effective parameter values");
        expect(bytes == net::serializePacket(decoded.packet),
               "runtime-param snapshot serialization should be deterministic");
    }

    {
        const net::Packet packet = makeRuntimeParamApplyResultPacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "runtime-param apply-result packets should deserialize");
        expect(decoded.packet == packet, "runtime-param apply-result packets should round-trip exactly");
        const auto& result = std::get<net::RuntimeParamApplyResult>(decoded.packet.payload);
        expect(result.scope == net::RuntimeParamScope::Bot && result.targetId == 1,
               "runtime-param apply-result packets should preserve their target scope");
        expect(result.key == "net.bot[1].loss_pct" && result.value == 10.0f,
               "runtime-param apply-result packets should preserve key and numeric payload");
        expect(result.applied && result.message == "applied immediately",
               "runtime-param apply-result packets should preserve mutation feedback");
        expect(result.stagedApplyBoundary == sim::StagedApplyBoundary::NextSnapshot,
               "runtime-param apply-result packets should preserve staged-apply timing vocabulary");
        expect(bytes == net::serializePacket(decoded.packet),
               "runtime-param apply-result serialization should be deterministic");
    }

    {
        const net::Packet packet = makeSessionActionRequestPacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "session-action request packets should deserialize");
        expect(decoded.packet == packet, "session-action request packets should round-trip exactly");
        const auto& request = std::get<net::SessionActionRequest>(decoded.packet.payload);
        expect(request.kind == net::SessionActionKind::SpawnFrozenBotAhead,
               "session-action request packets should preserve the explicit action identity");
        expect(bytes == net::serializePacket(decoded.packet),
               "session-action request serialization should be deterministic");
    }

    {
        const net::Packet packet = makeSessionActionResultPacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "session-action result packets should deserialize");
        expect(decoded.packet == packet, "session-action result packets should round-trip exactly");
        const auto& result = std::get<net::SessionActionResult>(decoded.packet.payload);
        expect(result.kind == net::SessionActionKind::SpawnFrozenBotAhead &&
                   result.applied &&
                   result.actorId == 101 &&
                   result.message == "spawned",
               "session-action result packets should preserve action feedback and actor identity");
        expect(bytes == net::serializePacket(decoded.packet),
               "session-action result serialization should be deterministic");
    }
}

void testControlPlanePacketKindsRemainExplicit() {
    expect(net::packetKindForPayload(net::PacketPayload{
               net::TeamChangeRequest{sim::TeamId::Defender}}) == net::PacketKind::TeamChangeRequest,
           "team-change control messages should keep their dedicated packet kind");
    expect(net::packetKindForPayload(net::PacketPayload{
               net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   2,
                   "net.player[2].latency_ms",
                   85.0f
               }}) == net::PacketKind::RuntimeParamChangeRequest,
           "runtime-parameter control messages should keep their dedicated packet kind");
    expect(net::packetKindForPayload(net::PacketPayload{
               net::SessionActionRequest{
                   net::SessionActionKind::SpawnFrozenBotAhead
               }}) == net::PacketKind::SessionActionRequest,
           "session-action control messages should keep their dedicated packet kind");
}

void testClientViewStateExposesTypedParticipantAndPaneContracts() {
    client::ClientViewState viewState;
    viewState.localParticipant.actorId = 2;
    viewState.localParticipant.label = "host-player";
    viewState.localParticipant.state.presence = sim::SessionPresence::Connected;
    viewState.localParticipant.state.team = sim::TeamId::Spectator;
    viewState.localParticipant.state.participation = sim::ParticipationState::Spectating;
    viewState.localParticipant.state.control =
        sim::ControlBinding{sim::ControlBindingKind::Actor, 14};
    viewState.pane.state.slot = sim::PaneSlot::Right;
    viewState.pane.state.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    viewState.pane.state.focused = false;
    viewState.pane.state.followTargetActorId = 22;
    viewState.pane.followTargetLabel = "BOT 22";
    viewState.timing.cadence.authoritativeTickHz = 60u;
    viewState.timing.authoritativeTime.serverTick = 18244u;
    viewState.timing.authoritativeTime.serverTimeUs = 304066666ULL;
    viewState.timing.authoritativeTime.viewedServerTimeUs = 303900000ULL;
    viewState.timing.ackedInputSeq = 142u;

    expect(viewState.localParticipant.state.team == sim::TeamId::Spectator &&
               viewState.localParticipant.state.participation == sim::ParticipationState::Spectating &&
               viewState.localParticipant.state.control.actorId == 14,
           "client-facing typed view state should expose participant identity without reading raw camera state");
    expect(viewState.pane.state.mode == sim::PaneViewMode::SpectatorFollowThirdPerson &&
               viewState.pane.state.followTargetActorId == 22 &&
               viewState.pane.followTargetLabel == "BOT 22",
           "client-facing typed view state should expose pane mode and follow target explicitly");
    expect(viewState.timing.authoritativeTime.serverTick == 18244u &&
               viewState.timing.authoritativeTime.viewedServerTimeUs == 303900000ULL &&
               viewState.timing.ackedInputSeq == 142u,
           "client-facing typed view state should expose authoritative timing and sequencing explicitly");
}

void testProxyPacketsRoundTrip() {
    {
        const net::Packet packet = makeProxyControlPacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "proxy-control packets should deserialize");
        expect(decoded.packet == packet, "proxy-control packets should round-trip exactly");
        const auto& control = std::get<net::ProxyControl>(decoded.packet.payload);
        expect(control.targetPeerId == 2u && control.upstream,
               "proxy-control packets should preserve the targeted link direction");
        expect(control.config.baseDelayMs == 45.0f &&
                   control.config.jitterMs == 5.0f &&
                   control.config.lossPct == 2.5f &&
                   control.config.duplicatePct == 0.5f &&
                   control.config.reorderPct == 1.0f &&
                   control.config.seed == 77u,
               "proxy-control packets should preserve transport artifact settings");
    }

    {
        const net::Packet packet = makeProxyStatsPacket();
        const net::ByteBuffer bytes = net::serializePacket(packet);
        const net::ParseResult decoded = net::deserializePacket(bytes);

        expect(decoded.ok, "proxy-stats packets should deserialize");
        expect(decoded.packet == packet, "proxy-stats packets should round-trip exactly");
        const auto& stats = std::get<net::ProxyStats>(decoded.packet.payload);
        expect(stats.receivedPackets == 120u &&
                   stats.forwardedPackets == 110u &&
                   stats.droppedPackets == 4u &&
                   stats.duplicatedPackets == 2u &&
                   stats.reorderedPackets == 1u &&
                   stats.queuedPackets == 3u,
               "proxy-stats packets should preserve transport statistics");
    }
}

void testInvalidShotEvaluationMetadataIsRejectedDeterministically() {
    const net::Packet packet = makeWelcomePacket();
    net::ByteBuffer bytes = net::serializePacket(packet);
    const auto& welcome = std::get<net::WelcomeMessage>(packet.payload);
    const std::size_t trailingMetadataBytes =
        sizeof(std::uint8_t) +  // botsFrozen
        sizeof(std::uint8_t) +  // botsCanShoot
        sizeof(std::uint8_t) +  // studyEventLoggingEnabled
        sizeof(std::uint16_t) + welcome.sessionMetadata.studyEventRunId.size();
    bytes[bytes.size() - trailingMetadataBytes - sizeof(std::uint8_t)] = 0xFFu;

    const net::ParseResult decoded = net::deserializePacket(bytes);
    expect(!decoded.ok, "unknown shot-evaluation modes should be rejected");
    expect(decoded.error == net::ParseError::InvalidShotEvaluationMode,
           "unknown shot-evaluation modes should surface a deterministic protocol error");
}

void testShotEvaluationModesRoundTripAndRejectInvalidValues() {
    for (const net::ShotEvaluationMode mode :
         {net::ShotEvaluationMode::SeenPosition, net::ShotEvaluationMode::LivePosition}) {
        const net::ParseResult decodedSnapshot = net::deserializePacket(
            net::serializePacket(makeSnapshotPacket(mode)));
        expect(decodedSnapshot.ok, "valid shot-evaluation modes should deserialize in snapshots");
        expect(std::get<net::WorldSnapshot>(decodedSnapshot.packet.payload).sessionMetadata.shotEvaluationMode == mode,
               "snapshot packets should round-trip both authoritative shot-evaluation modes");

        const net::ParseResult decodedWelcome = net::deserializePacket(
            net::serializePacket(makeWelcomePacket(mode)));
        expect(decodedWelcome.ok, "valid shot-evaluation modes should deserialize in welcomes");
        expect(std::get<net::WelcomeMessage>(decodedWelcome.packet.payload).sessionMetadata.shotEvaluationMode == mode,
               "welcome packets should round-trip both authoritative shot-evaluation modes");

        net::ShotEvaluationMode parsedMode;
        expect(net::tryParseShotEvaluationMode(static_cast<std::uint8_t>(mode), &parsedMode) &&
                   parsedMode == mode,
               "the shared protocol parser should accept each supported shot-evaluation enum");
        expect(net::tryParseShotEvaluationModeValue(static_cast<float>(static_cast<std::uint8_t>(mode)),
                                                    &parsedMode) &&
                   parsedMode == mode,
               "the shared protocol parser should accept runtime-param values for each supported shot-evaluation enum");
    }

    net::ShotEvaluationMode ignoredMode;
    expect(!net::tryParseShotEvaluationModeValue(2.0f, &ignoredMode) &&
               !net::tryParseShotEvaluationModeValue(0.5f, &ignoredMode),
           "the shared protocol parser should reject unsupported or non-integral shot-evaluation values");
}

void testSessionTickRateValuesAcceptIntegralBoundsAndRejectInvalidInputs() {
    std::uint16_t tickRateHz = 0u;
    expect(net::tryParseSessionTickRateHzValue(20.0f, &tickRateHz) && tickRateHz == 20u,
           "the shared protocol parser should accept the minimum supported session tick rate");
    expect(net::tryParseSessionTickRateHzValue(120.0f, &tickRateHz) && tickRateHz == 120u,
           "the shared protocol parser should accept representative supported session tick rates");
    expect(net::tryParseSessionTickRateHzValue(240.0f, &tickRateHz) && tickRateHz == 240u,
           "the shared protocol parser should accept the maximum supported session tick rate");
    expect(!net::tryParseSessionTickRateHzValue(19.0f, &tickRateHz) &&
               !net::tryParseSessionTickRateHzValue(241.0f, &tickRateHz) &&
               !net::tryParseSessionTickRateHzValue(60.5f, &tickRateHz),
           "the shared protocol parser should reject out-of-range or non-integral session tick rates");

    std::uint16_t snapshotRateHz = 0u;
    expect(net::tryParseSessionSnapshotRateHzValue(30.0f, &snapshotRateHz) && snapshotRateHz == 30u,
           "the shared protocol parser should accept supported session snapshot rates");
    expect(!net::tryParseSessionSnapshotRateHzValue(19.0f, &snapshotRateHz) &&
               !net::tryParseSessionSnapshotRateHzValue(60.5f, &snapshotRateHz),
           "the shared protocol parser should reject invalid session snapshot rates");
}

void testInvalidHeaderRejection() {
    net::Packet packet = makeSnapshotPacket();
    net::ByteBuffer bytes = net::serializePacket(packet);
    bytes[0] = 0x00;

    const net::ParseResult invalidMagic = net::deserializePacket(bytes);
    expect(!invalidMagic.ok, "invalid magic should be rejected");
    expect(invalidMagic.error == net::ParseError::InvalidMagic, "invalid magic should report the right error");

    bytes = net::serializePacket(packet);
    bytes[4] = 0xFF;
    bytes[5] = 0xFF;

    const net::ParseResult invalidVersion = net::deserializePacket(bytes);
    expect(!invalidVersion.ok, "invalid version should be rejected");
    expect(invalidVersion.error == net::ParseError::UnsupportedVersion, "invalid version should report the right error");
}

void testInvalidSessionActionKindIsRejectedDeterministically() {
    const std::size_t sessionActionPayloadOffset =
        net::serializeHeader(makeSessionActionRequestPacket().header).size();
    net::ByteBuffer requestBytes = net::serializePacket(makeSessionActionRequestPacket());
    requestBytes[sessionActionPayloadOffset] = 0xFFu;

    const net::ParseResult invalidRequest = net::deserializePacket(requestBytes);
    expect(!invalidRequest.ok, "invalid session-action request enums should be rejected");
    expect(invalidRequest.error == net::ParseError::InvalidSessionActionKind,
           "invalid session-action request enums should surface a deterministic protocol error");

    net::ByteBuffer resultBytes = net::serializePacket(makeSessionActionResultPacket());
    resultBytes[sessionActionPayloadOffset] = 0xFFu;

    const net::ParseResult invalidResult = net::deserializePacket(resultBytes);
    expect(!invalidResult.ok, "invalid session-action result enums should be rejected");
    expect(invalidResult.error == net::ParseError::InvalidSessionActionKind,
           "invalid session-action result enums should surface the same deterministic protocol error");
}

void testSequenceAndRetransmitHelpers() {
    expect(net::shouldAcceptSequence(100, 101), "newer sequence should be accepted");
    expect(!net::shouldAcceptSequence(100, 99), "older sequence should be rejected");

    net::ReliableControlState state;
    state.awaitingAck = true;
    state.lastSendTimeMs = 1000;
    state.retransmitTimeoutMs = 250;

    expect(!net::shouldRetransmit(state, 1249), "retransmit should wait until timeout");
    expect(net::shouldRetransmit(state, 1250), "retransmit should trigger at timeout");
}

}  // namespace

int main() {
    try {
        testPacketHeaderRoundTrip();
        testWorldSnapshotRoundTrip();
        testControlWorldSnapshotRoundTrip();
        testWelcomeRoundTripPreservesLevelIdentity();
        testCommandBundleRoundTripPreservesGameplayAndTimingAdapters();
        testControlCommandBundleRoundTrip();
        testHelloRoundTripPreservesRequestedTeam();
        testTeamChangeRequestRoundTrip();
        testRuntimeParamMessagesRoundTripDeterministically();
        testControlPlanePacketKindsRemainExplicit();
        testClientViewStateExposesTypedParticipantAndPaneContracts();
        testProxyPacketsRoundTrip();
        testInvalidShotEvaluationMetadataIsRejectedDeterministically();
        testShotEvaluationModesRoundTripAndRejectInvalidValues();
        testSessionTickRateValuesAcceptIntegralBoundsAndRejectInvalidInputs();
        testInvalidHeaderRejection();
        testInvalidSessionActionKindIsRejectedDeterministically();
        testSequenceAndRetransmitHelpers();
        std::cout << "ProtocolTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ProtocolTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
