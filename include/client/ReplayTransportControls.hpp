#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <raylib.h>

#include "Config3D.hpp"
#include "TypographyService.hpp"

namespace client {

enum class ReplayCassetteGlyph {
    Record,
    Stop,
    Rewind,
    Play,
    Pause,
    FastForward,
    Export,
    Exit
};

struct ReplayTransportButtonSpec {
    ReplayCassetteGlyph glyph{ReplayCassetteGlyph::Play};
    std::string_view hotkey{};
    bool enabled{true};
    bool pressed{false};
    Color accent{WHITE};
};

struct ReplayCheckpointOverlayState {
    bool visible{false};
    bool detachedCameraActive{false};
    int activeIndex{-1};
    std::size_t checkpointCount{0u};
    float transitionToNextSeconds{0.0f};
    bool transitionEditable{false};
};

struct ReplayTransportOverlayState {
    std::string primaryLine{"Replay idle"};
    std::string secondaryLine{};
    float titleY{28.0f};
    float subtitleY{74.0f};
    float buttonY{-1.0f};
    ReplayCheckpointOverlayState checkpoint{};
};

inline TypographyStyleId replayTransportStyleForFontSize(int fontSize) {
    if (fontSize >= 34) {
        return TypographyStyleId::ScoreboardSummary;
    }
    if (fontSize >= 30) {
        return TypographyStyleId::ScoreboardTitle;
    }
    if (fontSize >= 28) {
        return TypographyStyleId::OverlayTitle;
    }
    if (fontSize >= 24) {
        return TypographyStyleId::AppSubtitle;
    }
    if (fontSize >= 22) {
        return TypographyStyleId::OverlayAccent;
    }
    if (fontSize >= 20) {
        return TypographyStyleId::Body;
    }
    return TypographyStyleId::Caption;
}

inline void drawReplayTransportText(std::string_view text,
                                    float x,
                                    float y,
                                    int fontSize,
                                    Color color) {
    TypographyService::shared().draw(replayTransportStyleForFontSize(fontSize),
                                     text,
                                     Vector2{x, y},
                                     color);
}

inline int measureReplayTransportTextWidth(std::string_view text, int fontSize) {
    return TypographyService::shared().measureWidth(replayTransportStyleForFontSize(fontSize),
                                                   text);
}

inline std::string formatReplayTransportSeconds(float seconds) {
    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%.1fs",
                  std::max(0.0f, seconds));
    return std::string(buffer);
}

inline std::string replayCheckpointActiveValue(const ReplayCheckpointOverlayState& checkpoint) {
    if (!checkpoint.detachedCameraActive) {
        return "OFF";
    }
    if (checkpoint.checkpointCount == 0u) {
        return "--";
    }
    if (checkpoint.activeIndex >= 0) {
        return std::to_string(checkpoint.activeIndex + 1) + " / " +
               std::to_string(checkpoint.checkpointCount);
    }
    return "-- / " + std::to_string(checkpoint.checkpointCount);
}

inline std::string replayCheckpointTransitionValue(const ReplayCheckpointOverlayState& checkpoint) {
    if (!checkpoint.detachedCameraActive ||
        checkpoint.checkpointCount == 0u ||
        !checkpoint.transitionEditable) {
        return "--";
    }
    return formatReplayTransportSeconds(checkpoint.transitionToNextSeconds);
}

inline std::string replayCheckpointFooterLine(const ReplayCheckpointOverlayState& checkpoint) {
    if (!checkpoint.detachedCameraActive) {
        return "Free camera required";
    }
    if (checkpoint.checkpointCount == 0u) {
        return "Save a checkpoint";
    }
    if (checkpoint.transitionEditable) {
        return ", -0.25s    . +0.25s";
    }
    if (checkpoint.checkpointCount == 1u) {
        return "Need 2+ checkpoints";
    }
    return "Select a checkpoint";
}

inline void drawReplayTransportShadowedText(std::string_view text,
                                            float x,
                                            float y,
                                            int fontSize,
                                            Color color,
                                            Color shadow = Fade(BLACK, 0.55f)) {
    drawReplayTransportText(text, x + 2.0f, y + 2.0f, fontSize, shadow);
    drawReplayTransportText(text, x, y, fontSize, color);
}

inline void drawCenteredReplayTransportShadowedText(std::string_view text,
                                                   float centerX,
                                                   float y,
                                                   int fontSize,
                                                   Color color,
                                                   Color shadow = Fade(BLACK, 0.55f)) {
    const int textWidth = measureReplayTransportTextWidth(text, fontSize);
    const float x = centerX - (static_cast<float>(textWidth) * 0.5f);
    drawReplayTransportText(text, x + 2.0f, y + 2.0f, fontSize, shadow);
    drawReplayTransportText(text, x, y, fontSize, color);
}

inline void drawReplayCheckpointOverlay(const ReplayCheckpointOverlayState& checkpoint) {
    if (!checkpoint.visible) {
        return;
    }

    const Rectangle shadowRect{24.0f, 28.0f, 360.0f, 160.0f};
    const Rectangle faceRect{24.0f, 24.0f, 360.0f, 160.0f};
    const Color faceColor = Fade(Color{14, 18, 24, 255}, 0.86f);
    const Color borderColor = checkpoint.transitionEditable
        ? Fade(Color{182, 235, 255, 255}, 0.42f)
        : Fade(WHITE, 0.18f);

    DrawRectangleRounded(shadowRect, 0.18f, 10, Fade(BLACK, 0.22f));
    DrawRectangleRounded(faceRect, 0.18f, 10, faceColor);
    DrawRectangleRoundedLines(faceRect, 0.18f, 10, borderColor);

    drawReplayTransportShadowedText("CHECKPOINTS",
                                    faceRect.x + 18.0f,
                                    faceRect.y + 14.0f,
                                    18,
                                    Fade(WHITE, 0.9f),
                                    Fade(BLACK, 0.58f));

    const auto drawMetricRow = [&](float y,
                                   std::string_view label,
                                   const std::string& value,
                                   Color valueColor) {
        drawReplayTransportShadowedText(label,
                                        faceRect.x + 18.0f,
                                        y,
                                        16,
                                        Fade(WHITE, 0.55f),
                                        Fade(BLACK, 0.5f));
        const int valueWidth = measureReplayTransportTextWidth(value, 22);
        drawReplayTransportShadowedText(value,
                                        faceRect.x + faceRect.width - 18.0f -
                                            static_cast<float>(valueWidth),
                                        y - 4.0f,
                                        22,
                                        valueColor,
                                        Fade(BLACK, 0.58f));
    };

    drawMetricRow(faceRect.y + 56.0f,
                  "ACTIVE",
                  replayCheckpointActiveValue(checkpoint),
                  Fade(WHITE, checkpoint.detachedCameraActive ? 0.96f : 0.58f));
    drawMetricRow(faceRect.y + 94.0f,
                  "TO NEXT",
                  replayCheckpointTransitionValue(checkpoint),
                  checkpoint.transitionEditable
                      ? Color{182, 235, 255, 255}
                      : Fade(WHITE, 0.58f));

    drawCenteredReplayTransportShadowedText(replayCheckpointFooterLine(checkpoint),
                                            faceRect.x + (faceRect.width * 0.5f),
                                            faceRect.y + faceRect.height - 34.0f,
                                            16,
                                            checkpoint.transitionEditable
                                                ? Fade(WHITE, 0.88f)
                                                : Fade(WHITE, 0.6f),
                                            Fade(BLACK, 0.5f));
}

inline void drawReplayTransportTrianglePair(float centerX,
                                            float centerY,
                                            float triangleWidth,
                                            float triangleHeight,
                                            float gap,
                                            bool forward,
                                            Color color) {
    const float totalWidth = (triangleWidth * 2.0f) + gap;
    const float startX = centerX - (totalWidth * 0.5f);

    const auto drawTriangleShape = [&](float x) {
        if (forward) {
            DrawTriangle(Vector2{x, centerY - (triangleHeight * 0.5f)},
                         Vector2{x, centerY + (triangleHeight * 0.5f)},
                         Vector2{x + triangleWidth, centerY},
                         color);
        } else {
            DrawTriangle(Vector2{x + triangleWidth, centerY - (triangleHeight * 0.5f)},
                         Vector2{x, centerY},
                         Vector2{x + triangleWidth, centerY + (triangleHeight * 0.5f)},
                         color);
        }
    };

    drawTriangleShape(startX);
    drawTriangleShape(startX + triangleWidth + gap);
}

inline void drawReplayCassetteGlyph(ReplayCassetteGlyph glyph,
                                    const Rectangle& bounds,
                                    Color color) {
    const float centerX = bounds.x + (bounds.width * 0.5f);
    const float centerY = bounds.y + (bounds.height * 0.5f);
    const float glyphSize = std::min(bounds.width, bounds.height) * 0.42f;

    switch (glyph) {
        case ReplayCassetteGlyph::Record:
            DrawCircleV(Vector2{centerX, centerY}, glyphSize * 0.48f, color);
            break;
        case ReplayCassetteGlyph::Stop:
            DrawRectangleRounded(Rectangle{centerX - glyphSize * 0.55f,
                                           centerY - glyphSize * 0.55f,
                                           glyphSize * 1.1f,
                                           glyphSize * 1.1f},
                                 0.18f,
                                 6,
                                 color);
            break;
        case ReplayCassetteGlyph::Rewind:
            drawReplayTransportTrianglePair(centerX,
                                            centerY,
                                            glyphSize * 0.82f,
                                            glyphSize * 1.32f,
                                            glyphSize * 0.16f,
                                            false,
                                            color);
            break;
        case ReplayCassetteGlyph::Play:
            DrawTriangle(Vector2{centerX - glyphSize * 0.5f, centerY - glyphSize * 0.86f},
                         Vector2{centerX - glyphSize * 0.5f, centerY + glyphSize * 0.86f},
                         Vector2{centerX + glyphSize * 0.86f, centerY},
                         color);
            break;
        case ReplayCassetteGlyph::Pause:
            DrawRectangleRounded(Rectangle{centerX - glyphSize * 0.66f,
                                           centerY - glyphSize * 0.74f,
                                           glyphSize * 0.34f,
                                           glyphSize * 1.48f},
                                 0.18f,
                                 4,
                                 color);
            DrawRectangleRounded(Rectangle{centerX + glyphSize * 0.32f,
                                           centerY - glyphSize * 0.74f,
                                           glyphSize * 0.34f,
                                           glyphSize * 1.48f},
                                 0.18f,
                                 4,
                                 color);
            break;
        case ReplayCassetteGlyph::FastForward:
            drawReplayTransportTrianglePair(centerX,
                                            centerY,
                                            glyphSize * 0.82f,
                                            glyphSize * 1.32f,
                                            glyphSize * 0.16f,
                                            true,
                                            color);
            break;
        case ReplayCassetteGlyph::Export: {
            const float stemWidth = glyphSize * 0.28f;
            DrawRectangleRounded(Rectangle{centerX - stemWidth * 0.5f,
                                           centerY - glyphSize * 0.78f,
                                           stemWidth,
                                           glyphSize * 0.95f},
                                 0.18f,
                                 4,
                                 color);
            DrawTriangle(Vector2{centerX - glyphSize * 0.66f, centerY - glyphSize * 0.08f},
                         Vector2{centerX + glyphSize * 0.66f, centerY - glyphSize * 0.08f},
                         Vector2{centerX, centerY + glyphSize * 0.58f},
                         color);
            DrawRectangleRounded(Rectangle{centerX - glyphSize * 0.82f,
                                           centerY + glyphSize * 0.68f,
                                           glyphSize * 1.64f,
                                           glyphSize * 0.28f},
                                 0.18f,
                                 4,
                                 color);
            break;
        }
        case ReplayCassetteGlyph::Exit: {
            constexpr int kExitFontSize = 13;
            constexpr std::string_view kExitLine = "Exit";
            constexpr std::string_view kReplayLine = "Replay";
            const int exitWidth = measureReplayTransportTextWidth(kExitLine, kExitFontSize);
            const int replayWidth = measureReplayTransportTextWidth(kReplayLine, kExitFontSize);
            drawReplayTransportText(kExitLine,
                                    centerX - static_cast<float>(exitWidth) * 0.5f,
                                    centerY - 15.0f,
                                    kExitFontSize,
                                    color);
            drawReplayTransportText(kReplayLine,
                                    centerX - static_cast<float>(replayWidth) * 0.5f,
                                    centerY + 1.0f,
                                    kExitFontSize,
                                    color);
            break;
        }
    }
}

inline void drawReplayCassetteButton(const Rectangle& bounds,
                                     ReplayCassetteGlyph glyph,
                                     std::string_view hotkey,
                                     bool enabled,
                                     bool pressed,
                                     Color accent) {
    const float pressOffset = pressed ? 2.0f : 0.0f;
    const Rectangle shadowRect{bounds.x, bounds.y + 4.0f, bounds.width, bounds.height};
    const Rectangle faceRect{bounds.x, bounds.y + pressOffset, bounds.width, bounds.height};

    const Color shadowColor = enabled ? Fade(BLACK, 0.18f) : Fade(BLACK, 0.08f);
    const Color faceColor = enabled
        ? (pressed ? Fade(WHITE, 0.88f) : Fade(WHITE, 0.97f))
        : Fade(WHITE, 0.24f);
    const Color borderColor = enabled
        ? (pressed ? Fade(accent, 0.55f) : Fade(WHITE, 0.58f))
        : Fade(WHITE, 0.14f);
    const Color iconColor = enabled
        ? (pressed ? accent : Color{24, 28, 34, 255})
        : Fade(Color{24, 28, 34, 255}, 0.34f);
    const Color hotkeyColor = enabled ? Fade(WHITE, 0.98f) : Fade(WHITE, 0.42f);

    DrawRectangleRounded(shadowRect, 0.30f, 12, shadowColor);
    DrawRectangleRounded(faceRect, 0.30f, 12, faceColor);
    DrawRectangleRoundedLines(faceRect, 0.30f, 12, borderColor);
    drawReplayCassetteGlyph(glyph, faceRect, iconColor);

    const int hotkeyFontSize = hotkey.size() > 3u ? 16 : 24;
    drawCenteredReplayTransportShadowedText(hotkey,
                                            faceRect.x + (faceRect.width * 0.5f),
                                            faceRect.y + faceRect.height + 12.0f,
                                            hotkeyFontSize,
                                            hotkeyColor,
                                            Fade(BLACK, 0.62f));
}

template <typename ButtonContainer>
inline void drawCenteredReplayTransportButtons(const ButtonContainer& buttons,
                                               float centerX,
                                               float y,
                                               float buttonWidth = 78.0f,
                                               float buttonHeight = 46.0f,
                                               float buttonGap = 22.0f) {
    if (buttons.empty()) {
        return;
    }

    const float totalButtonsWidth =
        (buttonWidth * static_cast<float>(buttons.size())) +
        (buttonGap * static_cast<float>(buttons.size() - 1u));
    float buttonX = centerX - (totalButtonsWidth * 0.5f);
    for (const ReplayTransportButtonSpec& button : buttons) {
        drawReplayCassetteButton(Rectangle{buttonX, y, buttonWidth, buttonHeight},
                                 button.glyph,
                                 button.hotkey,
                                 button.enabled,
                                 button.pressed,
                                 button.accent);
        buttonX += buttonWidth + buttonGap;
    }
}

inline ReplayTransportButtonSpec makeReplayPlaybackToggleButton(bool playbackPlaying,
                                                                bool enabled) {
    const ReplayCassetteGlyph transportToggleGlyph =
        playbackPlaying ? ReplayCassetteGlyph::Pause : ReplayCassetteGlyph::Play;
    const Color transportToggleAccent =
        playbackPlaying ? Color{255, 214, 107, 255} : Color{116, 234, 159, 255};

    return ReplayTransportButtonSpec{
        transportToggleGlyph,
        "6",
        enabled,
        playbackPlaying,
        transportToggleAccent
    };
}

inline std::vector<ReplayTransportButtonSpec> makeReplayRecordingTransportButtons(
    bool recordingActive,
    bool playbackActive,
    bool playbackPlaying,
    bool hasCapture,
    bool includeExport) {
    const bool stopped = !recordingActive && !playbackActive;
    std::vector<ReplayTransportButtonSpec> buttons;
    buttons.reserve(includeExport ? 7u : 6u);
    buttons.push_back(
        ReplayTransportButtonSpec{ReplayCassetteGlyph::Record,
                                  "5",
                                  true,
                                  recordingActive,
                                  Color{255, 88, 102, 255}});
    buttons.push_back(makeReplayPlaybackToggleButton(playbackPlaying, hasCapture));
    buttons.push_back(
        ReplayTransportButtonSpec{ReplayCassetteGlyph::Rewind,
                                  "7",
                                  hasCapture,
                                  false,
                                  Color{156, 211, 255, 255}});
    buttons.push_back(
        ReplayTransportButtonSpec{ReplayCassetteGlyph::FastForward,
                                  "8",
                                  hasCapture,
                                  false,
                                  Color{156, 211, 255, 255}});
    if (includeExport) {
        buttons.push_back(
            ReplayTransportButtonSpec{ReplayCassetteGlyph::Export,
                                      "9",
                                      hasCapture,
                                      false,
                                      Color{182, 235, 255, 255}});
    }
    buttons.push_back(
        ReplayTransportButtonSpec{ReplayCassetteGlyph::Stop,
                                  "0",
                                  hasCapture,
                                  stopped && hasCapture,
                                  Color{255, 214, 107, 255}});
    buttons.push_back(
        ReplayTransportButtonSpec{ReplayCassetteGlyph::Exit,
                                  "Backspace",
                                  playbackActive,
                                  false,
                                  Color{255, 88, 102, 255}});
    return buttons;
}

inline std::array<ReplayTransportButtonSpec, 4> makeReplayPlaybackTransportButtons(
    bool playbackPlaying,
    bool stopped) {
    return std::array<ReplayTransportButtonSpec, 4>{{
        makeReplayPlaybackToggleButton(playbackPlaying, true),
        {ReplayCassetteGlyph::Rewind, "7", true, false, Color{156, 211, 255, 255}},
        {ReplayCassetteGlyph::FastForward, "8", true, false, Color{156, 211, 255, 255}},
        {ReplayCassetteGlyph::Stop, "0", true, stopped, Color{255, 214, 107, 255}}
    }};
}

template <typename ButtonContainer>
inline void renderReplayTransportOverlay(const ReplayTransportOverlayState& state,
                                         const ButtonContainer& buttons) {
    const float centerX = static_cast<float>(Config::SCREEN_WIDTH) * 0.5f;
    if (!state.primaryLine.empty()) {
        drawCenteredReplayTransportShadowedText(state.primaryLine,
                                                centerX,
                                                state.titleY,
                                                28,
                                                WHITE,
                                                Fade(BLACK, 0.6f));
    }
    if (!state.secondaryLine.empty()) {
        drawCenteredReplayTransportShadowedText(state.secondaryLine,
                                                centerX,
                                                state.subtitleY,
                                                18,
                                                Fade(WHITE, 0.9f),
                                                Fade(BLACK, 0.52f));
    }

    const float buttonY = state.buttonY >= 0.0f
        ? state.buttonY
        : (state.secondaryLine.empty() ? 86.0f : 114.0f);
    drawCenteredReplayTransportButtons(buttons, centerX, buttonY);
    drawReplayCheckpointOverlay(state.checkpoint);
}

}  // namespace client
