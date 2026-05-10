#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "client/ReplayRecording.hpp"

namespace app {

struct ReplayArchiveEntry {
    std::filesystem::path path{};
    client::ReplayRecordingMetadata metadata{};
    std::size_t frameCount{0u};
    std::uintmax_t fileSizeBytes{0u};
};

class ReplayArchive {
public:
    static constexpr const char* kReplayExtension = ".nlr";

    explicit ReplayArchive(std::filesystem::path directory = replayDirectory());

    const std::filesystem::path& directory() const;

    bool save(const client::ReplayRecording& recording,
              std::filesystem::path* savedPathOut = nullptr,
              std::string* errorOut = nullptr) const;
    bool load(const std::filesystem::path& path,
              client::ReplayRecording* recordingOut,
              std::string* errorOut = nullptr) const;
    bool loadMetadata(const std::filesystem::path& path,
                      client::ReplayRecordingMetadata* metadataOut,
                      std::size_t* frameCountOut = nullptr,
                      std::string* errorOut = nullptr) const;
    std::vector<ReplayArchiveEntry> list() const;

    static std::filesystem::path replayDirectory();
    static std::string defaultTitle(std::uint64_t createdUnixSeconds);

private:
    std::filesystem::path directory_{};
};

}  // namespace app
