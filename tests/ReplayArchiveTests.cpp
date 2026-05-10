#include "app/ReplayArchive.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
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

std::filesystem::path tempReplayDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("netcodesim_replay_archive_test_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

client::ReplayRecording makeRecording() {
    client::ReplayRecording recording;
    recording.metadata.title = "Archive Test";
    recording.metadata.sourceLabel = "Test Session";
    recording.metadata.levelSlot = 4;
    recording.metadata.levelHash = 12345u;
    recording.metadata.createdUnixSeconds = 1'700'000'000u;

    client::RecordedReplayFrame recordedFrame;
    recordedFrame.timestamp = 0.25f;
    recordedFrame.frame.hasSnapshot = true;
    recordedFrame.frame.camera.position = Vector3{1.0f, 2.0f, 3.0f};
    recordedFrame.frame.camera.target = Vector3{4.0f, 5.0f, 6.0f};
    recordedFrame.frame.camera.up = Vector3{0.0f, 1.0f, 0.0f};
    recordedFrame.frame.camera.fovy = 70.0f;
    recordedFrame.frame.camera.projection = CAMERA_PERSPECTIVE;
    recordedFrame.frame.hud.lines.push_back("HUD line");
    recordedFrame.frame.combatTraces.emplace_back(Vector3{0.0f, 1.0f, 0.0f},
                                                  Vector3{2.0f, 1.0f, 0.0f},
                                                  RED,
                                                  0.5f,
                                                  0.1f,
                                                  false,
                                                  true);

    client::RemoteEnemyRenderItem enemy;
    enemy.entityId = 7;
    enemy.displayPosition = Vector3{8.0f, 0.0f, 9.0f};
    enemy.yawRadians = 0.7f;
    enemy.healthPercent = 0.5f;
    enemy.alive = true;
    enemy.tint = PINK;
    recordedFrame.frame.remoteEnemies.push_back(enemy);

    client::RemotePlayerRenderItem player;
    player.actorId = 3;
    player.eyePosition = Vector3{1.0f, 1.8f, 1.0f};
    player.rootPosition = Vector3{1.0f, 0.0f, 1.0f};
    player.yawRadians = 1.0f;
    player.pitchRadians = 0.1f;
    player.healthPercent = 0.75f;
    player.alive = true;
    player.team = sim::TeamId::Attacker;
    player.tint = ORANGE;
    recordedFrame.frame.remotePlayers.push_back(player);
    player.actorId = 9;
    player.eyePosition = Vector3{3.0f, 1.8f, 2.0f};
    player.rootPosition = Vector3{3.0f, 0.0f, 2.0f};
    player.team = sim::TeamId::Defender;
    player.tint = BLUE;
    recordedFrame.localPlayerRenderItem = player;

    recordedFrame.frame.compactScore.visible = true;
    recordedFrame.frame.compactScore.score.available = true;
    recordedFrame.frame.compactScore.score.attackerScore = 2u;
    recordedFrame.frame.compactScore.score.defenderScore = 1u;
    recordedFrame.frame.compactScore.score.localIdentity = "Player";
    recordedFrame.frame.compactScore.score.localTeam = sim::TeamId::Attacker;
    recordedFrame.frame.replay.statusLine = "Recording";

    recording.frames.push_back(recordedFrame);
    return recording;
}

void testReplayArchiveRoundTripsRecordingFrames() {
    const std::filesystem::path directory = tempReplayDirectory();
    const app::ReplayArchive archive(directory);
    const client::ReplayRecording recording = makeRecording();

    std::filesystem::path savedPath;
    std::string error;
    expect(archive.save(recording, &savedPath, &error),
           "replay archive should save a recording: " + error);
    expect(std::filesystem::exists(savedPath),
           "replay archive should write the saved replay file");
    expect(savedPath.extension() == app::ReplayArchive::kReplayExtension,
           "replay archive should use the native replay extension");

    const std::vector<app::ReplayArchiveEntry> entries = archive.list();
    expect(entries.size() == 1u,
           "replay archive should list saved replay metadata");
    expect(entries.front().metadata.title == recording.metadata.title &&
               entries.front().frameCount == recording.frames.size(),
           "replay archive metadata listing should preserve title and frame count");

    client::ReplayRecording loaded;
    expect(archive.load(savedPath, &loaded, &error),
           "replay archive should load a saved replay: " + error);
    expect(loaded.metadata.title == recording.metadata.title &&
               loaded.metadata.sourceLabel == recording.metadata.sourceLabel &&
               loaded.metadata.levelSlot == recording.metadata.levelSlot &&
               loaded.metadata.levelHash == recording.metadata.levelHash,
           "loaded replay metadata should match the saved recording");
    expect(loaded.frames.size() == 1u &&
               almostEqual(loaded.frames.front().timestamp, 0.25f),
           "loaded replay should preserve frame timing");
    expect(loaded.frames.front().frame.hasSnapshot &&
               loaded.frames.front().frame.remoteEnemies.size() == 1u &&
               loaded.frames.front().frame.remoteEnemies.front().entityId == 7,
           "loaded replay should preserve remote enemy render items");
    expect(loaded.frames.front().frame.remotePlayers.size() == 1u &&
               loaded.frames.front().frame.remotePlayers.front().team == sim::TeamId::Attacker,
           "loaded replay should preserve remote player render items");
    expect(loaded.frames.front().localPlayerRenderItem.has_value() &&
               loaded.frames.front().localPlayerRenderItem->actorId == 9 &&
               loaded.frames.front().localPlayerRenderItem->team == sim::TeamId::Defender,
           "loaded replay should preserve the local player render item used by replay spectator mode");
    expect(loaded.frames.front().frame.hud.lines.size() == 1u &&
               loaded.frames.front().frame.hud.lines.front() == "HUD line",
           "loaded replay should preserve HUD lines");

    std::filesystem::remove_all(directory);
}

void testReplayArchiveRejectsEmptyRecordings() {
    const std::filesystem::path directory = tempReplayDirectory();
    const app::ReplayArchive archive(directory);
    client::ReplayRecording recording;
    std::string error;
    expect(!archive.save(recording, nullptr, &error) &&
               error == "no recorded frames to save",
           "replay archive should reject empty recordings with a clear error");
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    try {
        testReplayArchiveRoundTripsRecordingFrames();
        testReplayArchiveRejectsEmptyRecordings();
        std::cout << "ReplayArchiveTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ReplayArchiveTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
