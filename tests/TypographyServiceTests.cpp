#include "TypographyService.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path findRepoRoot() {
    std::filesystem::path probe = std::filesystem::current_path();
    while (!probe.empty()) {
        if (std::filesystem::exists(probe / "CMakeLists.txt") &&
            std::filesystem::exists(probe / "src/main_3d.cpp")) {
            return probe;
        }
        if (probe == probe.root_path()) {
            break;
        }
        probe = probe.parent_path();
    }

    throw std::runtime_error("failed to locate repository root for typography tests");
}

void testTypographyRegistryLoadsBundledInterAssets() {
    TypographyService service(findRepoRoot() / "assets" / "fonts");

    expect(service.initialize(), "typography registry should initialize when bundled Inter assets exist");
    expect(service.configuredFamily() == "Inter",
           "typography registry should expose Inter as the configured default family");
    expect(!service.isFallbackActive(),
           "typography registry should not enter fallback when all bundled assets are present");
    expect(service.failureReportCount() == 0u,
           "successful typography initialization should not report a fallback failure");

    const TypographyStyle& appTitle = service.style(TypographyStyleId::AppTitle);
    const TypographyStyle& overlayBody = service.style(TypographyStyleId::OverlayBody);
    const TypographyStyle& diagnostics = service.style(TypographyStyleId::Diagnostics);

    expect(appTitle.fontRole == TypographyFontRole::Bold && appTitle.fontSize == 72,
           "app title typography style should resolve deterministic bold weight metadata");
    expect(overlayBody.fontRole == TypographyFontRole::Regular && overlayBody.fontSize == 21,
           "overlay body typography style should resolve deterministic regular weight metadata");
    expect(diagnostics.fontRole == TypographyFontRole::Regular && diagnostics.fontSize == 18,
           "diagnostics typography style should resolve deterministic size metadata");
}

void testTypographyRegistryReportsOneCentralizedFallback() {
    const std::filesystem::path missingRoot =
        std::filesystem::temp_directory_path() / "netcodesim-missing-typography-assets";
    std::filesystem::remove_all(missingRoot);
    std::filesystem::create_directories(missingRoot);

    TypographyService service(missingRoot);

    expect(!service.initialize(),
           "typography registry should fail initialization when bundled assets are missing");
    expect(service.isFallbackActive(),
           "typography registry should enter centralized fallback when bundled assets are missing");
    expect(service.failureReportCount() == 1u,
           "typography registry should emit exactly one centralized fallback report");
    expect(service.fallbackSummary().find("Inter") != std::string::npos,
           "typography fallback summary should identify the configured family");

    const std::size_t reportCountBeforeSecondInitialize = service.failureReportCount();
    expect(!service.initialize(),
           "repeat initialization after a missing-asset failure should keep the registry in fallback");
    expect(service.failureReportCount() == reportCountBeforeSecondInitialize,
           "repeat initialization should not emit per-screen fallback failures");
}

void testBundledInterAssetsExistInBuildOutput() {
    const std::filesystem::path buildFontsRoot =
        std::filesystem::path(NETCODESIM_RUNTIME_ASSET_DIR) / "fonts";
    static constexpr std::array<const char*, 3> kExpectedFonts{
        "Inter-Regular.ttf",
        "Inter-SemiBold.ttf",
        "Inter-Bold.ttf"
    };

    for (const char* fileName : kExpectedFonts) {
        const std::filesystem::path fontPath = buildFontsRoot / fileName;
        expect(std::filesystem::exists(fontPath),
               "build output should contain bundled typography asset: " + fontPath.string());
    }

    TypographyService service(buildFontsRoot);
    expect(service.initialize(),
           "typography registry should initialize from the build-output asset copy");
    expect(!service.isFallbackActive(),
           "copied build-output typography assets should keep the service out of fallback mode");
}

}  // namespace

int main() {
    try {
        testTypographyRegistryLoadsBundledInterAssets();
        testTypographyRegistryReportsOneCentralizedFallback();
        testBundledInterAssetsExistInBuildOutput();
        std::cout << "TypographyServiceTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "TypographyServiceTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
