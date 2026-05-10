#pragma once

#include <algorithm>
#include <cstddef>

namespace app {

class ReplayTimeline {
public:
    bool isRecording{false};
    bool isPlayback{false};
    bool playbackPlaying{false};
    std::size_t playbackIndex{0};
    float playbackTimer{0.0f};
    float recordingTimer{0.0f};

    template <typename EntryContainer, typename TimestampFn>
    static std::size_t activeEntryIndexAt(const EntryContainer& entries,
                                          float playbackTimestamp,
                                          TimestampFn timestampFor) {
        if (entries.empty()) {
            return 0u;
        }

        std::size_t activeIndex = 0u;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (timestampFor(entries[index]) <= playbackTimestamp) {
                activeIndex = index;
                continue;
            }
            break;
        }
        return activeIndex;
    }

    void startRecording();
    void stopRecording();
    void stopPlayback();
    void reset();
    void advanceRecording(float dt);

    template <typename FrameContainer>
    bool startPlayback(const FrameContainer& frames, bool autoPlay) {
        if (frames.empty()) {
            return false;
        }

        stopRecording();
        isPlayback = true;
        playbackPlaying = autoPlay;
        playbackIndex = 0;
        playbackTimer = frames.front().timestamp;
        return true;
    }

    template <typename FrameContainer>
    bool resetPlayback(const FrameContainer& frames) {
        if (frames.empty()) {
            stopPlayback();
            return false;
        }

        isPlayback = true;
        playbackPlaying = false;
        playbackIndex = 0;
        playbackTimer = frames.front().timestamp;
        return true;
    }

    template <typename FrameContainer>
    bool seekBy(const FrameContainer& frames, int delta) {
        if (frames.empty()) {
            stopPlayback();
            return false;
        }
        if (!isPlayback) {
            return false;
        }

        playbackPlaying = false;
        const long long maxIndex = static_cast<long long>(frames.size()) - 1;
        const long long targetIndex = std::clamp(
            static_cast<long long>(playbackIndex) + static_cast<long long>(delta),
            0LL,
            maxIndex);
        playbackIndex = static_cast<std::size_t>(targetIndex);
        playbackTimer = frames[playbackIndex].timestamp;
        return true;
    }

    template <typename FrameContainer>
    bool updatePlayback(const FrameContainer& frames, float dt) {
        if (frames.empty()) {
            stopPlayback();
            return false;
        }
        if (!isPlayback) {
            return false;
        }

        if (playbackIndex >= frames.size()) {
            playbackIndex = frames.size() - 1;
        }

        if (playbackPlaying && playbackIndex + 1 < frames.size()) {
            playbackTimer += dt;
            while (playbackIndex + 1 < frames.size() &&
                   playbackTimer >= frames[playbackIndex + 1].timestamp) {
                ++playbackIndex;
            }
            if (playbackIndex == frames.size() - 1 &&
                playbackTimer > frames.back().timestamp) {
                playbackPlaying = false;
                playbackTimer = frames.back().timestamp;
            }
        }

        return true;
    }
};

}  // namespace app
