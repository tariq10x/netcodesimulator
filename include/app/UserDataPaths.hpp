#pragma once

#include <filesystem>

namespace app {

std::filesystem::path applicationRootDirectory();
std::filesystem::path userDataDirectory();

}  // namespace app
