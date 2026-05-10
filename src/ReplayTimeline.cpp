#include "app/ReplayTimeline.hpp"

namespace app {

void ReplayTimeline::startRecording() {
    stopPlayback();
    isRecording = true;
    recordingTimer = 0.0f;
}

void ReplayTimeline::stopRecording() {
    isRecording = false;
}

void ReplayTimeline::stopPlayback() {
    isPlayback = false;
    playbackPlaying = false;
    playbackIndex = 0;
    playbackTimer = 0.0f;
}

void ReplayTimeline::reset() {
    stopRecording();
    stopPlayback();
    recordingTimer = 0.0f;
}

void ReplayTimeline::advanceRecording(float dt) {
    if (isRecording) {
        recordingTimer += dt;
    }
}

}  // namespace app
