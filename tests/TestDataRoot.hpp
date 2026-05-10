#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace testsupport {

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

class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~ScopedDirectoryCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

private:
    std::filesystem::path path_;
};

inline std::filesystem::path makeUniqueTempDirectory(const std::string& prefix) {
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path();

    for (int attempt = 0; attempt < 1024; ++attempt) {
        const std::filesystem::path path =
            tempRoot / (prefix + "-" + std::to_string(seed) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (std::filesystem::create_directories(path, ec) && !ec) {
            return path;
        }
    }

    throw std::runtime_error("failed to create isolated test data root for " + prefix);
}

class ScopedTestDataRoot {
public:
    explicit ScopedTestDataRoot(const std::string& prefix)
        : root_(makeUniqueTempDirectory(prefix)),
          cleanup_(root_),
          dataRootEnv_("NETCODESIM_DATA_ROOT", root_.string()) {}

    const std::filesystem::path& root() const {
        return root_;
    }

private:
    std::filesystem::path root_;
    ScopedDirectoryCleanup cleanup_;
    ScopedEnvVar dataRootEnv_;
};

}  // namespace testsupport
