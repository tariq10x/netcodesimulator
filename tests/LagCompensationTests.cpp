#include "Config3D.hpp"
#include "net/LagCompensation.hpp"
#include "net/ServerRuntime.hpp"
#include "server/BotCommandSource.hpp"
#include "server/LagCompensationService.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

float yawFor(const sim::Vec3& origin, const sim::Vec3& target) {
    return std::atan2(target.x - origin.x, origin.z - target.z);
}

std::uint64_t subtractInterpDelayUs(std::uint64_t viewedServerTimeUs,
                                    std::uint32_t interpDelayMs) {
    const std::uint64_t interpDelayUs =
        static_cast<std::uint64_t>(interpDelayMs) * 1'000u;
    if (viewedServerTimeUs <= interpDelayUs) {
        return 0u;
    }
    return viewedServerTimeUs - interpDelayUs;
}

void testMovingTargetCanBeHitAtVisibleInterpolatedPosition() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{100u, 0u, "shooter"}, 900'000u, &shooterWelcome, nullptr),
           "shooter client should connect");
    expect(server.acceptClient(net::HelloMessage{200u, 0u, "target"}, 900'000u, &targetWelcome, nullptr),
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
    server.tickOnce(1'000'000u);
    server.takePendingPackets();

    target->position = sim::Vec3{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(1'100'000u);
    server.takePendingPackets();

    const sim::Vec3 rewoundVisibleTarget{5.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, rewoundVisibleTarget);
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 1'100'000u;
    fire.interpDelayMs = 50u;

    net::CommandBundle bundle;
    bundle.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, bundle, 1'140'000u),
           "fire command should enqueue");
    server.tickOnce(1'150'000u);

    expect(target->health < target->maxHealth,
           "lag-compensated rewind should allow the moving target to be hit");
}

void testRewindTimeIsClampedToMaximumWindow() {
    net::LagCompensationHistory history(net::LagCompensationConfig{100'000u, 16u});
    const std::uint64_t clamped =
        history.clampRewindTime(subtractInterpDelayUs(50'000u, 100u), 1'400'000u);
    expect(clamped == 1'300'000u,
           "requested rewind time after interpolation-delay subtraction should clamp safely to the retained window");
}

void testLagCompensationServiceMatchesHistoryPolicy() {
    const net::LagCompensationConfig config{100'000u, 16u};
    net::LagCompensationHistory history(config);
    net::server::LagCompensationService service(config);
    const std::uint64_t requested = subtractInterpDelayUs(50'000u, 100u);

    expect(service.clampRewindTime(requested, 1'400'000u) ==
               history.clampRewindTime(requested, 1'400'000u),
           "explicit lag-compensation service should preserve the historical rewind clamp policy");

    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(0u, sim::MovementEnvironment{}, simConfig);
    service.recordWorldState(world, simConfig, 2'000'000u);
    const auto targets = service.rewindTargets(2'000'000u, 2'000'000u);
    expect(!targets.empty(),
           "explicit lag-compensation service should publish rewind targets after recording world state");
}

void testMovingTargetHitUsesClampedInterpolatedRewindTime() {
    net::ServerConfig config;
    config.maxRewindMs = 100u;
    net::ServerRuntime server(config);

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{500u, 0u, "shooter"}, 1'200'000u, &shooterWelcome, nullptr),
           "shooter client should connect");
    expect(server.acceptClient(net::HelloMessage{600u, 0u, "target"}, 1'200'000u, &targetWelcome, nullptr),
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
    server.tickOnce(1'250'000u);
    server.takePendingPackets();

    target->position = sim::Vec3{20.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    server.tickOnce(1'350'000u);
    server.takePendingPackets();

    const sim::Vec3 clampedVisibleTarget{10.0f, Config::PLAYER_EYE_HEIGHT, -10.0f};
    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = yawFor(shooter->position, clampedVisibleTarget);
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 1'320'000u;
    fire.interpDelayMs = 100u;

    net::CommandBundle bundle;
    bundle.commands.push_back(fire);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, bundle, 1'390'000u),
           "fire command should enqueue");
    server.tickOnce(1'400'000u);

    expect(target->health < target->maxHealth,
           "interpolation-delay rewind should clamp to the retained history window and still hit the visible target");
}

void testEnemyTargetsAreRecordedFromBodyCenter() {
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(0u, sim::MovementEnvironment{}, simConfig);
    expect(!world.enemies.empty(), "default world should contain an enemy target");

    net::LagCompensationHistory history;
    history.recordWorldState(world, simConfig, 2'000'000u);

    const auto targets = history.rewindTargets(2'000'000u, 2'000'000u);
    const auto targetIt = std::find_if(
        targets.begin(),
        targets.end(),
        [enemyId = world.enemies.front().entityId](const sim::HitscanTarget& target) {
            return target.entityId == enemyId;
        });
    expect(targetIt != targets.end(),
           "lag-comp history should emit a rewind target for the recorded enemy");
    expect(std::fabs(targetIt->center.y - (Config::ENEMY_BODY_HEIGHT * 0.5f)) <= 0.0001f,
           "non-player rewind targets should be anchored at body center instead of ground/root position");
}

void testPlayerTargetsAreRecordedFromVisibleBodyCenter() {
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(1u, sim::MovementEnvironment{}, simConfig);
    world.enemies.clear();
    world.enemySpawns.clear();
    world.enemyWaypointIndices.clear();
    world.enemyRespawnTimers.clear();

    sim::ensurePlayer(&world, 7, sim::Vec3{0.0f, simConfig.playerEyeHeight, -10.0f}, simConfig);

    net::LagCompensationHistory history;
    history.recordWorldState(world, simConfig, 2'100'000u);

    const auto targets = history.rewindTargets(2'100'000u, 2'100'000u);
    const auto targetIt = std::find_if(
        targets.begin(),
        targets.end(),
        [](const sim::HitscanTarget& target) { return target.entityId == 7; });
    expect(targetIt != targets.end(),
           "lag-comp history should emit a rewind target for recorded human players");
    expect(std::fabs(targetIt->center.y - (simConfig.playerCollisionHeight * 0.5f)) <= 0.0001f,
           "player rewind targets should be anchored at visible body center instead of eye position");
    expect(std::fabs(targetIt->radius -
                     std::max(simConfig.playerRadius, simConfig.playerCollisionHeight * 0.5f)) <= 0.0001f,
           "player rewind targets should use a body-sized radius instead of collision width alone");
}

void testBotCommandSourceTargetsOpposingActorsByBotId() {
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(0u, sim::MovementEnvironment{}, simConfig);
    world.players.clear();
    world.roster.clear();
    world.enemies.clear();
    world.enemySpawns.clear();
    world.enemyWaypointIndices.clear();
    world.enemyRespawnTimers.clear();

    sim::ensurePlayer(&world, 1000, sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, 12.0f}, simConfig);
    sim::ensurePlayer(&world, 5, sim::Vec3{0.0f, Config::PLAYER_EYE_HEIGHT, -12.0f}, simConfig);
    if (sim::RosterEntry* botEntry = sim::findRosterEntry(&world, 1000)) {
        botEntry->team = sim::TeamId::Attacker;
        botEntry->isBot = true;
    }
    if (sim::RosterEntry* targetEntry = sim::findRosterEntry(&world, 5)) {
        targetEntry->team = sim::TeamId::Defender;
        targetEntry->isBot = false;
    }

    net::server::BotCommandSource source;
    const sim::PlayerCommand command = source.buildCommand(world, 1000, sim::TeamId::Attacker, 0.1f);

    expect(command.moveY == 1.0f,
           "bot command source should preserve simple advance behavior toward the nearest opposing actor");
    expect(command.has(sim::CommandButton::Fire),
           "bot command source should preserve simple fire behavior for an aligned opposing actor");
}

void testNonFireCommandsDoNotEnterLagCompensationHitPath() {
    net::ServerRuntime server;

    net::WelcomeMessage shooterWelcome;
    net::WelcomeMessage targetWelcome;
    expect(server.acceptClient(net::HelloMessage{300u, 0u, "shooter"}, 2'000'000u, &shooterWelcome, nullptr),
           "shooter client should connect");
    expect(server.acceptClient(net::HelloMessage{400u, 0u, "target"}, 2'000'000u, &targetWelcome, nullptr),
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
    const float healthBefore = target->health;

    sim::PlayerCommand movementOnly;
    movementOnly.seq = 1u;
    movementOnly.dtSeconds = 0.016f;
    movementOnly.moveY = 1.0f;
    movementOnly.viewedServerTimeUs = 1'950'000u;

    net::CommandBundle bundle;
    bundle.commands.push_back(movementOnly);
    expect(server.enqueueCommandBundle(shooterWelcome.assignedPeerId, bundle, 2'010'000u),
           "movement command should enqueue");
    server.tickOnce(2'020'000u);

    expect(target->health == healthBefore,
           "non-fire commands should not trigger lag-compensated hits in the MVP server path");
}

}  // namespace

int main() {
    try {
        testMovingTargetCanBeHitAtVisibleInterpolatedPosition();
        testRewindTimeIsClampedToMaximumWindow();
        testLagCompensationServiceMatchesHistoryPolicy();
        testMovingTargetHitUsesClampedInterpolatedRewindTime();
        testEnemyTargetsAreRecordedFromBodyCenter();
        testPlayerTargetsAreRecordedFromVisibleBodyCenter();
        testBotCommandSourceTargetsOpposingActorsByBotId();
        testNonFireCommandsDoNotEnterLagCompensationHitPath();
        std::cout << "LagCompensationTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "LagCompensationTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
