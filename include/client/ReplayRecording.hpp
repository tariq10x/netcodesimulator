#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "client/RenderFrame.hpp"

namespace client {

struct RecordedReplayFrame {
    float timestamp{0.0f};
    std::optional<RemotePlayerRenderItem> localPlayerRenderItem{};
    RenderFrame frame{};
};

struct ReplayRecordingMetadata {
    std::string title{};
    std::string sourceLabel{};
    int levelSlot{-1};
    std::uint32_t levelHash{0u};
    std::uint64_t createdUnixSeconds{0u};
    std::uint32_t formatVersion{1u};
};

struct ReplayRecording {
    ReplayRecordingMetadata metadata{};
    std::vector<RecordedReplayFrame> frames{};
};

}  // namespace client
