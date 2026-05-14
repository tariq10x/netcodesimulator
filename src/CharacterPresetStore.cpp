#include "app/CharacterPresetStore.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "app/UserDataPaths.hpp"

namespace app {
namespace {

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1u);
}

bool parseFloat(const std::string& text, float* valueOut) {
    if (valueOut == nullptr) {
        return false;
    }
    std::istringstream stream(text);
    float value = 0.0f;
    stream >> value;
    if (!stream || !stream.eof()) {
        return false;
    }
    *valueOut = value;
    return true;
}

bool parseProfileFile(const std::filesystem::path& path,
                      character::CharacterProfile* profileOut) {
    if (profileOut == nullptr) {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    character::CharacterProfile profile;
    profile.builtIn = false;
    bool sawVersion = false;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1u));
        if (key == "version") {
            sawVersion = value == "1";
        } else if (key == "id") {
            profile.id = value;
        } else if (key == "name") {
            profile.name = value;
        } else if (key == "shoulder_width") {
            parseFloat(value, &profile.appearance.shoulderWidth);
        } else if (key == "shoulder_height") {
            parseFloat(value, &profile.appearance.shoulderHeight);
        } else if (key == "shoulder_angle_deg") {
            parseFloat(value, &profile.appearance.shoulderAngleDeg);
        }
    }

    profile = character::normalizeProfile(profile);
    if (!sawVersion || profile.builtIn || profile.id.empty()) {
        return false;
    }

    *profileOut = profile;
    return true;
}

}  // namespace

std::filesystem::path CharacterPresetStore::persistentDirectory() const {
    return userDataDirectory() / "characters";
}

std::filesystem::path CharacterPresetStore::profilePath(const std::string& profileId) const {
    const std::string sanitized = character::sanitizeProfileId(profileId);
    return persistentDirectory() / (sanitized + ".character");
}

std::vector<character::CharacterProfile> CharacterPresetStore::loadProfiles() const {
    std::vector<character::CharacterProfile> profiles;
    profiles.push_back(character::defaultProfile());

    const std::filesystem::path directory = persistentDirectory();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec || !std::filesystem::exists(directory, ec)) {
        return profiles;
    }

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".character") {
            continue;
        }
        character::CharacterProfile profile;
        if (parseProfileFile(entry.path(), &profile)) {
            profiles.push_back(profile);
        }
    }

    std::sort(profiles.begin() + 1,
              profiles.end(),
              [](const character::CharacterProfile& lhs,
                 const character::CharacterProfile& rhs) {
                  if (lhs.name == rhs.name) {
                      return lhs.id < rhs.id;
                  }
                  return lhs.name < rhs.name;
              });
    return profiles;
}

bool CharacterPresetStore::save(const character::CharacterProfile& profile) const {
    character::CharacterProfile normalized = character::normalizeProfile(profile);
    if (normalized.builtIn || normalized.id.empty()) {
        return false;
    }

    const std::filesystem::path path = profilePath(normalized.id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file << "version=" << kCurrentVersion << '\n';
    file << "id=" << normalized.id << '\n';
    file << "name=" << normalized.name << '\n';
    file << "shoulder_width=" << normalized.appearance.shoulderWidth << '\n';
    file << "shoulder_height=" << normalized.appearance.shoulderHeight << '\n';
    file << "shoulder_angle_deg=" << normalized.appearance.shoulderAngleDeg << '\n';
    return true;
}

bool CharacterPresetStore::remove(const std::string& profileId) const {
    const std::string sanitized = character::sanitizeProfileId(profileId);
    if (sanitized.empty() || character::isBuiltInProfileId(sanitized)) {
        return false;
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(profilePath(sanitized), ec);
    return removed && !ec;
}

std::string CharacterPresetStore::nextAvailableProfileId(const std::string& requestedName) const {
    std::string base = character::sanitizeProfileId(requestedName);
    if (base.empty() || character::isBuiltInProfileId(base)) {
        base = "character";
    }

    const std::vector<character::CharacterProfile> profiles = loadProfiles();
    auto exists = [&profiles](const std::string& id) {
        return std::any_of(profiles.begin(),
                           profiles.end(),
                           [&id](const character::CharacterProfile& profile) {
                               return profile.id == id;
                           });
    };

    if (!exists(base)) {
        return base;
    }
    for (unsigned int suffix = 2u; suffix < 1000u; ++suffix) {
        const std::string candidate = base + "-" + std::to_string(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return base + "-copy";
}

}  // namespace app
