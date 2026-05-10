#include "app/ReplayTimeline.hpp"
#include "client/ReplaySubsystem.hpp"
#include "client/ReplayTransportControls.hpp"

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Frame {
    float timestamp;
    int payload;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool almostEqual(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.0001f;
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

    throw std::runtime_error("failed to locate repository root for replay timeline tests");
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    expect(file.is_open(), "expected to open source fixture: " + path.string());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void testStartPlaybackStopsRecordingAndInitializesToFirstFrame() {
    app::ReplayTimeline timeline;
    timeline.startRecording();
    timeline.advanceRecording(0.25f);
    expect(timeline.isRecording, "recording should start through the shared replay timeline");
    expect(almostEqual(timeline.recordingTimer, 0.25f),
           "recording timer should accumulate elapsed recording time");

    const std::vector<Frame> frames{{0.0f, 1}, {0.1f, 2}, {0.2f, 3}};
    expect(timeline.startPlayback(frames, true),
           "playback should start when a recording is available");
    expect(!timeline.isRecording, "starting playback should stop recording");
    expect(timeline.isPlayback, "starting playback should enter playback mode");
    expect(timeline.playbackPlaying, "auto-play playback should begin in the playing state");
    expect(timeline.playbackIndex == 0u, "playback should begin at the first frame");
    expect(almostEqual(timeline.playbackTimer, 0.0f),
           "playback should begin at the timestamp of the first frame");
}

void testUpdatePlaybackClampsAtEndAndPausesDeterministically() {
    app::ReplayTimeline timeline;
    const std::vector<Frame> frames{{0.0f, 1}, {0.1f, 2}, {0.2f, 3}};

    expect(timeline.startPlayback(frames, true),
           "playback should initialize before update tests run");
    expect(timeline.updatePlayback(frames, 0.15f),
           "playback update should succeed while a recording exists");
    expect(timeline.playbackIndex == 1u,
           "playback should advance to the latest frame at or before the current playback time");
    expect(almostEqual(timeline.playbackTimer, 0.15f),
           "playback timer should accumulate elapsed playback time while playing");

    expect(timeline.updatePlayback(frames, 0.2f),
           "playback update should continue advancing until the final frame");
    expect(timeline.playbackIndex == 2u,
           "playback should clamp to the last frame when the timer runs past the recording");
    expect(!timeline.playbackPlaying,
           "playback should pause automatically when it reaches the final frame");
    expect(almostEqual(timeline.playbackTimer, 0.2f),
           "playback timer should clamp to the final frame timestamp at the end");
}

void testResetAndSeekPreservePauseAndClampSemantics() {
    app::ReplayTimeline timeline;
    const std::vector<Frame> frames{{0.0f, 1}, {0.1f, 2}, {0.2f, 3}, {0.3f, 4}};

    expect(timeline.startPlayback(frames, true),
           "playback should start before reset and seek tests run");
    expect(timeline.seekBy(frames, 500),
           "large positive seeks should clamp to the final frame");
    expect(timeline.playbackIndex == 3u,
           "seek should clamp forward seeks to the final frame");
    expect(!timeline.playbackPlaying,
           "seeking should pause playback so the user can inspect the selected frame");
    expect(almostEqual(timeline.playbackTimer, 0.3f),
           "seek should align playback time to the selected frame timestamp");

    expect(timeline.resetPlayback(frames),
           "reset should reposition playback at the first frame");
    expect(timeline.isPlayback,
           "reset should keep playback mode active");
    expect(!timeline.playbackPlaying,
           "reset should leave playback paused at the start");
    expect(timeline.playbackIndex == 0u,
           "reset should restore the first playback frame");
    expect(almostEqual(timeline.playbackTimer, 0.0f),
           "reset should restore the first playback timestamp");

    expect(timeline.seekBy(frames, -200),
           "large negative seeks should clamp to the first frame");
    expect(timeline.playbackIndex == 0u,
           "seek should clamp backward seeks to the first frame");
}

void testReplayTimelineResetRestoresIdleState() {
    app::ReplayTimeline timeline;
    const std::vector<Frame> frames{{0.0f, 1}, {0.1f, 2}};

    timeline.startRecording();
    timeline.advanceRecording(0.3f);
    expect(timeline.startPlayback(frames, true),
           "playback should begin before reset restores the idle replay state");

    timeline.reset();
    expect(!timeline.isRecording &&
               !timeline.isPlayback &&
               !timeline.playbackPlaying,
           "reset should restore the replay timeline to a fully idle state");
    expect(timeline.playbackIndex == 0u &&
               almostEqual(timeline.playbackTimer, 0.0f) &&
               almostEqual(timeline.recordingTimer, 0.0f),
           "reset should clear both playback and recording cursors");
}

void testReplaySubsystemOwnsRecordingStatusPresentation() {
    client::ReplaySubsystem replaySubsystem;
    const std::vector<Frame> frames{{0.0f, 1}, {0.1f, 2}, {0.2f, 3}};

    const client::ReplayStatusView idle = replaySubsystem.statusView();
    expect(!idle.recordingActive &&
               !idle.playbackActive &&
               idle.statusLine == "Replay idle",
           "replay subsystem should expose an idle recording-status view before any capture starts");

    replaySubsystem.startRecording();
    replaySubsystem.advanceRecording(0.2f);
    const client::ReplayStatusView recording = replaySubsystem.statusView(4u);
    expect(recording.recordingActive &&
               !recording.playbackActive &&
               recording.statusLine == "Recording (4 frames)",
           "replay subsystem should own recording-status presentation while capture is active");

    expect(replaySubsystem.startPlayback(frames, true),
           "replay subsystem should proxy playback lifecycle through the shared replay timeline");
    const client::ReplayStatusView playback = replaySubsystem.statusView(frames.size());
    expect(!playback.recordingActive &&
               playback.playbackActive &&
               playback.statusLine == "Playback (playing) @1/3",
           "replay subsystem should expose playback status without depending on legacy mode classes");

    replaySubsystem.reset();
    const client::ReplayStatusView ready = replaySubsystem.statusView(frames.size());
    expect(!ready.recordingActive &&
               !ready.playbackActive &&
               ready.statusLine == "Replay ready",
           "replay subsystem should preserve ready-to-play presentation after resetting the live timeline state");
}

void testReplaySubsystemSurfacesShotStrategyTimelineAnnotations() {
    client::ReplaySubsystem replaySubsystem;
    replaySubsystem.setShotStrategyTimeline({
        {0.2f, net::ShotEvaluationMode::LivePosition},
        {0.0f, net::ShotEvaluationMode::SeenPosition},
        {0.35f, net::ShotEvaluationMode::SeenPosition}
    });

    const client::ReplayShotStrategyTimelineView readyAnnotations =
        replaySubsystem.shotStrategyTimelineView();
    expect(readyAnnotations.available &&
               readyAnnotations.entries.size() == 3u &&
               almostEqual(readyAnnotations.entries[0].timestamp, 0.0f) &&
               almostEqual(readyAnnotations.entries[1].timestamp, 0.2f) &&
               almostEqual(readyAnnotations.entries[2].timestamp, 0.35f),
           "replay annotations should preserve shot-strategy change timing in playback order");
    expect(readyAnnotations.activeEntryIndex == 0u &&
               readyAnnotations.activeRuleLabel == "Seen Position" &&
               readyAnnotations.activeRuleExplanation ==
                   "host rewinds targets to the shooter's view.",
           "replay annotations should expose the active rule before playback begins");

    const client::ReplayStatusView ready = replaySubsystem.statusView(4u);
    expect(ready.statusLine == "Replay ready | Rule Seen Position",
           "replay status should expose the active authoritative shot rule while playback is idle");

    const std::vector<Frame> frames{{0.0f, 1}, {0.1f, 2}, {0.25f, 3}, {0.4f, 4}};
    expect(replaySubsystem.startPlayback(frames, true),
           "replay playback should start before annotation timing checks run");
    expect(replaySubsystem.updatePlayback(frames, 0.26f),
           "replay playback should advance to the later rule change");

    const client::ReplayShotStrategyTimelineView midPlaybackAnnotations =
        replaySubsystem.shotStrategyTimelineView();
    expect(midPlaybackAnnotations.activeEntryIndex == 1u &&
               midPlaybackAnnotations.activeRuleLabel == "Live Position" &&
               midPlaybackAnnotations.activeRuleExplanation ==
                   "host judges hits against each target's live position.",
           "replay annotations should update the active rule as playback crosses strategy changes");
    expect(replaySubsystem.statusView(frames.size()).statusLine ==
               "Playback (playing) @3/4 | Rule Live Position",
           "replay status should surface the active rule during playback");

    expect(replaySubsystem.updatePlayback(frames, 0.20f),
           "replay playback should continue through the final rule change");
    const client::ReplayShotStrategyTimelineView endPlaybackAnnotations =
        replaySubsystem.shotStrategyTimelineView();
    expect(endPlaybackAnnotations.activeEntryIndex == 2u &&
               almostEqual(endPlaybackAnnotations.activeTimestamp, 0.35f) &&
               endPlaybackAnnotations.activeRuleLabel == "Seen Position",
           "replay annotations should preserve later strategy changes after playback reaches the end");
    expect(replaySubsystem.statusView(frames.size()).statusLine ==
               "Playback (paused) @4/4 | Rule Seen Position",
           "replay status should preserve the final active rule after playback pauses at the end");

    replaySubsystem.startRecording();
    expect(replaySubsystem.shotStrategyTimeline().empty(),
           "starting a new recording should clear stale replay-side shot-strategy annotations");
}

void testReplaySubsystemPreservesShotStrategyAndSpectatorContextInTickOrder() {
    client::ReplaySubsystem replaySubsystem;
    replaySubsystem.setShotStrategyTimeline({
        {0.25f, net::ShotEvaluationMode::LivePosition},
        {0.0f, net::ShotEvaluationMode::SeenPosition}
    });
    replaySubsystem.setSpectatorTimeline({
        {0.30f, sim::PaneViewMode::SpectatorFollowThirdPerson, 22, "BOT 22", true, true},
        {0.0f, sim::PaneViewMode::SpectatorFreeFly, -1, "Free Camera", true, false},
        {0.20f, sim::PaneViewMode::SpectatorFollowFirstPerson, 7, "Player 7", false, true}
    });

    const client::ReplaySpectatorTimelineView readySpectator =
        replaySubsystem.spectatorTimelineView();
    expect(readySpectator.available &&
               readySpectator.entries.size() == 3u &&
               almostEqual(readySpectator.entries[0].timestamp, 0.0f) &&
               almostEqual(readySpectator.entries[1].timestamp, 0.20f) &&
               almostEqual(readySpectator.entries[2].timestamp, 0.30f),
           "replay spectator metadata should preserve recorded spectator context in playback order");
    expect(readySpectator.activeEntryIndex == 0u &&
               readySpectator.activeModeLabel == "Spectator Free-Fly" &&
               readySpectator.activeFollowTargetLabel == "Free Camera" &&
               readySpectator.sessionSpectator,
           "replay spectator metadata should expose the earliest active spectator context before playback begins");

    const std::vector<Frame> frames{{0.0f, 1}, {0.15f, 2}, {0.3f, 3}, {0.4f, 4}};
    expect(replaySubsystem.startPlayback(frames, true),
           "replay playback should start before spectator-context timing checks run");
    expect(replaySubsystem.updatePlayback(frames, 0.26f),
           "replay playback should advance into later spectator metadata");

    const client::ReplayShotStrategyTimelineView shotView =
        replaySubsystem.shotStrategyTimelineView();
    const client::ReplaySpectatorTimelineView playbackSpectator =
        replaySubsystem.spectatorTimelineView();
    expect(shotView.activeEntryIndex == 1u &&
               shotView.activeRuleLabel == "Live Position",
           "replay shot-strategy metadata should advance in tick order during playback");
    expect(playbackSpectator.activeEntryIndex == 1u &&
               playbackSpectator.activeModeLabel == "Spectator Follow First Person" &&
               playbackSpectator.activeFollowTargetLabel == "Player 7" &&
               !playbackSpectator.sessionSpectator &&
               playbackSpectator.canReturnToCharacter,
           "replay spectator metadata should advance in tick order without losing pane-local spectator details");
}

void testReplayStatusViewExposesActiveShotRuleAndPaneBindingMetadata() {
    client::ReplaySubsystem replaySubsystem;
    replaySubsystem.setShotStrategyTimeline({
        {0.2f, net::ShotEvaluationMode::LivePosition},
        {0.0f, net::ShotEvaluationMode::SeenPosition}
    });
    replaySubsystem.setPaneBindingTimeline({
        {0.25f, sim::PaneSlot::Right, sim::PaneViewMode::ReplayCamera, -1, "Replay Camera B"},
        {0.0f, sim::PaneSlot::Left, sim::PaneViewMode::SpectatorFreeFly, -1, "Free Camera"},
        {0.10f, sim::PaneSlot::Left, sim::PaneViewMode::SpectatorFollowFirstPerson, 7, "Player 7"}
    });

    const client::ReplayPaneBindingTimelineView readyBindings =
        replaySubsystem.paneBindingTimelineView();
    expect(readyBindings.available &&
               readyBindings.entries.size() == 3u &&
               almostEqual(readyBindings.entries[0].timestamp, 0.0f) &&
               almostEqual(readyBindings.entries[1].timestamp, 0.10f) &&
               almostEqual(readyBindings.entries[2].timestamp, 0.25f),
           "replay pane-binding metadata should preserve recorded binding changes in playback order");
    expect(replaySubsystem.statusView(4u).statusLine ==
               "Replay ready | Rule Seen Position | Left Pane Free Camera",
           "replay status should expose the active shot strategy and pane binding before playback begins");

    const std::vector<Frame> frames{{0.0f, 1}, {0.15f, 2}, {0.3f, 3}, {0.4f, 4}};
    expect(replaySubsystem.startPlayback(frames, true),
           "replay playback should start before pane-binding status checks run");
    expect(replaySubsystem.updatePlayback(frames, 0.26f),
           "replay playback should advance into later pane-binding metadata");

    const client::ReplayPaneBindingTimelineView playbackBindings =
        replaySubsystem.paneBindingTimelineView();
    expect(playbackBindings.activeEntryIndex == 2u &&
               playbackBindings.activeSlot == sim::PaneSlot::Right &&
               playbackBindings.activeBindingLabel == "Replay Camera B" &&
               playbackBindings.activeModeLabel == "Replay Camera",
           "replay pane-binding metadata should expose the active binding at the current playback position");
    expect(replaySubsystem.statusView(frames.size()).statusLine ==
               "Playback (playing) @2/4 | Rule Live Position | Right Pane Replay Camera B",
           "replay status should expose the active shot strategy and pane binding for the current playback position");
}

void testRecordingOwnershipStaysOnReplaySubsystemAndClientRuntime() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string replaySubsystemHeader =
        readTextFile(repoRoot / "include/client/ReplaySubsystem.hpp");
    const std::string clientRuntimeHeader =
        readTextFile(repoRoot / "include/net/ClientRuntime.hpp");
    const std::string clientRuntimeSource =
        readTextFile(repoRoot / "src/ClientRuntime.cpp");

    expect(replaySubsystemHeader.find("ReplayStatusView statusView(") != std::string::npos &&
               replaySubsystemHeader.find("app::ReplayTimeline timeline_{};") != std::string::npos,
           "ReplaySubsystem should own replay timing and recording-status presentation for the client");
    expect(clientRuntimeHeader.find("client::ReplaySubsystem replaySubsystem_{};") != std::string::npos,
           "ClientRuntime should keep ReplaySubsystem as its replay-facing ownership seam");
    expect(clientRuntimeSource.find("replaySubsystem_.reset();") != std::string::npos &&
               clientRuntimeSource.find("client::ReplayStatusView ClientRuntime::replayStatusView() const") != std::string::npos &&
               clientRuntimeSource.find("replaySubsystem_.statusView(recordedReplayFrames_.size())") != std::string::npos,
           "ClientRuntime should reset replay lifecycle through ReplaySubsystem and route replay presentation through it");
}

void testRecordingTransportKeepsResetAndAddsExplicitExitButton() {
    const std::vector<client::ReplayTransportButtonSpec> buttons =
        client::makeReplayRecordingTransportButtons(false, true, true, true, true);

    expect(buttons.size() == 7u,
           "recording transport should include a distinct Backspace exit button");
    expect(buttons[5].glyph == client::ReplayCassetteGlyph::Stop &&
               buttons[5].hotkey == "0" &&
               buttons[5].enabled,
           "recording transport should keep 0 as the reset-to-first-frame control");
    expect(buttons[6].glyph == client::ReplayCassetteGlyph::Exit &&
               buttons[6].hotkey == "Backspace" &&
               buttons[6].enabled,
           "recording transport should expose Backspace as the replay-exit control label");
}

}  // namespace

int main() {
    try {
        testStartPlaybackStopsRecordingAndInitializesToFirstFrame();
        testUpdatePlaybackClampsAtEndAndPausesDeterministically();
        testResetAndSeekPreservePauseAndClampSemantics();
        testReplayTimelineResetRestoresIdleState();
        testReplaySubsystemOwnsRecordingStatusPresentation();
        testReplaySubsystemSurfacesShotStrategyTimelineAnnotations();
        testReplaySubsystemPreservesShotStrategyAndSpectatorContextInTickOrder();
        testReplayStatusViewExposesActiveShotRuleAndPaneBindingMetadata();
        testRecordingOwnershipStaysOnReplaySubsystemAndClientRuntime();
        testRecordingTransportKeepsResetAndAddsExplicitExitButton();
        std::cout << "ReplayTimelineTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ReplayTimelineTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
