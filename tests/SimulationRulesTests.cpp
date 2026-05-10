#include "Config3D.hpp"
#include "sim/SimulationRules.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool nearlyEqual(float lhs, float rhs, float epsilon = 0.001f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectNear(float actual, float expected, const std::string& label, float epsilon = 0.001f) {
    if (!nearlyEqual(actual, expected, epsilon)) {
        throw std::runtime_error(label + ": expected " + std::to_string(expected) +
                                 ", got " + std::to_string(actual));
    }
}

void testForwardMovement() {
    sim::PlayerState player;
    player.position = {0.0f, Config::PLAYER_EYE_HEIGHT, 0.0f};
    player.yaw = 0.0f;

    sim::PlayerCommand command;
    command.dtSeconds = 0.25f;
    command.moveY = 1.0f;
    command.yaw = 0.0f;
    command.pitch = 0.0f;

    const sim::PlayerState next = sim::applyPlayerCommand(player, command, sim::MovementEnvironment{});

    expectNear(next.position.x, 0.0f, "forward movement x");
    expectNear(next.position.z, -2.0f, "forward movement z");
    expect(next.grounded, "player should remain grounded after flat movement");
}

void testJumpRule() {
    sim::PlayerState player;
    player.position = {0.0f, Config::PLAYER_EYE_HEIGHT, 0.0f};

    sim::PlayerCommand command;
    command.dtSeconds = 0.1f;
    command.buttons = sim::commandButtonBit(sim::CommandButton::Jump);

    const sim::PlayerState next = sim::applyPlayerCommand(player, command, sim::MovementEnvironment{});

    expectNear(next.velocity.y, 8.6f, "jump vertical velocity");
    expectNear(next.position.y, 2.56f, "jump position y");
    expect(next.jumpsUsed == 1, "jump count should increment");
    expect(!next.grounded, "jump should leave the player airborne");
}

void testCollisionResolution() {
    sim::MovementEnvironment environment;
    environment.collisionBoxes.push_back(sim::CollisionBox{
        {0.0f, 1.4f, -3.0f},
        {4.0f, 1.4f, 1.0f}
    });

    sim::PlayerState player;
    player.position = {0.0f, Config::PLAYER_EYE_HEIGHT, -1.0f};

    sim::PlayerCommand command;
    command.dtSeconds = 0.5f;
    command.moveY = 1.0f;
    command.yaw = 0.0f;

    const sim::PlayerState next = sim::applyPlayerCommand(player, command, environment);
    const sim::CollisionBox& box = environment.collisionBoxes.front();

    const bool overlaps =
        next.position.x + Config::ENEMY_BODY_RADIUS > box.center.x - box.halfSize.x &&
        next.position.x - Config::ENEMY_BODY_RADIUS < box.center.x + box.halfSize.x &&
        next.position.z + Config::ENEMY_BODY_RADIUS > box.center.z - box.halfSize.z &&
        next.position.z - Config::ENEMY_BODY_RADIUS < box.center.z + box.halfSize.z;

    expect(!overlaps, "resolved position should remain outside the blocking obstacle");
}

void testFireCooldown() {
    sim::PlayerState player;

    sim::PlayerCommand fireCommand;
    fireCommand.dtSeconds = 0.1f;
    fireCommand.buttons = sim::commandButtonBit(sim::CommandButton::Fire);

    const sim::PlayerState firstFire = sim::applyPlayerCommand(player, fireCommand, sim::MovementEnvironment{});
    expectNear(firstFire.weaponCooldownRemaining, Config::SHOOT_COOLDOWN, "weapon cooldown starts");

    const sim::PlayerState secondFire = sim::applyPlayerCommand(firstFire, fireCommand, sim::MovementEnvironment{});
    expect(secondFire.weaponCooldownRemaining < firstFire.weaponCooldownRemaining,
           "cooldown should continue counting down instead of firing again");
    expectNear(secondFire.weaponCooldownRemaining, Config::SHOOT_COOLDOWN - 0.1f, "weapon cooldown decremented");
}

void testHitscanTrace() {
    sim::HitscanRay ray;
    ray.origin = {0.0f, 0.0f, 0.0f};
    ray.direction = {0.0f, 0.0f, -1.0f};
    ray.maxDistance = 50.0f;

    std::vector<sim::HitscanTarget> targets{
        sim::HitscanTarget{1, {0.0f, 0.0f, -8.0f}, 1.0f, true},
        sim::HitscanTarget{2, {0.0f, 0.0f, -20.0f}, 1.0f, true}
    };

    const sim::FireResult result = sim::traceHitscan(ray, targets);

    expect(result.fired, "hitscan should report that a shot was traced");
    expect(result.hit, "hitscan should hit the first sphere target");
    expect(result.hitEntityId == 1, "hitscan should select the closest target");
    expect(result.hitDistance > 0.0f, "hit distance should be positive");
}

void testPlayerHitscanTargetsUseVisibleBodyCenter() {
    sim::SimConfig config;
    sim::PlayerState player;
    player.playerId = 7;
    player.position = {0.0f, config.playerEyeHeight, -10.0f};

    const sim::HitscanTarget target = sim::buildPlayerHitscanTarget(player, config);

    expect(target.entityId == player.playerId, "player hitscan target should preserve the actor id");
    expectNear(target.center.y,
               config.playerCollisionHeight * 0.5f,
               "player hitscan targets should anchor to visible body center");
    expectNear(target.radius,
               std::max(config.playerRadius, config.playerCollisionHeight * 0.5f),
               "player hitscan targets should use a body-sized radius");
}

void testAuthoritativeReplayOfRemainingCommandsKeepsFullGameplayStateAligned() {
    sim::MovementEnvironment environment;
    sim::PlayerState start;
    start.position = {0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};

    sim::PlayerCommand first;
    first.dtSeconds = 0.1f;
    first.yaw = 0.35f;
    first.pitch = -0.2f;
    first.buttons = sim::commandButtonBit(sim::CommandButton::Jump) |
                    sim::commandButtonBit(sim::CommandButton::Fire);

    sim::PlayerCommand second;
    second.dtSeconds = 0.1f;
    second.moveY = 1.0f;
    second.yaw = 0.8f;
    second.pitch = 0.15f;

    const sim::PlayerState authoritative = sim::applyPlayerCommand(start, first, environment);
    const sim::PlayerState replayed = sim::applyPlayerCommand(authoritative, second, environment);

    expectNear(replayed.position.x, 0.573885f, "authoritative replay x", 0.001f);
    expectNear(replayed.position.y, 3.180000f, "authoritative replay y", 0.001f);
    expectNear(replayed.position.z, 4.443293f, "authoritative replay z", 0.001f);
    expectNear(replayed.yaw, second.yaw, "authoritative replay yaw");
    expectNear(replayed.pitch, second.pitch, "authoritative replay pitch");
    expectNear(replayed.weaponCooldownRemaining,
               Config::SHOOT_COOLDOWN - second.dtSeconds,
               "authoritative replay weapon cooldown");
    expect(replayed.jumpsUsed == 1,
           "authoritative replay should preserve jump-state counters");
    expect(!replayed.grounded,
           "authoritative replay should preserve airborne state after the jump remains unresolved");
}

}  // namespace

int main() {
    try {
        testForwardMovement();
        testJumpRule();
        testCollisionResolution();
        testFireCooldown();
        testHitscanTrace();
        testPlayerHitscanTargetsUseVisibleBodyCenter();
        testAuthoritativeReplayOfRemainingCommandsKeepsFullGameplayStateAligned();
        std::cout << "SimulationRulesTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "SimulationRulesTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
