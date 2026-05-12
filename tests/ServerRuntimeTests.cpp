#include "Config3D.hpp"
#include "net/ServerRuntime.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "server/ServerGateway.hpp"
#include "sim/SimulationRules.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool nearlyEqual(float lhs, float rhs, float tolerance = 0.0001f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

float planarDistanceSquared(const sim::Vec3& lhs, const sim::Vec3& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dz = lhs.z - rhs.z;
    return (dx * dx) + (dz * dz);
}

float yawFor(const sim::Vec3& origin, const sim::Vec3& target) {
    return std::atan2(target.x - origin.x, origin.z - target.z);
}

void expectVec3Near(const sim::Vec3& actual,
                    const sim::Vec3& expected,
                    const std::string& message) {
    if (!nearlyEqual(actual.x, expected.x) ||
        !nearlyEqual(actual.y, expected.y) ||
        !nearlyEqual(actual.z, expected.z)) {
        throw std::runtime_error(message);
    }
}

const sim::PlayerState& requirePlayer(const sim::WorldState& world, int playerId) {
    const sim::PlayerState* player = sim::findPlayer(world, playerId);
    if (player == nullptr) {
        throw std::runtime_error("player missing from authoritative world");
    }
    return *player;
}

const sim::RosterEntry& requireRosterEntry(const sim::WorldState& world, int actorId) {
    const sim::RosterEntry* entry = sim::findRosterEntry(world, actorId);
    if (entry == nullptr) {
        throw std::runtime_error("roster entry missing from authoritative world");
    }
    return *entry;
}

const sim::PlayerState* findReplicatedPlayer(const std::vector<sim::PlayerState>& players,
                                             int actorId) {
    const auto it = std::find_if(players.begin(),
                                 players.end(),
                                 [actorId](const sim::PlayerState& player) {
                                     return player.playerId == actorId;
                                 });
    return it != players.end() ? &*it : nullptr;
}

const net::WorldSnapshot& requireSnapshotForPeer(const std::vector<net::Packet>& packets,
                                                 std::uint16_t peerId) {
    for (const auto& packet : packets) {
        if (packet.header.kind != net::PacketKind::WorldSnapshot ||
            packet.header.peerId != peerId) {
            continue;
        }
        return std::get<net::WorldSnapshot>(packet.payload);
    }
    throw std::runtime_error("world snapshot missing for peer");
}

const net::WorldSnapshot* findSnapshotForPeer(const std::vector<net::Packet>& packets,
                                              std::uint16_t peerId) {
    for (const auto& packet : packets) {
        if (packet.header.kind != net::PacketKind::WorldSnapshot ||
            packet.header.peerId != peerId) {
            continue;
        }
        return &std::get<net::WorldSnapshot>(packet.payload);
    }
    return nullptr;
}

const net::RuntimeParamApplyResult& requireRuntimeParamApplyResultForPeer(
    const std::vector<net::Packet>& packets,
    std::uint16_t peerId,
    const std::string& key) {
    for (const auto& packet : packets) {
        if (packet.header.kind != net::PacketKind::RuntimeParamApplyResult ||
            packet.header.peerId != peerId) {
            continue;
        }

        const auto& result = std::get<net::RuntimeParamApplyResult>(packet.payload);
        if (result.key == key) {
            return result;
        }
    }
    throw std::runtime_error("runtime-param apply result missing for peer");
}

const net::SessionActionResult& requireSessionActionResultForPeer(
    const std::vector<net::Packet>& packets,
    std::uint16_t peerId,
    net::SessionActionKind kind) {
    for (const auto& packet : packets) {
        if (packet.header.kind != net::PacketKind::SessionActionResult ||
            packet.header.peerId != peerId) {
            continue;
        }

        const auto& result = std::get<net::SessionActionResult>(packet.payload);
        if (result.kind == kind) {
            return result;
        }
    }
    throw std::runtime_error("session-action result missing for peer");
}

const sim::RosterEntry& requireSnapshotRosterEntry(const net::WorldSnapshot& snapshot, int actorId) {
    for (const auto& entry : snapshot.roster) {
        if (entry.actorId == actorId) {
            return entry;
        }
    }
    throw std::runtime_error("roster entry missing from snapshot");
}

int requireBotActorId(const sim::WorldState& world, sim::TeamId team) {
    for (const auto& entry : world.roster) {
        if (entry.isBot && entry.team == team) {
            return entry.actorId;
        }
    }
    throw std::runtime_error("bot roster entry missing from authoritative world");
}

std::size_t countBotRosterEntries(const sim::WorldState& world) {
    return static_cast<std::size_t>(std::count_if(world.roster.begin(),
                                                  world.roster.end(),
                                                  [](const sim::RosterEntry& entry) {
                                                      return entry.isBot;
                                                  }));
}

void testSharedRifleHitscanMatchesSinglePlayerReferenceShape() {
    sim::PlayerState shooter;
    shooter.position = sim::Vec3{2.0f, Config::PLAYER_EYE_HEIGHT, 6.0f};
    shooter.yaw = 0.35f;
    shooter.pitch = -0.2f;

    const sim::HitscanRay ray = sim::buildRifleHitscan(shooter, shooter.yaw, shooter.pitch);

    const sim::Vec3 forward = sim::lookDirection(shooter.yaw, shooter.pitch);
    const sim::Vec3 right = sim::rightFromYaw(shooter.yaw);
    const sim::Vec3 expectedOrigin{
        shooter.position.x + (forward.x * Config::WEAPON_FORWARD_OFFSET) + (right.x * Config::WEAPON_RIGHT_OFFSET),
        shooter.position.y + (forward.y * Config::WEAPON_FORWARD_OFFSET) - Config::WEAPON_DOWN_OFFSET,
        shooter.position.z + (forward.z * Config::WEAPON_FORWARD_OFFSET) + (right.z * Config::WEAPON_RIGHT_OFFSET)
    };
    const sim::Vec3 aimPoint{
        shooter.position.x + (forward.x * Config::SHOOT_RANGE),
        shooter.position.y + (forward.y * Config::SHOOT_RANGE),
        shooter.position.z + (forward.z * Config::SHOOT_RANGE)
    };
    sim::Vec3 expectedDirection{
        aimPoint.x - expectedOrigin.x,
        0.0f,
        aimPoint.z - expectedOrigin.z
    };
    const float directionLength = std::sqrt((expectedDirection.x * expectedDirection.x) +
                                            (expectedDirection.z * expectedDirection.z));
    expectedDirection.x /= directionLength;
    expectedDirection.z /= directionLength;

    expectVec3Near(ray.origin, expectedOrigin,
                   "shared rifle helper should preserve the single-player muzzle offset");
    expectVec3Near(ray.direction, expectedDirection,
                   "shared rifle helper should preserve the single-player horizontal beam direction");
    expect(nearlyEqual(ray.maxDistance, Config::SHOOT_RANGE),
           "shared rifle helper should preserve the configured rifle range");
}

void testServerGatewayOwnsMembershipAndDeterministicPacketPublication() {
    net::ServerConfig config;
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(
        config.maxPlayers, sim::MovementEnvironment{}, simConfig);
    net::server::ServerGateway gateway;
    const net::HostedSessionMetadata hostedMetadata = net::makeHostedSessionMetadata(
        net::makeHostSessionLaunchConfig(-1, "host", config.listenPort));

    net::WelcomeMessage first;
    net::WelcomeMessage second;
    expect(gateway.acceptClient(
               net::HelloMessage{7u, 0u, "alpha"},
               config,
               hostedMetadata,
               simConfig,
               &world,
               0u,
               900'000u,
               &first,
               nullptr),
           "gateway should accept the first authoritative session directly");
    expect(gateway.acceptClient(
               net::HelloMessage{8u, 0u, "beta"},
               config,
               hostedMetadata,
               simConfig,
               &world,
               0u,
               910'000u,
               &second,
               nullptr),
           "gateway should accept the second authoritative session directly");
    expect(gateway.sessions().size() == 2u,
           "gateway should own the accepted authoritative session membership");

    const auto initialPackets = gateway.takePendingPackets();
    expect(initialPackets.size() == 2u,
           "gateway should emit one initial snapshot packet per accepted session");

    net::ClientSession* firstSession = gateway.findSessionMutable(first.assignedPeerId);
    expect(firstSession != nullptr,
           "gateway should expose the authoritative session record for control publication");

    net::RuntimeParamApplyResult result;
    result.scope = net::RuntimeParamScope::Player;
    result.targetId = static_cast<std::int32_t>(first.assignedPeerId);
    result.key = "net.player[1].latency_ms";
    result.value = 33.0f;
    result.applied = true;
    result.stagedApplyBoundary = sim::StagedApplyBoundary::NextTick;
    result.message = "applied";
    gateway.enqueueControlPayload(*firstSession, result);
    gateway.recordSnapshotEvent(
        net::SnapshotEvent{net::SnapshotEventKind::WeaponFired,
                           static_cast<int>(first.assignedPeerId),
                           static_cast<int>(second.assignedPeerId),
                           sim::Vec3{1.0f, 2.0f, 3.0f},
                           sim::Vec3{0.0f, 0.0f, -1.0f},
                           false});
    const std::vector<sim::PlayerState> serverControlPlayers;
    gateway.publishSnapshots(config, hostedMetadata, world, serverControlPlayers, 7u, 920'000u);

    const auto publishedPackets = gateway.takePendingPackets();
    expect(publishedPackets.size() == 3u,
           "gateway should preserve queued control packets alongside later snapshot publication");
    expect(publishedPackets.front().header.kind == net::PacketKind::RuntimeParamApplyResult,
           "gateway should publish earlier control packets before later snapshot packets deterministically");

    const auto& publishedFirstSnapshot = requireSnapshotForPeer(publishedPackets, first.assignedPeerId);
    expect(publishedFirstSnapshot.serverTick == 7u,
           "gateway-published snapshots should preserve the supplied authoritative server tick");
    expect(publishedFirstSnapshot.events.size() == 1u &&
               publishedFirstSnapshot.events.front().kind == net::SnapshotEventKind::WeaponFired,
           "gateway-published snapshots should include the queued replication events");

    gateway.publishSnapshots(config, hostedMetadata, world, serverControlPlayers, 8u, 930'000u);
    const auto followupPackets = gateway.takePendingPackets();
    expect(requireSnapshotForPeer(followupPackets, first.assignedPeerId).events.empty(),
           "gateway should clear published replication events before the next snapshot batch");
}

void testCommandsApplyInSequenceOrder() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{11, 0, "alpha"}, 1'000'000, &welcome, nullptr),
           "server should accept a single client");
    server.takePendingPackets();

    net::CommandBundle commands;
    sim::PlayerCommand second;
    second.seq = 2;
    second.dtSeconds = 0.1f;
    second.moveX = 1.0f;
    second.yaw = 0.0f;
    commands.commands.push_back(second);

    sim::PlayerCommand first;
    first.seq = 1;
    first.dtSeconds = 0.1f;
    first.moveY = 1.0f;
    first.yaw = 0.0f;
    commands.commands.push_back(first);

    expect(server.enqueueCommandBundle(welcome.assignedPeerId, commands, 1'050'000),
           "command bundle should enqueue");
    server.tickOnce(1'100'000);

    sim::PlayerState expected = requirePlayer(server.worldState(), welcome.assignedPeerId);
    sim::PlayerState start = expected;
    start.position = server.worldState().playerSpawns.at(welcome.assignedPeerId - 1);
    start.velocity = sim::Vec3{};
    start.weaponCooldownRemaining = 0.0f;
    start.jumpsUsed = 0;
    start.grounded = true;

    expected = sim::applyPlayerCommand(start, first, server.worldState().environment);
    expected = sim::applyPlayerCommand(expected, second, server.worldState().environment);

    const sim::PlayerState& authoritative = requirePlayer(server.worldState(), welcome.assignedPeerId);
    expect(authoritative.position.x == expected.position.x &&
           authoritative.position.z == expected.position.z,
           "authoritative position should reflect ordered command application");
}

void testDuplicateCommandsIgnored() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{12, 0, "beta"}, 2'000'000, &welcome, nullptr),
           "server should accept the client");
    server.takePendingPackets();

    sim::PlayerCommand command;
    command.seq = 1;
    command.dtSeconds = 0.1f;
    command.moveY = 1.0f;
    command.yaw = 0.0f;

    net::CommandBundle commands;
    commands.commands.push_back(command);
    commands.commands.push_back(command);

    expect(server.enqueueCommandBundle(welcome.assignedPeerId, commands, 2'050'000),
           "duplicate command bundle should still enqueue");
    server.tickOnce(2'100'000);
    server.tickOnce(2'116'666);
    server.tickOnce(2'133'332);

    const sim::PlayerState& authoritative = requirePlayer(server.worldState(), welcome.assignedPeerId);
    const sim::Vec3 spawn = server.worldState().playerSpawns.at(welcome.assignedPeerId - 1);
    expect(authoritative.position.z == spawn.z - (Config::PLAYER_SPEED * 0.1f),
           "duplicate sequence numbers should only move the player once");

    auto packets = server.takePendingPackets();
    expect(!packets.empty(), "server should emit a snapshot after ticking");
    const auto& snapshot = std::get<net::WorldSnapshot>(packets.back().payload);
    expect(snapshot.ackedInputSeq == 1u, "acked input sequence should stop at the first command");
}

void testMalformedAndStaleCommandsAreDroppedBeforeMutation() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{13u, 0u, "validation"}, 2'000'000u, &welcome, nullptr),
           "server should accept the validation client");
    server.takePendingPackets();

    sim::PlayerState* player = sim::findPlayer(&server.worldState(), welcome.assignedPeerId);
    expect(player != nullptr, "validation client should exist in the authoritative world");
    const sim::Vec3 spawn = server.worldState().playerSpawns.at(welcome.assignedPeerId - 1);
    const sim::TeamId baselineTeam = requireRosterEntry(server.worldState(), welcome.assignedPeerId).team;

    sim::PlayerCommand malformed;
    malformed.seq = 1u;
    malformed.dtSeconds = -0.1f;
    malformed.moveY = 1.0f;
    malformed.yaw = 0.0f;
    malformed.buttons = 0x80000000u;
    malformed.requestedTeam = sim::TeamId::Defender;

    net::CommandBundle malformedCommands;
    malformedCommands.commands.push_back(malformed);
    expect(server.enqueueCommandBundle(welcome.assignedPeerId, malformedCommands, 2'005'000u),
           "malformed command bundles should still traverse the authoritative intake path");
    server.tickOnce(2'010'000u);

    expectVec3Near(player->position,
                   spawn,
                   "malformed gameplay commands should be dropped before authoritative movement mutates");
    expect(requireRosterEntry(server.worldState(), welcome.assignedPeerId).team == baselineTeam,
           "malformed gameplay commands should be dropped before legacy team-shadow fields mutate authority");

    server.tickOnce(2'026'667u);
    server.tickOnce(2'043'334u);
    const auto malformedPackets = server.takePendingPackets();
    const auto& malformedSnapshot = requireSnapshotForPeer(malformedPackets, welcome.assignedPeerId);
    expect(malformedSnapshot.ackedInputSeq == 0u,
           "malformed gameplay commands should not advance authoritative acknowledgement");

    sim::PlayerCommand valid;
    valid.seq = 2u;
    valid.dtSeconds = 0.1f;
    valid.moveY = 1.0f;
    valid.yaw = 0.0f;

    net::CommandBundle validCommands;
    validCommands.commands.push_back(valid);
    expect(server.enqueueCommandBundle(welcome.assignedPeerId, validCommands, 2'050'000u),
           "valid commands should enqueue after malformed commands are dropped");
    server.tickOnce(2'060'000u);

    const float positionAfterValidCommand = player->position.z;
    expect(positionAfterValidCommand < spawn.z,
           "valid commands should still mutate the authoritative world after malformed drops");

    sim::PlayerCommand stale = valid;
    stale.seq = 1u;
    net::CommandBundle staleCommands;
    staleCommands.commands.push_back(stale);
    expect(server.enqueueCommandBundle(welcome.assignedPeerId, staleCommands, 2'070'000u),
           "stale command bundles should still reach the server intake path");
    server.tickOnce(2'076'667u);

    expect(nearlyEqual(player->position.z, positionAfterValidCommand),
           "stale gameplay commands should be dropped before any second authoritative movement mutation");

    server.tickOnce(2'093'334u);
    server.tickOnce(2'110'001u);
    const auto stalePackets = server.takePendingPackets();
    const auto& staleSnapshot = requireSnapshotForPeer(stalePackets, welcome.assignedPeerId);
    expect(staleSnapshot.ackedInputSeq == 2u,
           "stale gameplay commands should leave the last valid authoritative acknowledgement unchanged");
}

void testTwoClientsAcceptedAndInitialSnapshotsEmitted() {
    net::ServerRuntime server;

    net::WelcomeMessage first;
    net::WelcomeMessage second;
    expect(server.acceptClient(net::HelloMessage{21, 0, "one"}, 3'000'000, &first, nullptr),
           "first client should be accepted");
    expect(server.acceptClient(net::HelloMessage{22, 0, "two"}, 3'010'000, &second, nullptr),
           "second client should be accepted");

    expect(first.assignedPeerId == 1u, "first client should get peer id 1");
    expect(second.assignedPeerId == 2u, "second client should get peer id 2");

    const auto packets = server.takePendingPackets();
    expect(packets.size() == 2u, "initial snapshots should be emitted for both clients");

    const auto& firstSnapshot = std::get<net::WorldSnapshot>(packets[0].payload);
    const auto& secondSnapshot = std::get<net::WorldSnapshot>(packets[1].payload);
    expect(firstSnapshot.localPlayerState.playerId == 1, "first snapshot should target peer 1");
    expect(secondSnapshot.localPlayerState.playerId == 2, "second snapshot should target peer 2");
    expect(secondSnapshot.remotePlayers.size() == 1u,
           "second snapshot should include the other player in the dedicated remote-player list");
    expect(!secondSnapshot.remoteEnemies.empty(),
           "second snapshot should include enemies in the dedicated remote-enemy list");
}

void testAuthoritativeTeamAssignmentsRoundTripThroughSnapshots() {
    net::ServerRuntime server;

    net::WelcomeMessage first;
    net::WelcomeMessage second;
    expect(server.acceptClient(net::HelloMessage{24u, 0u, "host"}, 3'100'000u, &first, nullptr),
           "first client should be accepted for roster assignment");
    expect(server.acceptClient(net::HelloMessage{25u, 0u, "joiner"}, 3'110'000u, &second, nullptr),
           "second client should be accepted for roster assignment");

    server.takePendingPackets();
    server.tickOnce(3'126'667u);
    server.tickOnce(3'143'334u);
    server.tickOnce(3'160'001u);

    const auto packets = server.takePendingPackets();
    const auto& firstSnapshot = requireSnapshotForPeer(packets, first.assignedPeerId);
    const auto& secondSnapshot = requireSnapshotForPeer(packets, second.assignedPeerId);

    expect(requireRosterEntry(server.worldState(), first.assignedPeerId).team == sim::TeamId::Attacker,
           "the first connected player should become an attacker by default");
    expect(requireRosterEntry(server.worldState(), second.assignedPeerId).team == sim::TeamId::Defender,
           "the second connected player should become a defender by default");
    expect(firstSnapshot.roster.size() == 2u && secondSnapshot.roster.size() == 2u,
           "both peer-specific snapshots should include the authoritative two-player roster");
    expect(firstSnapshot.teamScores.attackers == 0u && firstSnapshot.teamScores.defenders == 0u,
           "initial roster snapshots should expose zeroed team totals");
    expect(requireSnapshotRosterEntry(firstSnapshot, first.assignedPeerId).team == sim::TeamId::Attacker,
           "host snapshots should preserve the first player's attacker assignment");
    expect(requireSnapshotRosterEntry(firstSnapshot, second.assignedPeerId).team == sim::TeamId::Defender,
           "host snapshots should preserve the second player's defender assignment");
    expect(requireSnapshotRosterEntry(secondSnapshot, first.assignedPeerId).team == sim::TeamId::Attacker,
           "join snapshots should preserve the host attacker assignment");
    expect(requireSnapshotRosterEntry(secondSnapshot, second.assignedPeerId).team == sim::TeamId::Defender,
           "join snapshots should preserve the joiner defender assignment");
}

void testExplicitTeamRequestsOverrideBalancedAssignment() {
    net::ServerRuntime server;

    net::WelcomeMessage first;
    net::WelcomeMessage second;
    expect(server.acceptClient(net::HelloMessage{124u, 0u, "host", sim::TeamId::Defender},
                               3'170'000u,
                               &first,
                               nullptr),
           "first client should be accepted with an explicit defender request");
    expect(server.acceptClient(net::HelloMessage{125u, 0u, "joiner", sim::TeamId::Attacker},
                               3'180'000u,
                               &second,
                               nullptr),
           "second client should be accepted with an explicit attacker request");

    expect(requireRosterEntry(server.worldState(), first.assignedPeerId).team == sim::TeamId::Defender,
           "explicit defender requests should override the balanced default assignment");
    expect(requireRosterEntry(server.worldState(), second.assignedPeerId).team == sim::TeamId::Attacker,
           "explicit attacker requests should override the balanced default assignment");
}

void testAdditionalParticipantsFavorSmallerTeamByDefault() {
    net::ServerConfig config;
    config.maxPlayers = 4u;
    net::ServerRuntime server(config);

    net::WelcomeMessage first;
    net::WelcomeMessage second;
    net::WelcomeMessage third;
    net::WelcomeMessage fourth;
    expect(server.acceptClient(net::HelloMessage{26u, 0u, "one"}, 3'200'000u, &first, nullptr),
           "first client should connect");
    expect(server.acceptClient(net::HelloMessage{27u, 0u, "two"}, 3'210'000u, &second, nullptr),
           "second client should connect");
    expect(server.acceptClient(net::HelloMessage{28u, 0u, "three"}, 3'220'000u, &third, nullptr),
           "third client should connect");
    expect(server.acceptClient(net::HelloMessage{29u, 0u, "four"}, 3'230'000u, &fourth, nullptr),
           "fourth client should connect");

    expect(requireRosterEntry(server.worldState(), first.assignedPeerId).team == sim::TeamId::Attacker,
           "the first participant should be an attacker");
    expect(requireRosterEntry(server.worldState(), second.assignedPeerId).team == sim::TeamId::Defender,
           "the second participant should balance to defenders");
    expect(requireRosterEntry(server.worldState(), third.assignedPeerId).team == sim::TeamId::Attacker,
           "the third participant should return to attackers on a tie");
    expect(requireRosterEntry(server.worldState(), fourth.assignedPeerId).team == sim::TeamId::Defender,
           "the fourth participant should balance defenders again");

    std::size_t attackerCount = 0u;
    std::size_t defenderCount = 0u;
    for (const auto& entry : server.worldState().roster) {
        if (entry.team == sim::TeamId::Attacker) {
            ++attackerCount;
        } else if (entry.team == sim::TeamId::Defender) {
            ++defenderCount;
        }
    }
    expect(attackerCount == 2u && defenderCount == 2u,
           "additional participants should be assigned to the smaller team by default");
}

void testConfiguredBotsSpawnIntoAuthoritativeRosterWithDistinctActorIds() {
    net::ServerConfig config;
    config.maxPlayers = 2u;
    config.attackerBotCount = 2u;
    config.defenderBotCount = 1u;
    net::ServerRuntime server(config);

    expect(server.worldState().roster.size() == 3u,
           "configured bots should populate the authoritative roster before any humans connect");

    std::size_t attackerBots = 0u;
    std::size_t defenderBots = 0u;
    std::vector<sim::Vec3> botPositions;
    for (const auto& entry : server.worldState().roster) {
        expect(entry.isBot, "configured bot roster entries should be marked as bots");
        expect(entry.actorId > static_cast<int>(config.maxPlayers),
               "configured bot actor ids should stay outside the human peer id range");
        const sim::PlayerState* bot = sim::findPlayer(server.worldState(), entry.actorId);
        expect(bot != nullptr,
               "configured bots should exist as authoritative player actors");
        for (const sim::Vec3& existing : botPositions) {
            expect(planarDistanceSquared(existing, bot->position) > 1.0f,
                   "configured bots should spawn at distinct randomized locations");
        }
        botPositions.push_back(bot->position);
        if (entry.team == sim::TeamId::Attacker) {
            ++attackerBots;
            expect(bot->position.z >= -0.01f,
                   "attacker bots should spawn on the attacker side of the arena");
        } else if (entry.team == sim::TeamId::Defender) {
            ++defenderBots;
            expect(bot->position.z <= 0.01f,
                   "defender bots should spawn on the defender side of the arena");
        }
    }
    expect(attackerBots == 2u && defenderBots == 1u,
           "configured bot roster entries should preserve the requested team counts");

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{33u, 0u, "human"}, 4'500'000u, &welcome, nullptr),
           "a human should still connect alongside configured bots");
    expect(welcome.assignedPeerId == 1u, "human connections should still use the normal peer-id range");

    for (const auto& entry : server.worldState().roster) {
        if (entry.isBot) {
            expect(entry.actorId != static_cast<int>(welcome.assignedPeerId),
                   "bot actor ids should remain distinct from connected human peer ids");
        }
    }
}

void testAuthoredLevelBotsSpawnIntoBalancedAuthoritativeRoster() {
    net::ServerConfig config;
    config.maxPlayers = 2u;
    config.authoredBotSpawns = {
        sim::Vec3{-8.0f, 0.0f, -9.0f},
        sim::Vec3{0.0f, 0.0f, -7.0f},
        sim::Vec3{8.0f, 0.0f, 9.0f}
    };

    net::ServerRuntime server(config);

    expect(server.worldState().enemies.empty(),
           "authored level bots should replace the legacy non-scoring enemy actors in hosted sessions");
    expect(countBotRosterEntries(server.worldState()) == 3u,
           "each level-editor enemy spawn should become a scoring authoritative bot");

    std::size_t attackerBots = 0u;
    std::size_t defenderBots = 0u;
    for (const auto& entry : server.worldState().roster) {
        expect(entry.isBot, "authored level roster entries should be marked as bots");
        expect(entry.participation == sim::ParticipationState::Playing,
               "authored level bots should join the active match roster");
        if (entry.team == sim::TeamId::Attacker) {
            ++attackerBots;
        } else if (entry.team == sim::TeamId::Defender) {
            ++defenderBots;
        }
    }

    expect(attackerBots == 2u && defenderBots == 1u,
           "authored level bots should be split across teams using the balanced assignment rule");

    expectVec3Near(requirePlayer(server.worldState(), static_cast<int>(net::kFirstBotTransportTargetId)).position,
                   sim::Vec3{-8.0f, Config::PLAYER_EYE_HEIGHT, -9.0f},
                   "the first authored bot should spawn at the level-editor position");
}

void testAuthoredLevelBotsBalanceAgainstHostTeamAndScoreAsPlayers() {
    net::ServerConfig config;
    config.maxPlayers = 1u;
    config.authoredBotTeamBias = sim::TeamId::Attacker;
    config.authoredBotSpawns = {
        sim::Vec3{0.0f, 0.0f, -10.0f}
    };
    net::ServerRuntime server(config);

    const int botId = static_cast<int>(net::kFirstBotTransportTargetId);
    expect(requireRosterEntry(server.worldState(), botId).team == sim::TeamId::Defender,
           "a single authored bot should balance onto the opposing team for the default host");
    expect(sim::findPlayer(server.worldState(), botId) != nullptr,
           "authored bot should be represented as an authoritative player");

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{34u, 0u, "host", sim::TeamId::Attacker},
                               4'700'000u,
                               &hostWelcome,
                               nullptr),
           "host should connect after authored bots are created");
    server.takePendingPackets();

    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "authored bot scoring test requires both host and bot players");

    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    bot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    bot->health = Config::SHOOT_DAMAGE;

    server.tickOnce(4'716'667u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 4'716'667u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(hostWelcome.assignedPeerId, commands, 4'733'334u),
           "host should be able to shoot the opposing authored bot");
    server.tickOnce(4'750'001u);

    expect(bot->health == 0.0f,
           "authored bot should take normal player damage");
    expect(requireRosterEntry(server.worldState(), hostWelcome.assignedPeerId).kills == 1u,
           "killing an authored bot should increment the human killer's roster stats");
    expect(requireRosterEntry(server.worldState(), botId).deaths == 1u,
           "authored bot deaths should be tracked in the roster");
    expect(server.worldState().teamScores.attackers == 1u &&
               server.worldState().teamScores.defenders == 0u,
           "killing an authored bot should award the killer's team score");
}

void testAuthoredLevelBotKillsCountAsPlayerKills() {
    net::ServerConfig config;
    config.maxPlayers = 1u;
    config.authoredBotTeamBias = sim::TeamId::Attacker;
    config.authoredBotSpawns = {
        sim::Vec3{0.0f, 0.0f, -12.0f}
    };
    config.botDirector.startFrozen = false;
    config.botDirector.reactionDelaySeconds = 0.0f;
    config.botDirector.shotCooldownSeconds = 0.0f;
    config.botDirector.accuracy = 1.0f;
    net::ServerRuntime server(config);

    const int botId = static_cast<int>(net::kFirstBotTransportTargetId);
    expect(requireRosterEntry(server.worldState(), botId).team == sim::TeamId::Defender,
           "authored bot kill regression requires the editor-placed bot on the opposing team");

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{35u, 0u, "host", sim::TeamId::Attacker},
                               4'800'000u,
                               &hostWelcome,
                               nullptr),
           "host should connect before authored bot combat is sampled");
    server.takePendingPackets();

    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "authored bot kill regression requires both host and bot players");

    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 12.0f};
    host->health = Config::SHOOT_DAMAGE;
    bot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    bot->yaw = 3.14159265358979323846f;

    for (int tick = 0; tick < 40; ++tick) {
        server.tickOnce(4'816'667u + static_cast<std::uint64_t>(tick) * 16'667u);
        if (requireRosterEntry(server.worldState(), botId).kills == 1u) {
            break;
        }
    }

    expect(requireRosterEntry(server.worldState(), botId).kills == 1u,
           "authored bot kills should increment the bot roster kill count");
    expect(requireRosterEntry(server.worldState(), hostWelcome.assignedPeerId).deaths == 1u,
           "authored bot kills should increment the defeated player's death count");
    expect(server.worldState().teamScores.attackers == 0u &&
               server.worldState().teamScores.defenders == 1u,
           "authored bot kills should award the bot's team score");
}

void testWelcomeIncludesConfiguredLevelIdentity() {
    net::ServerConfig config;
    config.levelSlot = 6;
    config.levelHash = net::makeLevelIdentityHash(6);

    net::ServerRuntime server(config);

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{23u, 0u, "level-test"}, 3'500'000u, &welcome, nullptr),
           "server should accept the client when testing welcome level identity");
    expect(welcome.levelSlot == 6, "welcome should expose the configured authoritative level slot");
    expect(welcome.levelHash == net::makeLevelIdentityHash(6),
           "welcome should expose the configured authoritative level hash");
}

void testDuplicateHelloReusesExistingPeerAssignment() {
    net::ServerConfig config;
    config.maxPlayers = 1u;
    net::ServerRuntime server(config);

    net::WelcomeMessage firstWelcome;
    net::WelcomeMessage duplicateWelcome;

    expect(server.acceptClient(net::HelloMessage{31u, 0u, "one"}, 4'000'000u, &firstWelcome, nullptr),
           "first hello should be accepted");
    expect(server.acceptClient(net::HelloMessage{31u, 0u, "one-retry"}, 4'050'000u, &duplicateWelcome, nullptr),
           "duplicate hello for the same session should be treated as a retry");

    expect(firstWelcome.assignedPeerId == 1u, "first client should get peer id 1");
    expect(duplicateWelcome.assignedPeerId == firstWelcome.assignedPeerId,
           "duplicate hello should reuse the original peer id");
    expect(server.sessions().size() == 1u,
           "duplicate hellos should not create extra authoritative sessions");
    expect(server.sessions().front().lastHeardTimeUs == 4'050'000u,
           "duplicate hello should refresh the existing session heartbeat");
    expect(server.worldState().players.size() == 1u,
           "duplicate hello should not create extra authoritative player entries");

    std::string rejectReason;
    net::WelcomeMessage otherWelcome;
    expect(!server.acceptClient(net::HelloMessage{32u, 0u, "two"}, 4'060'000u, &otherWelcome, &rejectReason),
           "a different session should still respect max player capacity");
    expect(rejectReason == "server_full",
           "server should only reject genuinely new sessions when full");
}

void testLateJoinInitialSnapshotCarriesCurrentAuthoritativeWorldState() {
    net::ServerConfig config;
    config.maxPlayers = 3u;
    config.maxHumanPlayers = 3u;
    config.sessionLabel = "Late Join Runtime";
    config.hostPlayerName = "host-player";
    config.publicJoinPort = 45210u;
    config.levelSlot = 7;
    config.levelHash = net::makeLevelIdentityHash(7);
    config.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{36u, 0u, "host-player"}, 6'100'000u, &hostWelcome, nullptr),
           "host should connect before late-join reconstruction testing");
    expect(server.acceptClient(net::HelloMessage{37u, 0u, "target"}, 6'100'100u, &targetWelcome, nullptr),
           "target should connect before late-join reconstruction testing");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "late-join reconstruction test requires the current authoritative host and target actors");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    server.tickOnce(6'110'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 6'105'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(hostWelcome.assignedPeerId, commands, 6'120'000u),
           "late-join reconstruction test should enqueue the host kill shot");
    server.tickOnce(6'130'000u);
    server.tickOnce(6'146'667u);
    server.tickOnce(6'163'334u);
    server.takePendingPackets();

    expect(server.worldState().teamScores.attackers == 1u,
           "late-join reconstruction test requires a pre-existing authoritative attacker score");

    net::WelcomeMessage lateWelcome;
    expect(server.acceptClient(net::HelloMessage{38u, 0u, "late-joiner"}, 6'200'000u, &lateWelcome, nullptr),
           "late joiner should connect after the world already contains score and roster history");

    const auto lateJoinPackets = server.takePendingPackets();
    const auto& lateJoinSnapshot = requireSnapshotForPeer(lateJoinPackets, lateWelcome.assignedPeerId);

    expect(lateWelcome.sessionMetadata.sessionLabel == "Late Join Runtime" &&
               lateWelcome.sessionMetadata.hostPlayerName == "host-player" &&
               lateWelcome.sessionMetadata.publicJoinPort == 45210u,
           "late join welcome should preserve authoritative hosted-session metadata");
    expect(lateJoinSnapshot.teamScores.attackers == 1u &&
               lateJoinSnapshot.teamScores.defenders == 0u,
           "late join snapshots should reconstruct the current authoritative team score immediately");
    expect(requireSnapshotRosterEntry(lateJoinSnapshot, hostWelcome.assignedPeerId).kills == 1u &&
               requireSnapshotRosterEntry(lateJoinSnapshot, targetWelcome.assignedPeerId).deaths == 1u,
           "late join snapshots should reconstruct the current authoritative roster stats immediately");
    expect(lateJoinSnapshot.sessionMetadata.sessionLabel == "Late Join Runtime" &&
               lateJoinSnapshot.sessionMetadata.hostPlayerName == "host-player" &&
               lateJoinSnapshot.sessionMetadata.publicJoinPort == 45210u &&
               lateJoinSnapshot.sessionMetadata.levelSlot == 7 &&
               lateJoinSnapshot.sessionMetadata.levelHash == net::makeLevelIdentityHash(7) &&
               lateJoinSnapshot.sessionMetadata.maxHumanPlayers == 3u &&
               lateJoinSnapshot.sessionMetadata.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "late join snapshots should preserve authoritative hosted-session metadata alongside gameplay state");
    expect(lateJoinSnapshot.localParticipantState.participation == sim::ParticipationState::Playing &&
               lateJoinSnapshot.localParticipantState.control.actorId == lateWelcome.assignedPeerId,
           "late join snapshots should expose the joining participant's authoritative gameplay identity");
    expect(lateJoinSnapshot.cadence.authoritativeTickHz == config.tickRateHz &&
               lateJoinSnapshot.cadence.snapshotCadenceHz == config.snapshotRateHz &&
               lateJoinSnapshot.authoritativeTime.serverTick == lateJoinSnapshot.serverTick &&
               lateJoinSnapshot.authoritativeTime.serverTimeUs == lateJoinSnapshot.serverTimeUs,
           "late join snapshots should preserve authoritative cadence and timing for reconciliation");
}

void testPlayerVsPlayerFireUsesSharedHitscanPath() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{41u, 0u, "shooter"}, 5'000'000u, &shooterWelcome, nullptr),
           "shooter client should connect");
    expect(server.acceptClient(net::HelloMessage{42u, 0u, "target"}, 5'000'000u, &targetWelcome, nullptr),
           "target client should connect");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr, "server should track both players");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    const float targetHealthBefore = target->health;
    server.tickOnce(5'100'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'090'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 5'120'000u),
           "fire command should enqueue");
    server.tickOnce(5'130'000u);

    expect(target->health < targetHealthBefore,
           "authoritative player-vs-player fire should damage the target through the shared rifle path");
}

void testFriendlyFireIsIgnoredForSameTeamPlayers() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{141u, 0u, "shooter", sim::TeamId::Attacker},
                               5'000'000u,
                               &shooterWelcome,
                               nullptr),
           "shooter client should connect");
    expect(server.acceptClient(net::HelloMessage{142u, 0u, "target", sim::TeamId::Attacker},
                               5'000'000u,
                               &targetWelcome,
                               nullptr),
           "target client should connect on the same team");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr, "server should track both same-team players");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    const float targetHealthBefore = target->health;
    server.tickOnce(5'100'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'090'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 5'120'000u),
           "friendly-fire test command should enqueue");
    server.tickOnce(5'130'000u);

    expect(target->health == targetHealthBefore,
           "same-team shots should not damage friendly players");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 0u,
           "friendly-fire suppression should not increment the shooter's kill count");
    expect(server.worldState().teamScores.attackers == 0u &&
               server.worldState().teamScores.defenders == 0u,
           "friendly-fire suppression should leave team scores unchanged");
}

void testTeamChangeControlRequestUpdatesRosterAndRespawnsPlayer() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{151u, 0u, "switcher", sim::TeamId::Attacker},
                               5'300'000u,
                               &welcome,
                               nullptr),
           "switching client should connect");
    server.takePendingPackets();

    sim::PlayerState* player = sim::findPlayer(&server.worldState(), welcome.assignedPeerId);
    expect(player != nullptr, "switching client should exist in the authoritative world");
    player->position = sim::Vec3{7.0f, Config::PLAYER_EYE_HEIGHT, 2.0f};
    player->health = 25.0f;

    sim::PlayerCommand legacyShadowCommand;
    legacyShadowCommand.seq = 1u;
    legacyShadowCommand.requestedTeam = sim::TeamId::Defender;

    net::CommandBundle commands;
    commands.commands.push_back(legacyShadowCommand);
    expect(server.enqueueCommandBundle(welcome.assignedPeerId, commands, 5'320'000u),
           "legacy gameplay shadow command should enqueue");
    server.tickOnce(5'330'000u);

    expect(requireRosterEntry(server.worldState(), welcome.assignedPeerId).team == sim::TeamId::Attacker,
           "gameplay-command overloads should no longer change the authoritative team");

    player->position = sim::Vec3{7.0f, Config::PLAYER_EYE_HEIGHT, 2.0f};
    player->health = 25.0f;
    expect(server.handleControlPayload(welcome.assignedPeerId,
                                       net::PacketPayload{net::TeamChangeRequest{sim::TeamId::Defender}},
                                       5'340'000u),
           "control-plane team-change requests should be accepted authoritatively");

    expect(requireRosterEntry(server.worldState(), welcome.assignedPeerId).team == sim::TeamId::Defender,
           "control-plane team-change requests should update the authoritative roster team");
    expect(player->health == Config::PLAYER_MAX_HEALTH,
           "control-plane team-change requests should respawn the player at full health");
    expectVec3Near(player->position,
                   server.worldState().playerSpawns.at(welcome.assignedPeerId - 1),
                   "control-plane team-change requests should respawn the player at the standard peer spawn");
}

void testRuntimeParamControlRequestsProduceAppliedAndStagedFeedback() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{152u, 0u, "runtime-owner"}, 5'360'000u, &welcome, nullptr),
           "runtime-param client should connect");
    server.takePendingPackets();

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(welcome.assignedPeerId),
                   "net.player[1].latency_ms",
                   85.0f}},
               5'370'000u),
           "player latency control requests should apply immediately");
    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(welcome.assignedPeerId),
                   "net.player[1].loss_pct",
                   10.0f}},
               5'370'100u),
           "player loss control requests should apply immediately");

    expect(requireRosterEntry(server.worldState(), welcome.assignedPeerId).latencyMs == 85u &&
               requireRosterEntry(server.worldState(), welcome.assignedPeerId).lossPct == 10u,
           "applied player runtime-parameter requests should update authoritative roster transport metrics");

    const auto appliedPackets = server.takePendingPackets();
    const auto latencyResult = requireRuntimeParamApplyResultForPeer(
        appliedPackets, welcome.assignedPeerId, "net.player[1].latency_ms");
    expect(latencyResult.applied &&
               latencyResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick,
           "player latency control requests should produce immediate apply feedback");
    const auto lossResult = requireRuntimeParamApplyResultForPeer(
        appliedPackets, welcome.assignedPeerId, "net.player[1].loss_pct");
    expect(lossResult.applied &&
               lossResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick,
           "player loss control requests should produce immediate apply feedback");

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.tickrate",
                   120.0f}},
               5'370'200u),
           "session tickrate control requests should be staged deterministically");
    expect(server.config().tickRateHz == 60u,
           "staged session runtime-parameter requests should not mutate live tickrate immediately");

    const auto stagedPackets = server.takePendingPackets();
    const auto stagedResult = requireRuntimeParamApplyResultForPeer(
        stagedPackets, welcome.assignedPeerId, "sv.tickrate");
    expect(!stagedResult.applied &&
               stagedResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
               stagedResult.message == "staged_for_next_tick",
           "session-owned runtime parameters should surface staged feedback for the next tick boundary");

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.snapshot_rate",
                   30.0f}},
               5'370'250u),
           "session snapshot-rate control requests should be staged deterministically");
    expect(server.config().snapshotRateHz == 20u,
           "staged snapshot-rate requests should not mutate live snapshot cadence immediately");

    const auto stagedSnapshotPackets = server.takePendingPackets();
    const auto stagedSnapshotResult = requireRuntimeParamApplyResultForPeer(
        stagedSnapshotPackets, welcome.assignedPeerId, "sv.snapshot_rate");
    expect(!stagedSnapshotResult.applied &&
               stagedSnapshotResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
               stagedSnapshotResult.message == "staged_for_next_tick",
           "session-owned snapshot-rate parameters should surface staged feedback for the next tick boundary");
}

void testInvalidSessionTickRateRequestsAreRejectedDeterministically() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{153u, 0u, "runtime-owner"}, 5'370'250u, &welcome, nullptr),
           "runtime-param client should connect for invalid tick-rate testing");
    server.takePendingPackets();

    expect(!server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.tickrate",
                   60.5f}},
               5'370'300u),
           "non-integral session tick-rate requests should be rejected deterministically");

    const auto packets = server.takePendingPackets();
    const auto result = requireRuntimeParamApplyResultForPeer(
        packets, welcome.assignedPeerId, "sv.tickrate");
    expect(!result.applied &&
               result.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
               result.message == "invalid_tick_rate",
           "invalid session tick-rate requests should return explicit next-tick rejection feedback");

    expect(!server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.snapshot_rate",
                   60.5f}},
               5'370'350u),
           "non-integral session snapshot-rate requests should be rejected deterministically");

    const auto snapshotPackets = server.takePendingPackets();
    const auto snapshotResult = requireRuntimeParamApplyResultForPeer(
        snapshotPackets, welcome.assignedPeerId, "sv.snapshot_rate");
    expect(!snapshotResult.applied &&
               snapshotResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
               snapshotResult.message == "invalid_snapshot_rate",
           "invalid session snapshot-rate requests should return explicit next-tick rejection feedback");
}

void testHostCanToggleStudyEventLoggingWithoutReplacingRunId() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{154u, 0u, "logging-host"}, 5'380'000u, &welcome, nullptr),
           "runtime-param client should connect for event logging control");
    server.takePendingPackets();
    expect(!welcome.sessionMetadata.studyEventLoggingEnabled &&
               welcome.sessionMetadata.studyEventRunId.empty(),
           "multiplayer sessions should start with event logging disabled by default");

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.event_logging",
                   1.0f}},
               5'380'100u),
           "host event-logging enable requests should be accepted");

    const auto enablePackets = server.takePendingPackets();
    const auto enableResult = requireRuntimeParamApplyResultForPeer(
        enablePackets, welcome.assignedPeerId, "sv.event_logging");
    expect(enableResult.applied && enableResult.value == 1.0f,
           "event-logging enable requests should produce immediate applied feedback");
    const std::string runId = server.config().studyEventRunId;
    expect(server.config().studyEventLoggingEnabled && !runId.empty(),
           "enabling event logging should activate the server recorder and allocate a stable run id");
    expect(enableResult.message == "applied:" + runId,
           "event-logging enable feedback should include the export run id");

    for (int tick = 0; tick < 4; ++tick) {
        server.tickOnce(5'396'667u + static_cast<std::uint64_t>(tick) * 16'667u);
    }
    const auto enabledSnapshotPackets = server.takePendingPackets();
    const net::WorldSnapshot* enabledSnapshot =
        findSnapshotForPeer(enabledSnapshotPackets, welcome.assignedPeerId);
    expect(enabledSnapshot != nullptr &&
               enabledSnapshot->sessionMetadata.studyEventLoggingEnabled &&
               enabledSnapshot->sessionMetadata.studyEventRunId == runId,
           "enabled event-logging state should replicate through hosted session metadata");

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.event_logging",
                   0.0f}},
               5'480'100u),
           "host event-logging disable requests should be accepted");
    const auto disablePackets = server.takePendingPackets();
    const auto disableResult = requireRuntimeParamApplyResultForPeer(
        disablePackets, welcome.assignedPeerId, "sv.event_logging");
    expect(disableResult.applied && disableResult.value == 0.0f,
           "event-logging disable requests should produce immediate applied feedback");
    expect(disableResult.message == "applied",
           "event-logging disable feedback should keep the standard applied message");
    expect(!server.config().studyEventLoggingEnabled &&
               server.config().studyEventRunId == runId,
           "disabling event logging should preserve the run id so future writes append instead of replacing logs");
}

void testHostCanToggleVisualizationModeAndReplicateMetadata() {
    net::ServerRuntime server;

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{155u, 0u, "visual-host"}, 5'500'000u, &hostWelcome, nullptr),
           "host should connect for visualization-mode control");
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{156u, 0u, "visual-guest"}, 5'500'100u, &guestWelcome, nullptr),
           "guest should connect for visualization-mode control");
    server.takePendingPackets();
    expect(hostWelcome.sessionMetadata.visualizationMode ==
               net::SessionVisualizationMode::Diagnostic,
           "sessions should start in diagnostic visualization mode by default");

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.visualization_mode",
                   static_cast<float>(static_cast<std::uint8_t>(
                       net::SessionVisualizationMode::Reality))}},
               5'500'200u),
           "host visualization-mode requests should be accepted");
    const auto applyPackets = server.takePendingPackets();
    const auto applyResult = requireRuntimeParamApplyResultForPeer(
        applyPackets, hostWelcome.assignedPeerId, "sv.visualization_mode");
    expect(applyResult.applied &&
               applyResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextSnapshot &&
               server.config().visualizationMode == net::SessionVisualizationMode::Reality,
           "visualization-mode requests should apply immediately and publish on the next snapshot");

    for (int tick = 0; tick < 4; ++tick) {
        server.tickOnce(5'516'667u + static_cast<std::uint64_t>(tick) * 16'667u);
    }
    const auto snapshotPackets = server.takePendingPackets();
    const net::WorldSnapshot* hostSnapshot =
        findSnapshotForPeer(snapshotPackets, hostWelcome.assignedPeerId);
    const net::WorldSnapshot* guestSnapshot =
        findSnapshotForPeer(snapshotPackets, guestWelcome.assignedPeerId);
    expect(hostSnapshot != nullptr &&
               guestSnapshot != nullptr &&
               hostSnapshot->sessionMetadata.visualizationMode ==
                   net::SessionVisualizationMode::Reality &&
               guestSnapshot->sessionMetadata.visualizationMode ==
                   net::SessionVisualizationMode::Reality,
           "visualization mode should replicate to every participant through session metadata");

    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.visualization_mode",
                   static_cast<float>(static_cast<std::uint8_t>(
                       net::SessionVisualizationMode::Diagnostic))}},
               5'600'000u),
           "guest visualization-mode requests should be rejected");
    const auto rejectPackets = server.takePendingPackets();
    const auto rejectResult = requireRuntimeParamApplyResultForPeer(
        rejectPackets, guestWelcome.assignedPeerId, "sv.visualization_mode");
    expect(!rejectResult.applied &&
               rejectResult.message == "host_only" &&
               server.config().visualizationMode == net::SessionVisualizationMode::Reality,
           "guest visualization-mode requests should leave authoritative mode unchanged");
}

void testStudyActionHostCanSpawnFrozenPassiveBotAhead() {
    net::ServerConfig config;
    config.studyActionsEnabled = true;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(
               net::HelloMessage{154u, 0u, "study-host", sim::TeamId::Attacker},
               5'380'000u,
               &hostWelcome,
               nullptr),
           "study host should connect for frozen-bot action coverage");
    server.takePendingPackets();

    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    expect(host != nullptr, "study host should own an authoritative player actor");
    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 6.0f};
    host->yaw = 0.0f;
    host->pitch = 0.0f;

    const std::size_t baselineBotCount = countBotRosterEntries(server.worldState());
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{
                   net::SessionActionRequest{net::SessionActionKind::SpawnFrozenBotAhead}},
               5'380'100u),
           "study hosts should be able to trigger the frozen-bot session action");

    const auto resultPackets = server.takePendingPackets();
    const auto& result = requireSessionActionResultForPeer(
        resultPackets, hostWelcome.assignedPeerId, net::SessionActionKind::SpawnFrozenBotAhead);
    expect(result.applied && result.actorId >= 0 && result.message == "spawned",
           "successful frozen-bot study actions should return an applied result with the spawned actor id");
    expect(countBotRosterEntries(server.worldState()) == baselineBotCount + 1u,
           "the frozen-bot study action should add exactly one authoritative bot roster entry");

    const sim::RosterEntry& botEntry = requireRosterEntry(server.worldState(), result.actorId);
    const sim::PlayerState& bot = requirePlayer(server.worldState(), result.actorId);
    expect(botEntry.isBot &&
               botEntry.team == sim::TeamId::Defender &&
               botEntry.control.kind == sim::ControlBindingKind::Actor &&
               botEntry.control.actorId == result.actorId &&
               botEntry.displayName == "Frozen BOT " + std::to_string(result.actorId),
           "spawned study bots should remain authoritative bot-controlled actors on the opposing team");
    expect(nearlyEqual(bot.position.x, 0.0f, 0.25f) &&
               nearlyEqual(bot.position.y, Config::PLAYER_EYE_HEIGHT, 0.25f) &&
               nearlyEqual(bot.position.z, 2.0f, 0.25f),
           "spawned frozen study bots should appear a short distance directly in front of the host");
    expect(nearlyEqual(bot.yaw, 3.14159265358979323846f, 0.001f),
           "spawned frozen study bots should face back toward the requesting host");

    const sim::Vec3 baselineBotPosition = bot.position;
    bool sawBotAttackEvent = false;
    std::uint64_t nowUs = 5'396'767u;
    for (int tick = 0; tick < 120; ++tick) {
        server.tickOnce(nowUs);
        const auto packets = server.takePendingPackets();
        for (const auto& packet : packets) {
            if (packet.header.kind != net::PacketKind::WorldSnapshot) {
                continue;
            }
            const auto& snapshot = std::get<net::WorldSnapshot>(packet.payload);
            sawBotAttackEvent = sawBotAttackEvent ||
                std::any_of(snapshot.events.begin(),
                            snapshot.events.end(),
                            [&](const net::SnapshotEvent& event) {
                                return event.sourcePlayerId == result.actorId &&
                                       (event.kind == net::SnapshotEventKind::WeaponFired ||
                                        event.kind == net::SnapshotEventKind::ConfirmedHit ||
                                        event.kind == net::SnapshotEventKind::DamageApplied);
                            });
        }
        nowUs += 16'667u;
    }

    const sim::PlayerState& passiveBot = requirePlayer(server.worldState(), result.actorId);
    expectVec3Near(passiveBot.position,
                   baselineBotPosition,
                   "frozen passive study bots should not roam after spawning");
    expect(!sawBotAttackEvent,
           "frozen passive study bots should not emit combat events after spawning");
}

void testStudyActionRejectsGuestRequests() {
    net::ServerConfig config;
    config.studyActionsEnabled = true;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(
               net::HelloMessage{155u, 0u, "study-host", sim::TeamId::Attacker},
               5'381'000u,
               &hostWelcome,
               nullptr),
           "study host should connect for guest authorization coverage");
    expect(server.acceptClient(
               net::HelloMessage{156u, 0u, "study-guest", sim::TeamId::Defender},
               5'381'100u,
               &guestWelcome,
               nullptr),
           "study guest should connect for guest authorization coverage");
    server.takePendingPackets();

    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{
                   net::SessionActionRequest{net::SessionActionKind::SpawnFrozenBotAhead}},
               5'381'200u),
           "non-host clients should not be allowed to apply frozen-bot study actions");

    const auto resultPackets = server.takePendingPackets();
    const auto& result = requireSessionActionResultForPeer(
        resultPackets, guestWelcome.assignedPeerId, net::SessionActionKind::SpawnFrozenBotAhead);
    expect(!result.applied && result.actorId < 0 && result.message == "host_only",
           "guest frozen-bot study actions should return an explicit host-only rejection");
    expect(countBotRosterEntries(server.worldState()) == 0u,
           "rejected guest frozen-bot study actions should not mutate the authoritative roster");
}

void testStudyActionRejectsNonStudySessions() {
    net::ServerRuntime server;

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(
               net::HelloMessage{157u, 0u, "runtime-host", sim::TeamId::Attacker},
               5'382'000u,
               &hostWelcome,
               nullptr),
           "runtime host should connect for non-study action rejection coverage");
    server.takePendingPackets();

    expect(!server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{
                   net::SessionActionRequest{net::SessionActionKind::SpawnFrozenBotAhead}},
               5'382'100u),
           "non-study sessions should reject frozen-bot study actions");

    const auto resultPackets = server.takePendingPackets();
    const auto& result = requireSessionActionResultForPeer(
        resultPackets, hostWelcome.assignedPeerId, net::SessionActionKind::SpawnFrozenBotAhead);
    expect(!result.applied && result.actorId < 0 && result.message == "study_only",
           "non-study sessions should return an explicit study-only rejection for frozen-bot actions");
    expect(countBotRosterEntries(server.worldState()) == 0u,
           "rejected non-study frozen-bot actions should not add authoritative bots");
}

void testStudyActionRejectsInvalidSpawnLocation() {
    net::ServerConfig config;
    config.studyActionsEnabled = true;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(
               net::HelloMessage{158u, 0u, "study-host", sim::TeamId::Attacker},
               5'383'000u,
               &hostWelcome,
               nullptr),
           "study host should connect for invalid spawn-point coverage");
    server.takePendingPackets();

    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    expect(host != nullptr, "study host should own an authoritative player actor");
    host->position = sim::Vec3{
        0.0f,
        Config::PLAYER_EYE_HEIGHT,
        -server.worldState().environment.arenaHalfSize + 0.25f
    };
    host->yaw = 0.0f;
    host->pitch = 0.0f;

    expect(!server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{
                   net::SessionActionRequest{net::SessionActionKind::SpawnFrozenBotAhead}},
               5'383'100u),
           "study actions should reject spawn requests when no clear forward point exists");

    const auto resultPackets = server.takePendingPackets();
    const auto& result = requireSessionActionResultForPeer(
        resultPackets, hostWelcome.assignedPeerId, net::SessionActionKind::SpawnFrozenBotAhead);
    expect(!result.applied && result.actorId < 0 && result.message == "invalid_spawn_point",
           "invalid frozen-bot spawn requests should return explicit spawn-location feedback");
    expect(countBotRosterEntries(server.worldState()) == 0u,
           "invalid frozen-bot spawn requests should leave the authoritative roster unchanged");
}

void testHostTickRateChangeStagesForNextTickAndBecomesAuthoritative() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{160u, 0u, "host"}, 5'390'000u, &welcome, nullptr),
           "host client should connect for live tick-rate testing");
    server.takePendingPackets();

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.tickrate",
                   120.0f}},
               5'390'100u),
           "host tick-rate requests should stage successfully");
    server.takePendingPackets();

    server.tickOnce(5'406'667u);

    expect(server.config().tickRateHz == 120u,
           "the next authoritative tick boundary should commit the staged live tick rate");
    expect(server.tickIntervalUs() == 8'333u,
           "committing a live tick-rate change should update the authoritative tick interval");
    expect(server.worldState().cadence.authoritativeTickHz == 120u,
           "authoritative world cadence should publish the new live tick rate after the boundary");

    const std::uint64_t changedCadenceTimeUs = server.worldState().authoritativeTime.serverTimeUs;
    server.tickOnce(0u);
    expect(server.worldState().authoritativeTime.serverTimeUs == changedCadenceTimeUs + 8'333u,
           "subsequent authoritative ticks should advance using the newly committed live interval");
}

void testHostSnapshotRateChangeStagesForNextTickAndBecomesAuthoritative() {
    net::ServerRuntime server;

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{161u, 0u, "host"}, 5'410'000u, &welcome, nullptr),
           "host client should connect for live snapshot-rate testing");
    server.takePendingPackets();

    expect(server.handleControlPayload(
               welcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.snapshot_rate",
                   30.0f}},
               5'410'100u),
           "host snapshot-rate requests should stage successfully");
    server.takePendingPackets();

    server.tickOnce(5'426'667u);

    expect(server.config().snapshotRateHz == 30u,
           "the next authoritative tick boundary should commit the staged live snapshot rate");
    expect(server.snapshotIntervalUs() == 33'333u,
           "committing a live snapshot-rate change should update the snapshot interval");
    expect(server.worldState().cadence.snapshotCadenceHz == 30u,
           "authoritative world cadence should publish the new live snapshot rate after the boundary");
}

void testLiveTickRateChangePreserves500MsHistoryAtHigherRates() {
    net::ServerConfig config;
    config.tickRateHz = 60u;
    config.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{57u, 0u, "host-shooter"}, 7'700'000u, &shooterWelcome, nullptr),
           "shooter host should connect for the live tick-rate history test");
    expect(server.acceptClient(net::HelloMessage{58u, 0u, "target"}, 7'700'100u, &targetWelcome, nullptr),
           "target client should connect for the live tick-rate history test");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "server should track both players for the live tick-rate history test");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};

    expect(server.handleControlPayload(
               shooterWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.tickrate",
                   240.0f}},
               7'700'200u),
           "host should be able to stage a higher live tick rate");
    server.takePendingPackets();

    std::uint64_t nowUs = 7'716'667u;
    server.tickOnce(nowUs);
    server.takePendingPackets();
    expect(server.config().tickRateHz == 240u,
           "live tick-rate history test should commit the higher cadence after the boundary");

    for (int tick = 0; tick < 72; ++tick) {
        nowUs += 4'167u;
        server.tickOnce(nowUs);
        server.takePendingPackets();
    }

    target->position = sim::Vec3{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    nowUs += 4'167u;
    server.tickOnce(nowUs);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f});
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = nowUs;
    fire.interpDelayMs = 300u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, nowUs + 1'000u),
           "live high-rate fire command should enqueue after the cadence change");
    nowUs += 4'167u;
    server.tickOnce(nowUs);

    expect(target->health < target->maxHealth,
           "raising the live tick rate should also expand lag-comp retention so 300 ms rewinds remain valid");
}

void testHostRuntimeParamRequestsCanTargetOtherParticipants() {
    net::ServerConfig config;
    config.attackerBotCount = 1u;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{252u, 0u, "host"}, 5'380'000u, &hostWelcome, nullptr),
           "host runtime-param client should connect");
    expect(server.acceptClient(net::HelloMessage{253u, 0u, "guest"}, 5'380'100u, &guestWelcome, nullptr),
           "guest runtime-param client should connect");
    const int attackerBotId = requireBotActorId(server.worldState(), sim::TeamId::Attacker);
    server.takePendingPackets();

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   "net.player[2].latency_ms",
                   95.0f}},
               5'380'200u),
           "host control requests should be able to target another human participant");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   attackerBotId,
                   "net.bot[" + std::to_string(attackerBotId) + "].loss_pct",
                   17.0f}},
               5'380'300u),
           "host control requests should be able to target an authoritative bot participant");

    expect(requireRosterEntry(server.worldState(), guestWelcome.assignedPeerId).latencyMs == 95u,
           "host-targeted player runtime-parameter requests should update the selected human roster metrics");
    expect(requireRosterEntry(server.worldState(), attackerBotId).lossPct == 17u,
           "host-targeted bot runtime-parameter requests should update the selected bot roster metrics");

    const auto packets = server.takePendingPackets();
    const auto playerResult = requireRuntimeParamApplyResultForPeer(
        packets, hostWelcome.assignedPeerId, "net.player[2].latency_ms");
    expect(playerResult.applied && playerResult.targetId == guestWelcome.assignedPeerId,
           "host-targeted player requests should echo applied feedback for the selected participant");
    const auto botResult = requireRuntimeParamApplyResultForPeer(
        packets, hostWelcome.assignedPeerId, "net.bot[" + std::to_string(attackerBotId) + "].loss_pct");
    expect(botResult.applied && botResult.scope == net::RuntimeParamScope::Bot,
           "host-targeted bot requests should echo bot-scoped apply feedback");
}

void testGuestRuntimeParamRequestsCannotTargetOtherParticipants() {
    net::ServerRuntime server;

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{257u, 0u, "host"}, 5'380'400u, &hostWelcome, nullptr),
           "host should connect for guest authority rejection testing");
    expect(server.acceptClient(net::HelloMessage{258u, 0u, "guest"}, 5'380'500u, &guestWelcome, nullptr),
           "guest should connect for guest authority rejection testing");
    server.takePendingPackets();

    const std::string predictionKey =
        net::runtimeParamKeyForTarget(hostWelcome.assignedPeerId, "prediction_enabled");
    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   predictionKey,
                   0.0f}},
               5'380'600u),
           "guests should not be able to edit another participant's runtime settings");
    expect(requireRosterEntry(server.worldState(), hostWelcome.assignedPeerId).predictionEnabled,
           "rejected guest runtime-setting requests should leave the target participant unchanged");

    const auto packets = server.takePendingPackets();
    const auto result = requireRuntimeParamApplyResultForPeer(
        packets, guestWelcome.assignedPeerId, predictionKey);
    expect(!result.applied && result.message == "host_only",
           "rejected guest runtime-setting requests should return an explicit host-only rejection");
}

void testHostScoreboardAdminCanSwitchAndKickRemotePlayers() {
    net::ServerRuntime server;

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(
               net::HelloMessage{261u, 0u, "host", sim::TeamId::Attacker},
               5'380'610u,
               &hostWelcome,
               nullptr),
           "host should connect for scoreboard-admin team switching");
    expect(server.acceptClient(
               net::HelloMessage{262u, 0u, "guest", sim::TeamId::Defender},
               5'380'620u,
               &guestWelcome,
               nullptr),
           "guest should connect for scoreboard-admin team switching");
    server.takePendingPackets();

    const std::string teamKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "admin_team");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   teamKey,
                   static_cast<float>(static_cast<std::uint8_t>(sim::TeamId::Attacker))}},
               5'380'630u),
           "hosts should be able to assign a remote player from the scoreboard admin menu");
    expect(requireRosterEntry(server.worldState(), guestWelcome.assignedPeerId).team ==
               sim::TeamId::Attacker,
           "host scoreboard team assignment should update the authoritative roster team");

    const auto teamPackets = server.takePendingPackets();
    const auto teamResult = requireRuntimeParamApplyResultForPeer(
        teamPackets, hostWelcome.assignedPeerId, teamKey);
    expect(teamResult.applied && teamResult.message == "applied",
           "host scoreboard team assignment should return applied feedback to the host");

    const std::string kickKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "admin_kick");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   kickKey,
                   1.0f}},
               5'380'640u),
           "hosts should be able to kick a remote player from the scoreboard admin menu");
    expect(server.findSession(guestWelcome.assignedPeerId) == nullptr,
           "host scoreboard kick should remove the target client session");
    expect(sim::findRosterEntry(server.worldState(), guestWelcome.assignedPeerId) == nullptr,
           "host scoreboard kick should remove the target from the authoritative roster");

    const auto kickPackets = server.takePendingPackets();
    const auto kickResult = requireRuntimeParamApplyResultForPeer(
        kickPackets, hostWelcome.assignedPeerId, kickKey);
    expect(kickResult.applied && kickResult.message == "applied",
           "host scoreboard kick should return applied feedback to the host");

    const auto disconnectIt = std::find_if(
        kickPackets.begin(),
        kickPackets.end(),
        [peerId = guestWelcome.assignedPeerId](const net::Packet& packet) {
            return packet.header.peerId == peerId &&
                   packet.header.kind == net::PacketKind::Disconnect;
        });
    expect(disconnectIt != kickPackets.end() &&
               std::get<net::DisconnectMessage>(disconnectIt->payload).reason == "kicked by host",
           "host scoreboard kick should send a disconnect reason to the kicked client");
}

void testHostScoreboardAdminCanSwitchBotTeams() {
    net::ServerConfig config;
    config.maxPlayers = 1u;
    config.defenderBotCount = 1u;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(
               net::HelloMessage{265u, 0u, "host", sim::TeamId::Attacker},
               5'380'642u,
               &hostWelcome,
               nullptr),
           "host should connect for scoreboard-admin bot team switching");
    server.takePendingPackets();

    const int botId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    const std::string teamKey =
        net::runtimeParamKeyForTarget(static_cast<std::uint16_t>(botId), "admin_team");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   botId,
                   teamKey,
                   static_cast<float>(static_cast<std::uint8_t>(sim::TeamId::Attacker))}},
               5'380'643u),
           "hosts should be able to assign a bot from the scoreboard admin menu");
    expect(requireRosterEntry(server.worldState(), botId).team == sim::TeamId::Attacker,
           "host scoreboard bot team assignment should update the authoritative roster team");

    const auto packets = server.takePendingPackets();
    const auto result = requireRuntimeParamApplyResultForPeer(
        packets, hostWelcome.assignedPeerId, teamKey);
    expect(result.applied && result.scope == net::RuntimeParamScope::Bot,
           "host scoreboard bot team assignment should return bot-scoped feedback");

    const std::string kickKey =
        net::runtimeParamKeyForTarget(static_cast<std::uint16_t>(botId), "admin_kick");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   botId,
                   kickKey,
                   1.0f}},
               5'380'644u),
           "hosts should be able to kick a bot from the scoreboard admin menu");
    expect(sim::findPlayer(server.worldState(), botId) == nullptr &&
               sim::findRosterEntry(server.worldState(), botId) == nullptr,
           "host scoreboard bot kick should remove the bot player and roster entry");

    const auto kickPackets = server.takePendingPackets();
    const auto kickResult = requireRuntimeParamApplyResultForPeer(
        kickPackets, hostWelcome.assignedPeerId, kickKey);
    expect(kickResult.applied && kickResult.scope == net::RuntimeParamScope::Bot,
           "host scoreboard bot kick should return bot-scoped feedback");
}

void testHostScoreboardAdminCanAddBalancedBots() {
    net::ServerConfig config;
    config.maxPlayers = 2u;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(
               net::HelloMessage{266u, 0u, "host", sim::TeamId::Attacker},
               5'380'649u,
               &hostWelcome,
               nullptr),
           "host should connect for scoreboard-admin bot add testing");
    expect(server.acceptClient(
               net::HelloMessage{267u, 0u, "guest", sim::TeamId::Attacker},
               5'380'650u,
               &guestWelcome,
               nullptr),
           "guest should connect for scoreboard-admin bot add rejection testing");
    server.takePendingPackets();

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.admin_add_bot",
                   0.0f}},
               5'380'651u),
           "hosts should be able to add a balanced bot from the scoreboard admin menu");

    const auto botIt = std::find_if(
        server.worldState().roster.begin(),
        server.worldState().roster.end(),
        [](const sim::RosterEntry& entry) {
            return entry.isBot;
        });
    expect(botIt != server.worldState().roster.end(),
           "host scoreboard add bot should create a server-owned roster entry");
    expect(botIt->team == sim::TeamId::Defender,
           "host scoreboard add bot should choose the smaller team automatically");
    expect(sim::findPlayer(server.worldState(), botIt->actorId) != nullptr,
           "host scoreboard add bot should create an authoritative player actor");

    const auto addPackets = server.takePendingPackets();
    const auto addResult = requireRuntimeParamApplyResultForPeer(
        addPackets, hostWelcome.assignedPeerId, "sv.admin_add_bot");
    expect(addResult.applied && addResult.targetId == botIt->actorId,
           "host scoreboard add bot should return the created bot actor id");

    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.admin_add_bot",
                   0.0f}},
               5'380'652u),
           "guests should not be able to add bots from the scoreboard admin menu");
    const auto rejectedPackets = server.takePendingPackets();
    const auto rejectedResult = requireRuntimeParamApplyResultForPeer(
        rejectedPackets, guestWelcome.assignedPeerId, "sv.admin_add_bot");
    expect(!rejectedResult.applied && rejectedResult.message == "host_only",
           "guest scoreboard add bot requests should return host-only feedback");
}

void testGuestScoreboardAdminRequestsAreRejectedAuthoritatively() {
    net::ServerRuntime server;

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(
               net::HelloMessage{263u, 0u, "host", sim::TeamId::Attacker},
               5'380'645u,
               &hostWelcome,
               nullptr),
           "host should connect for scoreboard-admin rejection testing");
    expect(server.acceptClient(
               net::HelloMessage{264u, 0u, "guest", sim::TeamId::Defender},
               5'380'646u,
               &guestWelcome,
               nullptr),
           "guest should connect for scoreboard-admin rejection testing");
    server.takePendingPackets();

    const std::string teamKey =
        net::runtimeParamKeyForTarget(hostWelcome.assignedPeerId, "admin_team");
    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   teamKey,
                   static_cast<float>(static_cast<std::uint8_t>(sim::TeamId::Defender))}},
               5'380'647u),
           "guest scoreboard-admin team requests should be rejected");
    expect(requireRosterEntry(server.worldState(), hostWelcome.assignedPeerId).team ==
               sim::TeamId::Attacker,
           "rejected guest scoreboard-admin team requests should not mutate the target team");

    const auto teamPackets = server.takePendingPackets();
    const auto teamResult = requireRuntimeParamApplyResultForPeer(
        teamPackets, guestWelcome.assignedPeerId, teamKey);
    expect(!teamResult.applied && teamResult.message == "host_only",
           "guest scoreboard-admin team requests should return host-only feedback");

    const std::string kickKey =
        net::runtimeParamKeyForTarget(hostWelcome.assignedPeerId, "admin_kick");
    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   kickKey,
                   1.0f}},
               5'380'648u),
           "guest scoreboard-admin kick requests should be rejected");
    expect(server.findSession(hostWelcome.assignedPeerId) != nullptr,
           "rejected guest scoreboard-admin kick requests should leave the host connected");

    const auto kickPackets = server.takePendingPackets();
    const auto kickResult = requireRuntimeParamApplyResultForPeer(
        kickPackets, guestWelcome.assignedPeerId, kickKey);
    expect(!kickResult.applied && kickResult.message == "host_only",
           "guest scoreboard-admin kick requests should return host-only feedback");
}

void testGuestSelfTransportSettingsRemainEditableButSyncSettingsRequireHost() {
    net::ServerRuntime server;

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{259u, 0u, "host"}, 5'380'650u, &hostWelcome, nullptr),
           "host should connect for guest self-setting authority testing");
    expect(server.acceptClient(net::HelloMessage{260u, 0u, "guest"}, 5'380'700u, &guestWelcome, nullptr),
           "guest should connect for guest self-setting authority testing");
    server.takePendingPackets();

    const std::string predictionKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "prediction_enabled");
    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   predictionKey,
                   0.0f}},
               5'380'750u),
           "guests should not be able to edit host-managed sync settings even for themselves");
    expect(requireRosterEntry(server.worldState(), guestWelcome.assignedPeerId).predictionEnabled,
           "rejected guest self sync-setting requests should leave the local participant unchanged");

    const std::string latencyKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "latency_ms");
    expect(server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   latencyKey,
                   85.0f}},
               5'380'800u),
           "guests should still be able to edit their own transport settings");
    expect(requireRosterEntry(server.worldState(), guestWelcome.assignedPeerId).latencyMs == 85u,
           "guest self transport-setting requests should still update authoritative transport metrics");

    const auto packets = server.takePendingPackets();
    const auto rejectedSyncResult = requireRuntimeParamApplyResultForPeer(
        packets, guestWelcome.assignedPeerId, predictionKey);
    expect(!rejectedSyncResult.applied && rejectedSyncResult.message == "host_only",
           "guest self sync-setting requests should return an explicit host-only rejection");
    const auto latencyResult = requireRuntimeParamApplyResultForPeer(
        packets, guestWelcome.assignedPeerId, latencyKey);
    expect(latencyResult.applied && latencyResult.message == "applied",
           "guest self transport-setting requests should still surface applied feedback");
}

void testHostParticipantRuntimeSettingRequestsReplicateThroughRosterSnapshots() {
    net::ServerConfig config;
    config.snapshotRateHz = config.tickRateHz;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{259u, 0u, "host"}, 5'380'700u, &hostWelcome, nullptr),
           "host should connect for participant runtime-setting replication testing");
    expect(server.acceptClient(net::HelloMessage{260u, 0u, "guest"}, 5'380'800u, &guestWelcome, nullptr),
           "guest should connect for participant runtime-setting replication testing");
    server.takePendingPackets();

    const std::string interpolationKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "interpolation_enabled");
    const std::string predictionKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "prediction_enabled");
    const std::string reconciliationKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "reconciliation_strategy");
    const std::string smoothWindowKey =
        net::runtimeParamKeyForTarget(guestWelcome.assignedPeerId, "smooth_correction_window_ms");

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   interpolationKey,
                   0.0f}},
               5'380'900u),
           "hosts should be able to disable interpolation for another participant");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   predictionKey,
                   0.0f}},
               5'381'000u),
           "hosts should be able to disable prediction for another participant");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   reconciliationKey,
                   static_cast<float>(sim::RuntimeReconciliationStrategy::Snap)}},
               5'381'100u),
           "hosts should be able to change another participant's reconciliation strategy");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(guestWelcome.assignedPeerId),
                   smoothWindowKey,
                   80.0f}},
               5'381'200u),
           "hosts should be able to change another participant's smooth correction window");

    const auto applyPackets = server.takePendingPackets();
    expect(requireRuntimeParamApplyResultForPeer(applyPackets, hostWelcome.assignedPeerId, interpolationKey).applied,
           "host-targeted interpolation updates should surface applied feedback");
    expect(requireRuntimeParamApplyResultForPeer(applyPackets, hostWelcome.assignedPeerId, predictionKey).applied,
           "host-targeted prediction updates should surface applied feedback");
    expect(requireRuntimeParamApplyResultForPeer(applyPackets, hostWelcome.assignedPeerId, reconciliationKey).applied,
           "host-targeted reconciliation updates should surface applied feedback");
    expect(requireRuntimeParamApplyResultForPeer(applyPackets, hostWelcome.assignedPeerId, smoothWindowKey).applied,
           "host-targeted smoothing updates should surface applied feedback");

    server.tickOnce(5'381'300u);
    const auto snapshotPackets = server.takePendingPackets();
    const auto& guestSnapshot = requireSnapshotForPeer(snapshotPackets, guestWelcome.assignedPeerId);
    const auto& guestRosterEntry =
        requireSnapshotRosterEntry(guestSnapshot, guestWelcome.assignedPeerId);
    expect(!guestRosterEntry.interpolationEnabled,
           "replicated snapshots should carry host-edited interpolation settings to the affected participant");
    expect(!guestRosterEntry.predictionEnabled,
           "replicated snapshots should carry host-edited prediction settings to the affected participant");
    expect(guestRosterEntry.reconciliationStrategy == sim::RuntimeReconciliationStrategy::Snap,
           "replicated snapshots should carry host-edited reconciliation strategies to the affected participant");
    expect(guestRosterEntry.smoothCorrectionWindowMs == 80u,
           "replicated snapshots should carry host-edited smooth correction windows to the affected participant");
    expect(guestSnapshot.sessionMetadata.hostPeerId == hostWelcome.assignedPeerId,
           "replicated snapshots should expose the authoritative host peer id");
}

void testControlSwitchRequestsTransferAuthorityToEligibleBots() {
    net::ServerConfig config;
    config.attackerBotCount = 1u;
    config.snapshotRateHz = config.tickRateHz;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{254u, 0u, "host", sim::TeamId::Attacker},
                               5'381'000u,
                               &hostWelcome,
                               nullptr),
           "host control-switch client should connect");
    const int attackerBotId = requireBotActorId(server.worldState(), sim::TeamId::Attacker);
    server.takePendingPackets();

    const sim::Vec3 hostStart = requirePlayer(server.worldState(), hostWelcome.assignedPeerId).position;
    const sim::Vec3 botStart = requirePlayer(server.worldState(), attackerBotId).position;

    const std::string controlKey =
        "net.player[" + std::to_string(hostWelcome.assignedPeerId) + "].control_actor_id";
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   controlKey,
                   static_cast<float>(attackerBotId)}},
               5'381'100u),
           "authoritative control-switch requests should accept eligible bot targets");
    const auto controlPackets = server.takePendingPackets();
    const auto controlResult =
        requireRuntimeParamApplyResultForPeer(controlPackets, hostWelcome.assignedPeerId, controlKey);
    expect(controlResult.applied && controlResult.targetId == hostWelcome.assignedPeerId,
           "eligible control-switch requests should echo applied feedback to the owning participant");

    sim::PlayerCommand move;
    move.seq = 1u;
    move.dtSeconds = 0.1f;
    move.moveY = 1.0f;
    move.yaw = requirePlayer(server.worldState(), attackerBotId).yaw;
    move.pitch = 0.0f;
    move.viewedServerTimeUs = 5'381'050u;

    net::CommandBundle commands;
    commands.commands.push_back(move);
    expect(server.enqueueCommandBundle(hostWelcome.assignedPeerId, commands, 5'381'150u),
           "server should enqueue gameplay commands after a bot control transfer");
    server.tickOnce(5'381'250u);

    expect(!nearlyEqual(requirePlayer(server.worldState(), attackerBotId).position.z, botStart.z),
           "after switching control, authoritative gameplay commands should move the selected bot actor");
    expectVec3Near(requirePlayer(server.worldState(), hostWelcome.assignedPeerId).position,
                   hostStart,
                   "after switching control, the host's original peer actor should not consume the moved command");

    const auto packets = server.takePendingPackets();
    const auto& snapshot = requireSnapshotForPeer(packets, hostWelcome.assignedPeerId);
    expect(snapshot.localParticipantState.control.actorId == attackerBotId &&
               snapshot.localPaneView.mode == sim::PaneViewMode::PlayerControlled &&
               snapshot.localPaneView.followTargetActorId == attackerBotId,
           "authoritative snapshots should publish the transferred control actor through the local participant and pane contracts");
    expect(std::none_of(snapshot.remotePlayers.begin(),
                        snapshot.remotePlayers.end(),
                        [attackerBotId, hostPeer = hostWelcome.assignedPeerId](const sim::PlayerState& player) {
                            return player.playerId == attackerBotId ||
                                   player.playerId == static_cast<int>(hostPeer);
                        }),
           "the local snapshot should not mirror either the selected control actor or the dormant peer actor as remote players");
}

void testSpectatorFollowRequestsCycleTargetsAndRejectRemoteHumanControl() {
    net::ServerConfig config;
    config.attackerBotCount = 1u;
    config.snapshotRateHz = config.tickRateHz;
    net::ServerRuntime server(config);

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{255u, 0u, "host", sim::TeamId::Attacker},
                               5'382'000u,
                               &hostWelcome,
                               nullptr),
           "host client should connect for spectator follow testing");
    expect(server.acceptClient(net::HelloMessage{256u, 0u, "guest", sim::TeamId::Defender},
                               5'382'100u,
                               &guestWelcome,
                               nullptr),
           "guest client should connect for spectator follow testing");
    const int attackerBotId = requireBotActorId(server.worldState(), sim::TeamId::Attacker);
    server.takePendingPackets();

    const std::string controlKey =
        "net.player[" + std::to_string(hostWelcome.assignedPeerId) + "].control_actor_id";
    expect(!server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   controlKey,
                   static_cast<float>(guestWelcome.assignedPeerId)}},
               5'382'200u),
           "remote-human actors should be rejected as local control targets");
    const auto rejectedPackets = server.takePendingPackets();
    const auto rejectedResult =
        requireRuntimeParamApplyResultForPeer(rejectedPackets, hostWelcome.assignedPeerId, controlKey);
    expect(!rejectedResult.applied && rejectedResult.message == "remote_human_rejected",
           "rejected control-switch requests should surface explicit remote-human feedback");

    expect(server.handleControlPayload(hostWelcome.assignedPeerId,
                                       net::PacketPayload{net::TeamChangeRequest{sim::TeamId::Spectator}},
                                       5'382'300u),
           "host should be able to enter spectator mode through the authoritative team-change control path");

    const std::string followKey =
        "net.player[" + std::to_string(hostWelcome.assignedPeerId) + "].follow_target_actor_id";
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   followKey,
                   static_cast<float>(guestWelcome.assignedPeerId)}},
               5'382'400u),
           "spectator follow requests should accept remote-human observation targets");
    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Player,
                   static_cast<std::int32_t>(hostWelcome.assignedPeerId),
                   followKey,
                   static_cast<float>(attackerBotId)}},
               5'382'500u),
           "spectator follow requests should also accept authoritative bot targets");
    const auto followPackets = server.takePendingPackets();
    const auto followResult =
        requireRuntimeParamApplyResultForPeer(followPackets, hostWelcome.assignedPeerId, followKey);
    expect(followResult.applied && followResult.targetId == hostWelcome.assignedPeerId,
           "spectator follow requests should echo applied feedback to the owning spectator");

    server.tickOnce(5'382'600u);
    const auto packets = server.takePendingPackets();
    const auto& snapshot = requireSnapshotForPeer(packets, hostWelcome.assignedPeerId);
    expect(snapshot.localParticipantState.participation == sim::ParticipationState::Spectating &&
               snapshot.localParticipantState.control.kind == sim::ControlBindingKind::None &&
               snapshot.localPaneView.mode == sim::PaneViewMode::SpectatorFollowThirdPerson &&
               snapshot.localPaneView.followTargetActorId == attackerBotId,
           "spectator snapshots should keep control ownership empty while routing switch requests into follow-target cycling");
}

void testKillUpdatesRosterStatsAndTeamScore() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{44u, 0u, "shooter"}, 5'500'000u, &shooterWelcome, nullptr),
           "shooter client should connect for score tracking");
    expect(server.acceptClient(net::HelloMessage{45u, 0u, "target"}, 5'500'000u, &targetWelcome, nullptr),
           "target client should connect for score tracking");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr, "server should track both players for score tracking");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    server.tickOnce(5'600'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'590'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 5'620'000u),
           "kill shot should enqueue");
    server.tickOnce(5'630'000u);

    expect(target->health == 0.0f,
           "kill shot should reduce the victim to zero health");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 1u,
           "killer roster stats should increment kills on a confirmed kill");
    expect(requireRosterEntry(server.worldState(), targetWelcome.assignedPeerId).deaths == 1u,
           "victim roster stats should increment deaths on a confirmed kill");
    expect(!requireRosterEntry(server.worldState(), targetWelcome.assignedPeerId).alive,
           "victim roster entry should mark the player dead after a confirmed kill");
    expect(server.worldState().teamScores.attackers == 1u &&
               server.worldState().teamScores.defenders == 0u,
           "the killer team score should increment exactly once on a confirmed kill");

    server.tickOnce(5'646'667u);
    server.tickOnce(5'663'334u);

    const auto packets = server.takePendingPackets();
    const auto& snapshot = requireSnapshotForPeer(packets, shooterWelcome.assignedPeerId);
    expect(snapshot.teamScores.attackers == 1u && snapshot.teamScores.defenders == 0u,
           "authoritative snapshots should publish the updated team score");
    const auto killEventIt = std::find_if(snapshot.events.begin(),
                                          snapshot.events.end(),
                                          [&](const net::SnapshotEvent& event) {
                                              return event.kind ==
                                                         net::SnapshotEventKind::PlayerKilled &&
                                                     event.sourcePlayerId ==
                                                         shooterWelcome.assignedPeerId &&
                                                     event.targetEntityId ==
                                                         targetWelcome.assignedPeerId;
                                          });
    expect(killEventIt != snapshot.events.end(),
           "authoritative snapshots should publish a dedicated PlayerKilled event for kill-feed consumers");
    expect(requireSnapshotRosterEntry(snapshot, shooterWelcome.assignedPeerId).kills == 1u,
           "authoritative snapshots should publish the killer's updated kill count");
    expect(requireSnapshotRosterEntry(snapshot, targetWelcome.assignedPeerId).deaths == 1u,
           "authoritative snapshots should publish the victim's updated death count");
}

void testPlayersStayDownThenRespawnAfterAuthoritativeDelay() {
    net::ServerConfig config;
    config.tickRateHz = 20u;
    config.snapshotRateHz = 20u;
    config.respawnDelaySeconds = 0.25f;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{64u, 0u, "respawn-shooter", sim::TeamId::Attacker},
                               5'900'000u,
                               &shooterWelcome,
                               nullptr),
           "respawn shooter should connect");
    expect(server.acceptClient(net::HelloMessage{65u, 0u, "respawn-target", sim::TeamId::Defender},
                               5'900'000u,
                               &targetWelcome,
                               nullptr),
           "respawn target should connect");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "respawn regression setup should track both authoritative players");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    server.tickOnce(5'950'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'940'000u;

    net::CommandBundle killCommands;
    killCommands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, killCommands, 5'970'000u),
           "respawn regression kill shot should enqueue");
    server.tickOnce(5'980'000u);

    expect(target->health == 0.0f,
           "respawn regression target should be dead immediately after the confirmed kill");
    const sim::Vec3 deathPosition = target->position;

    sim::PlayerCommand deadMove;
    deadMove.seq = 1u;
    deadMove.dtSeconds = 0.1f;
    deadMove.moveY = 1.0f;
    deadMove.yaw = 0.0f;
    deadMove.pitch = 0.0f;
    deadMove.viewedServerTimeUs = 5'980'000u;

    net::CommandBundle deadCommands;
    deadCommands.commands.push_back(deadMove);
    expect(server.enqueueCommandBundle(targetWelcome.assignedPeerId, deadCommands, 5'990'000u),
           "dead-player command should still be accepted at the transport layer");
    server.tickOnce(6'030'000u);

    expect(target->health == 0.0f,
           "dead players should stay down until the authoritative respawn delay elapses");
    expectVec3Near(target->position,
                   deathPosition,
                   "dead players should not keep moving during the respawn window");
    server.takePendingPackets();

    server.tickOnce(6'080'000u);
    server.tickOnce(6'130'000u);
    server.tickOnce(6'180'000u);

    expect(target->health == target->maxHealth,
           "authoritative respawn should restore the defeated player's full health after the delay");
    expect(requireRosterEntry(server.worldState(), targetWelcome.assignedPeerId).alive,
           "authoritative respawn should mark the defeated player alive again");
    const net::ClientSession* respawnedSession = server.findSession(targetWelcome.assignedPeerId);
    expect(respawnedSession != nullptr, "respawned player session should still be connected");
    expectVec3Near(target->position,
                   respawnedSession->spawnPosition,
                   "authoritative respawn should return the player to their spawn position");

    const auto packets = server.takePendingPackets();
    expect(std::any_of(
               packets.begin(),
               packets.end(),
               [](const net::Packet& packet) {
                   if (packet.header.kind != net::PacketKind::WorldSnapshot) {
                       return false;
                   }

                   const auto& snapshot = std::get<net::WorldSnapshot>(packet.payload);
                   return std::any_of(snapshot.events.begin(),
                                      snapshot.events.end(),
                                      [](const net::SnapshotEvent& event) {
                                          return event.kind == net::SnapshotEventKind::PlayerRespawned;
                                      });
               }),
           "authoritative respawns should publish a respawn event in the next snapshot");
}

void testBotsStayDownThenRespawnAfterAuthoritativeDelay() {
    net::ServerConfig config;
    config.tickRateHz = 20u;
    config.snapshotRateHz = 20u;
    config.maxPlayers = 1u;
    config.defenderBotCount = 1u;
    config.respawnDelaySeconds = 0.25f;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{164u, 0u, "bot-respawn-host", sim::TeamId::Attacker},
                               6'200'000u,
                               &hostWelcome,
                               nullptr),
           "bot respawn regression requires a connected player for snapshot publication");
    server.takePendingPackets();

    const int botId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(bot != nullptr, "bot respawn regression requires an authoritative bot player");
    const sim::Vec3 spawnPosition = bot->position;
    bot->position = sim::Vec3{7.0f, Config::PLAYER_EYE_HEIGHT, -8.0f};
    bot->velocity = sim::Vec3{2.0f, 0.0f, 2.0f};
    bot->health = 0.0f;
    sim::setRosterAlive(&server.worldState(), botId, false);
    const sim::Vec3 deathPosition = bot->position;

    server.tickOnce(6'250'000u);
    expect(bot->health == 0.0f,
           "dead bots should stay down until the authoritative respawn delay elapses");
    expectVec3Near(bot->position,
                   deathPosition,
                   "dead bots should not keep moving during the respawn window");
    expect(!requireRosterEntry(server.worldState(), botId).alive,
           "dead bots should stay marked dead before the respawn delay elapses");

    server.tickOnce(6'300'000u);
    server.tickOnce(6'350'000u);
    server.tickOnce(6'400'000u);
    server.tickOnce(6'450'000u);

    expect(bot->health == bot->maxHealth,
           "authoritative bot respawn should restore full health after the delay");
    expect(requireRosterEntry(server.worldState(), botId).alive,
           "authoritative bot respawn should mark the bot alive again");
    expectVec3Near(bot->position,
                   spawnPosition,
                   "authoritative bot respawn should return the bot to its spawn position");

    const auto packets = server.takePendingPackets();
    expect(std::any_of(
               packets.begin(),
               packets.end(),
               [botId](const net::Packet& packet) {
                   if (packet.header.kind != net::PacketKind::WorldSnapshot) {
                       return false;
                   }

                   const auto& snapshot = std::get<net::WorldSnapshot>(packet.payload);
                   return std::any_of(snapshot.events.begin(),
                                      snapshot.events.end(),
                                      [botId](const net::SnapshotEvent& event) {
                                          return event.kind == net::SnapshotEventKind::PlayerRespawned &&
                                                 event.targetEntityId == botId;
                                      });
               }),
           "authoritative bot respawns should publish the shared respawn event");
}

void testDefenderKillUpdatesRosterStatsAndTeamScore() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(
               net::HelloMessage{54u, 0u, "defender-shooter", sim::TeamId::Defender},
               5'650'000u,
               &shooterWelcome,
               nullptr),
           "defender shooter should connect for score tracking");
    expect(server.acceptClient(
               net::HelloMessage{55u, 0u, "attacker-target", sim::TeamId::Attacker},
               5'650'000u,
               &targetWelcome,
               nullptr),
           "attacker target should connect for score tracking");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "server should track both opposing players for defender score tracking");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    server.tickOnce(5'760'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'750'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 5'780'000u),
           "defender kill shot should enqueue");
    server.tickOnce(5'790'000u);

    expect(target->health == 0.0f,
           "defender kill shot should reduce the attacker to zero health");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 1u,
           "defender kill should increment the defender roster kill count");
    expect(requireRosterEntry(server.worldState(), targetWelcome.assignedPeerId).deaths == 1u,
           "defender kill should increment the attacker roster death count");
    expect(server.worldState().teamScores.attackers == 0u &&
               server.worldState().teamScores.defenders == 1u,
           "defender kill should increment the defender team score exactly once");

    server.tickOnce(5'806'667u);
    server.tickOnce(5'823'334u);

    const auto packets = server.takePendingPackets();
    const auto& snapshot = requireSnapshotForPeer(packets, shooterWelcome.assignedPeerId);
    expect(snapshot.teamScores.attackers == 0u && snapshot.teamScores.defenders == 1u,
           "authoritative snapshots should publish defender-driven team score updates");
    expect(requireSnapshotRosterEntry(snapshot, shooterWelcome.assignedPeerId).kills == 1u,
           "authoritative snapshots should publish the defender killer's updated kill count");
}

void testDuplicateCommandsAndLaterSnapshotsDoNotDoubleCountScore() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{46u, 0u, "shooter"}, 5'700'000u, &shooterWelcome, nullptr),
           "shooter client should connect for duplicate-score testing");
    expect(server.acceptClient(net::HelloMessage{47u, 0u, "target"}, 5'700'000u, &targetWelcome, nullptr),
           "target client should connect for duplicate-score testing");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr, "server should track both players for duplicate-score testing");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    server.tickOnce(5'800'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'790'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 5'820'000u),
           "initial kill shot should enqueue");
    server.tickOnce(5'830'000u);
    server.tickOnce(5'846'667u);
    server.tickOnce(5'863'334u);
    const auto firstPackets = server.takePendingPackets();
    const auto& firstSnapshot = requireSnapshotForPeer(firstPackets, shooterWelcome.assignedPeerId);

    expect(firstSnapshot.teamScores.attackers == 1u,
           "the first post-kill snapshot should publish a single attacker point");

    net::CommandBundle duplicateCommands;
    duplicateCommands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, duplicateCommands, 5'880'000u),
           "duplicate fire command should still enter the queue path");
    server.tickOnce(5'890'000u);
    server.tickOnce(5'906'667u);
    server.tickOnce(5'923'334u);

    const auto secondPackets = server.takePendingPackets();
    const auto& secondSnapshot = requireSnapshotForPeer(secondPackets, shooterWelcome.assignedPeerId);
    expect(server.worldState().teamScores.attackers == 1u &&
               server.worldState().teamScores.defenders == 0u,
           "duplicate or out-of-order fire commands should not increment score twice");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 1u,
           "duplicate commands should not increment killer stats twice");
    expect(requireRosterEntry(server.worldState(), targetWelcome.assignedPeerId).deaths == 1u,
           "duplicate commands should not increment victim stats twice");
    expect(secondSnapshot.teamScores.attackers == 1u &&
               secondSnapshot.teamScores.defenders == 0u,
           "later snapshots should keep publishing the same single kill score without inflation");
    expect(secondSnapshot.events.empty(),
           "later snapshots without new damage should not replay stale combat events");
}

void testDisconnectedClientPrunesRosterButPreservesTeamScore() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{48u, 0u, "shooter"}, 5'500'000u, &shooterWelcome, nullptr),
           "shooter client should connect for disconnect-prune testing");
    expect(server.acceptClient(net::HelloMessage{49u, 0u, "target"}, 5'500'000u, &targetWelcome, nullptr),
           "target client should connect for disconnect-prune testing");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr, "server should track both players for disconnect-prune testing");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    server.tickOnce(5'600'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 5'590'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 5'620'000u),
           "disconnect-prune kill shot should enqueue");
    server.tickOnce(5'630'000u);

    expect(server.worldState().teamScores.attackers == 1u &&
               server.worldState().teamScores.defenders == 0u,
           "disconnect-prune regression requires a confirmed attacker score before the defeated peer is removed");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 1u,
           "disconnect-prune regression requires the killer stats before pruning");
    server.takePendingPackets();

    expect(server.disconnectClient(targetWelcome.assignedPeerId, "left session"),
           "disconnect-prune regression requires the target peer to disconnect cleanly after the confirmed kill");
    server.tickOnce(5'646'667u);
    server.tickOnce(5'663'334u);

    expect(server.findSession(targetWelcome.assignedPeerId) == nullptr,
           "disconnected peer should be pruned from the authoritative session list");
    expect(server.findSession(shooterWelcome.assignedPeerId) != nullptr,
           "remaining shooter should stay connected after the other peer disconnects");
    expect(server.worldState().teamScores.attackers == 1u &&
               server.worldState().teamScores.defenders == 0u,
           "disconnecting the defeated peer should not erase the prior authoritative team score");
    expect(server.worldState().roster.size() == 1u,
           "disconnecting the defeated peer should remove only that roster entry");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 1u,
           "disconnecting the defeated peer should preserve the remaining player's kill stats");

    const auto packets = server.takePendingPackets();
    const auto& snapshot = requireSnapshotForPeer(packets, shooterWelcome.assignedPeerId);
    expect(snapshot.teamScores.attackers == 1u && snapshot.teamScores.defenders == 0u,
           "later snapshots after disconnect pruning should preserve the authoritative team score");
    expect(snapshot.roster.size() == 1u,
           "later snapshots after disconnect pruning should only include the surviving roster entry");
    expect(requireSnapshotRosterEntry(snapshot, shooterWelcome.assignedPeerId).kills == 1u,
           "later snapshots after disconnect pruning should preserve the surviving player's kill stats");
}

void testBotCombatUpdatesRosterStatsAndTeamScore() {
    net::ServerConfig config;
    config.attackerBotCount = 1u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.accuracy = 1.0f;
    config.botDirector.shotCooldownSeconds = 0.0f;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    const int attackerBotId = requireBotActorId(server.worldState(), sim::TeamId::Attacker);
    const int defenderBotId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* attacker = sim::findPlayer(&server.worldState(), attackerBotId);
    sim::PlayerState* defender = sim::findPlayer(&server.worldState(), defenderBotId);
    expect(attacker != nullptr && defender != nullptr,
           "configured bots should exist as authoritative players before bot combat starts");

    attacker->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 12.0f};
    attacker->yaw = 0.0f;
    defender->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    defender->yaw = 3.14159265358979323846f;

    for (int tick = 0; tick < 40; ++tick) {
        server.tickOnce(5'950'000u + static_cast<std::uint64_t>(tick) * 16'667u);
        if (requireRosterEntry(server.worldState(), attackerBotId).kills == 1u) {
            break;
        }
    }

    expect(requireRosterEntry(server.worldState(), attackerBotId).kills == 1u,
           "bot combat should increment the killer bot's roster kill count");
    expect(requireRosterEntry(server.worldState(), defenderBotId).deaths == 1u,
           "bot combat should increment the victim bot's roster death count");
    expect(!requireRosterEntry(server.worldState(), defenderBotId).alive,
           "bot combat should mark the defeated bot dead in the authoritative roster");
    expect(server.worldState().teamScores.attackers == 1u &&
               server.worldState().teamScores.defenders == 0u,
           "bot combat should award the killer bot's team score exactly once");

    server.tickOnce(5'966'667u);
    server.tickOnce(5'983'334u);

    net::WelcomeMessage spectatorWelcome;
    expect(server.acceptClient(net::HelloMessage{34u, 0u, "spectator"}, 6'000'000u, &spectatorWelcome, nullptr),
           "a later spectator should still connect and receive authoritative bot roster state");
    server.takePendingPackets();
    server.tickOnce(6'016'667u);
    server.tickOnce(6'033'334u);
    server.tickOnce(6'050'001u);

    const auto packets = server.takePendingPackets();
    const auto& snapshot = requireSnapshotForPeer(packets, spectatorWelcome.assignedPeerId);
    expect(snapshot.teamScores.attackers == 1u && snapshot.teamScores.defenders == 0u,
           "snapshots should publish bot-driven team score updates");
    expect(requireSnapshotRosterEntry(snapshot, attackerBotId).kills == 1u,
           "snapshots should publish the killer bot's updated kill count");
    expect(requireSnapshotRosterEntry(snapshot, defenderBotId).deaths == 1u,
           "snapshots should publish the defeated bot's updated death count");
}

void testObstacleBlocksAuthoritativeHitscanDamage() {
    sim::MovementEnvironment environment;
    environment.collisionBoxes.push_back(
        sim::CollisionBox{
            sim::Vec3{0.0f, 1.5f, -2.5f},
            sim::Vec3{4.0f, 1.5f, 1.25f}});

    net::ServerRuntime server(net::ServerConfig{}, sim::SimConfig{}, environment);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{66u, 0u, "shooter", sim::TeamId::Attacker},
                               6'400'000u,
                               &shooterWelcome,
                               nullptr),
           "occlusion regression requires the shooter client to connect");
    expect(server.acceptClient(net::HelloMessage{67u, 0u, "target", sim::TeamId::Defender},
                               6'400'100u,
                               &targetWelcome,
                               nullptr),
           "occlusion regression requires the target client to connect");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "occlusion regression requires both authoritative players");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    shooter->yaw = 0.0f;
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = target->maxHealth;

    net::CommandBundle commands;
    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 6'390'000u;
    commands.commands.push_back(fire);

    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 6'410'000u),
           "occlusion regression requires the blocked fire command to enqueue");
    server.tickOnce(6'416'667u);

    expect(nearlyEqual(target->health, target->maxHealth),
           "authoritative hitscan should not damage a target hidden behind collision geometry");
    expect(server.worldState().teamScores.attackers == 0u &&
               server.worldState().teamScores.defenders == 0u,
           "blocked hits should not increment team score");
    expect(requireRosterEntry(server.worldState(), shooterWelcome.assignedPeerId).kills == 0u &&
               requireRosterEntry(server.worldState(), targetWelcome.assignedPeerId).deaths == 0u,
           "blocked hits should not mutate roster kill or death stats");
}

void testBotReactionDelayPreventsImmediateOpeningShot() {
    net::ServerConfig config;
    config.attackerBotCount = 0u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{68u, 0u, "host", sim::TeamId::Attacker},
                               6'500'000u,
                               &hostWelcome,
                               nullptr),
           "bot reaction regression requires the host player to connect");
    server.takePendingPackets();

    const int defenderBotId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* hostPlayer = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* defenderBot = sim::findPlayer(&server.worldState(), defenderBotId);
    expect(hostPlayer != nullptr && defenderBot != nullptr,
           "bot reaction regression requires the local player and defender bot to exist");

    hostPlayer->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    defenderBot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    defenderBot->yaw = 3.14159265358979323846f;
    hostPlayer->health = hostPlayer->maxHealth;

    server.tickOnce(6'516'667u);

    expect(nearlyEqual(hostPlayer->health, hostPlayer->maxHealth),
           "bots should not land a lethal opening shot on the first authoritative tick");
    expect(server.worldState().teamScores.attackers == 0u &&
               server.worldState().teamScores.defenders == 0u,
           "the first bot-authoritative tick should not immediately award score");

    bool sawDelayedShot = false;
    float previousCooldown = defenderBot->weaponCooldownRemaining;
    for (int tick = 0; tick < 40; ++tick) {
        server.tickOnce(6'533'334u + static_cast<std::uint64_t>(tick) * 16'667u);
        if (previousCooldown <= 0.0f && defenderBot->weaponCooldownRemaining > 0.0f) {
            sawDelayedShot = true;
        }
        previousCooldown = defenderBot->weaponCooldownRemaining;
        server.takePendingPackets();
        if (sawDelayedShot) {
            break;
        }
    }

    expect(sawDelayedShot,
           "bots should still engage after the initial target-acquisition delay");
}

void testBotsStartFrozenUntilHostActivatesDirector() {
    net::ServerConfig config;
    config.attackerBotCount = 0u;
    config.defenderBotCount = 1u;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{69u, 0u, "host", sim::TeamId::Attacker},
                               6'600'000u,
                               &hostWelcome,
                               nullptr),
           "bot freeze regression requires the host player to connect");
    server.takePendingPackets();

    const int defenderBotId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* hostPlayer = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* defenderBot = sim::findPlayer(&server.worldState(), defenderBotId);
    expect(hostPlayer != nullptr && defenderBot != nullptr,
           "bot freeze regression requires the local player and defender bot to exist");

    hostPlayer->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    defenderBot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    defenderBot->yaw = 3.14159265358979323846f;

    for (int tick = 0; tick < 20; ++tick) {
        server.tickOnce(6'616'667u + static_cast<std::uint64_t>(tick) * 16'667u);
        server.takePendingPackets();
    }

    expect(nearlyEqual(hostPlayer->health, hostPlayer->maxHealth) &&
               nearlyEqual(defenderBot->position.x, 0.0f) &&
               nearlyEqual(defenderBot->position.z, -12.0f),
           "bots should remain frozen by default until the host explicitly starts the director");

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.bots_active",
                   1.0f}},
               6'950'000u),
           "host bot-director activation should be accepted");
    const auto applyPackets = server.takePendingPackets();
    const auto applyResult = requireRuntimeParamApplyResultForPeer(
        applyPackets, hostWelcome.assignedPeerId, "sv.bots_active");
    expect(applyResult.applied && applyResult.value == 1.0f,
           "bot-director activation should produce applied session feedback");

    bool sawMovementOrDamage = false;
    bool sawActiveMetadata = false;
    for (int tick = 0; tick < 180; ++tick) {
        server.tickOnce(6'966'667u + static_cast<std::uint64_t>(tick) * 16'667u);
        if (std::fabs(defenderBot->position.x) > 0.2f ||
            std::fabs(defenderBot->position.z + 12.0f) > 0.2f ||
            hostPlayer->health < hostPlayer->maxHealth) {
            sawMovementOrDamage = true;
        }
        const auto packets = server.takePendingPackets();
        if (const net::WorldSnapshot* snapshot =
                findSnapshotForPeer(packets, hostWelcome.assignedPeerId);
            snapshot != nullptr && !snapshot->sessionMetadata.botsFrozen) {
            sawActiveMetadata = true;
        }
        if (sawMovementOrDamage && sawActiveMetadata) {
            break;
        }
    }

    expect(sawMovementOrDamage,
           "bots should resume moving or fighting once the host starts the director");
    expect(sawActiveMetadata,
           "replicated session metadata should expose the active bot-director state");
}

void testHostCanToggleBotPeaceModeAndReplicateMetadata() {
    net::ServerConfig config;
    config.tickRateHz = 60u;
    config.snapshotRateHz = 60u;
    config.defenderBotCount = 1u;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    net::WelcomeMessage guestWelcome;
    expect(server.acceptClient(net::HelloMessage{169u, 0u, "host", sim::TeamId::Attacker},
                               6'700'000u,
                               &hostWelcome,
                               nullptr),
           "bot peace-mode toggle requires a connected host");
    expect(hostWelcome.sessionMetadata.botsCanShoot,
           "hosted sessions should default to armed bots for existing gameplay compatibility");
    expect(server.acceptClient(net::HelloMessage{170u, 0u, "guest", sim::TeamId::Defender},
                               6'700'100u,
                               &guestWelcome,
                               nullptr),
           "bot peace-mode host-only regression requires a connected guest");
    server.takePendingPackets();

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.bots_can_shoot",
                   0.0f}},
               6'710'000u),
           "host bot peace-mode toggle should be accepted");
    auto packets = server.takePendingPackets();
    const auto hostResult = requireRuntimeParamApplyResultForPeer(
        packets, hostWelcome.assignedPeerId, "sv.bots_can_shoot");
    expect(hostResult.applied && hostResult.value == 0.0f,
           "bot peace-mode toggle should echo applied disabled-shooting feedback");

    expect(!server.handleControlPayload(
               guestWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.bots_can_shoot",
                   1.0f}},
               6'710'100u),
           "guest bot peace-mode toggle should be rejected authoritatively");
    packets = server.takePendingPackets();
    const auto guestResult = requireRuntimeParamApplyResultForPeer(
        packets, guestWelcome.assignedPeerId, "sv.bots_can_shoot");
    expect(!guestResult.applied && guestResult.message == "host_only",
           "guest bot peace-mode toggle should return host-only feedback");

    server.tickOnce(6'716'667u);
    packets = server.takePendingPackets();
    const net::WorldSnapshot& snapshot =
        requireSnapshotForPeer(packets, hostWelcome.assignedPeerId);
    expect(!snapshot.sessionMetadata.botsCanShoot &&
               snapshot.sessionMetadata.botsFrozen,
           "replicated session metadata should expose peace mode separately from bot active or frozen state");
}

void testPeaceModeBotsMoveWithoutShooting() {
    net::ServerConfig config;
    config.tickRateHz = 60u;
    config.snapshotRateHz = 60u;
    config.maxPlayers = 1u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.shootingEnabled = false;
    config.botDirector.reactionDelaySeconds = 0.0f;
    config.botDirector.shotCooldownSeconds = 0.0f;
    config.botDirector.accuracy = 1.0f;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{171u, 0u, "host", sim::TeamId::Attacker},
                               6'800'000u,
                               &hostWelcome,
                               nullptr),
           "peace-mode movement regression requires a connected host target");
    server.takePendingPackets();

    const int botId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "peace-mode movement regression requires host and bot player actors");
    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 8.0f};
    host->health = host->maxHealth;
    bot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -20.0f};
    bot->yaw = 3.14159265358979323846f;
    const sim::Vec3 initialBotPosition = bot->position;

    bool sawWeaponFireEvent = false;
    for (int tick = 0; tick < 180; ++tick) {
        server.tickOnce(6'816'667u + static_cast<std::uint64_t>(tick) * 16'667u);
        const auto packets = server.takePendingPackets();
        for (const auto& packet : packets) {
            if (packet.header.kind != net::PacketKind::WorldSnapshot) {
                continue;
            }
            const auto& snapshot = std::get<net::WorldSnapshot>(packet.payload);
            sawWeaponFireEvent = sawWeaponFireEvent ||
                std::any_of(snapshot.events.begin(),
                            snapshot.events.end(),
                            [](const net::SnapshotEvent& event) {
                                return event.kind == net::SnapshotEventKind::WeaponFired;
                            });
        }
    }

    expect(planarDistanceSquared(initialBotPosition, bot->position) > 0.04f,
           "peace-mode bots should keep normal movement while the director is active");
    expect(nearlyEqual(host->health, host->maxHealth) && !sawWeaponFireEvent,
           "peace-mode bots should not fire or damage players");
}

void testPeaceModeSuppressesQueuedDelayedBotFire() {
    net::ServerConfig config;
    config.tickRateHz = 20u;
    config.snapshotRateHz = 20u;
    config.maxPlayers = 1u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.reactionDelaySeconds = 0.0f;
    config.botDirector.shotCooldownSeconds = 0.0f;
    config.botDirector.accuracy = 1.0f;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{172u, 0u, "host", sim::TeamId::Attacker},
                               6'900'000u,
                               &hostWelcome,
                               nullptr),
           "queued bot-fire peace regression requires a connected host target");
    server.takePendingPackets();

    const int botId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "queued bot-fire peace regression requires host and bot player actors");
    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    host->health = host->maxHealth;
    bot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    bot->yaw = 3.14159265358979323846f;

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   botId,
                   net::runtimeParamKeyForTarget(static_cast<std::uint16_t>(botId), "latency_ms"),
                   250.0f}},
               6'910'000u),
           "host should be able to add artificial latency to bot commands");
    server.takePendingPackets();

    server.tickOnce(6'950'000u);
    expect(nearlyEqual(host->health, host->maxHealth),
           "delayed bot fire should not resolve before the artificial latency elapses");

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.bots_can_shoot",
                   0.0f}},
               6'960'000u),
           "host should be able to enable peace mode while bot fire commands are queued");
    server.takePendingPackets();

    bool sawWeaponFireEvent = false;
    for (int tick = 0; tick < 8; ++tick) {
        server.tickOnce(7'000'000u + static_cast<std::uint64_t>(tick) * 50'000u);
        const auto packets = server.takePendingPackets();
        for (const auto& packet : packets) {
            if (packet.header.kind != net::PacketKind::WorldSnapshot) {
                continue;
            }
            const auto& snapshot = std::get<net::WorldSnapshot>(packet.payload);
            sawWeaponFireEvent = sawWeaponFireEvent ||
                std::any_of(snapshot.events.begin(),
                            snapshot.events.end(),
                            [](const net::SnapshotEvent& event) {
                                return event.kind == net::SnapshotEventKind::WeaponFired;
                            });
        }
    }

    expect(nearlyEqual(host->health, host->maxHealth) && !sawWeaponFireEvent,
           "peace mode should strip queued delayed bot fire before it can resolve");
}

void testBotBehaviorUsesImperfectAimCooldownAndLateralMovement() {
    net::ServerConfig config;
    config.attackerBotCount = 0u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.clientTimeoutUs = 0u;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{70u, 0u, "host", sim::TeamId::Attacker},
                               6'700'000u,
                               &hostWelcome,
                               nullptr),
           "bot behavior regression requires the host player to connect");
    server.takePendingPackets();

    const int defenderBotId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    const int hostActorId = static_cast<int>(hostWelcome.assignedPeerId);
    sim::PlayerState* hostPlayer = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* defenderBot = sim::findPlayer(&server.worldState(), defenderBotId);
    expect(hostPlayer != nullptr && defenderBot != nullptr,
           "bot behavior regression requires the local player and defender bot to exist");

    hostPlayer->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    hostPlayer->health = Config::SHOOT_DAMAGE * 100.0f;
    hostPlayer->maxHealth = hostPlayer->health;
    defenderBot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    defenderBot->yaw = 3.14159265358979323846f;

    std::vector<std::uint64_t> botShotTimesUs;
    bool sawLateralMovement = false;
    float previousCooldown = defenderBot->weaponCooldownRemaining;

    for (int tick = 0; tick < 420; ++tick) {
        const std::uint64_t tickTimeUs =
            6'716'667u + static_cast<std::uint64_t>(tick) * 16'667u;
        server.tickOnce(tickTimeUs);
        const sim::PlayerState* currentDefenderBot =
            sim::findPlayer(server.worldState(), defenderBotId);
        expect(currentDefenderBot != nullptr,
               "bot behavior regression requires the defender bot to remain in the world");
        if (std::fabs(currentDefenderBot->position.x) > 0.5f) {
            sawLateralMovement = true;
        }
        if (previousCooldown <= 0.0f && currentDefenderBot->weaponCooldownRemaining > 0.0f) {
            botShotTimesUs.push_back(tickTimeUs);
        }
        previousCooldown = currentDefenderBot->weaponCooldownRemaining;
        server.takePendingPackets();
    }

    expect(botShotTimesUs.size() >= 4u,
           "imperfect bot behavior should still produce repeated firing opportunities");
    for (std::size_t index = 1; index < botShotTimesUs.size(); ++index) {
        expect(botShotTimesUs[index] - botShotTimesUs[index - 1u] >= 600'000u,
               "bot shots should respect the configured refire cooldown instead of rapid-firing");
    }
    const sim::PlayerState* finalHostPlayer = sim::findPlayer(server.worldState(), hostActorId);
    expect(finalHostPlayer != nullptr,
           "bot behavior regression requires the host player to remain in the world");
    const float damageTaken = finalHostPlayer->maxHealth - finalHostPlayer->health;
    const float maxPossibleDamage =
        static_cast<float>(botShotTimesUs.size()) * Config::SHOOT_DAMAGE;
    expect(damageTaken < maxPossibleDamage,
           "bot accuracy should stay imperfect instead of landing every shot");
    expect(sawLateralMovement,
           "bot movement should include lateral wandering instead of only straight-line pursuit");
}

void testBotsRemainServerOwnedWithoutHumanCommandSessions() {
    net::ServerConfig config;
    config.attackerBotCount = 1u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.accuracy = 1.0f;
    config.botDirector.shotCooldownSeconds = 0.0f;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    expect(server.sessions().empty(),
           "configured bots should not allocate human client sessions");

    const int attackerBotId = requireBotActorId(server.worldState(), sim::TeamId::Attacker);
    const int defenderBotId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    expect(server.findSession(static_cast<std::uint16_t>(attackerBotId)) == nullptr &&
               server.findSession(static_cast<std::uint16_t>(defenderBotId)) == nullptr,
           "bot actor ids should stay outside the human peer-session authority path");

    const sim::RosterEntry& attackerEntry = requireRosterEntry(server.worldState(), attackerBotId);
    const sim::RosterEntry& defenderEntry = requireRosterEntry(server.worldState(), defenderBotId);
    expect(attackerEntry.isBot && defenderEntry.isBot,
           "server-owned bot participants should remain marked as bots in the authoritative roster");
    expect(attackerEntry.control.kind == sim::ControlBindingKind::Actor &&
               attackerEntry.control.actorId == attackerBotId &&
               defenderEntry.control.kind == sim::ControlBindingKind::Actor &&
               defenderEntry.control.actorId == defenderBotId,
           "bot roster entries should remain bound directly to authoritative actor ids");

    sim::PlayerState* attacker = sim::findPlayer(&server.worldState(), attackerBotId);
    sim::PlayerState* defender = sim::findPlayer(&server.worldState(), defenderBotId);
    expect(attacker != nullptr && defender != nullptr,
           "server-owned bot participants should exist as authoritative world actors");

    attacker->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 12.0f};
    attacker->yaw = 0.0f;
    defender->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    defender->yaw = 3.14159265358979323846f;

    for (int tick = 0; tick < 40; ++tick) {
        server.tickOnce(5'975'000u + static_cast<std::uint64_t>(tick) * 16'667u);
        if (requireRosterEntry(server.worldState(), attackerBotId).kills == 1u) {
            break;
        }
    }

    expect(requireRosterEntry(server.worldState(), attackerBotId).kills == 1u &&
               requireRosterEntry(server.worldState(), defenderBotId).deaths == 1u,
           "server-owned bot command sourcing should advance bot combat without any human command session");
}

void testBotTransportOverridesAffectAuthoritativeBotCombat() {
    net::ServerConfig config;
    config.attackerBotCount = 1u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.accuracy = 1.0f;
    config.botDirector.shotCooldownSeconds = 0.0f;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{35u, 0u, "host"}, 5'940'000u, &hostWelcome, nullptr),
           "host client should connect for bot transport control");
    server.takePendingPackets();

    const int attackerBotId = requireBotActorId(server.worldState(), sim::TeamId::Attacker);
    const int defenderBotId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* hostPlayer = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* attacker = sim::findPlayer(&server.worldState(), attackerBotId);
    sim::PlayerState* defender = sim::findPlayer(&server.worldState(), defenderBotId);
    expect(hostPlayer != nullptr && attacker != nullptr && defender != nullptr,
           "host and configured bots should all exist before authoritative bot transport testing");

    hostPlayer->position = sim::Vec3{25.0f, Config::PLAYER_EYE_HEIGHT, 25.0f};
    attacker->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 12.0f};
    attacker->yaw = 0.0f;
    defender->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f};
    defender->yaw = 3.14159265358979323846f;

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   attackerBotId,
                   "net.bot[" + std::to_string(attackerBotId) + "].latency_ms",
                   50.0f}},
               5'949'000u),
           "host bot transport control should be accepted before the combat tick");
    server.takePendingPackets();

    for (int tick = 0; tick < 40; ++tick) {
        server.tickOnce(5'950'000u + static_cast<std::uint64_t>(tick) * 16'667u);
        if (requireRosterEntry(server.worldState(), defenderBotId).kills == 1u) {
            break;
        }
    }

    expect(requireRosterEntry(server.worldState(), defenderBotId).kills == 1u,
           "delaying the attacker bot should let the opposing bot win the authoritative exchange");
    expect(requireRosterEntry(server.worldState(), attackerBotId).deaths == 1u,
           "delaying the attacker bot should mark the delayed bot as the defeated participant");
    expect(server.worldState().teamScores.attackers == 0u &&
               server.worldState().teamScores.defenders == 1u,
           "bot transport overrides should change the authoritative bot combat outcome on the real runtime path");
}

void testBotLatencyPublishesControlGhostTrack() {
    net::ServerConfig config;
    config.maxPlayers = 1u;
    config.tickRateHz = 60u;
    config.snapshotRateHz = 60u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.shootingEnabled = false;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{36u, 0u, "host", sim::TeamId::Attacker},
                               6'100'000u,
                               &hostWelcome,
                               nullptr),
           "host client should connect before bot ghost replication");
    server.takePendingPackets();

    const int botId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "bot ghost replication requires host and bot player actors");
    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 24.0f};
    bot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -16.0f};
    bot->yaw = 3.14159265358979323846f;

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   botId,
                   net::runtimeParamKeyForTarget(static_cast<std::uint16_t>(botId), "latency_ms"),
                   250.0f}},
               6'110'000u),
           "host should be able to add artificial latency to the bot");
    server.takePendingPackets();

    net::WorldSnapshot latestSnapshot;
    bool sawSnapshot = false;
    for (int tick = 0; tick < 36; ++tick) {
        server.tickOnce(6'120'000u + static_cast<std::uint64_t>(tick) * 16'667u);
        const auto packets = server.takePendingPackets();
        if (const net::WorldSnapshot* snapshot =
                findSnapshotForPeer(packets, hostWelcome.assignedPeerId)) {
            latestSnapshot = *snapshot;
            sawSnapshot = true;
        }
    }

    expect(sawSnapshot, "bot ghost replication should publish a host snapshot");
    const sim::PlayerState* replicatedBot =
        findReplicatedPlayer(latestSnapshot.remotePlayers, botId);
    const sim::PlayerState* replicatedGhost =
        findReplicatedPlayer(latestSnapshot.controlRemotePlayers, botId);
    expect(replicatedBot != nullptr && replicatedGhost != nullptr,
           "bot latency should publish both the delayed bot body and clean control ghost");
    expect(replicatedGhost->position.z > replicatedBot->position.z + 0.5f,
           "bot control ghost should lead the delayed bot under artificial latency");
}

void testLivePositionRuleUsesBotControlGhostEvaluation() {
    net::ServerConfig config;
    config.maxPlayers = 1u;
    config.tickRateHz = 60u;
    config.snapshotRateHz = 60u;
    config.defenderBotCount = 1u;
    config.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    config.botDirector.startFrozen = false;
    config.botDirector.shootingEnabled = false;
    net::ServerRuntime server(config);
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();

    net::WelcomeMessage hostWelcome;
    expect(server.acceptClient(net::HelloMessage{37u, 0u, "host", sim::TeamId::Attacker},
                               6'300'000u,
                               &hostWelcome,
                               nullptr),
           "host client should connect before bot live-position testing");
    server.takePendingPackets();

    const int botId = requireBotActorId(server.worldState(), sim::TeamId::Defender);
    sim::PlayerState* host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    sim::PlayerState* bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "bot live-position testing requires host and bot player actors");
    host->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 24.0f};
    bot->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -16.0f};
    bot->yaw = 3.14159265358979323846f;

    expect(server.handleControlPayload(
               hostWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Bot,
                   botId,
                   net::runtimeParamKeyForTarget(static_cast<std::uint16_t>(botId), "latency_ms"),
                   250.0f}},
               6'310'000u),
           "host should be able to delay bot commands before live-position testing");
    server.takePendingPackets();

    net::WorldSnapshot latestSnapshot;
    bool sawSeparatedGhost = false;
    for (int tick = 0; tick < 48; ++tick) {
        server.tickOnce(6'320'000u + static_cast<std::uint64_t>(tick) * 16'667u);
        const auto packets = server.takePendingPackets();
        const net::WorldSnapshot* snapshot =
            findSnapshotForPeer(packets, hostWelcome.assignedPeerId);
        if (snapshot == nullptr) {
            continue;
        }
        const sim::PlayerState* replicatedBot =
            findReplicatedPlayer(snapshot->remotePlayers, botId);
        const sim::PlayerState* replicatedGhost =
            findReplicatedPlayer(snapshot->controlRemotePlayers, botId);
        if (replicatedBot != nullptr &&
            replicatedGhost != nullptr &&
            replicatedGhost->position.z > replicatedBot->position.z + 1.0f) {
            latestSnapshot = *snapshot;
            sawSeparatedGhost = true;
            break;
        }
    }
    expect(sawSeparatedGhost,
           "bot live-position testing should establish a separated control ghost");

    const sim::PlayerState* replicatedGhost =
        findReplicatedPlayer(latestSnapshot.controlRemotePlayers, botId);
    expect(replicatedGhost != nullptr,
           "bot live-position testing requires the replicated control ghost");

    const sim::Vec3 ghostPosition = replicatedGhost->position;
    host = sim::findPlayer(&server.worldState(), hostWelcome.assignedPeerId);
    bot = sim::findPlayer(&server.worldState(), botId);
    expect(host != nullptr && bot != nullptr,
           "bot live-position fire requires host and bot player actors");
    host->position = sim::Vec3{ghostPosition.x - 12.0f,
                              Config::PLAYER_EYE_HEIGHT,
                              ghostPosition.z};
    host->yaw = yawFor(host->position, ghostPosition);
    host->pitch = 0.0f;
    host->weaponCooldownRemaining = 0.0f;
    bot->health = bot->maxHealth;

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = host->yaw;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 6'320'000u;
    fire.interpDelayMs = 0u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(hostWelcome.assignedPeerId, commands, 6'990'000u),
           "live-position bot ghost fire command should enqueue");
    server.tickOnce(7'000'000u);

    expect(bot->health < bot->maxHealth,
           "live-position mode should evaluate bot targets against the clean control ghost");
}

void testPlayerVsEnemyFireUsesBodyCenteredLagCompTarget() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    expect(server.acceptClient(net::HelloMessage{43u, 0u, "shooter"}, 6'000'000u, &shooterWelcome, nullptr),
           "shooter client should connect");
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    expect(shooter != nullptr, "server should track the shooter");
    expect(!server.worldState().enemies.empty(),
           "server should keep the default enemy target for the body-center regression");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    sim::RemoteActorState& targetEnemy = server.worldState().enemies.front();
    targetEnemy.position = sim::Vec3{0.0f, 0.0f, -10.0f};
    targetEnemy.health = Config::ENEMY_MAX_HEALTH;
    targetEnemy.alive = true;
    const float targetHealthBefore = targetEnemy.health;

    server.tickOnce(6'100'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 6'090'000u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 6'120'000u),
           "fire command should enqueue");
    server.tickOnce(6'130'000u);

    expect(targetEnemy.health < targetHealthBefore,
           "authoritative player-vs-enemy fire should hit the visible enemy body center");
}

void testSeenPositionRuleUsesLagCompensatedRewindForHumanFire() {
    net::ServerConfig config;
    config.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{51u, 0u, "shooter"}, 7'000'000u, &shooterWelcome, nullptr),
           "shooter client should connect for seen-position testing");
    expect(server.acceptClient(net::HelloMessage{52u, 0u, "target"}, 7'000'000u, &targetWelcome, nullptr),
           "target client should connect for seen-position testing");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "server should track both players for seen-position testing");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(7'050'000u);
    server.takePendingPackets();

    target->position = sim::Vec3{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(7'150'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, sim::Vec3{5.0f, Config::PLAYER_EYE_HEIGHT, -10.0f});
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 7'150'000u;
    fire.interpDelayMs = 50u;
    fire.controlViewedServerTimeUs = 7'200'000u;
    fire.controlInterpDelayMs = 50u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 7'190'000u),
           "seen-position fire command should enqueue");
    server.tickOnce(7'200'000u);

    expect(target->health < target->maxHealth,
           "seen-position mode should rewind to the visible target position and register the hit");
}

void testSeenPositionRuleRetains500MsHistoryAtHighTickRates() {
    net::ServerConfig config;
    config.tickRateHz = 240u;
    config.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{55u, 0u, "shooter"}, 7'500'000u, &shooterWelcome, nullptr),
           "shooter client should connect for the high-rate lag-compensation history test");
    expect(server.acceptClient(net::HelloMessage{56u, 0u, "target"}, 7'500'000u, &targetWelcome, nullptr),
           "target client should connect for the high-rate lag-compensation history test");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "server should track both players for the high-rate lag-compensation history test");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};

    std::uint64_t nowUs = 7'504'167u;
    server.tickOnce(nowUs);
    server.takePendingPackets();

    for (int tick = 0; tick < 72; ++tick) {
        nowUs += 4'167u;
        server.tickOnce(nowUs);
        server.takePendingPackets();
    }

    target->position = sim::Vec3{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    nowUs += 4'167u;
    server.tickOnce(nowUs);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f});
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = nowUs;
    fire.interpDelayMs = 300u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, nowUs + 1'000u),
           "high-rate seen-position fire command should enqueue");
    nowUs += 4'167u;
    server.tickOnce(nowUs);

    expect(target->health < target->maxHealth,
           "seen-position mode should retain roughly 300 ms of rewind history even after more than 64 high-rate samples");
}

void testLivePositionRuleUsesControlGhostEvaluationForHumanFire() {
    net::ServerConfig config;
    config.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{61u, 0u, "shooter"}, 8'000'000u, &shooterWelcome, nullptr),
           "shooter client should connect for live-position testing");
    expect(server.acceptClient(net::HelloMessage{62u, 0u, "target"}, 8'000'000u, &targetWelcome, nullptr),
           "target client should connect for live-position testing");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "server should track both players for live-position testing");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(8'050'000u);
    server.takePendingPackets();

    target->position = sim::Vec3{5.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(8'150'000u);
    server.takePendingPackets();

    target->position = sim::Vec3{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(8'250'000u);
    server.takePendingPackets();

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, sim::Vec3{2.5f, Config::PLAYER_EYE_HEIGHT, -10.0f});
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 8'050'000u;
    fire.interpDelayMs = 50u;
    fire.controlViewedServerTimeUs = 8'150'000u;
    fire.controlInterpDelayMs = 50u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 8'290'000u),
           "live-position fire command should enqueue");
    server.tickOnce(8'300'000u);

    expect(target->health < target->maxHealth,
           "live-position mode should evaluate against the clean ghost track when it is available");
}

void testHostShotEvaluationModeChangeStagesForNextTickAndBecomesAuthoritative() {
    net::ServerConfig config;
    config.snapshotRateHz = config.tickRateHz;
    config.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{71u, 0u, "shooter"}, 9'000'000u, &shooterWelcome, nullptr),
           "shooter client should connect for staged shot-mode testing");
    expect(server.acceptClient(net::HelloMessage{72u, 0u, "target"}, 9'000'000u, &targetWelcome, nullptr),
           "target client should connect for staged shot-mode testing");
    server.worldState().enemies.clear();
    server.worldState().enemySpawns.clear();
    server.worldState().enemyWaypointIndices.clear();
    server.worldState().enemyRespawnTimers.clear();
    server.takePendingPackets();

    sim::PlayerState* shooter = sim::findPlayer(&server.worldState(), shooterWelcome.assignedPeerId);
    sim::PlayerState* target = sim::findPlayer(&server.worldState(), targetWelcome.assignedPeerId);
    expect(shooter != nullptr && target != nullptr,
           "server should track both players for staged shot-mode testing");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(9'050'000u);
    server.takePendingPackets();

    expect(server.handleControlPayload(
               shooterWelcome.assignedPeerId,
               net::PacketPayload{net::RuntimeParamChangeRequest{
                   net::RuntimeParamScope::Session,
                   -1,
                   "sv.shot_mode",
                   static_cast<float>(static_cast<std::uint8_t>(net::ShotEvaluationMode::LivePosition))
               }},
               9'060'000u),
           "host shot-evaluation change requests should be accepted through the authoritative control path");
    expect(server.config().shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "staged shot-evaluation changes should not mutate live server state before the next tick");
    const auto stagedPackets = server.takePendingPackets();
    const auto stagedResult = requireRuntimeParamApplyResultForPeer(
        stagedPackets, shooterWelcome.assignedPeerId, "sv.shot_mode");
    expect(!stagedResult.applied &&
               stagedResult.stagedApplyBoundary == sim::StagedApplyBoundary::NextTick &&
               stagedResult.message == "staged_for_next_tick",
           "shot-evaluation change requests should surface deterministic next-tick staging feedback");

    target->position = sim::Vec3{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(9'150'000u);
    const auto authoritativePackets = server.takePendingPackets();
    const auto& authoritativeSnapshot = requireSnapshotForPeer(authoritativePackets, shooterWelcome.assignedPeerId);
    expect(server.config().shotEvaluationMode == net::ShotEvaluationMode::LivePosition &&
               authoritativeSnapshot.sessionMetadata.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "the next authoritative tick should publish the newly selected shot-evaluation mode");

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, sim::Vec3{5.0f, Config::PLAYER_EYE_HEIGHT, -10.0f});
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 9'050'000u;
    fire.interpDelayMs = 50u;

    net::CommandBundle commands;
    commands.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, commands, 9'190'000u),
           "post-stage fire command should enqueue");
    server.tickOnce(9'200'000u);

    expect(target->health == target->maxHealth,
           "shots after the next authoritative tick should use the staged live-position rule instead of the old rewind rule");
}

}  // namespace

int main() {
    try {
        testSharedRifleHitscanMatchesSinglePlayerReferenceShape();
        testServerGatewayOwnsMembershipAndDeterministicPacketPublication();
        testCommandsApplyInSequenceOrder();
        testDuplicateCommandsIgnored();
        testMalformedAndStaleCommandsAreDroppedBeforeMutation();
        testTwoClientsAcceptedAndInitialSnapshotsEmitted();
        testAuthoritativeTeamAssignmentsRoundTripThroughSnapshots();
        testExplicitTeamRequestsOverrideBalancedAssignment();
        testAdditionalParticipantsFavorSmallerTeamByDefault();
        testConfiguredBotsSpawnIntoAuthoritativeRosterWithDistinctActorIds();
        testAuthoredLevelBotsSpawnIntoBalancedAuthoritativeRoster();
        testAuthoredLevelBotsBalanceAgainstHostTeamAndScoreAsPlayers();
        testAuthoredLevelBotKillsCountAsPlayerKills();
        testWelcomeIncludesConfiguredLevelIdentity();
        testDuplicateHelloReusesExistingPeerAssignment();
        testLateJoinInitialSnapshotCarriesCurrentAuthoritativeWorldState();
        testPlayerVsPlayerFireUsesSharedHitscanPath();
        testFriendlyFireIsIgnoredForSameTeamPlayers();
        testTeamChangeControlRequestUpdatesRosterAndRespawnsPlayer();
        testRuntimeParamControlRequestsProduceAppliedAndStagedFeedback();
        testInvalidSessionTickRateRequestsAreRejectedDeterministically();
        testHostCanToggleStudyEventLoggingWithoutReplacingRunId();
        testHostCanToggleVisualizationModeAndReplicateMetadata();
        testStudyActionHostCanSpawnFrozenPassiveBotAhead();
        testStudyActionRejectsGuestRequests();
        testStudyActionRejectsNonStudySessions();
        testStudyActionRejectsInvalidSpawnLocation();
        testHostRuntimeParamRequestsCanTargetOtherParticipants();
        testGuestRuntimeParamRequestsCannotTargetOtherParticipants();
        testHostScoreboardAdminCanSwitchAndKickRemotePlayers();
        testHostScoreboardAdminCanSwitchBotTeams();
        testHostScoreboardAdminCanAddBalancedBots();
        testGuestScoreboardAdminRequestsAreRejectedAuthoritatively();
        testGuestSelfTransportSettingsRemainEditableButSyncSettingsRequireHost();
        testHostParticipantRuntimeSettingRequestsReplicateThroughRosterSnapshots();
        testControlSwitchRequestsTransferAuthorityToEligibleBots();
        testSpectatorFollowRequestsCycleTargetsAndRejectRemoteHumanControl();
        testKillUpdatesRosterStatsAndTeamScore();
        testPlayersStayDownThenRespawnAfterAuthoritativeDelay();
        testBotsStayDownThenRespawnAfterAuthoritativeDelay();
        testDefenderKillUpdatesRosterStatsAndTeamScore();
        testDuplicateCommandsAndLaterSnapshotsDoNotDoubleCountScore();
        testDisconnectedClientPrunesRosterButPreservesTeamScore();
        testBotCombatUpdatesRosterStatsAndTeamScore();
        testObstacleBlocksAuthoritativeHitscanDamage();
        testBotReactionDelayPreventsImmediateOpeningShot();
        testBotsStartFrozenUntilHostActivatesDirector();
        testHostCanToggleBotPeaceModeAndReplicateMetadata();
        testPeaceModeBotsMoveWithoutShooting();
        testPeaceModeSuppressesQueuedDelayedBotFire();
        testBotBehaviorUsesImperfectAimCooldownAndLateralMovement();
        testBotsRemainServerOwnedWithoutHumanCommandSessions();
        testBotTransportOverridesAffectAuthoritativeBotCombat();
        testBotLatencyPublishesControlGhostTrack();
        testLivePositionRuleUsesBotControlGhostEvaluation();
        testHostTickRateChangeStagesForNextTickAndBecomesAuthoritative();
        testHostSnapshotRateChangeStagesForNextTickAndBecomesAuthoritative();
        testPlayerVsEnemyFireUsesBodyCenteredLagCompTarget();
        testSeenPositionRuleUsesLagCompensatedRewindForHumanFire();
        testSeenPositionRuleRetains500MsHistoryAtHighTickRates();
        testLiveTickRateChangePreserves500MsHistoryAtHigherRates();
        testLivePositionRuleUsesControlGhostEvaluationForHumanFire();
        testHostShotEvaluationModeChangeStagesForNextTickAndBecomesAuthoritative();
        std::cout << "ServerRuntimeTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ServerRuntimeTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
