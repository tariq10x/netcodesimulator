#include "ReplayStudio.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "LevelData.hpp"
#include "app/UserSettings.hpp"
#include "client/HudOverlayRenderer.hpp"
#include "client/ReplayTransportControls.hpp"
#include "sim/WorldState.hpp"

namespace {

constexpr float kPanelX = 32.0f;
constexpr float kPanelY = 88.0f;
constexpr float kPanelWidth = 500.0f;
constexpr float kRowHeight = 82.0f;
constexpr float kRowGap = 12.0f;
constexpr int kSeekFrameCount = 500;
constexpr int kSeekCommandTickCount = 300;
constexpr float kReplayThirdPersonZoom = 4.8f;
constexpr float kReplayStatusSeconds = 2.5f;
constexpr float kCheckpointTransitionAdjustmentSeconds = 0.25f;
constexpr std::size_t kMaxReplayTitleLength = 64u;

enum class LibraryActionButton {
    None,
    Rename,
    Delete,
    Open
};

Color panelColor() {
    return Fade(Color{12, 16, 24, 255}, 0.9f);
}

Color panelBorderColor(bool active) {
    return active ? Color{94, 194, 255, 255} : Fade(WHITE, 0.18f);
}

void drawText(TypographyStyleId styleId, std::string_view text, Vector2 position, Color color) {
    TypographyService::shared().draw(styleId, text, position, color);
}

int textWidth(TypographyStyleId styleId, std::string_view text) {
    return TypographyService::shared().measureWidth(styleId, text);
}

void drawCentered(TypographyStyleId styleId, std::string_view text, float centerX, float y, Color color) {
    TypographyService::shared().drawCentered(styleId, text, centerX, y, color);
}

void drawButton(Rectangle rect, const char* label, bool active, bool enabled) {
    const Color face = enabled
        ? (active ? Color{50, 96, 132, 255} : Color{28, 36, 52, 255})
        : Fade(Color{28, 36, 52, 255}, 0.45f);
    const Color border = enabled
        ? (active ? SKYBLUE : Fade(WHITE, 0.22f))
        : Fade(WHITE, 0.1f);
    DrawRectangleRounded(rect, 0.18f, 8, face);
    DrawRectangleRoundedLines(rect, 0.18f, 8, border);

    const TypographyStyle& style = TypographyService::shared().style(TypographyStyleId::ButtonLabel);
    drawCentered(TypographyStyleId::ButtonLabel,
                 label,
                 rect.x + rect.width * 0.5f,
                 rect.y + rect.height * 0.5f - style.lineHeight * 0.5f,
                 enabled ? WHITE : Fade(WHITE, 0.45f));
}

Rectangle libraryRenameButtonRect() {
    return Rectangle{kPanelX + 24.0f, kPanelY + 700.0f, 132.0f, 40.0f};
}

Rectangle libraryDeleteButtonRect() {
    return Rectangle{kPanelX + 166.0f, kPanelY + 700.0f, 132.0f, 40.0f};
}

Rectangle libraryOpenButtonRect() {
    return Rectangle{kPanelX + 308.0f, kPanelY + 700.0f, 168.0f, 40.0f};
}

LibraryActionButton libraryActionButtonAt(Vector2 mouse) {
    if (CheckCollisionPointRec(mouse, libraryRenameButtonRect())) {
        return LibraryActionButton::Rename;
    }
    if (CheckCollisionPointRec(mouse, libraryDeleteButtonRect())) {
        return LibraryActionButton::Delete;
    }
    if (CheckCollisionPointRec(mouse, libraryOpenButtonRect())) {
        return LibraryActionButton::Open;
    }
    return LibraryActionButton::None;
}

Rectangle dialogRect(float width = 620.0f, float height = 250.0f) {
    return Rectangle{
        (static_cast<float>(Config::SCREEN_WIDTH) - width) * 0.5f,
        (static_cast<float>(Config::SCREEN_HEIGHT) - height) * 0.5f,
        width,
        height
    };
}

Rectangle dialogPrimaryButtonRect(const Rectangle& dialog) {
    return Rectangle{dialog.x + dialog.width - 292.0f, dialog.y + dialog.height - 64.0f, 124.0f, 40.0f};
}

Rectangle dialogSecondaryButtonRect(const Rectangle& dialog) {
    return Rectangle{dialog.x + dialog.width - 154.0f, dialog.y + dialog.height - 64.0f, 124.0f, 40.0f};
}

bool isReplayTitleCharacter(int codepoint) {
    return codepoint >= 32 &&
           codepoint <= 126 &&
           codepoint != '"' &&
           codepoint != '\\';
}

std::string sanitizeReplayTitle(std::string value) {
    std::string filtered;
    filtered.reserve(std::min(value.size(), kMaxReplayTitleLength));
    for (char ch : value) {
        if (!isReplayTitleCharacter(static_cast<unsigned char>(ch))) {
            continue;
        }
        if (filtered.size() >= kMaxReplayTitleLength) {
            break;
        }
        filtered.push_back(ch);
    }
    const std::size_t first = filtered.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "Replay";
    }
    const std::size_t last = filtered.find_last_not_of(' ');
    return filtered.substr(first, last - first + 1u);
}

std::string truncateText(std::string value, std::size_t maxLength) {
    if (value.size() <= maxLength) {
        return value;
    }
    if (maxLength <= 3u) {
        return value.substr(0u, maxLength);
    }
    return value.substr(0u, maxLength - 3u) + "...";
}

Vector3 toVector3(const sim::Vec3& value) {
    return Vector3{value.x, value.y, value.z};
}

sim::TeamId teamForActor(const sim::WorldState& world, int actorId) {
    if (const sim::RosterEntry* entry = sim::findRosterEntry(world, actorId);
        entry != nullptr) {
        return entry->team;
    }
    return sim::TeamId::None;
}

Color teamTint(sim::TeamId team, bool ghost = false) {
    Color tint{180, 180, 180, 255};
    switch (team) {
        case sim::TeamId::Attacker:
            tint = Color{255, 119, 72, 255};
            break;
        case sim::TeamId::Defender:
            tint = Color{72, 156, 255, 255};
            break;
        case sim::TeamId::Spectator:
        case sim::TeamId::None:
            break;
    }
    if (ghost) {
        tint.a = static_cast<unsigned char>(static_cast<float>(tint.a) * 0.38f);
    }
    return tint;
}

Camera3D cameraForWorld(const sim::WorldState& world,
                        const std::vector<sim::PlayerState>& controlPlayers) {
    sim::Vec3 focus{};
    if (!world.players.empty()) {
        focus = world.players.front().position;
    } else if (!controlPlayers.empty()) {
        focus = controlPlayers.front().position;
    }

    Camera3D camera{};
    camera.position = Vector3{focus.x + 10.0f, focus.y + 7.0f, focus.z + 12.0f};
    camera.target = toVector3(focus);
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    return camera;
}

sim::Vec3 lerpVec3(const sim::Vec3& from, const sim::Vec3& to, float alpha) {
    return sim::Vec3{
        from.x + ((to.x - from.x) * alpha),
        from.y + ((to.y - from.y) * alpha),
        from.z + ((to.z - from.z) * alpha)
    };
}

float lerpValue(float from, float to, float alpha) {
    return from + ((to - from) * alpha);
}

float lerpAngle(float from, float to, float alpha) {
    float delta = std::fmod(to - from + PI, 2.0f * PI);
    if (delta < 0.0f) {
        delta += 2.0f * PI;
    }
    delta -= PI;
    return from + (delta * alpha);
}

const sim::PlayerState* findPlayerById(const std::vector<sim::PlayerState>& players,
                                       int playerId) {
    const auto it = std::find_if(players.begin(),
                                 players.end(),
                                 [playerId](const sim::PlayerState& player) {
                                     return player.playerId == playerId;
                                 });
    return it == players.end() ? nullptr : &(*it);
}

const sim::RemoteActorState* findEnemyById(const std::vector<sim::RemoteActorState>& enemies,
                                          int entityId) {
    const auto it = std::find_if(enemies.begin(),
                                 enemies.end(),
                                 [entityId](const sim::RemoteActorState& enemy) {
                                     return enemy.entityId == entityId;
                                 });
    return it == enemies.end() ? nullptr : &(*it);
}

sim::PlayerState interpolatePlayer(const sim::PlayerState& from,
                                   const sim::PlayerState& to,
                                   float alpha) {
    sim::PlayerState result = from;
    result.position = lerpVec3(from.position, to.position, alpha);
    result.velocity = lerpVec3(from.velocity, to.velocity, alpha);
    result.yaw = lerpAngle(from.yaw, to.yaw, alpha);
    result.pitch = lerpAngle(from.pitch, to.pitch, alpha);
    result.health = lerpValue(from.health, to.health, alpha);
    result.maxHealth = lerpValue(from.maxHealth, to.maxHealth, alpha);
    result.weaponCooldownRemaining =
        lerpValue(from.weaponCooldownRemaining, to.weaponCooldownRemaining, alpha);
    result.jumpsUsed = alpha >= 1.0f ? to.jumpsUsed : from.jumpsUsed;
    result.grounded = alpha >= 1.0f ? to.grounded : from.grounded;
    return result;
}

sim::RemoteActorState interpolateEnemy(const sim::RemoteActorState& from,
                                       const sim::RemoteActorState& to,
                                       float alpha) {
    sim::RemoteActorState result = from;
    result.position = lerpVec3(from.position, to.position, alpha);
    result.velocity = lerpVec3(from.velocity, to.velocity, alpha);
    result.yaw = lerpAngle(from.yaw, to.yaw, alpha);
    result.pitch = lerpAngle(from.pitch, to.pitch, alpha);
    result.health = lerpValue(from.health, to.health, alpha);
    result.radius = lerpValue(from.radius, to.radius, alpha);
    result.alive = alpha >= 1.0f ? to.alive : from.alive;
    return result;
}

std::vector<sim::PlayerState> interpolatePlayers(const std::vector<sim::PlayerState>& from,
                                                 const std::vector<sim::PlayerState>& to,
                                                 float alpha) {
    std::vector<sim::PlayerState> result = from;
    for (sim::PlayerState& player : result) {
        if (const sim::PlayerState* next = findPlayerById(to, player.playerId);
            next != nullptr) {
            player = interpolatePlayer(player, *next, alpha);
        }
    }
    if (alpha >= 0.5f) {
        for (const sim::PlayerState& next : to) {
            if (findPlayerById(result, next.playerId) == nullptr) {
                result.push_back(next);
            }
        }
    }
    return result;
}

sim::WorldState interpolateWorld(const sim::WorldState& from,
                                 const sim::WorldState& to,
                                 float alpha) {
    sim::WorldState result = from;
    result.players = interpolatePlayers(from.players, to.players, alpha);
    for (sim::RemoteActorState& enemy : result.enemies) {
        if (const sim::RemoteActorState* next = findEnemyById(to.enemies, enemy.entityId);
            next != nullptr) {
            enemy = interpolateEnemy(enemy, *next, alpha);
        }
    }
    if (alpha >= 0.5f) {
        for (const sim::RemoteActorState& next : to.enemies) {
            if (findEnemyById(result.enemies, next.entityId) == nullptr) {
                result.enemies.push_back(next);
            }
        }
    }
    result.authoritativeTime = alpha >= 1.0f
        ? to.authoritativeTime
        : from.authoritativeTime;
    return result;
}

}  // namespace

ReplayStudio::ReplayStudio() {
    replayControlBindings_ = app::UserSettingsStore().load().settings.controls;
    refreshLibrary();
}

GameMode ReplayStudio::update(float dtSeconds, bool allowBackShortcut) {
    if (immersiveReplayActive_) {
        updateImmersiveCommandReplay(dtSeconds);
        return GameMode::REPLAY_STUDIO;
    }

    if (renameDialogVisible_ || deleteConfirmVisible_) {
        updateLibraryDialogInput();
        return GameMode::REPLAY_STUDIO;
    }

    if (allowBackShortcut && (IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_ESCAPE))) {
        return GameMode::MAIN_MENU;
    }

    if (IsKeyPressed(KEY_R)) {
        refreshLibrary();
    }

    updateRows();
    const Vector2 mouse = display::mousePosition();
    const bool mousePressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const LibraryActionButton hoveredAction =
        !libraryEntries_.empty() ? libraryActionButtonAt(mouse) : LibraryActionButton::None;
    const bool libraryActionHovered = hoveredAction != LibraryActionButton::None;
    bool mouseClickConsumed = false;

    if (mousePressed && libraryActionHovered) {
        mouseClickConsumed = true;
        if (selectedReplayAvailable()) {
            switch (hoveredAction) {
                case LibraryActionButton::Rename:
                    beginRenameSelectedReplay();
                    break;
                case LibraryActionButton::Delete:
                    requestDeleteSelectedReplay();
                    break;
                case LibraryActionButton::Open:
                    if (loadSelectedReplay() && commandLoaded_) {
                        enterCommandReplay();
                    }
                    break;
                case LibraryActionButton::None:
                    break;
            }
        }
    }

    for (std::size_t index = 0u; index < rows_.size(); ++index) {
        rows_[index].hovered =
            !libraryActionHovered && CheckCollisionPointRec(mouse, rows_[index].bounds);
        if (rows_[index].hovered && mousePressed && !mouseClickConsumed) {
            selectedIndex_ = static_cast<int>(index);
            previewSelectedReplay();
        }
    }

    if (!libraryEntries_.empty()) {
        if (IsKeyPressed(KEY_DOWN)) {
            selectedIndex_ = (selectedIndex_ + 1) % static_cast<int>(libraryEntries_.size());
            previewSelectedReplay();
        }
        if (IsKeyPressed(KEY_UP)) {
            selectedIndex_ = (selectedIndex_ - 1 + static_cast<int>(libraryEntries_.size())) %
                             static_cast<int>(libraryEntries_.size());
            previewSelectedReplay();
        }
        if (IsKeyPressed(KEY_F2) || IsKeyPressed(KEY_N)) {
            beginRenameSelectedReplay();
        }
        if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_D)) {
            requestDeleteSelectedReplay();
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            if (loadSelectedReplay() && commandLoaded_) {
                enterCommandReplay();
            }
        }
    }

    if (commandLoaded_) {
        if (IsKeyPressed(KEY_E)) {
            enterCommandReplay();
            return GameMode::REPLAY_STUDIO;
        }
        if (IsKeyPressed(KEY_SIX)) {
            commandPlaybackPlaying_ = !commandPlaybackPlaying_;
        }
        if (IsKeyPressed(KEY_ZERO)) {
            resetCommandPlayback();
        }
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_SEVEN)) {
            seekCommandPlaybackByTicks(-kSeekCommandTickCount);
        }
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_EIGHT)) {
            seekCommandPlaybackByTicks(kSeekCommandTickCount);
        }
        if (IsKeyPressed(KEY_T)) {
            toggleCommandTrack();
        }
        updateCommandPlayback(dtSeconds, true);
    } else if (loaded_) {
        if (IsKeyPressed(KEY_SIX)) {
            timeline_.playbackPlaying = !timeline_.playbackPlaying;
        }
        if (IsKeyPressed(KEY_ZERO)) {
            timeline_.resetPlayback(loadedRecording_.frames);
        }
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_SEVEN)) {
            timeline_.seekBy(loadedRecording_.frames, -kSeekFrameCount);
        }
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_EIGHT)) {
            timeline_.seekBy(loadedRecording_.frames, kSeekFrameCount);
        }
        updateLegacyPlayback(dtSeconds, true);
    }

    return GameMode::REPLAY_STUDIO;
}

void ReplayStudio::render() const {
    ClearBackground(Color{8, 10, 15, 255});
    if (immersiveReplayActive_) {
        renderImmersiveCommandReplay();
        return;
    }

    renderLoadedReplay();
    renderLibrary();
    if (!renameDialogVisible_ && !deleteConfirmVisible_) {
        renderTransport();
    }
}

const std::vector<app::ReplayArchiveEntry>& ReplayStudio::entriesForTest() const {
    return legacyEntriesForTest_;
}

bool ReplayStudio::hasLoadedRecordingForTest() const {
    return loaded_;
}

void ReplayStudio::refreshLibrary() {
    legacyEntriesForTest_ = archive_.list();
    libraryEntries_.clear();

    const std::filesystem::path directory = app::ReplayArchive::replayDirectory();
    std::error_code ec;
    if (std::filesystem::exists(directory, ec)) {
        for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) {
                break;
            }
            if (!item.is_regular_file(ec) ||
                item.path().extension() != replay::ReplayArchive::kCommandReplayExtension) {
                continue;
            }

            replay::ReplayDemo demo;
            std::string error;
            if (!commandArchive_.load(item.path(), &demo, &error)) {
                continue;
            }

            LibraryEntry entry;
            entry.kind = EntryKind::Command;
            entry.path = item.path();
            entry.commandHeader = demo.header;
            entry.mapLabel = formatMapLabel(demo.header.levelSlot, demo.header.levelHash);
            entry.commandCount = demo.commandEvents.size();
            entry.fileSizeBytes = item.file_size(ec);
            if (ec) {
                entry.fileSizeBytes = 0u;
                ec.clear();
            }
            libraryEntries_.push_back(std::move(entry));
        }
    }

    for (const app::ReplayArchiveEntry& legacy : legacyEntriesForTest_) {
        LibraryEntry entry;
        entry.kind = EntryKind::LegacyFrame;
        entry.path = legacy.path;
        entry.legacyMetadata = legacy.metadata;
        entry.mapLabel = formatMapLabel(legacy.metadata.levelSlot, legacy.metadata.levelHash);
        entry.frameCount = legacy.frameCount;
        entry.fileSizeBytes = legacy.fileSizeBytes;
        libraryEntries_.push_back(std::move(entry));
    }

    std::sort(libraryEntries_.begin(),
              libraryEntries_.end(),
              [](const LibraryEntry& lhs, const LibraryEntry& rhs) {
                  const std::uint64_t lhsTime = lhs.kind == EntryKind::Command
                      ? lhs.commandHeader.recordedAtUnixSeconds
                      : lhs.legacyMetadata.createdUnixSeconds;
                  const std::uint64_t rhsTime = rhs.kind == EntryKind::Command
                      ? rhs.commandHeader.recordedAtUnixSeconds
                      : rhs.legacyMetadata.createdUnixSeconds;
                  if (lhsTime != rhsTime) {
                      return lhsTime > rhsTime;
                  }
                  return lhs.kind == EntryKind::Command && rhs.kind != EntryKind::Command;
              });

    if (libraryEntries_.empty()) {
        selectedIndex_ = 0;
        statusMessage_ = "No saved replays";
    } else {
        selectedIndex_ = std::clamp(selectedIndex_, 0, static_cast<int>(libraryEntries_.size()) - 1);
        statusMessage_ = std::to_string(libraryEntries_.size()) + " saved replay" +
            (libraryEntries_.size() == 1u ? "" : "s");
    }
    updateRows();
}

bool ReplayStudio::loadSelectedReplay() {
    if (libraryEntries_.empty() ||
        selectedIndex_ < 0 ||
        selectedIndex_ >= static_cast<int>(libraryEntries_.size())) {
        statusMessage_ = "No replay selected";
        return false;
    }

    const LibraryEntry& entry = libraryEntries_[selectedIndex_];
    return entry.kind == EntryKind::Command
        ? loadCommandReplay(entry)
        : loadLegacyReplay(entry);
}

bool ReplayStudio::previewSelectedReplay() {
    if (!loadSelectedReplay()) {
        return false;
    }

    if (commandLoaded_) {
        commandPlaybackPlaying_ = true;
    } else if (loaded_) {
        timeline_.playbackPlaying = true;
    }
    return true;
}

bool ReplayStudio::selectedReplayAvailable() const {
    return !libraryEntries_.empty() &&
           selectedIndex_ >= 0 &&
           selectedIndex_ < static_cast<int>(libraryEntries_.size());
}

std::string ReplayStudio::selectedReplayTitle() const {
    if (!selectedReplayAvailable()) {
        return {};
    }
    const LibraryEntry& entry = libraryEntries_[selectedIndex_];
    if (entry.kind == EntryKind::Command) {
        return entry.commandHeader.title.empty()
            ? entry.path.stem().string()
            : entry.commandHeader.title;
    }
    return entry.legacyMetadata.title.empty()
        ? entry.path.stem().string()
        : entry.legacyMetadata.title;
}

void ReplayStudio::beginRenameSelectedReplay() {
    if (!selectedReplayAvailable()) {
        statusMessage_ = "No replay selected";
        return;
    }
    deleteConfirmVisible_ = false;
    renameDialogVisible_ = true;
    renameText_ = selectedReplayTitle();
}

void ReplayStudio::requestDeleteSelectedReplay() {
    if (!selectedReplayAvailable()) {
        statusMessage_ = "No replay selected";
        return;
    }
    renameDialogVisible_ = false;
    deleteConfirmVisible_ = true;
}

void ReplayStudio::cancelLibraryDialog() {
    renameDialogVisible_ = false;
    deleteConfirmVisible_ = false;
    renameText_.clear();
}

void ReplayStudio::applyRenameSelectedReplay() {
    if (!selectedReplayAvailable()) {
        cancelLibraryDialog();
        statusMessage_ = "No replay selected";
        return;
    }

    const std::string title = sanitizeReplayTitle(renameText_);
    std::filesystem::path selectedPath;
    if (!renameReplay(libraryEntries_[selectedIndex_], title, &selectedPath)) {
        return;
    }

    cancelLibraryDialog();
    refreshLibrary();
    selectLibraryEntryByPath(selectedPath);
    statusMessage_ = "Renamed replay";
}

void ReplayStudio::confirmDeleteSelectedReplay() {
    if (!selectedReplayAvailable()) {
        cancelLibraryDialog();
        statusMessage_ = "No replay selected";
        return;
    }

    const int nextSelection = selectedIndex_;
    const LibraryEntry entry = libraryEntries_[selectedIndex_];
    if (!deleteReplay(entry)) {
        return;
    }

    cancelLibraryDialog();
    if (entry.path == loadedReplayPath_) {
        clearLoadedReplay();
    }
    refreshLibrary();
    if (!libraryEntries_.empty()) {
        selectedIndex_ = std::clamp(nextSelection,
                                    0,
                                    static_cast<int>(libraryEntries_.size()) - 1);
    }
    statusMessage_ = "Deleted replay";
}

void ReplayStudio::updateLibraryDialogInput() {
    const Vector2 mouse = display::mousePosition();

    if (renameDialogVisible_) {
        int codepoint = GetCharPressed();
        while (codepoint > 0) {
            if (isReplayTitleCharacter(codepoint) &&
                renameText_.size() < kMaxReplayTitleLength) {
                renameText_.push_back(static_cast<char>(codepoint));
            }
            codepoint = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !renameText_.empty()) {
            renameText_.pop_back();
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            applyRenameSelectedReplay();
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            cancelLibraryDialog();
            return;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Rectangle dialog = dialogRect();
            if (CheckCollisionPointRec(mouse, dialogPrimaryButtonRect(dialog))) {
                applyRenameSelectedReplay();
            } else if (CheckCollisionPointRec(mouse, dialogSecondaryButtonRect(dialog))) {
                cancelLibraryDialog();
            }
        }
        return;
    }

    if (deleteConfirmVisible_) {
        if (IsKeyPressed(KEY_Y) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            confirmDeleteSelectedReplay();
            return;
        }
        if (IsKeyPressed(KEY_N) || IsKeyPressed(KEY_ESCAPE)) {
            cancelLibraryDialog();
            return;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Rectangle dialog = dialogRect(600.0f, 220.0f);
            if (CheckCollisionPointRec(mouse, dialogPrimaryButtonRect(dialog))) {
                confirmDeleteSelectedReplay();
            } else if (CheckCollisionPointRec(mouse, dialogSecondaryButtonRect(dialog))) {
                cancelLibraryDialog();
            }
        }
    }
}

bool ReplayStudio::renameReplay(LibraryEntry entry,
                                const std::string& title,
                                std::filesystem::path* selectedPathOut) {
    if (entry.kind == EntryKind::Command) {
        replay::ReplayDemo demo;
        std::string error;
        if (!commandArchive_.load(entry.path, &demo, &error)) {
            statusMessage_ = error.empty() ? "Failed to rename replay" : error;
            return false;
        }

        demo.header.title = title;
        const std::filesystem::path tempPath = entry.path.string() + ".tmp";
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        ec.clear();
        if (!commandArchive_.save(demo, tempPath, &error)) {
            statusMessage_ = error.empty() ? "Failed to rename replay" : error;
            return false;
        }
        std::filesystem::remove(entry.path, ec);
        if (ec) {
            std::filesystem::remove(tempPath, ec);
            statusMessage_ = "Failed to replace replay file";
            return false;
        }
        ec.clear();
        std::filesystem::rename(tempPath, entry.path, ec);
        if (ec) {
            statusMessage_ = "Failed to publish renamed replay";
            return false;
        }
        if (loadedReplayPath_ == entry.path && commandLoaded_) {
            loadedCommandReplay_.header.title = title;
        }
        if (selectedPathOut != nullptr) {
            *selectedPathOut = entry.path;
        }
        return true;
    }

    client::ReplayRecording recording;
    std::string error;
    if (!archive_.load(entry.path, &recording, &error)) {
        statusMessage_ = error.empty() ? "Failed to rename replay" : error;
        return false;
    }

    recording.metadata.title = title;
    std::filesystem::path savedPath;
    if (!archive_.save(recording, &savedPath, &error)) {
        statusMessage_ = error.empty() ? "Failed to rename replay" : error;
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(entry.path, ec);
    if (ec) {
        std::error_code cleanupEc;
        std::filesystem::remove(savedPath, cleanupEc);
        statusMessage_ = "Failed to replace replay file";
        return false;
    }
    if (loadedReplayPath_ == entry.path && loaded_ && !commandLoaded_) {
        loadedReplayPath_ = savedPath;
        loadedRecording_.metadata.title = title;
    }
    if (selectedPathOut != nullptr) {
        *selectedPathOut = savedPath;
    }
    return true;
}

bool ReplayStudio::deleteReplay(const LibraryEntry& entry) {
    std::error_code ec;
    const bool removed = std::filesystem::remove(entry.path, ec);
    if (ec || !removed) {
        statusMessage_ = "Failed to delete replay";
        return false;
    }
    return true;
}

void ReplayStudio::clearLoadedReplay() {
    loadedReplayPath_.clear();
    loaded_ = false;
    commandLoaded_ = false;
    immersiveReplayActive_ = false;
    replaySettingsVisible_ = false;
    replayTransportOverlayVisible_ = false;
    loadedRecording_ = client::ReplayRecording{};
    loadedCommandReplay_ = replay::ReplayDemo{};
    timeline_.reset();
}

void ReplayStudio::selectLibraryEntryByPath(const std::filesystem::path& path) {
    const auto it = std::find_if(libraryEntries_.begin(),
                                 libraryEntries_.end(),
                                 [&path](const LibraryEntry& entry) {
                                     return entry.path == path;
                                 });
    if (it != libraryEntries_.end()) {
        selectedIndex_ = static_cast<int>(std::distance(libraryEntries_.begin(), it));
    }
}

void ReplayStudio::enterCommandReplay() {
    if (!commandLoaded_) {
        return;
    }

    if (loadedCommandReplay_.header.hasControlLane &&
        commandTrack_ != replay::ReplayPlaybackTrack::Control) {
        commandTrack_ = replay::ReplayPlaybackTrack::Control;
        commandPlayback_.load(loadedCommandReplay_, commandTrack_);
        commandNextPlayback_.load(loadedCommandReplay_, commandTrack_);
    }
    updateCommandPlayback(0.0f);
    ensureReplayFollowTarget();
    const Camera3D entryCamera = activeCommandReplayCamera();
    if (activeCommandReplayPlayer() == nullptr) {
        replayCameraMode_ = ReplayCameraMode::SpectatorFreeFly;
        replaySpectatorCamera_.resetFromCamera(entryCamera);
    }

    loadReplayCheckpoints();
    immersiveReplayActive_ = true;
    setImmersiveStatus(commandTrack_ == replay::ReplayPlaybackTrack::Control
                           ? "Entered command replay (control track)"
                           : "Entered command replay");
    display::disableCursorForCapture();
}

void ReplayStudio::exitCommandReplay() {
    if (!immersiveReplayActive_) {
        return;
    }

    immersiveReplayActive_ = false;
    replayCheckpointTransitionActive_ = false;
    replaySettingsVisible_ = false;
    replayTransportOverlayVisible_ = false;
    replaySettingsOverlay_.resetInteraction();
    saveReplayCheckpoints();
    statusMessage_ = "Exited replay";
    display::enableCursorPreservingPosition();
}

bool ReplayStudio::loadCommandReplay(const LibraryEntry& entry) {
    std::string error;
    replay::ReplayDemo demo;
    if (!commandArchive_.load(entry.path, &demo, &error)) {
        loaded_ = false;
        commandLoaded_ = false;
        immersiveReplayActive_ = false;
        loadedReplayPath_.clear();
        loadedCommandReplay_ = replay::ReplayDemo{};
        statusMessage_ = error.empty() ? "Failed to load command replay" : error;
        replaySettingsOverlay_.resetInteraction();
        return false;
    }

    loadedCommandReplay_ = std::move(demo);
    loadedReplayPath_ = entry.path;
    loadedRecording_ = client::ReplayRecording{};
    commandLoaded_ = true;
    loaded_ = true;
    immersiveReplayActive_ = false;
    replaySettingsVisible_ = false;
    replayTransportOverlayVisible_ = false;
    replaySettingsOverlay_.resetInteraction();
    commandTrack_ = loadedCommandReplay_.header.hasControlLane
        ? replay::ReplayPlaybackTrack::Control
        : replay::ReplayPlaybackTrack::ServerTruth;
    commandPlaybackPlaying_ = true;
    commandPlaybackTimer_ = 0.0f;
    replayCameraMode_ = ReplayCameraMode::FollowFirstPerson;
    replayFollowActorId_ = -1;
    replayCheckpointTransitionActive_ = false;
    commandStartTick_ = loadedCommandReplay_.initialState.worldState.authoritativeTime.serverTick;
    commandEndTick_ = commandStartTick_;
    for (const replay::ServerCommandEvent& event : loadedCommandReplay_.commandEvents) {
        commandEndTick_ = std::max(commandEndTick_, event.serverTick);
    }
    for (const replay::WorldKeyframe& keyframe : loadedCommandReplay_.keyframes) {
        commandEndTick_ = std::max(commandEndTick_, keyframe.serverTick);
    }
    if (loadedCommandReplay_.header.durationUs > 0u &&
        loadedCommandReplay_.header.tickRateHz > 0u) {
        const std::uint32_t durationTicks = static_cast<std::uint32_t>(
            (loadedCommandReplay_.header.durationUs *
             static_cast<std::uint64_t>(loadedCommandReplay_.header.tickRateHz)) /
            1'000'000u);
        commandEndTick_ = std::max(commandEndTick_, commandStartTick_ + durationTicks);
    }

    loadArenaForRecording(loadedCommandReplay_.header.levelSlot);
    commandPlayback_.load(loadedCommandReplay_, commandTrack_);
    resetCommandPlayback();
    ensureReplayFollowTarget();
    statusMessage_ = "Loaded command replay " +
        (loadedCommandReplay_.header.title.empty()
             ? entry.path.stem().string()
             : loadedCommandReplay_.header.title);
    return true;
}

bool ReplayStudio::loadLegacyReplay(const LibraryEntry& entry) {
    std::string error;
    client::ReplayRecording recording;
    if (!archive_.load(entry.path, &recording, &error) ||
        recording.frames.empty()) {
        loaded_ = false;
        commandLoaded_ = false;
        immersiveReplayActive_ = false;
        loadedReplayPath_.clear();
        loadedRecording_ = client::ReplayRecording{};
        timeline_.reset();
        statusMessage_ = error.empty() ? "Failed to load replay" : error;
        return false;
    }

    loadedRecording_ = std::move(recording);
    loadedReplayPath_ = entry.path;
    loadedCommandReplay_ = replay::ReplayDemo{};
    commandLoaded_ = false;
    immersiveReplayActive_ = false;
    replaySettingsVisible_ = false;
    replayTransportOverlayVisible_ = false;
    replaySettingsOverlay_.resetInteraction();
    loaded_ = true;
    loadArenaForRecording(loadedRecording_.metadata.levelSlot);
    timeline_.startPlayback(loadedRecording_.frames, true);
    statusMessage_ = "Loaded legacy frame replay " + loadedRecording_.metadata.title;
    return true;
}

void ReplayStudio::loadArenaForRecording(int levelSlot) {
    arena_ = Arena3D{};
    if (levelSlot >= 1 && levelSlot <= 9) {
        LevelData::LevelDefinition level;
        if (LevelData::loadLevel(level, levelSlot)) {
            arena_.loadLevel(level);
        }
    }
}

const client::RecordedReplayFrame* ReplayStudio::activeFrame() const {
    if (!loaded_ || loadedRecording_.frames.empty()) {
        return nullptr;
    }
    const std::size_t index = std::min(timeline_.playbackIndex,
                                       loadedRecording_.frames.size() - 1u);
    return &loadedRecording_.frames[index];
}

float ReplayStudio::loadedDurationSeconds() const {
    if (!loaded_ || loadedRecording_.frames.empty()) {
        return 0.0f;
    }
    return std::max(0.0f, loadedRecording_.frames.back().timestamp -
        loadedRecording_.frames.front().timestamp);
}

float ReplayStudio::commandDurationSeconds() const {
    if (!commandLoaded_) {
        return 0.0f;
    }
    if (loadedCommandReplay_.header.durationUs > 0u) {
        return static_cast<float>(
            static_cast<double>(loadedCommandReplay_.header.durationUs) / 1'000'000.0);
    }
    const std::uint16_t tickRate = loadedCommandReplay_.header.tickRateHz == 0u
        ? 60u
        : loadedCommandReplay_.header.tickRateHz;
    if (commandEndTick_ <= commandStartTick_) {
        return 0.0f;
    }
    return static_cast<float>(commandEndTick_ - commandStartTick_) /
           static_cast<float>(tickRate);
}

void ReplayStudio::updateCommandPlayback(float dtSeconds, bool loopPlayback) {
    if (!commandLoaded_) {
        return;
    }
    if (commandPlaybackPlaying_) {
        commandPlaybackTimer_ += std::max(0.0f, dtSeconds);
        const float duration = commandDurationSeconds();
        if (duration > 0.0f && commandPlaybackTimer_ > duration) {
            if (loopPlayback) {
                commandPlaybackTimer_ = std::fmod(commandPlaybackTimer_, duration);
                commandPlayback_.load(loadedCommandReplay_, commandTrack_);
                commandNextPlayback_.load(loadedCommandReplay_, commandTrack_);
            } else {
                commandPlaybackTimer_ = duration;
                commandPlaybackPlaying_ = false;
            }
        }
    }

    const std::uint16_t tickRate = loadedCommandReplay_.header.tickRateHz == 0u
        ? 60u
        : loadedCommandReplay_.header.tickRateHz;
    const float exactTickPosition =
        static_cast<float>(commandStartTick_) +
        (commandPlaybackTimer_ * static_cast<float>(tickRate));
    refreshCommandRenderSample(exactTickPosition);
}

void ReplayStudio::updateLegacyPlayback(float dtSeconds, bool loopPlayback) {
    if (!loaded_ || commandLoaded_) {
        return;
    }

    const bool wasPlaying = timeline_.playbackPlaying;
    timeline_.updatePlayback(loadedRecording_.frames, dtSeconds);
    if (loopPlayback &&
        wasPlaying &&
        !timeline_.playbackPlaying &&
        !loadedRecording_.frames.empty() &&
        timeline_.playbackIndex + 1u >= loadedRecording_.frames.size()) {
        timeline_.startPlayback(loadedRecording_.frames, true);
    }
}

void ReplayStudio::refreshCommandRenderSample(float exactTickPosition) {
    if (!commandLoaded_) {
        return;
    }

    const float minTick = static_cast<float>(commandStartTick_);
    const float maxTick = static_cast<float>(std::max(commandStartTick_, commandEndTick_));
    const float clampedTick = std::clamp(exactTickPosition, minTick, maxTick);

    if (!commandInterpolationEnabled_) {
        const std::uint32_t targetTick = std::min(
            static_cast<std::uint32_t>(std::round(clampedTick)),
            commandEndTick_);
        commandPlayback_.seekToTick(targetTick);
        commandRenderBaseTick_ = targetTick;
        commandRenderNextTick_ = targetTick;
        commandInterpolationAlpha_ = 0.0f;
        interpolatedCommandWorld_ = commandPlayback_.worldState();
        interpolatedControlPlayers_ = commandPlayback_.controlPlayers();
        return;
    }

    const std::uint32_t baseTick = std::min(
        static_cast<std::uint32_t>(std::floor(clampedTick)),
        commandEndTick_);
    const std::uint32_t nextTick = std::min(baseTick + 1u, commandEndTick_);
    const float alpha = nextTick > baseTick
        ? std::clamp(clampedTick - static_cast<float>(baseTick), 0.0f, 1.0f)
        : 0.0f;

    commandPlayback_.seekToTick(baseTick);
    commandNextPlayback_.seekToTick(nextTick);
    commandRenderBaseTick_ = baseTick;
    commandRenderNextTick_ = nextTick;
    commandInterpolationAlpha_ = alpha;
    interpolatedCommandWorld_ =
        interpolateWorld(commandPlayback_.worldState(), commandNextPlayback_.worldState(), alpha);
    interpolatedControlPlayers_ =
        interpolatePlayers(commandPlayback_.controlPlayers(),
                           commandNextPlayback_.controlPlayers(),
                           alpha);
}

void ReplayStudio::seekCommandPlaybackByTicks(int tickDelta) {
    if (!commandLoaded_) {
        return;
    }
    const std::uint16_t tickRate = loadedCommandReplay_.header.tickRateHz == 0u
        ? 60u
        : loadedCommandReplay_.header.tickRateHz;
    const std::int64_t currentTick =
        static_cast<std::int64_t>(commandStartTick_) +
        static_cast<std::int64_t>(std::round(commandPlaybackTimer_ * static_cast<float>(tickRate)));
    const std::int64_t targetTick =
        std::clamp<std::int64_t>(currentTick + tickDelta,
                                 static_cast<std::int64_t>(commandStartTick_),
                                 static_cast<std::int64_t>(commandEndTick_));
    commandPlaybackTimer_ =
        static_cast<float>(targetTick - static_cast<std::int64_t>(commandStartTick_)) /
        static_cast<float>(tickRate);
    refreshCommandRenderSample(static_cast<float>(targetTick));
}

void ReplayStudio::resetCommandPlayback() {
    if (!commandLoaded_) {
        return;
    }
    commandPlaybackTimer_ = 0.0f;
    commandPlayback_.load(loadedCommandReplay_, commandTrack_);
    commandNextPlayback_.load(loadedCommandReplay_, commandTrack_);
    refreshCommandRenderSample(static_cast<float>(commandStartTick_));
}

void ReplayStudio::setCommandTrack(replay::ReplayPlaybackTrack track) {
    if (!commandLoaded_) {
        return;
    }

    if (track == replay::ReplayPlaybackTrack::Control &&
        !loadedCommandReplay_.header.hasControlLane) {
        statusMessage_ = "Command replay has no control track";
        setImmersiveStatus(statusMessage_);
        return;
    }

    if (commandTrack_ == track) {
        return;
    }

    commandTrack_ = track;
    commandPlayback_.load(loadedCommandReplay_, commandTrack_);
    commandNextPlayback_.load(loadedCommandReplay_, commandTrack_);
    updateCommandPlayback(0.0f);
    statusMessage_ = commandTrack_ == replay::ReplayPlaybackTrack::ServerTruth
        ? "Command replay track: Server Truth"
        : "Command replay track: Control Track";
    setImmersiveStatus(statusMessage_);
    ensureReplayFollowTarget();
}

void ReplayStudio::toggleCommandTrack() {
    const replay::ReplayPlaybackTrack nextTrack = commandTrack_ == replay::ReplayPlaybackTrack::ServerTruth
        ? replay::ReplayPlaybackTrack::Control
        : replay::ReplayPlaybackTrack::ServerTruth;
    setCommandTrack(nextTrack);
}

void ReplayStudio::stopCommandPlayback() {
    if (!commandLoaded_) {
        return;
    }
    commandPlaybackPlaying_ = false;
    resetCommandPlayback();
}

void ReplayStudio::toggleCommandInterpolation() {
    commandInterpolationEnabled_ = !commandInterpolationEnabled_;
    updateCommandPlayback(0.0f);
    setImmersiveStatus(commandInterpolationEnabled_
                           ? "Replay interpolation on"
                           : "Replay interpolation off");
}

void ReplayStudio::toggleCommandGhosts() {
    commandGhostsVisible_ = !commandGhostsVisible_;
    setImmersiveStatus(commandGhostsVisible_
                           ? "Replay ghosts shown"
                           : "Replay ghosts hidden");
}

RuntimeSettingsOverlay::State ReplayStudio::buildReplaySettingsOverlayState() const {
    RuntimeSettingsOverlay::State state;
    state.visible = replaySettingsVisible_;
    state.title = "Replay Settings";
    state.subtitle = "U closes | Replay-only controls";
    state.showTargetSections = false;
    state.leftSectionTitle = "Replay";

    RuntimeSettingsOverlay::ControlState interpolation;
    interpolation.id = RuntimeSettingsOverlay::ControlId::Interpolation;
    interpolation.type = RuntimeSettingsOverlay::ControlType::Toggle;
    interpolation.label = "Interpolation";
    interpolation.description = "Blend reconstructed command ticks for smoother playback.";
    interpolation.toggleValue = commandInterpolationEnabled_;
    state.leftControls.push_back(std::move(interpolation));

    RuntimeSettingsOverlay::ControlState ghosts;
    ghosts.id = RuntimeSettingsOverlay::ControlId::ReplayGhosts;
    ghosts.type = RuntimeSettingsOverlay::ControlType::Toggle;
    ghosts.label = "Prediction Ghosts";
    ghosts.description = "Show or hide control-lane ghost bodies in command replay.";
    ghosts.toggleValue = commandGhostsVisible_;
    state.leftControls.push_back(std::move(ghosts));

    RuntimeSettingsOverlay::ControlState replayTrack;
    replayTrack.id = RuntimeSettingsOverlay::ControlId::ReplayTrack;
    replayTrack.type = RuntimeSettingsOverlay::ControlType::Choice;
    replayTrack.label = "Replay Track";
    replayTrack.description = "Switch between server truth and the predicted control track.";
    replayTrack.valueLabel = commandTrack_ == replay::ReplayPlaybackTrack::Control
        ? "Control"
        : "Server";
    replayTrack.choices.push_back(RuntimeSettingsOverlay::ChoiceState{
        "Server",
        0,
        commandTrack_ == replay::ReplayPlaybackTrack::ServerTruth,
        true
    });
    replayTrack.choices.push_back(RuntimeSettingsOverlay::ChoiceState{
        "Control",
        1,
        commandTrack_ == replay::ReplayPlaybackTrack::Control,
        loadedCommandReplay_.header.hasControlLane
    });
    state.leftControls.push_back(std::move(replayTrack));

    return state;
}

void ReplayStudio::applyReplaySettingsAction(const RuntimeSettingsOverlay::Action& action) {
    switch (action.kind) {
        case RuntimeSettingsOverlay::Action::Kind::None:
            return;
        case RuntimeSettingsOverlay::Action::Kind::Close:
            replaySettingsVisible_ = false;
            replaySettingsOverlay_.resetInteraction();
            display::disableCursorForCapture();
            setImmersiveStatus("Replay settings hidden");
            return;
        case RuntimeSettingsOverlay::Action::Kind::ToggleChanged:
            switch (action.controlId) {
                case RuntimeSettingsOverlay::ControlId::Interpolation:
                    if (commandInterpolationEnabled_ != action.toggleValue) {
                        toggleCommandInterpolation();
                    }
                    return;
                case RuntimeSettingsOverlay::ControlId::ReplayGhosts:
                    if (commandGhostsVisible_ != action.toggleValue) {
                        toggleCommandGhosts();
                    }
                    return;
                default:
                    return;
            }
        case RuntimeSettingsOverlay::Action::Kind::ChoiceSelected:
            if (action.controlId == RuntimeSettingsOverlay::ControlId::ReplayTrack) {
                setCommandTrack(action.choiceValue == 1
                                    ? replay::ReplayPlaybackTrack::Control
                                    : replay::ReplayPlaybackTrack::ServerTruth);
            }
            return;
        case RuntimeSettingsOverlay::Action::Kind::SliderChanged:
        case RuntimeSettingsOverlay::Action::Kind::TargetSelected:
            return;
    }
}

void ReplayStudio::updateImmersiveCommandReplay(float dtSeconds) {
    if (!commandLoaded_) {
        exitCommandReplay();
        return;
    }

    InputHandler3D::InputState input = InputHandler3D::poll(replayControlBindings_);
    if (input.quit) {
        exitCommandReplay();
        return;
    }
    if (input.menuConfirm && display::releaseCursorIfCaptured()) {
        input.lookDelta = Vector2{0.0f, 0.0f};
        setImmersiveStatus("Mouse released");
    }

    if (input.toggleRecordingOverlay) {
        replayTransportOverlayVisible_ = !replayTransportOverlayVisible_;
        setImmersiveStatus(replayTransportOverlayVisible_
                               ? "Replay transport overlay shown"
                               : "Replay transport overlay hidden");
    }
    if (input.toggleUIMode || input.toggleUIPanel) {
        replaySettingsVisible_ = !replaySettingsVisible_;
        replaySettingsOverlay_.resetInteraction();
        if (replaySettingsVisible_) {
            replayTransportOverlayVisible_ = false;
            display::enableCursorPreservingPosition();
        } else {
            display::disableCursorForCapture();
        }
        setImmersiveStatus(replaySettingsVisible_
                               ? "Replay settings shown"
                               : "Replay settings hidden");
    }
    if (replaySettingsVisible_) {
        if (input.toggleInterp) {
            toggleCommandInterpolation();
        }
        if (input.spawnFrozenBotAhead || input.toggleFovCones) {
            toggleCommandGhosts();
        }
        const RuntimeSettingsOverlay::State settingsState = buildReplaySettingsOverlayState();
        for (const RuntimeSettingsOverlay::Action& action :
             replaySettingsOverlay_.handleMouse(settingsState)) {
            applyReplaySettingsAction(action);
        }
    }

    if (input.togglePlayback) {
        commandPlaybackPlaying_ = !commandPlaybackPlaying_;
        setImmersiveStatus(commandPlaybackPlaying_ ? "Replay playing" : "Replay paused");
    }
    if (input.resetPlayback) {
        stopCommandPlayback();
        setImmersiveStatus("Replay stopped");
    }
    if (input.stepBackward || input.menuLeft) {
        seekCommandPlaybackByTicks(-kSeekCommandTickCount);
    }
    if (input.stepForward || input.menuRight) {
        seekCommandPlaybackByTicks(kSeekCommandTickCount);
    }
    if (input.toggleTickInfo) {
        toggleCommandTrack();
    }
    if (input.toggleView) {
        cycleReplayFollowTarget();
    }
    if (input.toggleSplitMode || input.toggleRealMode) {
        cycleReplayFollowCamera();
    }
    if (input.toggleSpectator) {
        toggleReplaySpectatorMode();
    }

    updateCommandPlayback(dtSeconds);
    ensureReplayFollowTarget();

    if (replayCameraMode_ == ReplayCameraMode::SpectatorFreeFly) {
        if (input.addSpectatorCheckpoint) {
            createReplayCheckpoint();
        }
        if (input.prevSpectatorCheckpoint && !replayCheckpoints_.empty()) {
            beginReplayCheckpointTransition(
                app::CheckpointStore::cyclePreviousCheckpoint(
                    replayCheckpoints_, currentReplayCheckpoint_));
        }
        if (input.nextSpectatorCheckpoint && !replayCheckpoints_.empty()) {
            beginReplayCheckpointTransition(
                app::CheckpointStore::cycleNextCheckpoint(
                    replayCheckpoints_, currentReplayCheckpoint_));
        }
        if (input.deleteSpectatorCheckpoint && !replayCheckpoints_.empty()) {
            currentReplayCheckpoint_ = app::CheckpointStore::deleteCheckpoint(
                replayCheckpoints_, currentReplayCheckpoint_);
            replayCheckpointTransitionActive_ = false;
            if (currentReplayCheckpoint_ >= 0 &&
                currentReplayCheckpoint_ < static_cast<int>(replayCheckpoints_.size())) {
                replaySpectatorCamera_.setState(replayCheckpoints_[currentReplayCheckpoint_]);
            }
            saveReplayCheckpoints();
            setImmersiveStatus(replayCheckpoints_.empty()
                                   ? "Checkpoint deleted"
                                   : "Checkpoint deleted, moved to next");
        }
        if (input.decreaseSpectatorTransitionDuration) {
            adjustReplayCheckpointTransition(-kCheckpointTransitionAdjustmentSeconds);
        }
        if (input.increaseSpectatorTransitionDuration) {
            adjustReplayCheckpointTransition(kCheckpointTransitionAdjustmentSeconds);
        }

        if (replayCheckpointTransitionActive_) {
            updateReplayCheckpointTransition(dtSeconds);
        } else if (!replaySettingsVisible_ && display::isCursorCaptured()) {
            replaySpectatorCamera_.update(input.moveInput,
                                          input.moveUp,
                                          input.moveDown,
                                          input.fastModifier,
                                          input.lookDelta,
                                          dtSeconds);
        }
    } else if (input.addSpectatorCheckpoint ||
               input.prevSpectatorCheckpoint ||
               input.nextSpectatorCheckpoint ||
               input.deleteSpectatorCheckpoint) {
        setImmersiveStatus("Checkpoint controls need spectator mode");
    }

    if (immersiveStatusTimer_ > 0.0f) {
        immersiveStatusTimer_ = std::max(0.0f, immersiveStatusTimer_ - dtSeconds);
    }
}

void ReplayStudio::cycleReplayFollowTarget() {
    const std::vector<int> actorIds = commandReplayActorIds();
    if (actorIds.empty()) {
        replayFollowActorId_ = -1;
        replayCameraMode_ = ReplayCameraMode::SpectatorFreeFly;
        setImmersiveStatus("No player targets in replay");
        return;
    }

    auto current = std::find(actorIds.begin(), actorIds.end(), replayFollowActorId_);
    if (current == actorIds.end()) {
        replayFollowActorId_ = actorIds.front();
    } else {
        ++current;
        replayFollowActorId_ = current == actorIds.end() ? actorIds.front() : *current;
    }

    if (replayCameraMode_ == ReplayCameraMode::SpectatorFreeFly) {
        replayCameraMode_ = ReplayCameraMode::FollowFirstPerson;
    }
    setImmersiveStatus("Following player " + std::to_string(replayFollowActorId_));
}

void ReplayStudio::cycleReplayFollowCamera() {
    if (replayCameraMode_ == ReplayCameraMode::SpectatorFreeFly) {
        replayCameraMode_ = ReplayCameraMode::FollowFirstPerson;
    } else if (replayCameraMode_ == ReplayCameraMode::FollowFirstPerson) {
        replayCameraMode_ = ReplayCameraMode::FollowThirdPerson;
    } else {
        replayCameraMode_ = ReplayCameraMode::FollowFirstPerson;
    }
    replayCheckpointTransitionActive_ = false;
    ensureReplayFollowTarget();
    setImmersiveStatus(replayCameraModeLabel());
}

void ReplayStudio::toggleReplaySpectatorMode() {
    if (replayCameraMode_ == ReplayCameraMode::SpectatorFreeFly) {
        replayCameraMode_ = ReplayCameraMode::FollowFirstPerson;
        replayCheckpointTransitionActive_ = false;
        ensureReplayFollowTarget();
        setImmersiveStatus("Spectator mode off");
        return;
    }

    replaySpectatorCamera_.resetFromCamera(activeCommandReplayCamera());
    replayCameraMode_ = ReplayCameraMode::SpectatorFreeFly;
    replayCheckpointTransitionActive_ = false;
    setImmersiveStatus("Spectator mode on");
}

void ReplayStudio::ensureReplayFollowTarget() {
    if (commandReplayPlayer(replayFollowActorId_) != nullptr) {
        return;
    }

    const std::vector<int> actorIds = commandReplayActorIds();
    replayFollowActorId_ = actorIds.empty() ? -1 : actorIds.front();
}

std::vector<int> ReplayStudio::commandReplayActorIds() const {
    std::vector<int> actorIds;
    auto pushUnique = [&actorIds](int actorId) {
        if (std::find(actorIds.begin(), actorIds.end(), actorId) == actorIds.end()) {
            actorIds.push_back(actorId);
        }
    };

    for (const sim::PlayerState& player : commandRenderWorld().players) {
        pushUnique(player.playerId);
    }
    for (const sim::PlayerState& player : commandRenderControlPlayers()) {
        pushUnique(player.playerId);
    }
    return actorIds;
}

const sim::PlayerState* ReplayStudio::commandReplayPlayer(int actorId) const {
    if (actorId < 0) {
        return nullptr;
    }

    auto findPlayer = [actorId](const std::vector<sim::PlayerState>& players)
        -> const sim::PlayerState* {
        const auto it = std::find_if(players.begin(),
                                     players.end(),
                                     [actorId](const sim::PlayerState& player) {
                                         return player.playerId == actorId;
                                     });
        return it == players.end() ? nullptr : &(*it);
    };

    if (commandTrack_ == replay::ReplayPlaybackTrack::Control) {
        if (const sim::PlayerState* player = findPlayer(commandRenderControlPlayers());
            player != nullptr) {
            return player;
        }
    }

    if (const sim::PlayerState* player = findPlayer(commandRenderWorld().players);
        player != nullptr) {
        return player;
    }
    return findPlayer(commandRenderControlPlayers());
}

const sim::PlayerState* ReplayStudio::activeCommandReplayPlayer() const {
    if (const sim::PlayerState* player = commandReplayPlayer(replayFollowActorId_);
        player != nullptr) {
        return player;
    }

    const std::vector<int> actorIds = commandReplayActorIds();
    return actorIds.empty() ? nullptr : commandReplayPlayer(actorIds.front());
}

const sim::WorldState& ReplayStudio::commandRenderWorld() const {
    return commandInterpolationEnabled_
        ? interpolatedCommandWorld_
        : commandPlayback_.worldState();
}

const std::vector<sim::PlayerState>& ReplayStudio::commandRenderControlPlayers() const {
    return commandInterpolationEnabled_
        ? interpolatedControlPlayers_
        : commandPlayback_.controlPlayers();
}

Camera3D ReplayStudio::activeCommandReplayCamera() const {
    if (replayCameraMode_ == ReplayCameraMode::SpectatorFreeFly) {
        return replaySpectatorCamera_.getCamera();
    }

    const sim::PlayerState* player = activeCommandReplayPlayer();
    if (player == nullptr) {
        return cameraForWorld(commandRenderWorld(), commandRenderControlPlayers());
    }

    const Vector3 eyePosition = toVector3(player->position);
    if (replayCameraMode_ == ReplayCameraMode::FollowFirstPerson) {
        return SpectatorCamera::followFirstPersonCamera(eyePosition,
                                                        player->yaw,
                                                        player->pitch);
    }

    return SpectatorCamera::followThirdPersonCamera(
        arena_.playerRenderRootFromEyePosition(player->position),
        player->yaw,
        player->pitch,
        kReplayThirdPersonZoom);
}

void ReplayStudio::loadReplayCheckpoints() {
    replayCheckpoints_.clear();
    currentReplayCheckpoint_ = -1;
    replayCheckpointTransitionActive_ = false;

    app::CheckpointLoadResult loaded = replayCheckpointStore_.load();
    if (!loaded.loaded || loaded.checkpoints.empty()) {
        return;
    }

    replayCheckpoints_ = std::move(loaded.checkpoints);
    currentReplayCheckpoint_ = app::CheckpointStore::initialCheckpointIndex(replayCheckpoints_);
    if (loaded.migratedLegacy) {
        saveReplayCheckpoints();
    }
}

void ReplayStudio::saveReplayCheckpoints() const {
    replayCheckpointStore_.save(replayCheckpoints_);
}

void ReplayStudio::createReplayCheckpoint() {
    SpectatorCamera::Checkpoint checkpoint = replaySpectatorCamera_.getState();
    checkpoint.transitionDurationSeconds = SpectatorCamera::kDefaultCheckpointTransitionSeconds;
    const int index = app::CheckpointStore::createCheckpoint(replayCheckpoints_, checkpoint);
    saveReplayCheckpoints();
    beginReplayCheckpointTransition(index);
}

void ReplayStudio::beginReplayCheckpointTransition(int targetIndex) {
    if (targetIndex < 0 ||
        targetIndex >= static_cast<int>(replayCheckpoints_.size())) {
        return;
    }

    const int originIndex =
        (currentReplayCheckpoint_ >= 0 &&
         currentReplayCheckpoint_ < static_cast<int>(replayCheckpoints_.size()))
            ? currentReplayCheckpoint_
            : -1;

    replayCheckpointTransitionStart_ = replaySpectatorCamera_.getState();
    replayCheckpointTransitionEnd_ = replayCheckpoints_[targetIndex];
    if (originIndex >= 0 &&
        ((originIndex + 1) % static_cast<int>(replayCheckpoints_.size())) == targetIndex) {
        replayCheckpointTransitionDuration_ =
            std::max(0.0f, replayCheckpoints_[originIndex].transitionDurationSeconds);
    } else {
        replayCheckpointTransitionDuration_ =
            std::max(0.0f, replayCheckpoints_[targetIndex].transitionDurationSeconds);
    }
    replayCheckpointTransitionTimer_ = 0.0f;
    replayCheckpointTransitionActive_ = true;
    currentReplayCheckpoint_ = targetIndex;
    setImmersiveStatus("Checkpoint " + std::to_string(targetIndex + 1) +
                       "/" + std::to_string(replayCheckpoints_.size()));
}

void ReplayStudio::updateReplayCheckpointTransition(float dtSeconds) {
    if (!replayCheckpointTransitionActive_ || replayCheckpoints_.empty()) {
        return;
    }

    replayCheckpointTransitionTimer_ += std::max(0.0f, dtSeconds);
    const float t = std::clamp(
        replayCheckpointTransitionDuration_ > 0.0f
            ? replayCheckpointTransitionTimer_ / replayCheckpointTransitionDuration_
            : 1.0f,
        0.0f,
        1.0f);
    const float eased = t * t * (3.0f - (2.0f * t));

    SpectatorCamera::Checkpoint interpolated{};
    interpolated.position = Vector3Lerp(replayCheckpointTransitionStart_.position,
                                        replayCheckpointTransitionEnd_.position,
                                        eased);
    interpolated.yaw = replayCheckpointTransitionStart_.yaw +
                       (replayCheckpointTransitionEnd_.yaw -
                        replayCheckpointTransitionStart_.yaw) * eased;
    interpolated.pitch = replayCheckpointTransitionStart_.pitch +
                         (replayCheckpointTransitionEnd_.pitch -
                          replayCheckpointTransitionStart_.pitch) * eased;
    replaySpectatorCamera_.setState(interpolated);

    if (replayCheckpointTransitionTimer_ >= replayCheckpointTransitionDuration_) {
        replayCheckpointTransitionActive_ = false;
        replaySpectatorCamera_.setState(replayCheckpointTransitionEnd_);
    }
}

void ReplayStudio::adjustReplayCheckpointTransition(float deltaSeconds) {
    if (currentReplayCheckpoint_ < 0 ||
        currentReplayCheckpoint_ >= static_cast<int>(replayCheckpoints_.size()) ||
        replayCheckpoints_.size() < 2u) {
        return;
    }

    SpectatorCamera::Checkpoint& checkpoint =
        replayCheckpoints_[currentReplayCheckpoint_];
    checkpoint.transitionDurationSeconds =
        std::max(0.0f, checkpoint.transitionDurationSeconds + deltaSeconds);
    saveReplayCheckpoints();

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2)
           << "Next transition " << checkpoint.transitionDurationSeconds << "s";
    setImmersiveStatus(stream.str());
}

void ReplayStudio::setImmersiveStatus(std::string message) {
    immersiveStatusMessage_ = std::move(message);
    immersiveStatusTimer_ = kReplayStatusSeconds;
}

const char* ReplayStudio::replayCameraModeLabel() const {
    switch (replayCameraMode_) {
        case ReplayCameraMode::FollowFirstPerson:
            return "Player first-person";
        case ReplayCameraMode::FollowThirdPerson:
            return "Player third-person";
        case ReplayCameraMode::SpectatorFreeFly:
            return "Spectator free-fly";
    }
    return "Player first-person";
}

void ReplayStudio::updateRows() {
    rows_.clear();
    rows_.reserve(libraryEntries_.size());
    for (std::size_t index = 0u; index < libraryEntries_.size(); ++index) {
        rows_.push_back(LibraryRow{
            Rectangle{
                kPanelX + 18.0f,
                kPanelY + 104.0f + static_cast<float>(index) * (kRowHeight + kRowGap),
                kPanelWidth - 36.0f,
                kRowHeight
            },
            false
        });
    }
}

void ReplayStudio::renderLibrary() const {
    const Rectangle panel{kPanelX, kPanelY, kPanelWidth, 760.0f};
    DrawRectangleRounded(panel, 0.04f, 10, panelColor());
    DrawRectangleRoundedLines(panel, 0.04f, 10, Fade(WHITE, 0.18f));

    drawText(TypographyStyleId::ScreenTitle,
             "Replay Studio",
             Vector2{panel.x + 24.0f, panel.y + 22.0f},
             SKYBLUE);
    drawText(TypographyStyleId::Body,
             statusMessage_,
             Vector2{panel.x + 26.0f, panel.y + 70.0f},
             Fade(WHITE, 0.74f));

    if (libraryEntries_.empty()) {
        drawCentered(TypographyStyleId::AppSubtitle,
                     "No saved recordings",
                     panel.x + panel.width * 0.5f,
                     panel.y + 290.0f,
                     Fade(WHITE, 0.64f));
        drawCentered(TypographyStyleId::Body,
                     "Record a session, then press 9 to export it",
                     panel.x + panel.width * 0.5f,
                     panel.y + 340.0f,
                     Fade(WHITE, 0.54f));
        return;
    }

    const std::size_t visibleCount = std::min<std::size_t>(libraryEntries_.size(), rows_.size());
    for (std::size_t index = 0u; index < visibleCount; ++index) {
        const LibraryEntry& entry = libraryEntries_[index];
        const LibraryRow& row = rows_[index];
        const bool selected = static_cast<int>(index) == selectedIndex_;
        const bool highlighted = selected || row.hovered;
        DrawRectangleRounded(row.bounds,
                             0.08f,
                             8,
                             highlighted ? Color{35, 58, 82, 255} : Color{22, 30, 44, 255});
        DrawRectangleRoundedLines(row.bounds, 0.08f, 8, panelBorderColor(highlighted));

        std::string title = entry.kind == EntryKind::Command
            ? (entry.commandHeader.title.empty()
                   ? entry.path.stem().string()
                   : entry.commandHeader.title)
            : (entry.legacyMetadata.title.empty()
                   ? entry.path.stem().string()
                   : entry.legacyMetadata.title);
        if (title.size() > 32u) {
            title = title.substr(0u, 29u) + "...";
        }
        drawText(TypographyStyleId::OverlayTitle,
                 truncateText(title, 32u),
                 Vector2{row.bounds.x + 16.0f, row.bounds.y + 10.0f},
                 WHITE);

        const std::uint64_t created = entry.kind == EntryKind::Command
            ? entry.commandHeader.recordedAtUnixSeconds
            : entry.legacyMetadata.createdUnixSeconds;
        const std::string kindLabel = entry.kind == EntryKind::Command
            ? "Command"
            : "Legacy";
        const std::string countLabel = entry.kind == EntryKind::Command
            ? std::to_string(entry.commandCount) + " commands"
            : std::to_string(entry.frameCount) + " frames";
        const std::string detail =
            kindLabel + " | " + formatTimestamp(created) +
            " | " + countLabel +
            " | " + formatFileSize(entry.fileSizeBytes);
        drawText(TypographyStyleId::Caption,
                 truncateText(entry.mapLabel, 54u),
                 Vector2{row.bounds.x + 16.0f, row.bounds.y + 40.0f},
                 Color{148, 205, 218, 255});
        drawText(TypographyStyleId::Caption,
                 truncateText(detail, 58u),
                 Vector2{row.bounds.x + 16.0f, row.bounds.y + 61.0f},
                 Fade(WHITE, 0.68f));
    }

    drawButton(libraryRenameButtonRect(), "Rename", false, selectedReplayAvailable());
    drawButton(libraryDeleteButtonRect(), "Delete", false, selectedReplayAvailable());
    drawButton(libraryOpenButtonRect(), "Open Replay", false, selectedReplayAvailable());

    if (renameDialogVisible_) {
        DrawRectangle(0, 0, Config::SCREEN_WIDTH, Config::SCREEN_HEIGHT, Fade(BLACK, 0.42f));
        const Rectangle dialog = dialogRect();
        DrawRectangleRounded(dialog, 0.045f, 16, Color{14, 18, 26, 250});
        DrawRectangleRoundedLines(dialog, 0.045f, 16, Color{92, 124, 182, 255});
        drawText(TypographyStyleId::OverlayTitle,
                 "Rename Replay",
                 Vector2{dialog.x + 24.0f, dialog.y + 20.0f},
                 WHITE);
        const Rectangle input{dialog.x + 24.0f, dialog.y + 82.0f, dialog.width - 48.0f, 54.0f};
        DrawRectangleRounded(input, 0.12f, 8, Color{28, 36, 52, 255});
        DrawRectangleRoundedLines(input, 0.12f, 8, Color{92, 124, 182, 255});
        const std::string visibleTitle = truncateText(renameText_, 58u);
        drawText(TypographyStyleId::Body,
                 visibleTitle + "_",
                 Vector2{input.x + 16.0f, input.y + 16.0f},
                 WHITE);
        drawButton(dialogPrimaryButtonRect(dialog), "Save", false, true);
        drawButton(dialogSecondaryButtonRect(dialog), "Cancel", false, true);
    } else if (deleteConfirmVisible_) {
        DrawRectangle(0, 0, Config::SCREEN_WIDTH, Config::SCREEN_HEIGHT, Fade(BLACK, 0.42f));
        const Rectangle dialog = dialogRect(600.0f, 220.0f);
        DrawRectangleRounded(dialog, 0.045f, 16, Color{14, 18, 26, 250});
        DrawRectangleRoundedLines(dialog, 0.045f, 16, Color{144, 78, 78, 255});
        drawText(TypographyStyleId::OverlayTitle,
                 "Delete Replay",
                 Vector2{dialog.x + 24.0f, dialog.y + 20.0f},
                 WHITE);
        drawText(TypographyStyleId::Body,
                 truncateText(selectedReplayTitle(), 44u),
                 Vector2{dialog.x + 24.0f, dialog.y + 78.0f},
                 Fade(WHITE, 0.82f));
        drawText(TypographyStyleId::Caption,
                 "This removes the replay file from disk.",
                 Vector2{dialog.x + 24.0f, dialog.y + 112.0f},
                 Fade(WHITE, 0.62f));
        drawButton(dialogPrimaryButtonRect(dialog), "Delete", false, true);
        drawButton(dialogSecondaryButtonRect(dialog), "Cancel", false, true);
    }
}

void ReplayStudio::renderLoadedReplay() const {
    if (commandLoaded_) {
        renderCommandWorld();
        return;
    }

    const client::RecordedReplayFrame* frame = activeFrame();
    if (frame == nullptr) {
        DrawRectangleGradientV(0,
                               0,
                               Config::SCREEN_WIDTH,
                               Config::SCREEN_HEIGHT,
                               Color{10, 13, 19, 255},
                               Color{14, 19, 30, 255});
        drawCentered(TypographyStyleId::ScreenTitle,
                     "Replay Studio",
                     static_cast<float>(Config::SCREEN_WIDTH) * 0.65f,
                     255.0f,
                     Fade(SKYBLUE, 0.68f));
        return;
    }

    renderFrameWorld(frame->frame);
}

void ReplayStudio::renderCommandWorld() const {
    if (!commandLoaded_) {
        return;
    }

    const sim::WorldState& world = commandRenderWorld();
    const std::vector<sim::PlayerState>& controlPlayers = commandRenderControlPlayers();
    const Camera3D camera = cameraForWorld(world, controlPlayers);
    renderCommandWorld(camera, true);
}

void ReplayStudio::renderCommandWorld(const Camera3D& camera, bool drawStudioOverlay) const {
    if (!commandLoaded_) {
        return;
    }

    const sim::WorldState& world = commandRenderWorld();
    const std::vector<sim::PlayerState>& controlPlayers = commandRenderControlPlayers();
    const bool hideFollowedFirstPerson =
        !drawStudioOverlay &&
        replayCameraMode_ == ReplayCameraMode::FollowFirstPerson &&
        replayFollowActorId_ >= 0;

    BeginMode3D(camera);
    arena_.render(client::ArenaRenderLayer{true, 0.0f});

    for (const sim::RemoteActorState& enemy : world.enemies) {
        client::RemoteEnemyRenderItem item;
        item.entityId = enemy.entityId;
        item.displayPosition = toVector3(enemy.position);
        item.yawRadians = enemy.yaw;
        item.healthPercent = enemy.radius > 0.0f
            ? std::clamp(enemy.health / std::max(1.0f, sim::defaults::kEnemyMaxHealth), 0.0f, 1.0f)
            : 0.0f;
        item.alive = enemy.alive;
        item.tint = enemy.alive ? Color{255, 39, 104, 255} : Color{120, 120, 120, 180};
        Enemy3D enemyVisual(std::shared_ptr<Model3DWrapper>{});
        enemyVisual.render(item);
        enemyVisual.renderHealthBar(item, camera, 1.0f);
    }

    for (const sim::PlayerState& player : world.players) {
        if (hideFollowedFirstPerson && player.playerId == replayFollowActorId_) {
            continue;
        }
        client::RemotePlayerRenderItem item;
        item.actorId = player.playerId;
        item.eyePosition = toVector3(player.position);
        item.rootPosition = arena_.playerRenderRootFromEyePosition(player.position);
        item.yawRadians = player.yaw;
        item.pitchRadians = player.pitch;
        item.healthPercent = player.maxHealth > 0.0f
            ? std::clamp(player.health / player.maxHealth, 0.0f, 1.0f)
            : 0.0f;
        item.alive = player.health > 0.0f;
        item.team = teamForActor(world, player.playerId);
        item.tint = teamTint(item.team);
        Player3D playerVisual(std::shared_ptr<Model3DWrapper>{});
        playerVisual.render(item);
        playerVisual.renderHealthBar(item, camera, 1.0f);
    }

    if (commandTrack_ == replay::ReplayPlaybackTrack::Control && commandGhostsVisible_) {
        for (const sim::PlayerState& player : controlPlayers) {
            if (hideFollowedFirstPerson && player.playerId == replayFollowActorId_) {
                continue;
            }
            client::RemotePlayerRenderItem item;
            item.actorId = player.playerId;
            item.eyePosition = toVector3(player.position);
            item.rootPosition = arena_.playerRenderRootFromEyePosition(player.position);
            item.yawRadians = player.yaw;
            item.pitchRadians = player.pitch;
            item.healthPercent = player.maxHealth > 0.0f
                ? std::clamp(player.health / player.maxHealth, 0.0f, 1.0f)
                : 0.0f;
            item.alive = player.health > 0.0f;
            item.team = teamForActor(world, player.playerId);
            item.tint = teamTint(item.team, true);
            item.ghost = true;
            Player3D playerVisual(std::shared_ptr<Model3DWrapper>{});
            playerVisual.render(item);
            playerVisual.renderHealthBar(item, camera, 1.0f);
        }
    }

    EndMode3D();

    if (!drawStudioOverlay) {
        return;
    }

    const char* trackLabel = commandTrack_ == replay::ReplayPlaybackTrack::ServerTruth
        ? "Server Truth"
        : "Control Track";
    drawText(TypographyStyleId::OverlayTitle,
             trackLabel,
             Vector2{580.0f, 102.0f},
             SKYBLUE);
    drawText(TypographyStyleId::Body,
             "Command replay (.nlcmd) | T switches track",
             Vector2{580.0f, 136.0f},
             Fade(WHITE, 0.74f));
}

void ReplayStudio::renderImmersiveCommandReplay() const {
    if (!commandLoaded_) {
        return;
    }

    const Camera3D camera = activeCommandReplayCamera();
    renderCommandWorld(camera, false);
    renderImmersiveOverlay(camera);
}

void ReplayStudio::renderImmersiveOverlay(const Camera3D& camera) const {
    (void)camera;

    if (replayTransportOverlayVisible_) {
        renderReplayTransportOverlay();
    }
    if (replaySettingsVisible_) {
        renderReplaySettingsOverlay();
    }

    if (immersiveStatusTimer_ > 0.0f && !immersiveStatusMessage_.empty()) {
        const int width = textWidth(TypographyStyleId::Body, immersiveStatusMessage_);
        DrawRectangle(static_cast<int>(Config::SCREEN_WIDTH * 0.5f - width * 0.5f - 18.0f),
                      Config::SCREEN_HEIGHT - 76,
                      width + 36,
                      42,
                      Fade(Color{8, 10, 15, 255}, 0.58f));
        drawCentered(TypographyStyleId::Body,
                     immersiveStatusMessage_,
                     static_cast<float>(Config::SCREEN_WIDTH) * 0.5f,
                     static_cast<float>(Config::SCREEN_HEIGHT - 64),
                     WHITE);
    }
}

void ReplayStudio::renderReplayTransportOverlay() const {
    client::ReplayTransportOverlayState overlay;
    overlay.primaryLine = "Command Replay";
    overlay.secondaryLine =
        formatDuration(commandPlaybackTimer_) + " / " + formatDuration(commandDurationSeconds());
    const bool spectatorActive = replayCameraMode_ == ReplayCameraMode::SpectatorFreeFly;
    const bool checkpointSelected =
        currentReplayCheckpoint_ >= 0 &&
        currentReplayCheckpoint_ < static_cast<int>(replayCheckpoints_.size());
    const float transitionSeconds = checkpointSelected
        ? std::max(0.0f, replayCheckpoints_[currentReplayCheckpoint_].transitionDurationSeconds)
        : SpectatorCamera::kDefaultCheckpointTransitionSeconds;
    overlay.checkpoint = client::ReplayCheckpointOverlayState{
        true,
        spectatorActive,
        currentReplayCheckpoint_,
        replayCheckpoints_.size(),
        transitionSeconds,
        spectatorActive && checkpointSelected && replayCheckpoints_.size() > 1u
    };

    const auto buttons = client::makeReplayPlaybackTransportButtons(
        commandPlaybackPlaying_,
        !commandPlaybackPlaying_ && commandPlaybackTimer_ <= 0.001f);
    client::renderReplayTransportOverlay(overlay, buttons);
}

void ReplayStudio::renderReplaySettingsOverlay() const {
    replaySettingsOverlay_.render(buildReplaySettingsOverlayState());
}

void ReplayStudio::renderFrameWorld(const client::RenderFrame& frame) const {
    if (!frame.hasSnapshot) {
        drawText(TypographyStyleId::ScreenTitle,
                 frame.waiting.title.empty() ? "Replay" : frame.waiting.title,
                 Vector2{580.0f, 120.0f},
                 SKYBLUE);
        drawText(TypographyStyleId::AppSubtitle,
                 frame.waiting.statusMessage,
                 Vector2{580.0f, 178.0f},
                 LIGHTGRAY);
        return;
    }

    BeginMode3D(frame.camera);
    arena_.render(frame.arena);
    for (const LaserBeam3D& trace : frame.combatTraces) {
        trace.render();
    }
    for (const client::RemoteEnemyRenderItem& enemyItem : frame.remoteEnemies) {
        Enemy3D enemyVisual(std::shared_ptr<Model3DWrapper>{});
        enemyVisual.render(enemyItem);
        enemyVisual.renderHealthBar(enemyItem, frame.camera, 1.0f);
    }
    for (const client::RemotePlayerRenderItem& playerItem : frame.remotePlayers) {
        Player3D playerVisual(std::shared_ptr<Model3DWrapper>{});
        playerVisual.render(playerItem);
        playerVisual.renderHealthBar(playerItem, frame.camera, 1.0f);
    }
    for (const client::RemotePlayerRenderItem& playerItem : frame.remotePlayerGhosts) {
        Player3D playerVisual(std::shared_ptr<Model3DWrapper>{});
        playerVisual.render(playerItem);
        playerVisual.renderHealthBar(playerItem, frame.camera, 1.0f);
    }
    EndMode3D();

    client::HudOverlayRenderer::renderKillFeed(frame.killFeed);
    client::HudOverlayRenderer::renderCompactScore(frame.compactScore);
    client::HudOverlayRenderer::renderScoreboard(frame.scoreboard);

    if (!frame.hud.lines.empty()) {
        float y = static_cast<float>(Config::SCREEN_HEIGHT - 48);
        for (std::size_t index = 0; index < frame.hud.lines.size(); ++index) {
            const std::string& line = frame.hud.lines[frame.hud.lines.size() - 1u - index];
            drawText(TypographyStyleId::Body,
                     line,
                     Vector2{580.0f, y},
                     line.find("Down |") != std::string::npos ? ORANGE : LIGHTGRAY);
            y -= 32.0f;
            if (y < static_cast<float>(Config::SCREEN_HEIGHT - 176)) {
                break;
            }
        }
    }
}

void ReplayStudio::renderTransport() const {
    const Rectangle panel{560.0f,
                          static_cast<float>(Config::SCREEN_HEIGHT - 132),
                          static_cast<float>(Config::SCREEN_WIDTH - 600),
                          92.0f};
    DrawRectangleRounded(panel, 0.06f, 10, Fade(Color{12, 16, 24, 255}, 0.84f));
    DrawRectangleRoundedLines(panel, 0.06f, 10, Fade(WHITE, 0.16f));

    const bool canPlay = commandLoaded_ || (loaded_ && !loadedRecording_.frames.empty());
    const std::string title = commandLoaded_
        ? (loadedCommandReplay_.header.title.empty()
               ? "Command Replay"
               : loadedCommandReplay_.header.title)
        : (loaded_ ? loadedRecording_.metadata.title : "Select a saved replay");
    drawText(TypographyStyleId::OverlayTitle,
             title,
             Vector2{panel.x + 22.0f, panel.y + 16.0f},
             canPlay ? WHITE : Fade(WHITE, 0.58f));

    std::string timeLine = "--";
    if (commandLoaded_) {
        timeLine = formatDuration(commandPlaybackTimer_) + " / " +
                   formatDuration(commandDurationSeconds());
    } else if (canPlay) {
        timeLine = formatDuration(timeline_.playbackTimer) + " / " +
                   formatDuration(loadedDurationSeconds());
    }
    const int timeWidth = textWidth(TypographyStyleId::Body, timeLine);
    drawText(TypographyStyleId::Body,
             timeLine,
             Vector2{panel.x + panel.width - 22.0f - static_cast<float>(timeWidth),
                     panel.y + 22.0f},
             Fade(WHITE, 0.76f));

    const float buttonY = panel.y + 50.0f;
    drawButton(Rectangle{panel.x + 22.0f, buttonY, 96.0f, 30.0f},
               "7",
               false,
               canPlay);
    drawButton(Rectangle{panel.x + 130.0f, buttonY, 96.0f, 30.0f},
               (commandLoaded_ ? commandPlaybackPlaying_ : timeline_.playbackPlaying) ? "Pause" : "Play",
               commandLoaded_ ? commandPlaybackPlaying_ : timeline_.playbackPlaying,
               canPlay);
    drawButton(Rectangle{panel.x + 238.0f, buttonY, 96.0f, 30.0f},
               "8",
               false,
               canPlay);
    drawButton(Rectangle{panel.x + 346.0f, buttonY, 96.0f, 30.0f},
               "0",
               false,
               canPlay);
    if (commandLoaded_) {
        drawButton(Rectangle{panel.x + 454.0f, buttonY, 132.0f, 30.0f},
                   commandTrack_ == replay::ReplayPlaybackTrack::ServerTruth ? "Server" : "Control",
                   true,
                   canPlay);
    }
}

std::string ReplayStudio::formatTimestamp(std::uint64_t unixSeconds) {
    if (unixSeconds == 0u) {
        return "unknown date";
    }
    const std::time_t timestamp = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &timestamp);
#else
    if (const std::tm* local = std::localtime(&timestamp); local != nullptr) {
        tm = *local;
    }
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%b %d %H:%M");
    return stream.str();
}

std::string ReplayStudio::formatDuration(float seconds) {
    const int clamped = std::max(0, static_cast<int>(std::round(seconds)));
    const int minutes = clamped / 60;
    const int secs = clamped % 60;
    std::ostringstream stream;
    stream << minutes << ":"
           << std::setw(2)
           << std::setfill('0')
           << secs;
    return stream.str();
}

std::string ReplayStudio::formatFileSize(std::uintmax_t bytes) {
    std::ostringstream stream;
    if (bytes >= 1'000'000u) {
        stream << std::fixed << std::setprecision(1)
               << static_cast<double>(bytes) / 1'000'000.0 << " MB";
    } else if (bytes >= 1'000u) {
        stream << std::fixed << std::setprecision(1)
               << static_cast<double>(bytes) / 1'000.0 << " KB";
    } else {
        stream << bytes << " B";
    }
    return stream.str();
}

std::string ReplayStudio::formatMapLabel(int levelSlot, std::uint32_t levelHash) {
    (void)levelHash;
    if (levelSlot <= 0) {
        return "Map: Unknown";
    }

    LevelData::LevelDefinition level;
    if (LevelData::loadLevel(level, levelSlot)) {
        return "Map: " + level.name + " (slot " + std::to_string(levelSlot) + ")";
    }
    return "Map: Slot " + std::to_string(levelSlot);
}
