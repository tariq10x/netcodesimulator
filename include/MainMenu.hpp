#pragma once

#include <raylib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <string>

#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "TypographyService.hpp"
#include "net/SessionLaunchConfig.hpp"

enum class GameMode {
    MAIN_MENU,
    LEVEL_SELECT,
    LEVEL_EDITOR,
    GAMEPLAY,
    FREE_GAME,
    MULTIPLAYER_SESSION,
    REPLAY_STUDIO,
    SETTINGS,
    QUIT
};

class MainMenu {
public:
    enum class AppShellSurface : std::uint8_t {
        None = 0,
        Multiplayer = 1,
        LabStudy = 2,
        LevelEditor = 3,
        ReplayStudio = 4,
        Settings = 5
    };

    enum class ExternalLinkTarget : std::uint8_t {
        None = 0,
        YouTube = 1,
        Patreon = 2,
        GitHub = 3
    };

    struct NavigationSelection {
        AppShellSurface surface{AppShellSurface::None};
        net::SessionEntryPoint entryPoint{net::SessionEntryPoint::None};
    };

private:
    static constexpr int kRootOptionCount = 5;
    static constexpr int kMultiplayerOptionCount = 2;
    static constexpr int kMaxOptionCount = kRootOptionCount;
    static constexpr const char* kGitHubMarkAssetPath = "assets/icons/GitHub_Invertocat_White.png";
    static constexpr float kButtonWidth = 420.0f;
    static constexpr float kButtonHeight = 72.0f;
    static constexpr float kButtonGap = 16.0f;
    static constexpr float kButtonSpacing = kButtonHeight + kButtonGap;
    static constexpr float kRootTitleY = 150.0f;
    static constexpr float kRootTitleToSubtitleGap = 10.0f;
    static constexpr float kRootSubtitleToActionsGap = 38.0f;
    static constexpr float kRootActionToSocialGap = 36.0f;
    static constexpr float kSocialButtonSize = 76.0f;
    static constexpr float kSocialButtonGap = 24.0f;
    static constexpr float kBackdropCycleSeconds = 12.0f;

    enum class View : std::uint8_t {
        Root = 0,
        Multiplayer = 1
    };

    struct Button {
        Rectangle rect;
        const char* text;
        bool hovered;
    };

    struct ExternalButton {
        Rectangle rect;
        ExternalLinkTarget target{ExternalLinkTarget::None};
        bool hovered{false};
    };

    int selectedOption;
    View currentView_;
    net::SessionLaunchMode requestedSessionMode_;
    NavigationSelection requestedNavigation_{};
    ExternalLinkTarget requestedExternalLink_{ExternalLinkTarget::None};
    std::string statusMessage_{};
    bool suppressQuitShortcutUntilRelease_{false};
    std::array<const char*, kRootOptionCount> rootMenuOptions_{
        "Multiplayer",
        "Lab Study",
        "Level Editor",
        "Replay Studio",
        "Settings"
    };
    std::array<const char*, kMultiplayerOptionCount> multiplayerMenuOptions_{
        "Host",
        "Join"
    };
    std::array<Button, kMaxOptionCount> buttons_;
    Rectangle multiplayerBackButton_{40.0f, 56.0f, 180.0f, 48.0f};
    std::array<ExternalButton, 3> externalButtons_{{
        ExternalButton{Rectangle{}, ExternalLinkTarget::YouTube, false},
        ExternalButton{Rectangle{}, ExternalLinkTarget::Patreon, false},
        ExternalButton{Rectangle{}, ExternalLinkTarget::GitHub, false}
    }};
    Texture2D githubMarkTexture_{};
    bool githubMarkTextureLoaded_{false};
    bool githubMarkTextureLoadAttempted_{false};

    static Vector2 rectCenter(const Rectangle& rect) {
        return Vector2{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
    }

    static Rectangle insetRectangle(const Rectangle& rect, float insetX, float insetY) {
        return Rectangle{
            rect.x + insetX,
            rect.y + insetY,
            std::max(0.0f, rect.width - insetX * 2.0f),
            std::max(0.0f, rect.height - insetY * 2.0f)
        };
    }

    static float rootSubtitleY() {
        return kRootTitleY +
            TypographyTheme::style(TypographyStyleId::MenuTitle).lineHeight +
            kRootTitleToSubtitleGap;
    }

    static float rootFirstButtonY() {
        return rootSubtitleY() +
            TypographyTheme::style(TypographyStyleId::AppSubtitle).lineHeight +
            kRootSubtitleToActionsGap;
    }

    static float rootSocialY() {
        return rootFirstButtonY() +
            static_cast<float>(kRootOptionCount - 1) * kButtonSpacing +
            kButtonHeight +
            kRootActionToSocialGap;
    }

    static float clamp01(float value) {
        return std::clamp(value, 0.0f, 1.0f);
    }

    static float smoothStep01(float value) {
        const float t = clamp01(value);
        return t * t * (3.0f - 2.0f * t);
    }

    static float wrapSeconds(float value, float cycleSeconds) {
        if (cycleSeconds <= 0.0f) {
            return 0.0f;
        }

        const float wrapped = std::fmod(value, cycleSeconds);
        return wrapped >= 0.0f ? wrapped : wrapped + cycleSeconds;
    }

    static float pulseAt(float timeSeconds, float centerSeconds, float halfWidthSeconds) {
        if (halfWidthSeconds <= 0.0f) {
            return 0.0f;
        }

        return std::max(0.0f,
                        1.0f - std::fabs(timeSeconds - centerSeconds) / halfWidthSeconds);
    }

    static Vector3 lerpVector(Vector3 from, Vector3 to, float t) {
        const float clamped = clamp01(t);
        return Vector3{
            from.x + (to.x - from.x) * clamped,
            from.y + (to.y - from.y) * clamped,
            from.z + (to.z - from.z) * clamped
        };
    }

    static Vector3 forwardFromYaw(float yaw) {
        return Vector3{std::sin(yaw), 0.0f, -std::cos(yaw)};
    }

    static Vector3 rightFromYaw(float yaw) {
        return Vector3{std::cos(yaw), 0.0f, std::sin(yaw)};
    }

    static float yawToward(Vector3 from, Vector3 to) {
        const Vector3 delta{to.x - from.x, to.y - from.y, to.z - from.z};
        return std::atan2(delta.x, -delta.z);
    }

    static Vector3 scriptedPathPoint(float cycleSeconds,
                                     const std::array<Vector3, 5>& points,
                                     const std::array<float, 4>& segmentEnds) {
        float segmentStart = 0.0f;
        for (std::size_t index = 0; index < segmentEnds.size(); ++index) {
            const float segmentEnd = segmentEnds[index];
            if (cycleSeconds <= segmentEnd) {
                const float duration = std::max(0.001f, segmentEnd - segmentStart);
                const float localProgress = smoothStep01((cycleSeconds - segmentStart) / duration);
                return lerpVector(points[index], points[index + 1], localProgress);
            }
            segmentStart = segmentEnd;
        }

        return points.back();
    }

    static void drawBackdropArena(float timeSeconds) {
        const float glowPulse = 0.5f + 0.5f * std::sin(timeSeconds * 0.9f);
        const Color floorColor{17, 21, 29, 255};
        const Color structureColor{34, 43, 58, 255};
        const Color flankColor{28, 35, 47, 255};

        DrawCube(Vector3{0.0f, -0.1f, 0.0f}, 88.0f, 0.2f, 88.0f, floorColor);
        DrawGrid(44, 2.0f);

        DrawCube(Vector3{0.0f, 1.35f, 0.0f}, 8.0f, 2.7f, 8.0f, structureColor);
        DrawCubeWires(Vector3{0.0f, 1.35f, 0.0f},
                      8.3f,
                      2.9f,
                      8.3f,
                      Fade(SKYBLUE, 0.18f + glowPulse * 0.12f));

        DrawCube(Vector3{-11.0f, 1.2f, 9.5f}, 5.0f, 2.4f, 10.0f, flankColor);
        DrawCube(Vector3{11.0f, 1.2f, -9.5f}, 5.0f, 2.4f, 10.0f, flankColor);
        DrawCube(Vector3{-18.0f, 1.0f, -2.0f}, 6.0f, 2.0f, 4.0f, structureColor);
        DrawCube(Vector3{18.0f, 1.0f, 2.0f}, 6.0f, 2.0f, 4.0f, structureColor);

        DrawCubeWires(Vector3{-11.0f, 1.2f, 9.5f},
                      5.2f,
                      2.5f,
                      10.2f,
                      Fade(Color{255, 92, 92, 255}, 0.16f + glowPulse * 0.1f));
        DrawCubeWires(Vector3{11.0f, 1.2f, -9.5f},
                      5.2f,
                      2.5f,
                      10.2f,
                      Fade(Color{92, 156, 255, 255}, 0.16f + glowPulse * 0.1f));
    }

    static Vector3 backdropWeaponStart(Vector3 rootPosition, float yaw) {
        const Vector3 forward = forwardFromYaw(yaw);
        const Vector3 right = rightFromYaw(yaw);
        return Vector3{
            rootPosition.x + right.x * 0.14f + forward.x * (Config::ENEMY_BODY_RADIUS + 0.18f),
            rootPosition.y + Config::ENEMY_BODY_HEIGHT * 0.76f,
            rootPosition.z + right.z * 0.14f + forward.z * (Config::ENEMY_BODY_RADIUS + 0.18f)
        };
    }

    static Vector3 backdropWeaponEnd(Vector3 rootPosition, float yaw) {
        const Vector3 forward = forwardFromYaw(yaw);
        const Vector3 start = backdropWeaponStart(rootPosition, yaw);
        return Vector3{
            start.x + forward.x * 0.92f,
            start.y + 0.03f,
            start.z + forward.z * 0.92f
        };
    }

    static void drawBackdropActor(Vector3 actorPosition,
                                  float yaw,
                                  Color tint,
                                  float stridePhase,
                                  float muzzleFlash) {
        const float bob = std::sin(stridePhase) * 0.06f;
        const Vector3 rootPosition{actorPosition.x, actorPosition.y + bob, actorPosition.z};
        const Vector3 headCenter{
            rootPosition.x,
            rootPosition.y + Config::ENEMY_HEAD_OFFSET,
            rootPosition.z
        };
        const Vector3 weaponEnd = backdropWeaponEnd(rootPosition, yaw);

        DrawCylinder(rootPosition,
                     Config::ENEMY_BODY_RADIUS,
                     Config::ENEMY_BODY_RADIUS,
                     Config::ENEMY_BODY_HEIGHT,
                     16,
                     tint);
        DrawSphere(headCenter, Config::ENEMY_HEAD_RADIUS, tint);

        if (muzzleFlash > 0.0f) {
            DrawSphere(weaponEnd,
                       0.1f + muzzleFlash * 0.15f,
                       Fade(Color{255, 208, 96, 255}, 0.45f + muzzleFlash * 0.45f));
            DrawSphere(weaponEnd,
                       0.05f + muzzleFlash * 0.08f,
                       Fade(Color{255, 244, 180, 255}, 0.5f + muzzleFlash * 0.5f));
        }
    }

    static void drawBackdropTracer(Vector3 start, Vector3 end, float intensity, Color tint) {
        if (intensity <= 0.0f) {
            return;
        }

        const float beamRadius = 0.05f + intensity * 0.06f;
        DrawCylinderEx(start,
                       end,
                       beamRadius,
                       beamRadius,
                       10,
                       Fade(tint, 0.2f + intensity * 0.32f));
        DrawCylinderEx(start,
                       end,
                       beamRadius * 0.42f,
                       beamRadius * 0.42f,
                       8,
                       Fade(Color{255, 240, 186, 255}, 0.42f + intensity * 0.44f));
        DrawSphere(end,
                   0.1f + intensity * 0.09f,
                   Fade(tint, 0.2f + intensity * 0.38f));
    }

    static void renderGameplayBackdrop() {
        ClearBackground(Color{8, 10, 15, 255});

        const float timeSeconds = static_cast<float>(GetTime());
        const float cycleSeconds = wrapSeconds(timeSeconds, kBackdropCycleSeconds);
        const std::array<Vector3, 5> attackerPath{{
            Vector3{-14.0f, 0.0f, 12.0f},
            Vector3{-8.0f, 0.0f, 6.0f},
            Vector3{-2.0f, 0.0f, 1.0f},
            Vector3{6.0f, 0.0f, -6.0f},
            Vector3{-14.0f, 0.0f, 12.0f}
        }};
        const std::array<Vector3, 5> defenderPath{{
            Vector3{14.0f, 0.0f, -12.0f},
            Vector3{9.0f, 0.0f, -6.0f},
            Vector3{3.0f, 0.0f, -1.0f},
            Vector3{-7.0f, 0.0f, 7.0f},
            Vector3{14.0f, 0.0f, -12.0f}
        }};
        const std::array<float, 4> pathSegmentEnds{{3.0f, 5.4f, 8.8f, kBackdropCycleSeconds}};

        const Vector3 attackerPosition =
            scriptedPathPoint(cycleSeconds, attackerPath, pathSegmentEnds);
        const Vector3 defenderPosition =
            scriptedPathPoint(cycleSeconds, defenderPath, pathSegmentEnds);

        const float attackerYaw = yawToward(attackerPosition,
                                            Vector3{defenderPosition.x, 1.5f, defenderPosition.z});
        const float defenderYaw = yawToward(defenderPosition,
                                            Vector3{attackerPosition.x, 1.5f, attackerPosition.z});

        const float attackerFlash = std::max(
            std::max(pulseAt(cycleSeconds, 2.9f, 0.16f), pulseAt(cycleSeconds, 6.2f, 0.18f)),
            pulseAt(cycleSeconds, 9.1f, 0.16f));
        const float defenderFlash = std::max(
            std::max(pulseAt(cycleSeconds, 3.8f, 0.16f), pulseAt(cycleSeconds, 6.9f, 0.18f)),
            pulseAt(cycleSeconds, 10.0f, 0.16f));

        const Vector3 focusPoint = lerpVector(attackerPosition, defenderPosition, 0.5f);
        const float orbit = timeSeconds * 0.18f;

        Camera3D camera{};
        camera.position = Vector3{
            focusPoint.x + 18.0f + std::sin(orbit) * 8.0f,
            10.0f + std::sin(timeSeconds * 0.33f) * 1.3f,
            focusPoint.z + 20.0f + std::cos(orbit) * 5.5f
        };
        camera.target = Vector3{
            focusPoint.x,
            2.0f + std::sin(timeSeconds * 0.21f) * 0.22f,
            focusPoint.z
        };
        camera.up = Vector3{0.0f, 1.0f, 0.0f};
        camera.fovy = 46.0f;
        camera.projection = CAMERA_PERSPECTIVE;

        BeginMode3D(camera);
        drawBackdropArena(timeSeconds);
        drawBackdropActor(attackerPosition,
                          attackerYaw,
                          Color{176, 64, 64, 255},
                          cycleSeconds * 3.2f,
                          attackerFlash);
        drawBackdropActor(defenderPosition,
                          defenderYaw,
                          Color{70, 120, 210, 255},
                          cycleSeconds * 3.0f + 1.2f,
                          defenderFlash);
        drawBackdropTracer(backdropWeaponEnd(Vector3{attackerPosition.x,
                                                     attackerPosition.y + std::sin(cycleSeconds * 3.2f) * 0.06f,
                                                     attackerPosition.z},
                                            attackerYaw),
                           Vector3{defenderPosition.x, 1.55f, defenderPosition.z},
                           attackerFlash,
                           Color{255, 96, 96, 255});
        drawBackdropTracer(backdropWeaponEnd(Vector3{defenderPosition.x,
                                                     defenderPosition.y + std::sin(cycleSeconds * 3.0f + 1.2f) * 0.06f,
                                                     defenderPosition.z},
                                            defenderYaw),
                           Vector3{attackerPosition.x, 1.55f, attackerPosition.z},
                           defenderFlash,
                           Color{88, 164, 255, 255});
        EndMode3D();

        DrawRectangle(0,
                      0,
                      Config::SCREEN_WIDTH,
                      Config::SCREEN_HEIGHT,
                      Fade(Color{6, 9, 14, 255}, 0.22f));
        DrawRectangleGradientH(0,
                               0,
                               Config::SCREEN_WIDTH / 2,
                               Config::SCREEN_HEIGHT,
                               Fade(BLACK, 0.46f),
                               Fade(BLACK, 0.16f));
        DrawRectangleGradientH(Config::SCREEN_WIDTH / 2,
                               0,
                               Config::SCREEN_WIDTH / 2,
                               Config::SCREEN_HEIGHT,
                               Fade(BLACK, 0.16f),
                               Fade(BLACK, 0.46f));
        DrawRectangleGradientV(0,
                               0,
                               Config::SCREEN_WIDTH,
                               Config::SCREEN_HEIGHT,
                               Fade(Color{8, 12, 18, 255}, 0.1f),
                               Fade(Color{8, 12, 18, 255}, 0.38f));
    }

    static void drawYouTubeGlyph(const Rectangle& tileRect, bool hovered) {
        const Color accent = hovered ? Color{255, 96, 96, 255} : Color{239, 68, 68, 255};
        const Color glow = hovered ? Fade(accent, 0.18f) : Fade(accent, 0.11f);
        const Vector2 center = rectCenter(tileRect);
        DrawCircleV(center, tileRect.width * 0.29f, glow);

        const Rectangle badge = insetRectangle(tileRect, tileRect.width * 0.14f, tileRect.height * 0.24f);
        DrawRectangleRounded(badge, 0.42f, 10, accent);
        DrawRectangleRoundedLines(badge, 0.42f, 10, Fade(WHITE, hovered ? 0.24f : 0.16f));

        const float triangleHalfWidth = badge.width * 0.15f;
        const float triangleHalfHeight = badge.height * 0.19f;
        const Vector2 playCenter{badge.x + badge.width * 0.54f, badge.y + badge.height * 0.5f};
        DrawTriangle(Vector2{playCenter.x - triangleHalfWidth * 0.7f, playCenter.y - triangleHalfHeight},
                     Vector2{playCenter.x - triangleHalfWidth * 0.7f, playCenter.y + triangleHalfHeight},
                     Vector2{playCenter.x + triangleHalfWidth, playCenter.y},
                     WHITE);
    }

    static void drawPatreonGlyph(const Rectangle& tileRect, bool hovered, Color maskColor) {
        const Color accent = hovered ? Color{255, 136, 112, 255} : Color{255, 102, 82, 255};
        const Vector2 center = rectCenter(tileRect);
        DrawCircleV(Vector2{center.x - tileRect.width * 0.06f, center.y},
                    tileRect.width * 0.26f,
                    hovered ? Fade(accent, 0.16f) : Fade(accent, 0.1f));

        const Rectangle iconBounds = insetRectangle(tileRect, tileRect.width * 0.22f, tileRect.height * 0.18f);
        const float stemWidth = std::max(8.0f, iconBounds.width * 0.18f);
        const Rectangle stem{
            iconBounds.x + iconBounds.width * 0.08f,
            iconBounds.y + iconBounds.height * 0.08f,
            stemWidth,
            iconBounds.height * 0.84f
        };
        const float outerRadius = iconBounds.height * 0.24f;
        const Vector2 circleCenter{
            stem.x + stem.width + outerRadius * 1.15f,
            iconBounds.y + outerRadius + iconBounds.height * 0.1f
        };
        const float innerRadius = std::max(outerRadius - stemWidth * 0.72f, 2.0f);

        DrawRectangleRounded(stem, 0.45f, 8, accent);
        DrawCircleV(circleCenter, outerRadius, accent);
        DrawCircleV(circleCenter, innerRadius, maskColor);
    }

    void ensureGitHubMarkTextureLoaded() {
        if (githubMarkTextureLoaded_ || githubMarkTextureLoadAttempted_ || !IsWindowReady()) {
            return;
        }

        githubMarkTextureLoadAttempted_ = true;
        Image githubMark = LoadImage(kGitHubMarkAssetPath);
        if (githubMark.data == nullptr) {
            return;
        }

        githubMarkTexture_ = LoadTextureFromImage(githubMark);
        UnloadImage(githubMark);
        githubMarkTextureLoaded_ = githubMarkTexture_.id != 0;
    }

    void drawGitHubGlyph(const Rectangle& tileRect) {
        ensureGitHubMarkTextureLoaded();
        if (!githubMarkTextureLoaded_) {
            return;
        }

        const Vector2 center = rectCenter(tileRect);
        const float logoWidth = tileRect.width * 0.54f;
        const float aspectRatio =
            static_cast<float>(githubMarkTexture_.height) /
            static_cast<float>(githubMarkTexture_.width);
        const float logoHeight = logoWidth * aspectRatio;
        const Rectangle destination{
            center.x - logoWidth * 0.5f,
            center.y - logoHeight * 0.5f,
            logoWidth,
            logoHeight
        };

        DrawTexturePro(githubMarkTexture_,
                       Rectangle{0.0f,
                                 0.0f,
                                 static_cast<float>(githubMarkTexture_.width),
                                 static_cast<float>(githubMarkTexture_.height)},
                       destination,
                       Vector2{},
                       0.0f,
                       WHITE);
    }

    void drawExternalButtonGlyph(const ExternalButton& button,
                                 const Rectangle& tileRect,
                                 Color tileColor) {
        switch (button.target) {
            case ExternalLinkTarget::YouTube:
                drawYouTubeGlyph(tileRect, button.hovered);
                break;
            case ExternalLinkTarget::Patreon:
                drawPatreonGlyph(tileRect, button.hovered, tileColor);
                break;
            case ExternalLinkTarget::GitHub:
                drawGitHubGlyph(tileRect);
                break;
            case ExternalLinkTarget::None:
                break;
        }
    }

    void renderExternalButton(const ExternalButton& button) {
        const Color shell = button.hovered ? Color{48, 60, 84, 255} : Color{28, 34, 48, 255};
        const Color border = button.hovered ? SKYBLUE : Color{82, 96, 126, 255};
        const Color tile = button.hovered ? Color{24, 30, 42, 255} : Color{20, 24, 35, 255};
        const Rectangle tileRect = insetRectangle(button.rect, 6.0f, 6.0f);

        DrawRectangleRounded(button.rect, 0.28f, 10, shell);
        DrawRectangleRoundedLines(button.rect, 0.28f, 10, border);
        DrawRectangleRounded(tileRect, 0.24f, 10, tile);
        DrawRectangleRoundedLines(tileRect,
                                  0.24f,
                                  10,
                                  button.hovered ? Fade(border, 0.7f) : Fade(border, 0.45f));
        drawExternalButtonGlyph(button, tileRect, tile);
    }

    int activeOptionCount() const {
        return currentView_ == View::Root ? kRootOptionCount : kMultiplayerOptionCount;
    }

    const char* activeOptionLabel(int index) const {
        if (index < 0 || index >= activeOptionCount()) {
            return "";
        }
        if (currentView_ == View::Root) {
            return rootMenuOptions_[index];
        }
        return multiplayerMenuOptions_[index];
    }

    GameMode activateRootOption(int index) {
        requestedSessionMode_ = net::SessionLaunchMode::None;
        switch (index) {
            case 0:
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::Multiplayer,
                    net::SessionEntryPoint::None};
                currentView_ = View::Multiplayer;
                selectedOption = 0;
                return GameMode::MAIN_MENU;
            case 1:
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::LabStudy,
                    net::SessionEntryPoint::LabStudy};
                currentView_ = View::Root;
                selectedOption = 0;
                return GameMode::LEVEL_SELECT;
            case 2:
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::LevelEditor,
                    net::SessionEntryPoint::LevelEditor};
                currentView_ = View::Root;
                selectedOption = 0;
                return GameMode::LEVEL_SELECT;
            case 3:
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::ReplayStudio,
                    net::SessionEntryPoint::Replay};
                currentView_ = View::Root;
                selectedOption = 0;
                return GameMode::REPLAY_STUDIO;
            case 4:
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::Settings,
                    net::SessionEntryPoint::None};
                return GameMode::SETTINGS;
            default:
                requestedNavigation_ = NavigationSelection{};
                return GameMode::MAIN_MENU;
        }
    }

    GameMode activateMultiplayerOption(int index) {
        requestedSessionMode_ = net::SessionLaunchMode::None;
        switch (index) {
            case 0:
                requestedSessionMode_ = net::SessionLaunchMode::Host;
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::Multiplayer,
                    net::SessionEntryPoint::Host};
                selectedOption = index;
                return GameMode::LEVEL_SELECT;
            case 1:
                requestedSessionMode_ = net::SessionLaunchMode::Join;
                requestedNavigation_ = NavigationSelection{
                    AppShellSurface::Multiplayer,
                    net::SessionEntryPoint::Join};
                selectedOption = index;
                return GameMode::MULTIPLAYER_SESSION;
            default:
                requestedNavigation_ = NavigationSelection{};
                return GameMode::MAIN_MENU;
        }
    }

    GameMode activateOption(int index) {
        return currentView_ == View::Root
            ? activateRootOption(index)
            : activateMultiplayerOption(index);
    }

    GameMode returnFromMultiplayerSubmenu() {
        requestedSessionMode_ = net::SessionLaunchMode::None;
        requestedNavigation_ = NavigationSelection{};
        currentView_ = View::Root;
        selectedOption = 0;
        return GameMode::MAIN_MENU;
    }

    void updateQuitShortcutSuppression(bool quitShortcutDown) {
        if (suppressQuitShortcutUntilRelease_ && !quitShortcutDown) {
            suppressQuitShortcutUntilRelease_ = false;
        }
    }

    GameMode handleQuitShortcutPressed() {
        if (suppressQuitShortcutUntilRelease_) {
            return GameMode::MAIN_MENU;
        }
        if (currentView_ == View::Multiplayer) {
            suppressQuitShortcutUntilRelease_ = true;
            return returnFromMultiplayerSubmenu();
        }
        requestedSessionMode_ = net::SessionLaunchMode::None;
        requestedNavigation_ = NavigationSelection{};
        return GameMode::QUIT;
    }

public:
    MainMenu()
        : selectedOption(0),
          currentView_(View::Root),
          requestedSessionMode_(net::SessionLaunchMode::None) {
        const float buttonX = Config::SCREEN_WIDTH * 0.5f - kButtonWidth * 0.5f;

        for (int i = 0; i < kMaxOptionCount; ++i) {
            buttons_[i].rect = Rectangle{
                buttonX,
                rootFirstButtonY() + static_cast<float>(i) * kButtonSpacing,
                kButtonWidth,
                kButtonHeight
            };
            buttons_[i].text = rootMenuOptions_[std::min(i, kRootOptionCount - 1)];
            buttons_[i].hovered = false;
        }

        const float socialTotalWidth =
            kSocialButtonSize * static_cast<float>(externalButtons_.size()) +
            kSocialButtonGap * static_cast<float>(externalButtons_.size() - 1);
        const float socialStartX = Config::SCREEN_WIDTH * 0.5f - socialTotalWidth * 0.5f;
        for (std::size_t index = 0; index < externalButtons_.size(); ++index) {
            externalButtons_[index].rect = Rectangle{
                socialStartX + static_cast<float>(index) * (kSocialButtonSize + kSocialButtonGap),
                rootSocialY(),
                kSocialButtonSize,
                kSocialButtonSize
            };
        }
    }

    ~MainMenu() {
        if (githubMarkTextureLoaded_ && IsWindowReady()) {
            UnloadTexture(githubMarkTexture_);
        }
    }

    int optionCount() const {
        return activeOptionCount();
    }

    const char* optionLabel(int index) const {
        return activeOptionLabel(index);
    }

    net::SessionLaunchMode requestedSessionMode() const {
        return requestedSessionMode_;
    }

    NavigationSelection requestedNavigation() const {
        return requestedNavigation_;
    }

    ExternalLinkTarget requestedExternalLink() const {
        return requestedExternalLink_;
    }

    AppShellSurface requestedSurface() const {
        return requestedNavigation_.surface;
    }

    net::SessionEntryPoint requestedEntryPoint() const {
        return requestedNavigation_.entryPoint;
    }

    bool showingMultiplayerSubmenu() const {
        return currentView_ == View::Multiplayer;
    }

    void clearRequestedSessionMode() {
        requestedSessionMode_ = net::SessionLaunchMode::None;
    }

    void clearRequestedNavigation() {
        requestedNavigation_ = NavigationSelection{};
    }

    void clearRequestedExternalLink() {
        requestedExternalLink_ = ExternalLinkTarget::None;
    }

    void setStatusMessage(const std::string& message) {
        statusMessage_ = message;
    }

    void suppressQuitShortcutUntilReleased() {
        suppressQuitShortcutUntilRelease_ = true;
    }

    void resetToRootView() {
        currentView_ = View::Root;
        selectedOption = 0;
        requestedSessionMode_ = net::SessionLaunchMode::None;
        requestedNavigation_ = NavigationSelection{};
    }

    GameMode triggerOptionForTest(int index) {
        return activateOption(index);
    }

    GameMode triggerQuitShortcutForTest() {
        return handleQuitShortcutPressed();
    }

    GameMode triggerSubmenuBackForTest() {
        return returnFromMultiplayerSubmenu();
    }

    GameMode triggerExternalLinkForTest(ExternalLinkTarget target) {
        requestedExternalLink_ = target;
        return GameMode::MAIN_MENU;
    }

    Rectangle optionRectForTest(int index) const {
        if (index < 0 || index >= kMaxOptionCount) {
            return Rectangle{};
        }
        return buttons_[index].rect;
    }

    float rootTitleYForTest() const {
        return kRootTitleY;
    }

    float rootSubtitleYForTest() const {
        return rootSubtitleY();
    }

    Rectangle externalButtonRectForTest(ExternalLinkTarget target) const {
        for (const auto& button : externalButtons_) {
            if (button.target == target) {
                return button.rect;
            }
        }
        return Rectangle{};
    }

    Rectangle multiplayerBackButtonRectForTest() const {
        return multiplayerBackButton_;
    }

    void releaseQuitShortcutSuppressionForTest() {
        updateQuitShortcutSuppression(false);
    }

    bool quitShortcutSuppressedForTest() const {
        return suppressQuitShortcutUntilRelease_;
    }

    GameMode update(bool allowQuitShortcut = true) {
        Vector2 mousePos = display::mousePosition();
        const int optionCount = activeOptionCount();
        updateQuitShortcutSuppression(IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_Q));

        for (int i = 0; i < kMaxOptionCount; ++i) {
            buttons_[i].hovered = false;
        }

        if (currentView_ == View::Multiplayer &&
            CheckCollisionPointRec(mousePos, multiplayerBackButton_) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            return returnFromMultiplayerSubmenu();
        }

        for (int i = 0; i < optionCount; ++i) {
            buttons_[i].text = activeOptionLabel(i);
            buttons_[i].hovered = CheckCollisionPointRec(mousePos, buttons_[i].rect);
            if (buttons_[i].hovered) {
                selectedOption = i;
            }
            if (buttons_[i].hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                return activateOption(i);
            }
        }

        if (currentView_ == View::Root) {
            for (auto& button : externalButtons_) {
                button.hovered = CheckCollisionPointRec(mousePos, button.rect);
                if (button.hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    requestedExternalLink_ = button.target;
                    return GameMode::MAIN_MENU;
                }
            }
        }

        if (IsKeyPressed(KEY_DOWN)) {
            selectedOption = (selectedOption + 1) % optionCount;
        }
        if (IsKeyPressed(KEY_UP)) {
            selectedOption = (selectedOption - 1 + optionCount) % optionCount;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
            return activateOption(selectedOption);
        }

        const int shortcutKeys[kMaxOptionCount] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE};
        for (int i = 0; i < optionCount; ++i) {
            if (IsKeyPressed(shortcutKeys[i])) {
                selectedOption = i;
                return activateOption(i);
            }
        }

        if (allowQuitShortcut && (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q))) {
            return handleQuitShortcutPressed();
        }

        return GameMode::MAIN_MENU;
    }

    void render() {
        renderGameplayBackdrop();
        TypographyService& typography = TypographyService::shared();
        const bool rootView = currentView_ == View::Root;

        const char* title = rootView ? "NETCODE SIMULATOR" : "MULTIPLAYER";
        typography.drawCentered(rootView
                                    ? TypographyStyleId::MenuTitle
                                    : TypographyStyleId::ScreenTitle,
                                title,
                                Config::SCREEN_WIDTH * 0.5f,
                                rootView ? kRootTitleY : 140.0f,
                                SKYBLUE);

        const char* subtitle = rootView
            ? "Real-time Network Artifact Sandbox"
            : "Choose whether to host locally or join an existing match";
        typography.drawCentered(TypographyStyleId::AppSubtitle,
                                subtitle,
                                Config::SCREEN_WIDTH * 0.5f,
                                rootView ? rootSubtitleY() : 245.0f,
                                LIGHTGRAY);

        if (currentView_ == View::Multiplayer) {
            const bool backHovered =
                CheckCollisionPointRec(display::mousePosition(), multiplayerBackButton_);
            DrawRectangleRounded(multiplayerBackButton_,
                                 0.18f,
                                 8,
                                 backHovered ? Color{56, 72, 96, 255} : Color{34, 40, 56, 255});
            DrawRectangleRoundedLines(multiplayerBackButton_,
                                      0.18f,
                                      8,
                                      backHovered ? SKYBLUE : Color{88, 100, 126, 255});
            const TypographyStyle& backStyle = typography.style(TypographyStyleId::ButtonLabel);
            typography.drawCentered(TypographyStyleId::ButtonLabel,
                                    "Back",
                                    multiplayerBackButton_.x + multiplayerBackButton_.width * 0.5f,
                                    multiplayerBackButton_.y + multiplayerBackButton_.height * 0.5f -
                                        backStyle.lineHeight * 0.5f,
                                    LIGHTGRAY);
        }

        const int optionCount = activeOptionCount();
        for (int i = 0; i < optionCount; ++i) {
            buttons_[i].text = activeOptionLabel(i);
            const bool highlighted = buttons_[i].hovered || selectedOption == i;
            const Color buttonColor = highlighted
                ? Color{70, 130, 180, 255}
                : Color{40, 50, 70, 255};
            const Color borderColor = highlighted ? SKYBLUE : DARKGRAY;

            DrawRectangleRounded(buttons_[i].rect, 0.15f, 12, buttonColor);
            DrawRectangleRoundedLines(buttons_[i].rect, 0.15f, 12, borderColor);

            const TypographyStyle& buttonStyle = typography.style(TypographyStyleId::ButtonLabel);
            typography.drawCentered(TypographyStyleId::ButtonLabel,
                                    buttons_[i].text,
                                    buttons_[i].rect.x + buttons_[i].rect.width * 0.5f,
                                    buttons_[i].rect.y + buttons_[i].rect.height * 0.5f -
                                        buttonStyle.lineHeight * 0.5f,
                                    WHITE);
        }

        if (rootView) {
            for (const auto& button : externalButtons_) {
                renderExternalButton(button);
            }
        }

        if (!statusMessage_.empty()) {
            typography.drawCentered(TypographyStyleId::Body,
                                    statusMessage_,
                                    Config::SCREEN_WIDTH * 0.5f,
                                    rootView ? rootSocialY() + kSocialButtonSize + 54.0f : 900.0f,
                                    LIGHTGRAY);
        }
    }
};
