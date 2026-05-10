#include "TypographyService.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::array<std::pair<TypographyFontRole, const char*>, 3> kFontFiles{{
    {TypographyFontRole::Regular, "Inter-Regular.ttf"},
    {TypographyFontRole::SemiBold, "Inter-SemiBold.ttf"},
    {TypographyFontRole::Bold, "Inter-Bold.ttf"},
}};

std::filesystem::path sourceAssetRoot() {
#ifdef NETCODESIM_SOURCE_DIR
    return std::filesystem::path(NETCODESIM_SOURCE_DIR) / "assets" / "fonts";
#else
    return {};
#endif
}

std::filesystem::path executableAssetRoot() {
    const char* appDirectory = GetApplicationDirectory();
    if (appDirectory == nullptr || *appDirectory == '\0') {
        return {};
    }
    return std::filesystem::path(appDirectory) / "assets" / "fonts";
}

std::filesystem::path envAssetRoot() {
#if defined(_WIN32)
    char* configured = nullptr;
    std::size_t configuredLength = 0u;
    if (_dupenv_s(&configured, &configuredLength, "NETCODESIM_TYPOGRAPHY_ASSET_ROOT") != 0 ||
        configured == nullptr) {
        return {};
    }
    std::string configuredValue(configured);
    std::free(configured);
#else
    const char* configured = std::getenv("NETCODESIM_TYPOGRAPHY_ASSET_ROOT");
    const std::string configuredValue = configured != nullptr ? std::string(configured) : std::string{};
#endif
    if (configuredValue.empty()) {
        return {};
    }
    return std::filesystem::path(configuredValue);
}

std::string missingAssetSummary(const std::vector<std::filesystem::path>& missing) {
    std::ostringstream summary;
    summary << "Typography asset load failed for family "
            << TypographyTheme::family()
            << "; missing assets:";
    for (const auto& path : missing) {
        summary << ' ' << path.string();
    }
    return summary.str();
}

}  // namespace

TypographyService::TypographyService(std::filesystem::path assetRoot)
    : assetRoot_(std::move(assetRoot)) {
    for (const auto& [role, fileName] : kFontFiles) {
        assets_.emplace(role, FontAsset{fileName});
    }
}

TypographyService::~TypographyService() {
    shutdown();
}

TypographyService& TypographyService::shared() {
    static TypographyService service;
    return service;
}

void TypographyService::setAssetRoot(std::filesystem::path assetRoot) {
    shutdown();
    assetRoot_ = std::move(assetRoot);
}

bool TypographyService::initialize() {
    if (initialized_) {
        return !fallbackActive_;
    }

    initialized_ = true;
    fallbackActive_ = false;
    fallbackSummary_.clear();
    configuredFamily_ = TypographyTheme::family();
    populateAssetPaths();

    std::vector<std::filesystem::path> missing;
    for (const auto& [_, asset] : assets_) {
        if (!asset.assetPresent) {
            missing.push_back(asset.resolvedPath);
        }
    }
    if (!missing.empty()) {
        activateFallback(missingAssetSummary(missing));
    }

    return !fallbackActive_;
}

void TypographyService::shutdown() {
    for (auto& [_, asset] : assets_) {
        if (asset.fontLoaded && asset.font.texture.id != 0) {
            UnloadFont(asset.font);
        }
        asset.font = Font{};
        asset.fontLoaded = false;
        asset.assetPresent = false;
        asset.resolvedPath.clear();
    }
    initialized_ = false;
    fallbackActive_ = false;
    fallbackSummary_.clear();
    failureReportCount_ = 0u;
    configuredFamily_ = TypographyTheme::family();
}

const TypographyStyle& TypographyService::style(TypographyStyleId id) const {
    return TypographyTheme::style(id);
}

TypographyMeasurement TypographyService::measure(TypographyStyleId id, std::string_view text) {
    initialize();

    const TypographyStyle& textStyle = style(id);
    Font font = activeFontFor(textStyle.fontRole);
    const std::string textCopy{text};

    if (!fallbackActive_ && font.texture.id != 0) {
        const Vector2 measurement = MeasureTextEx(
            font,
            textCopy.c_str(),
            static_cast<float>(textStyle.fontSize),
            textStyle.spacing);
        return TypographyMeasurement{measurement.x, measurement.y, textStyle.lineHeight};
    }

    return TypographyMeasurement{
        static_cast<float>(MeasureText(textCopy.c_str(), textStyle.fontSize)),
        textStyle.lineHeight,
        textStyle.lineHeight};
}

int TypographyService::measureWidth(TypographyStyleId id, std::string_view text) {
    return static_cast<int>(measure(id, text).width);
}

void TypographyService::draw(TypographyStyleId id,
                             std::string_view text,
                             Vector2 position,
                             Color color) {
    initialize();

    const TypographyStyle& textStyle = style(id);
    Font font = activeFontFor(textStyle.fontRole);
    const std::string textCopy{text};

    if (!fallbackActive_ && font.texture.id != 0) {
        DrawTextEx(
            font,
            textCopy.c_str(),
            position,
            static_cast<float>(textStyle.fontSize),
            textStyle.spacing,
            color);
        return;
    }

    DrawText(textCopy.c_str(),
             static_cast<int>(position.x),
             static_cast<int>(position.y),
             textStyle.fontSize,
             color);
}

void TypographyService::drawCentered(TypographyStyleId id,
                                     std::string_view text,
                                     float centerX,
                                     float y,
                                     Color color) {
    const TypographyMeasurement measurement = measure(id, text);
    draw(id, text, Vector2{centerX - measurement.width * 0.5f, y}, color);
}

bool TypographyService::isFallbackActive() const {
    return fallbackActive_;
}

std::size_t TypographyService::failureReportCount() const {
    return failureReportCount_;
}

const std::string& TypographyService::fallbackSummary() const {
    return fallbackSummary_;
}

const std::string& TypographyService::configuredFamily() const {
    return configuredFamily_;
}

const std::filesystem::path& TypographyService::assetRoot() const {
    return assetRoot_;
}

const TypographyService::FontAsset& TypographyService::assetFor(TypographyFontRole role) const {
    return assets_.at(role);
}

TypographyService::FontAsset& TypographyService::assetFor(TypographyFontRole role) {
    return assets_.at(role);
}

std::filesystem::path TypographyService::resolveAssetRoot() const {
    if (!assetRoot_.empty()) {
        return assetRoot_;
    }

    const std::array<std::filesystem::path, 6> candidates{{
        envAssetRoot(),
        executableAssetRoot(),
        std::filesystem::current_path() / "assets" / "fonts",
        std::filesystem::current_path() / ".." / "assets" / "fonts",
        std::filesystem::current_path() / ".." / ".." / "assets" / "fonts",
        sourceAssetRoot(),
    }};

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return sourceAssetRoot();
}

void TypographyService::populateAssetPaths() {
    assetRoot_ = resolveAssetRoot();
    for (auto& [_, asset] : assets_) {
        asset.resolvedPath = assetRoot_ / asset.fileName;
        asset.assetPresent = std::filesystem::exists(asset.resolvedPath);
    }
}

void TypographyService::activateFallback(const std::string& reason) {
    fallbackActive_ = true;
    if (failureReportCount_ == 0u) {
        fallbackSummary_ = reason;
        ++failureReportCount_;
        std::cerr << reason << std::endl;
    }
}

bool TypographyService::ensureFontLoaded(TypographyFontRole role) {
    FontAsset& asset = assetFor(role);
    if (asset.fontLoaded) {
        return true;
    }
    if (!asset.assetPresent) {
        return false;
    }
    if (!IsWindowReady()) {
        return false;
    }

    asset.font = LoadFontEx(asset.resolvedPath.string().c_str(), 128, nullptr, 0);
    if (asset.font.texture.id == 0) {
        activateFallback("Typography font load failed for " + asset.resolvedPath.string());
        return false;
    }

    SetTextureFilter(asset.font.texture, TEXTURE_FILTER_BILINEAR);
    asset.fontLoaded = true;
    return true;
}

Font TypographyService::activeFontFor(TypographyFontRole role) {
    initialize();
    if (!fallbackActive_ && ensureFontLoaded(role)) {
        return assetFor(role).font;
    }
    return GetFontDefault();
}
