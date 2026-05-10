#include "net/ClientRuntime.hpp"

#include "app/AppFlow.hpp"
#include "LevelData.hpp"
#include "TestDataRoot.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "server/AuthoritativeSimulation.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using ClientAccess = net::ClientRuntimeTestAccess;

constexpr std::uint16_t kStudyModeAttackerBotCount = 1u;
constexpr std::uint16_t kStudyModeDefenderBotCount = 1u;
constexpr std::uint16_t kCombatStudyPublicPort = 48300u;
constexpr std::uint16_t kCombatStudyDiscoveryPort = 48301u;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool nearlyEqual(float lhs, float rhs, float epsilon = 0.001f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

bool playerPositionsChangedFromBaseline(const std::vector<sim::PlayerState>& baseline,
                                        const std::vector<sim::PlayerState>& current,
                                        float epsilon = 0.05f) {
    for (const auto& player : current) {
        const auto baselineIt =
            std::find_if(baseline.begin(),
                         baseline.end(),
                         [&player](const sim::PlayerState& candidate) {
                             return candidate.playerId == player.playerId;
                         });
        if (baselineIt == baseline.end()) {
            return true;
        }
        if (!nearlyEqual(player.position.x, baselineIt->position.x, epsilon) ||
            !nearlyEqual(player.position.z, baselineIt->position.z, epsilon)) {
            return true;
        }
    }
    return current.size() != baseline.size();
}

template <typename Predicate>
bool waitForPredicate(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

void ensureTestLevelExists(int slot) {
    LevelData::Level level("Combat Parity Level " + std::to_string(slot));
    level.obstacles.push_back(LevelData::Obstacle{
        static_cast<float>(slot),
        -static_cast<float>(slot),
        6.0f,
        3.0f,
        2.5f,
        Color{120, 200, 120, 255}
    });
    expect(LevelData::saveLevel(level, slot),
           "combat parity level fixture should save successfully");
}

net::SessionLaunchConfig makeStudyLaunchConfig(int levelSlot) {
    net::SessionLaunchConfig config =
        net::makeStudySessionLaunchConfig(levelSlot,
                                          "player",
                                          kCombatStudyPublicPort,
                                          kStudyModeAttackerBotCount,
                                          kStudyModeDefenderBotCount);
    config.discoveryPort = kCombatStudyDiscoveryPort;
    return config;
}

std::unique_ptr<net::ClientRuntime> makeConnectedClient() {
    net::ClientConfig config;
    config.serverPort = 45970u;
    config.sessionId = 7001u;

    auto client = std::make_unique<net::ClientRuntime>(config);
    expect(client->start(), "client should bind a local socket for combat parity tests");
    ClientAccess::forceConnectedSnapshot(*client, 1u, 1'000'000u);
    return client;
}

void testImmediateLocalFireFeedbackAppearsBeforeSnapshot() {
    auto client = makeConnectedClient();

    InputHandler3D::InputState input;
    input.firePressed = true;

    client->update(1.0f / 60.0f, &input);

    expect(ClientAccess::combatTraceCount(*client) == 1u,
           "firing should create an immediate local combat trace before any snapshot arrives");
    expect(!ClientAccess::firstCombatTraceAuthoritative(*client),
           "the first immediate trace should be predictive local feedback");
    expect(ClientAccess::firstCombatTraceThickness(*client) >= 0.05f,
           "predictive local fire should create a thick beam presentation object");
    expect(client->lastCombatEventText() == "Shot fired (pending)",
           "local fire feedback should remain visible while awaiting authoritative confirmation");
}

void testMissRemainsVisibleWithoutDamageEvent() {
    auto client = makeConnectedClient();

    InputHandler3D::InputState input;
    input.firePressed = true;
    client->update(1.0f / 60.0f, &input);

    net::WorldSnapshot snapshot = ClientAccess::latestSnapshot(*client);
    snapshot.serverTick = 1u;
    snapshot.serverTimeUs = 1'016'667u;
    snapshot.ackedInputSeq = ClientAccess::commandSeq(*client);

    net::Packet packet;
    packet.header.kind = net::PacketKind::WorldSnapshot;
    packet.header.peerId = client->peerId();
    packet.header.seq = 1u;
    packet.payload = snapshot;
    ClientAccess::handlePacket(*client, packet);

    expect(ClientAccess::combatTraceCount(*client) == 1u,
           "local fire feedback should remain visible even when no authoritative damage event follows");
    expect(ClientAccess::firstCombatTraceThickness(*client) >= 0.05f,
           "a miss should remain readable through the thick beam presentation");
    expect(client->lastCombatEventText() == "Shot fired (pending)",
           "a miss without authoritative damage should not erase the visible fired-shot feedback");
}

void testAuthoritativeCombatEventsConsumedExactlyOnce() {
    auto client = makeConnectedClient();

    net::WorldSnapshot snapshot = ClientAccess::latestSnapshot(*client);
    snapshot.serverTick = 2u;
    snapshot.serverTimeUs = 1'033'334u;
    snapshot.events = {
        net::SnapshotEvent{
            net::SnapshotEventKind::WeaponFired,
            1,
            2,
            sim::Vec3{0.2f, 1.45f, 4.65f},
            sim::Vec3{0.0f, 0.0f, -1.0f},
            true
        },
        net::SnapshotEvent{
            net::SnapshotEventKind::ConfirmedHit,
            1,
            2,
            sim::Vec3{0.2f, 1.45f, 4.65f},
            sim::Vec3{0.0f, 0.0f, -1.0f},
            true
        },
        net::SnapshotEvent{
            net::SnapshotEventKind::DamageApplied,
            1,
            2,
            sim::Vec3{0.2f, 1.45f, 4.65f},
            sim::Vec3{0.0f, 0.0f, -1.0f},
            true
        }
    };

    net::Packet packet;
    packet.header.kind = net::PacketKind::WorldSnapshot;
    packet.header.peerId = client->peerId();
    packet.header.seq = 42u;
    packet.payload = snapshot;

    ClientAccess::handlePacket(*client, packet);
    expect(ClientAccess::consumedCombatEventCount(*client) == 3u,
           "the client should consume each authoritative combat event in the accepted snapshot");
    expect(ClientAccess::combatTraceCount(*client) == 1u,
           "authoritative weapon-fired events should create one authoritative combat trace");
    expect(ClientAccess::firstCombatTraceThickness(*client) >= 0.05f,
           "authoritative weapon-fired events should use the thick beam presentation");

    ClientAccess::handlePacket(*client, packet);
    expect(ClientAccess::consumedCombatEventCount(*client) == 3u,
           "duplicate snapshots with the same sequence should not consume combat events a second time");
    expect(ClientAccess::combatTraceCount(*client) == 1u,
           "duplicate snapshots should not create duplicate authoritative combat traces");
}

void testAuthoritativeSnapshotPackagesCompleteServerOwnedState() {
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(2u, sim::MovementEnvironment{}, simConfig);
    world.teamScores.attackers = 3u;
    world.teamScores.defenders = 1u;
    world.cadence = sim::TimingCadence{60u, 20u, 60u};
    world.authoritativeTime = sim::AuthoritativeTime{61u, 1'016'667u, 1'000'000u};
    world.sessionMetadata.sessionId = 7001u;
    world.sessionMetadata.levelSlot = 8;
    world.sessionMetadata.levelHash = net::makeLevelIdentityHash(8);
    world.sessionMetadata.maxHumanPlayers = 2u;

    net::ClientSession hostSession;
    hostSession.peerId = 1u;
    hostSession.sessionId = world.sessionMetadata.sessionId;
    hostSession.team = sim::TeamId::Attacker;
    hostSession.connected = true;
    hostSession.lastAckedInputSeq = 9u;
    hostSession.playerName = "host-player";
    hostSession.spawnPosition = world.playerSpawns.at(0);

    sim::ensurePlayer(&world, hostSession.peerId, hostSession.spawnPosition, simConfig);
    if (sim::RosterEntry* hostEntry = sim::findRosterEntry(&world, hostSession.peerId)) {
        hostEntry->team = sim::TeamId::Attacker;
        hostEntry->sessionPresence = sim::SessionPresence::Connected;
        hostEntry->participation = sim::ParticipationState::Playing;
        hostEntry->control = sim::ControlBinding{sim::ControlBindingKind::Actor,
                                                 static_cast<int>(hostSession.peerId)};
        hostEntry->displayName = hostSession.playerName;
    }

    const int botActorId = 1001;
    sim::ensurePlayer(&world, botActorId, world.playerSpawns.at(1), simConfig);
    if (sim::RosterEntry* botEntry =
            sim::findRosterEntry(&world, botActorId)) {
        botEntry->team = sim::TeamId::Defender;
        botEntry->sessionPresence = sim::SessionPresence::Connected;
        botEntry->participation = sim::ParticipationState::Playing;
        botEntry->control = sim::ControlBinding{sim::ControlBindingKind::Actor, botActorId};
        botEntry->isBot = true;
        botEntry->displayName = "BOT 1001";
    }

    std::vector<net::ClientSession> sessions{hostSession};
    net::syncConnectedSessionMetadata(&world, sessions);

    const net::AuthoritativeSnapshot snapshot = net::AuthoritativeSnapshot::fromSession(
        world, hostSession, net::ShotEvaluationMode::SeenPosition);

    expect(snapshot.worldState.players.size() == 2u,
           "authoritative snapshots should package both human and bot player actors through one world model");
    expect(snapshot.worldState.enemies.size() == 1u,
           "authoritative snapshots should retain server-owned entity state alongside player actors");
    expect(snapshot.worldState.teamScores.attackers == 3u &&
               snapshot.worldState.teamScores.defenders == 1u,
           "authoritative snapshots should preserve authoritative team-score state");
    expect(snapshot.worldState.sessionMetadata.levelSlot == 8 &&
               snapshot.worldState.sessionMetadata.levelHash == net::makeLevelIdentityHash(8) &&
               snapshot.worldState.sessionMetadata.connectedHumanPlayers == 1u &&
               snapshot.worldState.sessionMetadata.connectedBotPlayers == 1u,
           "authoritative snapshots should preserve session metadata and connected participant counts");
    expect(snapshot.localParticipantState.team == sim::TeamId::Attacker &&
               snapshot.localParticipantState.participation == sim::ParticipationState::Playing &&
               snapshot.localParticipantState.control.actorId == 1,
           "authoritative snapshots should preserve the local participant's authoritative gameplay identity");
    expect(snapshot.ackedInputSeq == 9u &&
               snapshot.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "authoritative snapshots should preserve acknowledged input sequencing and shot-rule ownership");
}

void testAuthoritativeSimulationProducesCombatEventsAndScoreForConfirmedHit() {
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(2u, sim::MovementEnvironment{}, simConfig);
    world.enemies.clear();
    world.enemySpawns.clear();
    world.enemyWaypointIndices.clear();
    world.enemyRespawnTimers.clear();

    net::ClientSession shooterSession;
    shooterSession.peerId = 1u;
    shooterSession.team = sim::TeamId::Attacker;
    shooterSession.connected = true;
    shooterSession.spawnPosition = world.playerSpawns.at(0);

    net::ClientSession targetSession;
    targetSession.peerId = 2u;
    targetSession.team = sim::TeamId::Defender;
    targetSession.connected = true;
    targetSession.spawnPosition = world.playerSpawns.at(1);

    world.sessionMetadata.sessionId = 7002u;
    world.sessionMetadata.levelSlot = 9;
    world.sessionMetadata.levelHash = net::makeLevelIdentityHash(9);
    world.sessionMetadata.maxHumanPlayers = 2u;

    sim::ensurePlayer(&world, shooterSession.peerId, shooterSession.spawnPosition, simConfig);
    sim::ensurePlayer(&world, targetSession.peerId, targetSession.spawnPosition, simConfig);
    if (sim::RosterEntry* shooterEntry = sim::findRosterEntry(&world, shooterSession.peerId)) {
        shooterEntry->team = sim::TeamId::Attacker;
    }
    if (sim::RosterEntry* targetEntry = sim::findRosterEntry(&world, targetSession.peerId)) {
        targetEntry->team = sim::TeamId::Defender;
    }

    sim::PlayerState* shooter = sim::findPlayer(&world, shooterSession.peerId);
    sim::PlayerState* target = sim::findPlayer(&world, targetSession.peerId);
    expect(shooter != nullptr && target != nullptr,
           "authoritative simulation parity setup should create both player actors");

    shooter->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
    target->position = sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    target->health = Config::SHOOT_DAMAGE;

    net::LagCompensationHistory lagCompensation;
    lagCompensation.recordWorldState(world, simConfig, 1'000'000u);

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 1'000'000u;
    shooterSession.pendingCommands.emplace(fire.seq, fire);

    std::vector<net::ClientSession> sessions;
    sessions.push_back(shooterSession);
    sessions.push_back(targetSession);
    net::syncConnectedSessionMetadata(&world, sessions);

    net::server::AuthoritativeSimulation simulation;
    net::server::StepContext context;
    context.dtSeconds = 1.0f / 60.0f;
    context.respawnDelaySeconds = 5.0f;
    context.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    context.cadence = sim::TimingCadence{60u, 20u, 60u};
    context.authoritativeTime = sim::AuthoritativeTime{61u, 1'016'667u, 1'000'000u};
    std::vector<net::SnapshotEvent> events;
    simulation.stepWorld(world,
                         sessions,
                         context,
                         simConfig,
                         lagCompensation,
                         &events);

    expect(target->health == 0.0f,
           "authoritative simulation should apply lethal rifle damage through the extracted seam");
    expect(world.teamScores.attackers == 1u && world.teamScores.defenders == 0u,
           "authoritative simulation should preserve authoritative team-score updates");
    expect(events.size() == 4u &&
               events[0].kind == net::SnapshotEventKind::WeaponFired &&
               events[1].kind == net::SnapshotEventKind::ConfirmedHit &&
               events[2].kind == net::SnapshotEventKind::DamageApplied &&
               events[3].kind == net::SnapshotEventKind::PlayerKilled,
           "authoritative simulation should emit deterministic combat events in fired-hit-damage-kill order");
    expect(world.cadence.authoritativeTickHz == context.cadence.authoritativeTickHz &&
               world.cadence.snapshotCadenceHz == context.cadence.snapshotCadenceHz &&
               world.authoritativeTime.serverTick == context.authoritativeTime.serverTick &&
               world.authoritativeTime.serverTimeUs == context.authoritativeTime.serverTimeUs,
           "authoritative step contexts should stamp cadence and server time onto the shared world seam");
    expect(world.sessionMetadata.connectedHumanPlayers == 2u &&
               world.sessionMetadata.levelHash == net::makeLevelIdentityHash(9),
           "authoritative world state should retain session metadata while deterministic gameplay stepping occurs");
}

void testStudyModeCompositionRunsOnRealRuntimeStackWithAuthoritativeBots() {
    ensureTestLevelExists(8);

    net::SessionLaunchConfig config = makeStudyLaunchConfig(8);
    config.clientSessionId = 0x08700001u;
    config.clientConnectTimeoutUs = 400'000u;
    config.clientHelloRetryIntervalUs = 20'000u;

    app::AppFlow::SessionStartResult sessionStart = app::AppFlow::startSession(config);
    expect(sessionStart.sessionFlow != nullptr,
           "study mode composition should always return a shared session controller");
    expect(sessionStart.started,
           "study mode should start through the same app-flow session composition used by multiplayer");

    std::unique_ptr<net::SessionFlowController> sessionFlow = std::move(sessionStart.sessionFlow);
    const bool running = waitForPredicate([&]() {
        sessionFlow->update(1.0f / 60.0f, nullptr);
        return sessionFlow->state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));

    expect(running,
           "study mode should reach the running state on the real runtime stack");
    expect(sessionFlow->hostedServer() != nullptr,
           "study mode should expose an authoritative hosted server once running");
    expect(config.surface == net::SessionProductSurface::LabStudy &&
               config.entryPoint == net::SessionEntryPoint::LabStudy &&
               sessionFlow->hostedServer()->worldState().environment.collisionBoxes.size() == 1u,
           "study mode compatibility launches should preserve the lab-study surface and load level geometry into the authoritative runtime");
    expect(sessionFlow->clientRuntime() != nullptr &&
               sessionFlow->clientRuntime()->hasLocalNetworkControls(),
           "study mode should preserve the host diagnostics and local-network study tools on the shared runtime path");

    std::size_t botCount = 0u;
    std::size_t attackerBots = 0u;
    std::size_t defenderBots = 0u;
    std::size_t humanCount = 0u;
    for (const auto& entry : sessionFlow->hostedServer()->worldState().roster) {
        if (entry.isBot) {
            ++botCount;
            if (entry.team == sim::TeamId::Attacker) {
                ++attackerBots;
            } else if (entry.team == sim::TeamId::Defender) {
                ++defenderBots;
            }
        } else {
            ++humanCount;
        }
    }

    expect(botCount == (kStudyModeAttackerBotCount + kStudyModeDefenderBotCount),
           "study mode should seed the configured authoritative bot roster through the shared host session");
    expect(attackerBots == kStudyModeAttackerBotCount &&
               defenderBots == kStudyModeDefenderBotCount &&
               humanCount == 1u,
           "study mode should keep the local player alongside one simple attacker bot and one simple defender bot");
}

void testStudyModeBotsDoNotFireImmediatelyOnStartup() {
    ensureTestLevelExists(7);

    net::SessionLaunchConfig config = makeStudyLaunchConfig(7);
    config.clientSessionId = 0x08700002u;
    config.clientConnectTimeoutUs = 400'000u;
    config.clientHelloRetryIntervalUs = 20'000u;

    app::AppFlow::SessionStartResult sessionStart = app::AppFlow::startSession(config);
    expect(sessionStart.sessionFlow != nullptr,
           "study respawn regression should produce a session controller");
    expect(sessionStart.started,
           "study respawn regression should start through the shared app-flow path");

    std::unique_ptr<net::SessionFlowController> sessionFlow = std::move(sessionStart.sessionFlow);
    const bool running = waitForPredicate([&]() {
        sessionFlow->update(1.0f / 60.0f, nullptr);
        return sessionFlow->state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running,
           "study startup regression should reach the running state before bot behavior is sampled");

    net::ClientRuntime* client = sessionFlow->clientRuntime();
    expect(client != nullptr &&
               client->hasAuthoritativeSessionMetadata() &&
               client->authoritativeSessionMetadata().botsFrozen,
           "study mode should start with the authoritative bot director frozen");

    const net::ServerRuntime* hostedServer = sessionFlow->hostedServer();
    expect(hostedServer != nullptr,
           "study mode bot-director regression requires an authoritative hosted server");
    const std::vector<sim::PlayerState> baselineAuthoritativePlayers = hostedServer->worldState().players;
    const std::vector<sim::PlayerState> baselineRemotePlayers = client->remotePlayers();

    bool sawImmediateBotFire = false;
    for (int frame = 0; frame < 60; ++frame) {
        sessionFlow->update(1.0f / 60.0f, nullptr);
        if (ClientAccess::hasCombatTraces(*client) || client->localPlayerState().health <= 0.0f) {
            sawImmediateBotFire = true;
            break;
        }
    }
    expect(!sawImmediateBotFire,
           "study mode bots should remain frozen instead of engaging before the host starts them");

    InputHandler3D::InputState startBotsInput;
    startBotsInput.toggleEnemyAI = true;
    sessionFlow->update(1.0f / 60.0f, &startBotsInput);

    bool sawServerBotActivity = false;
    bool sawClientDirectorActive = false;
    bool sawClientBotActivity = false;
    for (int frame = 0; frame < 480; ++frame) {
        sessionFlow->update(1.0f / 60.0f, nullptr);
        if (playerPositionsChangedFromBaseline(baselineAuthoritativePlayers,
                                               sessionFlow->hostedServer()->worldState().players) ||
            sessionFlow->hostedServer()->worldState().players.front().health <
                sessionFlow->hostedServer()->worldState().players.front().maxHealth) {
            sawServerBotActivity = true;
        }
        if (client->hasAuthoritativeSessionMetadata() &&
            !client->authoritativeSessionMetadata().botsFrozen) {
            sawClientDirectorActive = true;
            if (ClientAccess::hasCombatTraces(*client) ||
                playerPositionsChangedFromBaseline(baselineRemotePlayers, client->remotePlayers())) {
                sawClientBotActivity = true;
            }
        }
        if (sawServerBotActivity && sawClientDirectorActive && sawClientBotActivity) {
            break;
        }
    }
    expect(sawServerBotActivity,
           "study mode bots should move or fight in the authoritative world after activation");
    expect(sawClientDirectorActive,
           "study mode clients should receive the replicated active bot-director state");
    expect(sawClientBotActivity,
           "study mode clients should eventually observe bot movement or combat after activation");
}

}  // namespace

int main() {
    try {
        const testsupport::ScopedTestDataRoot scopedDataRoot("netcodesim-combat-parity");
        (void)scopedDataRoot;

        testImmediateLocalFireFeedbackAppearsBeforeSnapshot();
        testMissRemainsVisibleWithoutDamageEvent();
        testAuthoritativeCombatEventsConsumedExactlyOnce();
        testAuthoritativeSnapshotPackagesCompleteServerOwnedState();
        testAuthoritativeSimulationProducesCombatEventsAndScoreForConfirmedHit();
        testStudyModeCompositionRunsOnRealRuntimeStackWithAuthoritativeBots();
        testStudyModeBotsDoNotFireImmediatelyOnStartup();
        std::cout << "CombatParityTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "CombatParityTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
