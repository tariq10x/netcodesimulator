#include "app/UserSettings.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::vector<std::string> splitCommaSeparated(const std::string& text) {
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        values.push_back(trim(item));
    }
    if (values.empty()) {
        values.push_back(trim(text));
    }
    return values;
}

}  // namespace

std::filesystem::path UserSettingsStore::persistentDirectory() const {
    return userDataDirectory();
}

std::filesystem::path UserSettingsStore::persistentPath() const {
    return persistentDirectory() / "user_settings.cfg";
}

UserSettingsLoadResult UserSettingsStore::load() const {
    UserSettingsLoadResult result;
    std::ifstream file(persistentPath());
    if (!file.is_open()) {
        return result;
    }

    result.loaded = true;
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
            continue;
        }

        for (const input::ActionDescriptor& descriptor : input::actionDescriptors()) {
            if (key != descriptor.persistentId) {
                continue;
            }

            input::ActionBinding* binding = result.settings.controls.mutableBinding(descriptor.id);
            if (binding == nullptr) {
                break;
            }

            *binding = input::defaultBinding(descriptor.id);
            const std::vector<std::string> tokens = splitCommaSeparated(value);
            const std::size_t slotCount = std::min(tokens.size(), binding->slots.size());
            for (std::size_t slotIndex = 0; slotIndex < slotCount; ++slotIndex) {
                input::InputToken parsedToken;
                if (input::tryParseToken(tokens[slotIndex], &parsedToken)) {
                    binding->slots[slotIndex] = parsedToken;
                }
            }
            break;
        }
    }

    return result;
}

bool UserSettingsStore::save(const UserSettings& settings) const {
    const std::filesystem::path path = persistentPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file << "version=" << kCurrentVersion << '\n';
    for (const input::ActionDescriptor& descriptor : input::actionDescriptors()) {
        const input::ActionBinding& binding = settings.controls.binding(descriptor.id);
        file << descriptor.persistentId << '=';
        for (std::size_t slotIndex = 0; slotIndex < binding.slots.size(); ++slotIndex) {
            if (slotIndex > 0u) {
                file << ',';
            }
            file << input::serializeToken(binding.slots[slotIndex]);
        }
        file << '\n';
    }

    return true;
}

}  // namespace app
