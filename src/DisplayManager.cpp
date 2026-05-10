#include "DisplayManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>

#include "Config3D.hpp"

#ifndef NETCODESIM_ENABLE_HIGHDPI
#define NETCODESIM_ENABLE_HIGHDPI 1
#endif

namespace display {
namespace {

struct WindowDimensions {
    int width{0};
    int height{0};
};

struct DisplayState {
    ResolutionPreset preset{ResolutionPreset::HD540};
    RenderTexture2D target{};
    bool targetLoaded{false};
    bool frameUsingRenderTarget{false};
    bool frameUsingDirectFallback{false};
    bool renderTargetWarningReported{false};
    WindowDimensions lastWindowSize{};
    WindowDimensions lastFramebufferSize{};
    DisplayMetrics lastMetrics{};
    bool hasDisplayMetrics{false};
    Vector2 lastFreeMousePosition{};
    bool hasLastFreeMousePosition{false};
};

constexpr WindowDimensions kVirtualRenderSize{Config::SCREEN_WIDTH, Config::SCREEN_HEIGHT};
constexpr float kLockedAspectRatio =
    static_cast<float>(Config::SCREEN_WIDTH) / static_cast<float>(Config::SCREEN_HEIGHT);
constexpr WindowDimensions kMinimumWindowSize{640, 360};

constexpr std::array<ResolutionOption, 2> kResolutionOptions{{
    {ResolutionPreset::HD1080, 1920, 1080, "1920x1080"},
    {ResolutionPreset::HD540, 960, 540, "960x540"}
}};

DisplayState& state() {
    static DisplayState displayState;
    return displayState;
}

const ResolutionOption& fallbackResolutionOption() {
    return kResolutionOptions.front();
}

WindowDimensions presetWindowSize(ResolutionPreset preset) {
    for (const auto& option : kResolutionOptions) {
        if (option.preset == preset) {
            return WindowDimensions{option.windowWidth, option.windowHeight};
        }
    }

    const ResolutionOption& fallback = fallbackResolutionOption();
    return WindowDimensions{fallback.windowWidth, fallback.windowHeight};
}

WindowDimensions currentLogicalWindowSize() {
    if (!IsWindowReady()) {
        return presetWindowSize(state().preset);
    }

    return WindowDimensions{GetScreenWidth(), GetScreenHeight()};
}

WindowDimensions currentFramebufferSize() {
    DisplayState& current = state();
    if (!IsWindowReady()) {
        return presetWindowSize(current.preset);
    }

    if (current.frameUsingRenderTarget && current.hasDisplayMetrics) {
        return current.lastFramebufferSize;
    }

    WindowDimensions size{GetRenderWidth(), GetRenderHeight()};
    if (size.width <= 0 || size.height <= 0) {
        size = currentLogicalWindowSize();
    }
    return size;
}

WindowDimensions sanitizeDimensions(WindowDimensions size, WindowDimensions fallback) {
    if (size.width <= 0 || size.height <= 0) {
        return fallback;
    }
    return size;
}

Rectangle fitVirtualCanvasInto(WindowDimensions surface) {
    surface = sanitizeDimensions(surface, kVirtualRenderSize);
    const float scale = std::min(static_cast<float>(surface.width) /
                                     static_cast<float>(Config::SCREEN_WIDTH),
                                 static_cast<float>(surface.height) /
                                     static_cast<float>(Config::SCREEN_HEIGHT));
    const float width = static_cast<float>(Config::SCREEN_WIDTH) * scale;
    const float height = static_cast<float>(Config::SCREEN_HEIGHT) * scale;
    return Rectangle{
        (static_cast<float>(surface.width) - width) * 0.5f,
        (static_cast<float>(surface.height) - height) * 0.5f,
        width,
        height
    };
}

DisplayMetrics buildDisplayMetrics(WindowDimensions logicalWindowSize,
                                   WindowDimensions framebufferSize) {
    logicalWindowSize = sanitizeDimensions(logicalWindowSize, kVirtualRenderSize);
    framebufferSize = sanitizeDimensions(framebufferSize, logicalWindowSize);
    return DisplayMetrics{
        logicalWindowSize.width,
        logicalWindowSize.height,
        framebufferSize.width,
        framebufferSize.height,
        static_cast<float>(framebufferSize.width) /
            static_cast<float>(std::max(logicalWindowSize.width, 1)),
        static_cast<float>(framebufferSize.height) /
            static_cast<float>(std::max(logicalWindowSize.height, 1)),
        fitVirtualCanvasInto(logicalWindowSize),
        fitVirtualCanvasInto(framebufferSize)
    };
}

bool metricsChanged(const DisplayMetrics& lhs, const DisplayMetrics& rhs) {
    return lhs.logicalWindowWidth != rhs.logicalWindowWidth ||
           lhs.logicalWindowHeight != rhs.logicalWindowHeight ||
           lhs.framebufferWidth != rhs.framebufferWidth ||
           lhs.framebufferHeight != rhs.framebufferHeight;
}

void reportDisplayMetricsIfChanged(const char* reason, const DisplayMetrics& metrics) {
    DisplayState& current = state();
    if (current.hasDisplayMetrics && !metricsChanged(current.lastMetrics, metrics)) {
        return;
    }

    std::cout << "Display metrics";
    if (reason != nullptr && reason[0] != '\0') {
        std::cout << " (" << reason << ")";
    }
    std::cout << ": logical=" << metrics.logicalWindowWidth << "x"
              << metrics.logicalWindowHeight
              << " framebuffer=" << metrics.framebufferWidth << "x"
              << metrics.framebufferHeight
              << " scale=" << metrics.framebufferScaleX << "x"
              << metrics.framebufferScaleY
              << " virtual=" << Config::SCREEN_WIDTH << "x"
              << Config::SCREEN_HEIGHT
              << std::endl;
}

DisplayMetrics refreshDisplayMetrics(const char* reason) {
    DisplayState& current = state();
    const DisplayMetrics metrics =
        buildDisplayMetrics(currentLogicalWindowSize(), currentFramebufferSize());
    reportDisplayMetricsIfChanged(reason, metrics);
    current.lastFramebufferSize = WindowDimensions{
        metrics.framebufferWidth,
        metrics.framebufferHeight
    };
    current.lastMetrics = metrics;
    current.hasDisplayMetrics = true;
    return metrics;
}

WindowDimensions clampWindowSize(WindowDimensions size) {
    size.width = std::max(size.width, kMinimumWindowSize.width);
    size.height = std::max(size.height, kMinimumWindowSize.height);
    return size;
}

WindowDimensions lockAspectRatio(WindowDimensions requested,
                                 WindowDimensions previous) {
    requested = clampWindowSize(requested);
    previous = clampWindowSize(previous);

    const int widthDelta = std::abs(requested.width - previous.width);
    const int heightDelta = std::abs(requested.height - previous.height);
    const int widthFromHeight =
        std::max(kMinimumWindowSize.width,
                 static_cast<int>(std::lround(static_cast<float>(requested.height) *
                                              kLockedAspectRatio)));
    const int heightFromWidth =
        std::max(kMinimumWindowSize.height,
                 static_cast<int>(std::lround(static_cast<float>(requested.width) /
                                              kLockedAspectRatio)));

    if (widthDelta > heightDelta) {
        requested.height = heightFromWidth;
    } else if (heightDelta > widthDelta) {
        requested.width = widthFromHeight;
    } else {
        const int widthDrivenDelta = std::abs(heightFromWidth - requested.height);
        const int heightDrivenDelta = std::abs(widthFromHeight - requested.width);
        if (widthDrivenDelta <= heightDrivenDelta) {
            requested.height = heightFromWidth;
        } else {
            requested.width = widthFromHeight;
        }
    }

    return clampWindowSize(requested);
}

void cacheWindowSize(WindowDimensions size) {
    state().lastWindowSize = clampWindowSize(size);
}

Vector2 clampMousePositionToWindow(Vector2 position) {
    const WindowDimensions size = currentLogicalWindowSize();
    return Vector2{
        std::clamp(position.x, 0.0f, static_cast<float>(std::max(size.width - 1, 0))),
        std::clamp(position.y, 0.0f, static_cast<float>(std::max(size.height - 1, 0)))
    };
}

void rememberFreeMousePosition(Vector2 position) {
    DisplayState& current = state();
    current.lastFreeMousePosition = clampMousePositionToWindow(position);
    current.hasLastFreeMousePosition = true;
}

void releaseRenderTarget() {
    DisplayState& current = state();
    if (!current.targetLoaded || !IsWindowReady()) {
        current.targetLoaded = false;
        current.target = RenderTexture2D{};
        return;
    }

    UnloadRenderTexture(current.target);
    current.target = RenderTexture2D{};
    current.targetLoaded = false;
}

void ensureRenderTarget() {
    DisplayState& current = state();
    if (!IsWindowReady()) {
        return;
    }
    if (current.targetLoaded &&
        current.target.texture.width == Config::SCREEN_WIDTH &&
        current.target.texture.height == Config::SCREEN_HEIGHT) {
        return;
    }
    if (current.targetLoaded) {
        releaseRenderTarget();
    }

    current.target = LoadRenderTexture(Config::SCREEN_WIDTH, Config::SCREEN_HEIGHT);
    current.targetLoaded = current.target.id != 0u && current.target.texture.id != 0u;
    if (current.targetLoaded) {
        SetTextureFilter(current.target.texture, TEXTURE_FILTER_BILINEAR);
        return;
    }

    if (current.target.id != 0u || current.target.texture.id != 0u) {
        UnloadRenderTexture(current.target);
    }
    current.target = RenderTexture2D{};
    if (!current.renderTargetWarningReported) {
        current.renderTargetWarningReported = true;
        std::cerr << "Display warning: failed to create "
                  << Config::SCREEN_WIDTH << "x" << Config::SCREEN_HEIGHT
                  << " render target; falling back to direct window rendering."
                  << std::endl;
    }
}

void presentStartupFrame() {
    if (!IsWindowReady()) {
        return;
    }

    BeginDrawing();
    ClearBackground(Color{8, 10, 15, 255});
    DrawText("Starting Netcode Simulator...",
             24,
             std::max(24, GetScreenHeight() - 48),
             20,
             LIGHTGRAY);
    EndDrawing();
}

}  // namespace

std::size_t resolutionOptionCount() {
    return kResolutionOptions.size();
}

ResolutionPreset resolutionPresetAt(std::size_t index) {
    return kResolutionOptions[std::min(index, kResolutionOptions.size() - 1)].preset;
}

const ResolutionOption& resolutionOption(ResolutionPreset preset) {
    for (const auto& option : kResolutionOptions) {
        if (option.preset == preset) {
            return option;
        }
    }
    return fallbackResolutionOption();
}

ResolutionPreset currentResolutionPreset() {
    return state().preset;
}

std::string currentWindowLabel() {
    const WindowDimensions size = currentLogicalWindowSize();
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

int currentWindowWidth() {
    return currentLogicalWindowSize().width;
}

int currentWindowHeight() {
    return currentLogicalWindowSize().height;
}

DisplayMetrics currentDisplayMetrics() {
    if (!IsWindowReady()) {
        const WindowDimensions fallback = presetWindowSize(state().preset);
        return buildDisplayMetrics(fallback, fallback);
    }

    if (state().frameUsingRenderTarget && state().hasDisplayMetrics) {
        return state().lastMetrics;
    }

    return refreshDisplayMetrics("query");
}

void setResolutionPreset(ResolutionPreset preset) {
    state().preset = preset;
}

void applyResolutionPreset(ResolutionPreset preset) {
    setResolutionPreset(preset);
    if (IsWindowReady()) {
        const WindowDimensions targetSize = presetWindowSize(preset);
        SetWindowSize(targetSize.width, targetSize.height);
        cacheWindowSize(targetSize);
        updateWindowPolicy();
    }
}

void initWindow(const char* title) {
    if (IsWindowReady()) {
        shutdownWindow();
    }

    const WindowDimensions initialSize = presetWindowSize(state().preset);
    unsigned int windowFlags = FLAG_WINDOW_RESIZABLE;
#if NETCODESIM_ENABLE_HIGHDPI
    windowFlags |= FLAG_WINDOW_HIGHDPI;
#endif
    SetConfigFlags(windowFlags);
    InitWindow(initialSize.width, initialSize.height, title);
    SetTargetFPS(Config::TARGET_FPS);
    SetExitKey(KEY_NULL);
    SetWindowMinSize(kMinimumWindowSize.width, kMinimumWindowSize.height);
    cacheWindowSize(currentLogicalWindowSize());
    refreshDisplayMetrics("init");
    updateWindowPolicy();
    ensureRenderTarget();
    presentStartupFrame();
}

void shutdownWindow() {
    if (IsWindowReady()) {
        releaseRenderTarget();
        CloseWindow();
    } else {
        DisplayState& current = state();
        current.target = RenderTexture2D{};
        current.targetLoaded = false;
    }
}

Rectangle destinationRect() {
    if (!IsWindowReady()) {
        return Rectangle{
            0.0f,
            0.0f,
            static_cast<float>(Config::SCREEN_WIDTH),
            static_cast<float>(Config::SCREEN_HEIGHT)
        };
    }

    return currentDisplayMetrics().logicalDestination;
}

void syncInputTransform() {
    if (!IsWindowReady() || IsWindowMinimized()) {
        return;
    }

    // Keep raylib's global mouse state in logical window coordinates. UI code
    // maps to virtual render coordinates explicitly via display::mousePosition().
    // Do not reset raylib's mouse scale here: raylib 6.0 uses that scale for
    // platform-specific HighDPI correction on Windows and Linux.
    SetMouseOffset(0, 0);
}

void enableCursorPreservingPosition() {
    if (!IsWindowReady()) {
        EnableCursor();
        return;
    }

    syncInputTransform();
    if (!IsCursorHidden()) {
        rememberFreeMousePosition(GetMousePosition());
        return;
    }

    const Vector2 restorePosition = state().hasLastFreeMousePosition
        ? state().lastFreeMousePosition
        : GetMousePosition();

    EnableCursor();
    const Vector2 clampedPosition = clampMousePositionToWindow(restorePosition);
    SetMousePosition(static_cast<int>(std::lround(clampedPosition.x)),
                     static_cast<int>(std::lround(clampedPosition.y)));
    rememberFreeMousePosition(clampedPosition);
    syncInputTransform();
}

void disableCursorForCapture() {
    if (!IsWindowReady()) {
        DisableCursor();
        return;
    }

    syncInputTransform();
    if (!IsCursorHidden()) {
        rememberFreeMousePosition(GetMousePosition());
    }
    DisableCursor();
    syncInputTransform();
}

bool isCursorCaptured() {
    return IsWindowReady() && IsCursorHidden();
}

bool releaseCursorIfCaptured() {
    if (!isCursorCaptured()) {
        return false;
    }

    enableCursorPreservingPosition();
    return true;
}

Vector2 windowPointToVirtual(Vector2 windowPosition, Rectangle dest) {
    const float safeWidth = std::max(dest.width, 1.0f);
    const float safeHeight = std::max(dest.height, 1.0f);
    const float scaleX = static_cast<float>(Config::SCREEN_WIDTH) / safeWidth;
    const float scaleY = static_cast<float>(Config::SCREEN_HEIGHT) / safeHeight;

    return Vector2{
        (windowPosition.x - dest.x) * scaleX,
        (windowPosition.y - dest.y) * scaleY
    };
}

Vector2 windowDeltaToVirtual(Vector2 windowDelta, Rectangle dest) {
    const float safeWidth = std::max(dest.width, 1.0f);
    const float safeHeight = std::max(dest.height, 1.0f);
    const float scaleX = static_cast<float>(Config::SCREEN_WIDTH) / safeWidth;
    const float scaleY = static_cast<float>(Config::SCREEN_HEIGHT) / safeHeight;

    return Vector2{
        windowDelta.x * scaleX,
        windowDelta.y * scaleY
    };
}

Vector2 mousePosition() {
    if (!IsWindowReady() || IsWindowMinimized()) {
        return GetMousePosition();
    }

    const Vector2 rawPosition = GetMousePosition();
    if (!IsCursorHidden()) {
        rememberFreeMousePosition(rawPosition);
    }
    return windowPointToVirtual(rawPosition, destinationRect());
}

Vector2 mouseDelta() {
    if (!IsWindowReady() || IsWindowMinimized()) {
        return GetMouseDelta();
    }

    return windowDeltaToVirtual(GetMouseDelta(), destinationRect());
}

void updateWindowPolicy() {
    if (!IsWindowReady() || IsWindowMinimized()) {
        return;
    }

    WindowDimensions actualSize = currentLogicalWindowSize();
    if (state().lastWindowSize.width <= 0 || state().lastWindowSize.height <= 0) {
        cacheWindowSize(actualSize);
    }

    const WindowDimensions adjustedSize = lockAspectRatio(actualSize, state().lastWindowSize);
    if (adjustedSize.width != actualSize.width || adjustedSize.height != actualSize.height) {
        SetWindowSize(adjustedSize.width, adjustedSize.height);
        actualSize = adjustedSize;
    }

    cacheWindowSize(actualSize);
    syncInputTransform();
    refreshDisplayMetrics("policy");
}

void beginFrame() {
    const DisplayMetrics metrics = IsWindowReady() && !IsWindowMinimized()
        ? refreshDisplayMetrics("frame")
        : currentDisplayMetrics();
    ensureRenderTarget();
    DisplayState& current = state();
    current.frameUsingRenderTarget = current.targetLoaded;
    current.frameUsingDirectFallback = !current.targetLoaded;

    if (current.frameUsingRenderTarget) {
        BeginTextureMode(current.target);
        return;
    }

    BeginDrawing();
    ClearBackground(BLACK);

    const Rectangle dest = metrics.logicalDestination;
    BeginScissorMode(static_cast<int>(std::lround(dest.x)),
                     static_cast<int>(std::lround(dest.y)),
                     std::max(1, static_cast<int>(std::lround(dest.width))),
                     std::max(1, static_cast<int>(std::lround(dest.height))));
    Camera2D virtualCamera{};
    virtualCamera.offset = Vector2{
        metrics.framebufferDestination.x,
        metrics.framebufferDestination.y
    };
    virtualCamera.target = Vector2{0.0f, 0.0f};
    virtualCamera.rotation = 0.0f;
    virtualCamera.zoom = std::max(1.0f, metrics.framebufferDestination.width) /
        static_cast<float>(Config::SCREEN_WIDTH);
    BeginMode2D(virtualCamera);
}

void endFrame() {
    DisplayState& current = state();
    if (current.frameUsingDirectFallback) {
        EndMode2D();
        EndScissorMode();
        EndDrawing();
        current.frameUsingDirectFallback = false;
        return;
    }

    if (!current.frameUsingRenderTarget) {
        return;
    }

    EndTextureMode();

    BeginDrawing();
    ClearBackground(BLACK);

    // Final present only. World/effect policy must stay outside DisplayManager.
    const Rectangle source{
        0.0f,
        0.0f,
        static_cast<float>(Config::SCREEN_WIDTH),
        -static_cast<float>(Config::SCREEN_HEIGHT)
    };
    DrawTexturePro(current.target.texture,
                   source,
                   currentDisplayMetrics().logicalDestination,
                   Vector2{0.0f, 0.0f},
                   0.0f,
                   WHITE);
    EndDrawing();
    current.frameUsingRenderTarget = false;
}

DisplayMetrics displayMetricsForTest(int logicalWindowWidth,
                                     int logicalWindowHeight,
                                     int framebufferWidth,
                                     int framebufferHeight) {
    return buildDisplayMetrics(WindowDimensions{logicalWindowWidth, logicalWindowHeight},
                               WindowDimensions{framebufferWidth, framebufferHeight});
}

}  // namespace display
