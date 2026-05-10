#pragma once

#include <filesystem>
#include <string>

#include "replay/ReplayDemo.hpp"

namespace replay {

class ReplayArchive {
public:
    static constexpr const char* kCommandReplayExtension = ".nlcmd";

    bool save(const ReplayDemo& demo,
              const std::filesystem::path& path,
              std::string* errorOut = nullptr) const;
    bool load(const std::filesystem::path& path,
              ReplayDemo* demoOut,
              std::string* errorOut = nullptr) const;

    static bool writeBytes(const ReplayDemo& demo,
                           net::ByteBuffer* bytesOut,
                           std::string* errorOut = nullptr);
    static bool readBytes(const net::ByteBuffer& bytes,
                          ReplayDemo* demoOut,
                          std::string* errorOut = nullptr);
};

}  // namespace replay
