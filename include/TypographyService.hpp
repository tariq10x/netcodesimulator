#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include <raylib.h>

#include "TypographyTheme.hpp"

struct TypographyMeasurement {
    float width{0.0f};
    float height{0.0f};
    float lineHeight{0.0f};
};

class TypographyService {
public:
    explicit TypographyService(std::filesystem::path assetRoot = {});
    ~TypographyService();

    TypographyService(const TypographyService&) = delete;
    TypographyService& operator=(const TypographyService&) = delete;

    static TypographyService& shared();

    void setAssetRoot(std::filesystem::path assetRoot);
    bool initialize();
    void shutdown();

    const TypographyStyle& style(TypographyStyleId id) const;
    TypographyMeasurement measure(TypographyStyleId id, std::string_view text);
    int measureWidth(TypographyStyleId id, std::string_view text);
    void draw(TypographyStyleId id, std::string_view text, Vector2 position, Color color);
    void drawCentered(TypographyStyleId id, std::string_view text, float centerX, float y, Color color);

    bool isFallbackActive() const;
    std::size_t failureReportCount() const;
    const std::string& fallbackSummary() const;
    const std::string& configuredFamily() const;
    const std::filesystem::path& assetRoot() const;

private:
    struct FontAsset {
        const char* fileName{nullptr};
        std::filesystem::path resolvedPath{};
        Font font{};
        bool assetPresent{false};
        bool fontLoaded{false};
    };

    const FontAsset& assetFor(TypographyFontRole role) const;
    FontAsset& assetFor(TypographyFontRole role);
    std::filesystem::path resolveAssetRoot() const;
    void populateAssetPaths();
    void activateFallback(const std::string& reason);
    bool ensureFontLoaded(TypographyFontRole role);
    Font activeFontFor(TypographyFontRole role);

    std::filesystem::path assetRoot_{};
    bool initialized_{false};
    bool fallbackActive_{false};
    std::size_t failureReportCount_{0u};
    std::string fallbackSummary_{};
    std::string configuredFamily_{TypographyTheme::family()};
    std::unordered_map<TypographyFontRole, FontAsset> assets_{};
};
