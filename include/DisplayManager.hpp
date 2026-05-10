#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <raylib.h>

namespace display {

// Platform adapter for window, render-target, and input-transform lifecycle only.
// It must not own gameplay, networking, or study-policy semantics.

enum class ResolutionPreset : std::uint8_t {
    HD1080 = 0,
    HD540 = 1
};

struct ResolutionOption {
    ResolutionPreset preset;
    int windowWidth;
    int windowHeight;
    const char* label;
};

struct DisplayMetrics {
    int logicalWindowWidth;
    int logicalWindowHeight;
    int framebufferWidth;
    int framebufferHeight;
    float framebufferScaleX;
    float framebufferScaleY;
    Rectangle logicalDestination;
    Rectangle framebufferDestination;
};

std::size_t resolutionOptionCount();
ResolutionPreset resolutionPresetAt(std::size_t index);
const ResolutionOption& resolutionOption(ResolutionPreset preset);

ResolutionPreset currentResolutionPreset();
std::string currentWindowLabel();
int currentWindowWidth();
int currentWindowHeight();
DisplayMetrics currentDisplayMetrics();

void setResolutionPreset(ResolutionPreset preset);
void applyResolutionPreset(ResolutionPreset preset);

void initWindow(const char* title);
void shutdownWindow();

void updateWindowPolicy();
void syncInputTransform();
Rectangle destinationRect();
void enableCursorPreservingPosition();
void disableCursorForCapture();
bool isCursorCaptured();
bool releaseCursorIfCaptured();
Vector2 windowPointToVirtual(Vector2 windowPosition, Rectangle destination);
Vector2 windowDeltaToVirtual(Vector2 windowDelta, Rectangle destination);
Vector2 mousePosition();
Vector2 mouseDelta();

void beginFrame();
void endFrame();

DisplayMetrics displayMetricsForTest(int logicalWindowWidth,
                                     int logicalWindowHeight,
                                     int framebufferWidth,
                                     int framebufferHeight);

}  // namespace display
