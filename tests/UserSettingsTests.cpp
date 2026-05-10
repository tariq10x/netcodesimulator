#include "app/UserSettings.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <raylib.h>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
#ifdef _WIN32
        char* existing = nullptr;
        std::size_t existingLength = 0u;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
            std::free(existing);
        }
#else
        const char* existing = std::getenv(name_.c_str());
        if (existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
        }
#endif
        set(value);
    }

    ~ScopedEnvVar() {
        if (hadOriginal_) {
            set(originalValue_);
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    bool hadOriginal_{false};
    std::string originalValue_{};
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(original_);
    }

private:
    std::filesystem::path original_;
};

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    expect(file.is_open(), "expected user settings fixture file to open");
    file << text;
}

void testPersistentPathUsesSharedUserDirectory() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-user-settings-path";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    const app::UserSettingsStore store;
    expect(std::filesystem::weakly_canonical(store.persistentPath()) ==
               std::filesystem::weakly_canonical(tempRoot / "repo" / ".netcodesim" / "user_settings.cfg"),
           "user settings should persist under the application run root");
}

void testRoundTripPersistsCustomBindings() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-user-settings-roundtrip";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    app::UserSettings settings;
    settings.controls.mutableBinding(input::ActionId::MoveForward)->slots[0] =
        input::keyboardToken(KEY_UP);
    settings.controls.mutableBinding(input::ActionId::FirePrimary)->slots[0] =
        input::mouseButtonToken(MOUSE_BUTTON_RIGHT);
    settings.controls.mutableBinding(input::ActionId::SpectatorBoost)->slots[0] =
        input::keyboardToken(KEY_LEFT_ALT);
    settings.controls.mutableBinding(input::ActionId::SpectatorBoost)->slots[1] =
        input::keyboardToken(KEY_RIGHT_ALT);

    const app::UserSettingsStore store;
    expect(store.save(settings),
           "user settings should save the current control bindings");
    expect(std::filesystem::exists(store.persistentPath()),
           "user settings should write the persistent config file");

    const app::UserSettingsLoadResult loaded = store.load();
    expect(loaded.loaded, "user settings should reload after a successful save");
    expect(loaded.settings.controls.binding(input::ActionId::MoveForward).slots[0] ==
               input::keyboardToken(KEY_UP),
           "move forward should round-trip through the user settings store");
    expect(loaded.settings.controls.binding(input::ActionId::FirePrimary).slots[0] ==
               input::mouseButtonToken(MOUSE_BUTTON_RIGHT),
           "fire should round-trip mouse-button bindings through the user settings store");
    expect(loaded.settings.controls.binding(input::ActionId::SpectatorBoost).slots[0] ==
               input::keyboardToken(KEY_LEFT_ALT) &&
               loaded.settings.controls.binding(input::ActionId::SpectatorBoost).slots[1] ==
                   input::keyboardToken(KEY_RIGHT_ALT),
           "spectator boost should preserve both binding slots through persistence");
}

void testInvalidEntriesFallBackActionByAction() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-user-settings-invalid";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    const app::UserSettingsStore store;
    writeTextFile(store.persistentPath(),
                  "version=1\n"
                  "move_forward=key:not_real\n"
                  "jump=none,none\n"
                  "fire_primary=mouse:right,none\n"
                  "spectator_boost=key:left_alt,key:not_real\n");

    const app::UserSettingsLoadResult loaded = store.load();
    expect(loaded.loaded, "user settings should read a present config file");
    expect(loaded.settings.controls.binding(input::ActionId::MoveForward).slots[0] ==
               input::keyboardToken(KEY_W),
           "invalid action entries should fall back to the default binding for that action");
    expect(!loaded.settings.controls.binding(input::ActionId::Jump).slots[0].isBound() &&
               !loaded.settings.controls.binding(input::ActionId::Jump).slots[1].isBound(),
           "explicit none entries should allow a control slot to be cleared");
    expect(loaded.settings.controls.binding(input::ActionId::FirePrimary).slots[0] ==
               input::mouseButtonToken(MOUSE_BUTTON_RIGHT),
           "valid persisted bindings should still override defaults when neighboring entries are absent");
    expect(loaded.settings.controls.binding(input::ActionId::SpectatorBoost).slots[0] ==
               input::keyboardToken(KEY_LEFT_ALT) &&
               loaded.settings.controls.binding(input::ActionId::SpectatorBoost).slots[1] ==
                   input::keyboardToken(KEY_RIGHT_SHIFT),
           "invalid secondary entries should leave the untouched default slot in place");
}

void testDefaultOverlapPolicyRemainsExplicit() {
    const input::ControlBindings defaults = input::ControlBindings::defaults();
    expect(input::bindingConflicts(defaults, input::ActionId::Jump),
           "default jump binding should still surface its overlap with spectator ascend");
    expect(input::bindingConflicts(defaults, input::ActionId::SpectatorAscend),
           "default spectator ascend binding should still surface its overlap with jump");
    expect(!input::bindingConflicts(defaults, input::ActionId::FirePrimary),
           "independent actions should not report overlaps when their bindings are unique");
}

}  // namespace

int main() {
    try {
        testPersistentPathUsesSharedUserDirectory();
        testRoundTripPersistsCustomBindings();
        testInvalidEntriesFallBackActionByAction();
        testDefaultOverlapPolicyRemainsExplicit();
        std::cout << "UserSettingsTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "UserSettingsTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
