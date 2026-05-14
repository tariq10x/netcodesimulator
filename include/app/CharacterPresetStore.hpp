#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "character/CharacterProfile.hpp"

namespace app {

class CharacterPresetStore {
public:
    static constexpr unsigned int kCurrentVersion = 1u;

    std::filesystem::path persistentDirectory() const;
    std::filesystem::path profilePath(const std::string& profileId) const;

    std::vector<character::CharacterProfile> loadProfiles() const;
    bool save(const character::CharacterProfile& profile) const;
    bool remove(const std::string& profileId) const;
    std::string nextAvailableProfileId(const std::string& requestedName) const;
};

}  // namespace app
