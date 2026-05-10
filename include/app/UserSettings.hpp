#pragma once

#include <cstdint>
#include <filesystem>

#include "input/ControlBindings.hpp"

namespace app {

struct UserSettings {
    input::ControlBindings controls{};
};

struct UserSettingsLoadResult {
    bool loaded{false};
    UserSettings settings{};
};

class UserSettingsStore {
public:
    static constexpr std::uint32_t kCurrentVersion = 1u;

    std::filesystem::path persistentDirectory() const;
    std::filesystem::path persistentPath() const;

    UserSettingsLoadResult load() const;
    bool save(const UserSettings& settings) const;
};

}  // namespace app
