#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

#include <raylib.h>

#include "Arena3D.hpp"
#include "Enemy3D.hpp"
#include "InputHandler3D.hpp"
#include "MainMenu.hpp"
#include "Player3D.hpp"
#include "RuntimeSettingsOverlay.hpp"
#include "SpectatorCamera.hpp"
#include "TypographyService.hpp"
#include "app/CheckpointStore.hpp"
#include "app/ReplayArchive.hpp"
#include "app/ReplayTimeline.hpp"
#include "input/ControlBindings.hpp"
#include "client/ReplayRecording.hpp"
#include "replay/ReplayArchive.hpp"
#include "replay/ReplayPlaybackRuntime.hpp"

class ReplayStudio {
public:
    ReplayStudio();

    GameMode update(float dtSeconds, bool allowBackShortcut = true);
    void render() const;

    const std::vector<app::ReplayArchiveEntry>& entriesForTest() const;
    bool hasLoadedRecordingForTest() const;

private:
    enum class EntryKind {
        Command,
        LegacyFrame
    };

    enum class ReplayCameraMode {
        FollowFirstPerson,
        FollowThirdPerson,
        SpectatorFreeFly
    };

    struct LibraryEntry {
        EntryKind kind{EntryKind::Command};
        std::filesystem::path path{};
        client::ReplayRecordingMetadata legacyMetadata{};
        replay::ReplayHeader commandHeader{};
        std::string mapLabel{};
        std::size_t frameCount{0u};
        std::size_t commandCount{0u};
        std::uintmax_t fileSizeBytes{0u};
    };

    struct LibraryRow {
        Rectangle bounds{};
        bool hovered{false};
    };

    app::ReplayArchive archive_{};
    replay::ReplayArchive commandArchive_{};
    std::vector<LibraryEntry> libraryEntries_{};
    std::vector<app::ReplayArchiveEntry> legacyEntriesForTest_{};
    std::vector<LibraryRow> rows_{};
    client::ReplayRecording loadedRecording_{};
    replay::ReplayDemo loadedCommandReplay_{};
    replay::ReplayPlaybackRuntime commandPlayback_{};
    replay::ReplayPlaybackRuntime commandNextPlayback_{};
    sim::WorldState interpolatedCommandWorld_{};
    std::vector<sim::PlayerState> interpolatedControlPlayers_{};
    app::ReplayTimeline timeline_{};
    app::CheckpointStore replayCheckpointStore_{app::CheckpointCollection::ReplayStudio};
    Arena3D arena_{};
    int selectedIndex_{0};
    std::filesystem::path loadedReplayPath_{};
    bool loaded_{false};
    bool commandLoaded_{false};
    bool commandPlaybackPlaying_{false};
    replay::ReplayPlaybackTrack commandTrack_{replay::ReplayPlaybackTrack::ServerTruth};
    float commandPlaybackTimer_{0.0f};
    std::uint32_t commandStartTick_{0u};
    std::uint32_t commandEndTick_{0u};
    std::uint32_t commandRenderBaseTick_{0u};
    std::uint32_t commandRenderNextTick_{0u};
    float commandInterpolationAlpha_{0.0f};
    bool commandInterpolationEnabled_{true};
    bool commandGhostsVisible_{true};
    std::string statusMessage_{};
    bool renameDialogVisible_{false};
    bool deleteConfirmVisible_{false};
    std::string renameText_{};
    bool immersiveReplayActive_{false};
    bool replayTransportOverlayVisible_{false};
    bool replaySettingsVisible_{false};
    RuntimeSettingsOverlay replaySettingsOverlay_{};
    ReplayCameraMode replayCameraMode_{ReplayCameraMode::FollowFirstPerson};
    int replayFollowActorId_{-1};
    SpectatorCamera replaySpectatorCamera_{};
    std::vector<SpectatorCamera::Checkpoint> replayCheckpoints_{};
    input::ControlBindings replayControlBindings_{input::ControlBindings::defaults()};
    int currentReplayCheckpoint_{-1};
    bool replayCheckpointTransitionActive_{false};
    float replayCheckpointTransitionTimer_{0.0f};
    float replayCheckpointTransitionDuration_{0.0f};
    SpectatorCamera::Checkpoint replayCheckpointTransitionStart_{};
    SpectatorCamera::Checkpoint replayCheckpointTransitionEnd_{};
    std::string immersiveStatusMessage_{};
    float immersiveStatusTimer_{0.0f};

    void refreshLibrary();
    bool loadSelectedReplay();
    bool previewSelectedReplay();
    void beginRenameSelectedReplay();
    void applyRenameSelectedReplay();
    void requestDeleteSelectedReplay();
    void confirmDeleteSelectedReplay();
    void cancelLibraryDialog();
    void updateLibraryDialogInput();
    bool selectedReplayAvailable() const;
    std::string selectedReplayTitle() const;
    bool renameReplay(LibraryEntry entry, const std::string& title, std::filesystem::path* selectedPathOut);
    bool deleteReplay(const LibraryEntry& entry);
    void clearLoadedReplay();
    void selectLibraryEntryByPath(const std::filesystem::path& path);
    void enterCommandReplay();
    void exitCommandReplay();
    void loadArenaForRecording(int levelSlot);
    const client::RecordedReplayFrame* activeFrame() const;
    float loadedDurationSeconds() const;
    float commandDurationSeconds() const;
    bool loadCommandReplay(const LibraryEntry& entry);
    bool loadLegacyReplay(const LibraryEntry& entry);
    void updateCommandPlayback(float dtSeconds, bool loopPlayback = false);
    void updateLegacyPlayback(float dtSeconds, bool loopPlayback = false);
    void refreshCommandRenderSample(float exactTickPosition);
    void seekCommandPlaybackByTicks(int tickDelta);
    void resetCommandPlayback();
    void setCommandTrack(replay::ReplayPlaybackTrack track);
    void toggleCommandTrack();
    void stopCommandPlayback();
    void toggleCommandInterpolation();
    void toggleCommandGhosts();
    RuntimeSettingsOverlay::State buildReplaySettingsOverlayState() const;
    void applyReplaySettingsAction(const RuntimeSettingsOverlay::Action& action);
    void updateImmersiveCommandReplay(float dtSeconds);
    void cycleReplayFollowTarget();
    void cycleReplayFollowCamera();
    void toggleReplaySpectatorMode();
    void ensureReplayFollowTarget();
    std::vector<int> commandReplayActorIds() const;
    const sim::PlayerState* commandReplayPlayer(int actorId) const;
    const sim::PlayerState* activeCommandReplayPlayer() const;
    const sim::WorldState& commandRenderWorld() const;
    const std::vector<sim::PlayerState>& commandRenderControlPlayers() const;
    Camera3D activeCommandReplayCamera() const;
    void loadReplayCheckpoints();
    void saveReplayCheckpoints() const;
    void createReplayCheckpoint();
    void beginReplayCheckpointTransition(int targetIndex);
    void updateReplayCheckpointTransition(float dtSeconds);
    void adjustReplayCheckpointTransition(float deltaSeconds);
    void setImmersiveStatus(std::string message);
    const char* replayCameraModeLabel() const;
    void updateRows();
    void renderLibrary() const;
    void renderLoadedReplay() const;
    void renderFrameWorld(const client::RenderFrame& frame) const;
    void renderCommandWorld() const;
    void renderCommandWorld(const Camera3D& camera, bool drawStudioOverlay) const;
    void renderImmersiveCommandReplay() const;
    void renderImmersiveOverlay(const Camera3D& camera) const;
    void renderReplayTransportOverlay() const;
    void renderReplaySettingsOverlay() const;
    void renderTransport() const;
    static std::string formatTimestamp(std::uint64_t unixSeconds);
    static std::string formatDuration(float seconds);
    static std::string formatFileSize(std::uintmax_t bytes);
    static std::string formatMapLabel(int levelSlot, std::uint32_t levelHash);
};
