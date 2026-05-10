#include "app/UserDataPaths.hpp"

#include <cstdlib>
#include <string>
#include <system_error>

namespace app {
namespace {

constexpr const char* kDataDirectoryName = ".netcodesim";
constexpr const char* kDataRootEnvironmentVariable = "NETCODESIM_DATA_ROOT";

std::filesystem::path defaultDataRoot() {
#ifdef NETCODESIM_SOURCE_DIR
    return std::filesystem::path(NETCODESIM_SOURCE_DIR);
#else
    return std::filesystem::current_path();
#endif
}

std::string environmentVariable(const char* name) {
#if defined(_WIN32)
    char* rawValue = nullptr;
    std::size_t valueLength = 0u;
    if (_dupenv_s(&rawValue, &valueLength, name) != 0 || rawValue == nullptr) {
        return {};
    }
    std::string value(rawValue);
    std::free(rawValue);
    return value;
#else
    const char* rawValue = std::getenv(name);
    return rawValue != nullptr ? std::string(rawValue) : std::string{};
#endif
}

}  // namespace

std::filesystem::path applicationRootDirectory() {
    std::filesystem::path root = defaultDataRoot();
    const std::string overrideRoot = environmentVariable(kDataRootEnvironmentVariable);
    if (!overrideRoot.empty()) {
        root = std::filesystem::path(overrideRoot);
    }
    return root;
}

std::filesystem::path userDataDirectory() {
    std::filesystem::path root = applicationRootDirectory();
    std::filesystem::path base = root / kDataDirectoryName;
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return base;
}

}  // namespace app
