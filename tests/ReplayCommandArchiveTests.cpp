#include "net/ServerRuntime.hpp"
#include "replay/ReplayArchive.hpp"
#include "replay/ReplayPlaybackRuntime.hpp"
#include "replay/ReplayRecorder.hpp"
#include "sim/SimulationRules.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool almostEqual(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.0001f;
}

int firstBotActorId(const sim::WorldState& world) {
    for (const sim::RosterEntry& entry : world.roster) {
        if (entry.isBot) {
            return entry.actorId;
        }
    }
    return -1;
}

sim::WorldState makeWorld() {
    sim::WorldState world = sim::createDefaultWorld(2u, sim::MovementEnvironment{});
    sim::ensurePlayer(&world, 1, sim::Vec3{0.0f, sim::defaults::kPlayerEyeHeight, 0.0f});
    sim::ensureRosterEntry(&world, 1, sim::TeamId::Attacker);
    world.cadence = sim::TimingCadence{60u, 20u, 60u};
    world.authoritativeTime = sim::AuthoritativeTime{4u, 66'668u, 66'668u};
    world.sessionMetadata.levelSlot = 3;
    world.sessionMetadata.levelHash = 44u;
    return world;
}

sim::WorldState makeCombatWorld() {
    sim::SimConfig simConfig;
    sim::WorldState world = sim::createDefaultWorld(2u, sim::MovementEnvironment{}, simConfig);
    world.enemies.clear();
    world.enemySpawns.clear();
    world.enemyWaypointIndices.clear();
    world.enemyRespawnTimers.clear();
    sim::ensurePlayer(&world, 1, sim::Vec3{0.0f, sim::defaults::kPlayerEyeHeight, 5.0f});
    sim::ensurePlayer(&world, 2, sim::Vec3{0.0f, sim::defaults::kPlayerEyeHeight, -10.0f});
    if (sim::RosterEntry* shooter = sim::findRosterEntry(&world, 1)) {
        shooter->team = sim::TeamId::Attacker;
        shooter->displayName = "shooter";
    }
    if (sim::RosterEntry* target = sim::findRosterEntry(&world, 2)) {
        target->team = sim::TeamId::Defender;
        target->displayName = "target";
    }
    world.cadence = sim::TimingCadence{60u, 20u, 60u};
    world.authoritativeTime = sim::AuthoritativeTime{4u, 66'668u, 66'668u};
    return world;
}

sim::PlayerCommand makeCommand(std::uint32_t seq, float yaw) {
    sim::PlayerCommand command;
    command.seq = seq;
    command.dtSeconds = 1.0f / 60.0f;
    command.yaw = yaw;
    command.pitch = 0.15f;
    command.viewedServerTimeUs = 50'000u;
    command.interpDelayMs = 100u;
    command.controlViewedServerTimeUs = 66'668u;
    command.controlInterpDelayMs = 100u;
    return command;
}

sim::PlayerCommand makeMoveCommand(std::uint32_t seq, float yaw = 0.0f) {
    sim::PlayerCommand command = makeCommand(seq, yaw);
    command.moveY = 1.0f;
    return command;
}

void testCommandReplayArchiveRoundTripsSchema() {
    replay::CommandReplayRecorder recorder;
    replay::ReplayRecordingInfo info;
    info.levelSlot = 3;
    info.levelHash = 44u;
    info.tickRateHz = 60u;
    info.snapshotRateHz = 20u;
    info.maxRewindMs = 750u;
    info.respawnDelaySeconds = 3.0f;
    info.spawnProtectionSeconds = 1.25f;
    info.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    info.startedServerTimeUs = 66'668u;
    info.title = "Command Archive Test";
    info.sourceLabel = "unit";

    const sim::WorldState world = makeWorld();
    recorder.start(info, world, sim::SimConfig{});
    recorder.recordCommandReceived(replay::ReplayTrack::Authoritative,
                                   1u,
                                   4u,
                                   66'668u,
                                   makeCommand(1u, 0.25f));
    recorder.recordCommandApplied(replay::ReplayTrack::Authoritative,
                                  1u,
                                  1,
                                  5u,
                                  83'335u,
                                  makeCommand(1u, 0.25f));
    recorder.recordCommandReceived(replay::ReplayTrack::Control,
                                   1u,
                                   4u,
                                   66'668u,
                                   makeCommand(1u, 0.25f));
    recorder.recordCommandApplied(replay::ReplayTrack::Control,
                                  1u,
                                  1,
                                  5u,
                                  83'335u,
                                  makeCommand(1u, 0.25f));
    recorder.recordAuthoritativeKeyframe(5u, 83'335u, world);
    recorder.stop(83'335u);

    net::ByteBuffer bytes;
    std::string error;
    expect(replay::ReplayArchive::writeBytes(recorder.demo(), &bytes, &error),
           "command replay archive should serialize: " + error);

    replay::ReplayDemo loaded;
    expect(replay::ReplayArchive::readBytes(bytes, &loaded, &error),
           "command replay archive should deserialize: " + error);
    expect(loaded.header.title == "Command Archive Test" &&
               loaded.header.levelSlot == 3 &&
               loaded.header.maxRewindMs == 750u &&
               almostEqual(loaded.header.respawnDelaySeconds, 3.0f) &&
               almostEqual(loaded.header.spawnProtectionSeconds, 1.25f) &&
               loaded.header.shotEvaluationMode == net::ShotEvaluationMode::LivePosition &&
               loaded.header.hasControlLane,
           "loaded command replay header should preserve metadata");
    expect(loaded.commandEvents.size() == 4u &&
               loaded.commandEvents[1].stage == replay::ReplayCommandStage::Applied &&
               loaded.commandEvents[1].command.seq == 1u,
           "loaded command replay should preserve command events");
    expect(loaded.keyframes.size() == 1u &&
               loaded.keyframes.front().worldState.players.size() == 1u,
           "loaded command replay should preserve keyframes");
}

void testReplayPlaybackAppliesAuthoritativeCommands() {
    replay::ReplayDemo demo;
    demo.header.startedServerTimeUs = 0u;
    demo.initialState.worldState = makeWorld();
    demo.initialState.simConfig = sim::SimConfig{};
    demo.commandEvents.push_back(replay::ServerCommandEvent{
        replay::ReplayTrack::Authoritative,
        replay::ReplayCommandStage::Applied,
        1u,
        1,
        5u,
        83'335u,
        makeCommand(1u, 0.8f)});

    replay::ReplayPlaybackRuntime playback;
    expect(playback.load(demo, replay::ReplayPlaybackTrack::ServerTruth),
           "playback runtime should load command replay");
    expect(playback.seekToTick(5u), "playback runtime should seek to command tick");
    const sim::PlayerState* player = sim::findPlayer(playback.worldState(), 1);
    expect(player != nullptr && almostEqual(player->yaw, 0.8f),
           "playback runtime should apply authoritative command events");
}

void testControlReplayReconstructsAuthoritativeWorldBetweenKeyframes() {
    replay::ReplayDemo demo;
    demo.header.startedServerTimeUs = 66'668u;
    demo.header.tickRateHz = 60u;
    demo.header.snapshotRateHz = 20u;
    demo.header.hasControlLane = true;
    demo.initialState.worldState = makeWorld();
    demo.initialState.simConfig = sim::SimConfig{};

    replay::WorldKeyframe authoritativeKeyframe;
    authoritativeKeyframe.track = replay::ReplayTrack::Authoritative;
    authoritativeKeyframe.serverTick = 4u;
    authoritativeKeyframe.serverTimeUs = 66'668u;
    authoritativeKeyframe.worldState = demo.initialState.worldState;
    demo.keyframes.push_back(authoritativeKeyframe);

    replay::WorldKeyframe controlKeyframe;
    controlKeyframe.track = replay::ReplayTrack::Control;
    controlKeyframe.serverTick = 4u;
    controlKeyframe.serverTimeUs = 66'668u;
    controlKeyframe.worldState = demo.initialState.worldState;
    controlKeyframe.controlPlayers = demo.initialState.worldState.players;
    demo.keyframes.push_back(controlKeyframe);

    demo.commandEvents.push_back(replay::ServerCommandEvent{
        replay::ReplayTrack::Authoritative,
        replay::ReplayCommandStage::Applied,
        1u,
        1,
        5u,
        83'335u,
        makeMoveCommand(1u)});
    demo.commandEvents.push_back(replay::ServerCommandEvent{
        replay::ReplayTrack::Control,
        replay::ReplayCommandStage::Applied,
        1u,
        1,
        5u,
        83'335u,
        makeMoveCommand(1u)});

    const sim::PlayerState* initialPlayer =
        sim::findPlayer(demo.initialState.worldState, 1);
    expect(initialPlayer != nullptr, "control replay test should have an initial player");

    replay::ReplayPlaybackRuntime playback;
    expect(playback.load(demo, replay::ReplayPlaybackTrack::Control),
           "control replay playback should load command replay");
    expect(playback.seekToTick(5u), "control replay playback should seek between keyframes");

    const sim::PlayerState* worldPlayer = sim::findPlayer(playback.worldState(), 1);
    expect(worldPlayer != nullptr &&
               std::fabs(worldPlayer->position.z - initialPlayer->position.z) > 0.001f,
           "control replay should reconstruct the authoritative world every tick, not hold it on the previous snapshot keyframe");
    expect(!playback.controlPlayers().empty() &&
               std::fabs(playback.controlPlayers().front().position.z -
                         initialPlayer->position.z) > 0.001f,
           "control replay should still advance the control-lane player state between keyframes");
}

void testReplayPlaybackUsesAuthoritativeSimulationForCombat() {
    replay::ReplayDemo demo;
    demo.header.startedServerTimeUs = 66'668u;
    demo.header.tickRateHz = 60u;
    demo.header.snapshotRateHz = 20u;
    demo.header.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    demo.initialState.worldState = makeCombatWorld();
    demo.initialState.simConfig = sim::SimConfig{};

    sim::PlayerCommand fire;
    fire.seq = 1u;
    fire.dtSeconds = 0.0f;
    fire.yaw = 0.0f;
    fire.pitch = 0.0f;
    fire.buttons = sim::commandButtonBit(sim::CommandButton::Fire);
    fire.viewedServerTimeUs = 66'668u;

    demo.commandEvents.push_back(replay::ServerCommandEvent{
        replay::ReplayTrack::Authoritative,
        replay::ReplayCommandStage::Applied,
        1u,
        1,
        5u,
        83'335u,
        fire});

    replay::ReplayPlaybackRuntime playback;
    expect(playback.load(demo, replay::ReplayPlaybackTrack::ServerTruth),
           "playback runtime should load combat replay");
    expect(playback.seekToTick(5u), "playback runtime should seek to combat command tick");

    const sim::PlayerState* target = sim::findPlayer(playback.worldState(), 2);
    expect(target != nullptr && target->health <= 0.0f,
           "command replay playback should process combat through the authoritative simulation");
    const sim::RosterEntry* shooterEntry = sim::findRosterEntry(playback.worldState(), 1);
    const sim::RosterEntry* targetEntry = sim::findRosterEntry(playback.worldState(), 2);
    expect(shooterEntry != nullptr && targetEntry != nullptr &&
               shooterEntry->kills == 1u &&
               targetEntry->deaths == 1u,
           "authoritative replay playback should preserve scoring side effects");
}

void testServerRuntimeCapturesDualCommandLanes() {
    net::ServerConfig config;
    config.maxPlayers = 2u;
    config.maxHumanPlayers = 2u;
    config.tickRateHz = 60u;
    config.snapshotRateHz = 20u;
    config.levelSlot = 7;
    config.levelHash = 77u;
    net::ServerRuntime server(config);

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{1u, 0u, "player", sim::TeamId::Attacker},
                               0u,
                               &welcome),
           "server should accept replay capture test client");
    expect(welcome.assignedPeerId == 1u, "test client should receive peer id one");

    server.setCommandReplayRecordingEnabled(true);
    server.tickOnce(16'667u);

    net::CommandBundle gameplay;
    gameplay.commands.push_back(makeCommand(1u, 0.4f));
    net::ControlCommandBundle control;
    control.commands.push_back(makeCommand(1u, 0.4f));
    expect(server.enqueueCommandBundle(1u, gameplay, 20'000u),
           "server should enqueue gameplay command for replay capture");
    expect(server.enqueueControlCommandBundle(1u, control, 20'000u),
           "server should enqueue control command for replay capture");

    server.tickOnce(33'334u);
    server.setCommandReplayRecordingEnabled(false);

    const replay::ReplayDemo& demo = server.commandReplayDemo();
    std::size_t authoritativeApplied = 0u;
    std::size_t controlApplied = 0u;
    std::size_t authoritativeReceived = 0u;
    std::size_t controlReceived = 0u;
    for (const replay::ServerCommandEvent& event : demo.commandEvents) {
        if (event.track == replay::ReplayTrack::Authoritative &&
            event.stage == replay::ReplayCommandStage::Received) {
            ++authoritativeReceived;
        }
        if (event.track == replay::ReplayTrack::Control &&
            event.stage == replay::ReplayCommandStage::Received) {
            ++controlReceived;
        }
        if (event.track == replay::ReplayTrack::Authoritative &&
            event.stage == replay::ReplayCommandStage::Applied) {
            ++authoritativeApplied;
        }
        if (event.track == replay::ReplayTrack::Control &&
            event.stage == replay::ReplayCommandStage::Applied) {
            ++controlApplied;
        }
    }

    expect(authoritativeReceived == 1u &&
               controlReceived == 1u &&
               authoritativeApplied == 1u &&
               controlApplied == 1u,
           "server command replay recorder should capture received and applied events from both lanes");
    expect(demo.header.hasControlLane && !demo.keyframes.empty(),
           "server command replay recorder should mark control lane and store keyframes");
}

void testServerRuntimeCapturesBotAuthoritativeCommands() {
    net::ServerConfig config;
    config.maxPlayers = 3u;
    config.maxHumanPlayers = 1u;
    config.tickRateHz = 60u;
    config.snapshotRateHz = 20u;
    config.defenderBotCount = 1u;
    config.botDirector.startFrozen = false;
    config.botDirector.shootingEnabled = false;
    net::ServerRuntime server(config);

    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{1u, 0u, "player", sim::TeamId::Attacker},
                               0u,
                               &welcome),
           "server should accept replay bot test client");
    const int botActorId = firstBotActorId(server.worldState());
    expect(botActorId >= 0, "replay bot test should spawn a configured bot");
    sim::PlayerState* hostPlayer =
        sim::findPlayer(&server.worldState(), static_cast<int>(welcome.assignedPeerId));
    sim::PlayerState* botPlayer = sim::findPlayer(&server.worldState(), botActorId);
    expect(hostPlayer != nullptr && botPlayer != nullptr,
           "replay bot test should have host and bot players");
    hostPlayer->position = sim::Vec3{0.0f, sim::defaults::kPlayerEyeHeight, 0.0f};
    botPlayer->position = sim::Vec3{0.0f, sim::defaults::kPlayerEyeHeight, -20.0f};

    server.setCommandReplayRecordingEnabled(true);
    for (std::uint32_t tick = 1u; tick <= 8u; ++tick) {
        server.tickOnce(static_cast<std::uint64_t>(tick) * 16'667u);
    }
    server.setCommandReplayRecordingEnabled(false);

    const replay::ReplayDemo& demo = server.commandReplayDemo();
    bool sawBotCommand = false;
    std::uint32_t botCommandTick = 0u;
    for (const replay::ServerCommandEvent& event : demo.commandEvents) {
        if (event.track == replay::ReplayTrack::Authoritative &&
            event.stage == replay::ReplayCommandStage::Applied &&
            event.actorId == botActorId) {
            sawBotCommand = true;
            botCommandTick = event.serverTick;
            break;
        }
    }
    expect(sawBotCommand,
           "command replay recorder should capture applied authoritative bot commands");

    const sim::PlayerState* initialBot =
        sim::findPlayer(demo.initialState.worldState, botActorId);
    expect(initialBot != nullptr, "recorded bot should exist in the initial replay world");

    replay::ReplayPlaybackRuntime playback;
    expect(playback.load(demo, replay::ReplayPlaybackTrack::ServerTruth),
           "bot command replay playback should load");
    expect(playback.seekToTick(botCommandTick),
           "bot command replay playback should seek to the bot command tick");
    const sim::PlayerState* replayedBot =
        sim::findPlayer(playback.worldState(), botActorId);
    expect(replayedBot != nullptr &&
               (std::fabs(replayedBot->position.x - initialBot->position.x) > 0.0001f ||
                std::fabs(replayedBot->position.z - initialBot->position.z) > 0.0001f),
           "bot command replay playback should drive bots from recorded commands instead of waiting for snapshot keyframes");
}

}  // namespace

int main() {
    try {
        testCommandReplayArchiveRoundTripsSchema();
        testReplayPlaybackAppliesAuthoritativeCommands();
        testControlReplayReconstructsAuthoritativeWorldBetweenKeyframes();
        testReplayPlaybackUsesAuthoritativeSimulationForCombat();
        testServerRuntimeCapturesDualCommandLanes();
        testServerRuntimeCapturesBotAuthoritativeCommands();
        std::cout << "ReplayCommandArchiveTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ReplayCommandArchiveTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
