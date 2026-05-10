#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "app/ReplayTimeline.hpp"
#include "client/PaneViewState.hpp"
#include "net/SessionDiscovery.hpp"

namespace client {

struct ReplayShotStrategyChange {
    float timestamp{0.0f};
    net::ShotEvaluationMode mode{net::ShotEvaluationMode::SeenPosition};
};

struct ReplayShotStrategyChangeView {
    float timestamp{0.0f};
    net::ShotEvaluationMode mode{net::ShotEvaluationMode::SeenPosition};
    std::string ruleLabel{};
    std::string ruleExplanation{};
};

struct ReplayShotStrategyTimelineView {
    bool available{false};
    std::size_t activeEntryIndex{0u};
    float activeTimestamp{0.0f};
    std::string activeRuleLabel{};
    std::string activeRuleExplanation{};
    std::vector<ReplayShotStrategyChangeView> entries{};
};

struct ReplaySpectatorContextChange {
    float timestamp{0.0f};
    sim::PaneViewMode mode{sim::PaneViewMode::SpectatorFreeFly};
    int followTargetActorId{-1};
    std::string followTargetLabel{};
    bool sessionSpectator{false};
    bool canReturnToCharacter{false};
};

struct ReplaySpectatorContextChangeView {
    float timestamp{0.0f};
    sim::PaneViewMode mode{sim::PaneViewMode::SpectatorFreeFly};
    std::string modeLabel{};
    int followTargetActorId{-1};
    std::string followTargetLabel{};
    bool sessionSpectator{false};
    bool canReturnToCharacter{false};
};

struct ReplaySpectatorTimelineView {
    bool available{false};
    std::size_t activeEntryIndex{0u};
    float activeTimestamp{0.0f};
    std::string activeModeLabel{};
    std::string activeFollowTargetLabel{};
    bool sessionSpectator{false};
    bool canReturnToCharacter{false};
    std::vector<ReplaySpectatorContextChangeView> entries{};
};

struct ReplayPaneBindingChange {
    float timestamp{0.0f};
    sim::PaneSlot slot{sim::PaneSlot::Left};
    sim::PaneViewMode mode{sim::PaneViewMode::ReplayCamera};
    int actorId{-1};
    std::string bindingLabel{};
};

struct ReplayPaneBindingChangeView {
    float timestamp{0.0f};
    sim::PaneSlot slot{sim::PaneSlot::Left};
    std::string slotLabel{};
    sim::PaneViewMode mode{sim::PaneViewMode::ReplayCamera};
    std::string modeLabel{};
    int actorId{-1};
    std::string bindingLabel{};
};

struct ReplayPaneBindingTimelineView {
    bool available{false};
    std::size_t activeEntryIndex{0u};
    float activeTimestamp{0.0f};
    sim::PaneSlot activeSlot{sim::PaneSlot::Left};
    std::string activeSlotLabel{};
    std::string activeModeLabel{};
    std::string activeBindingLabel{};
    std::vector<ReplayPaneBindingChangeView> entries{};
};

struct ReplayPaneSlotLayout {
    sim::PaneSlot slot{sim::PaneSlot::Left};
    bool active{false};
    bool focused{false};
    client::PaneBinding binding{};
};

struct ReplayPaneLayout {
    bool available{false};
    sim::PaneSlot focusedSlot{sim::PaneSlot::Left};
    ReplayPaneSlotLayout left{};
    ReplayPaneSlotLayout right{};

    const ReplayPaneSlotLayout* paneFor(sim::PaneSlot slot) const {
        return slot == sim::PaneSlot::Right ? &right : &left;
    }

    std::size_t activePaneCount() const {
        return static_cast<std::size_t>(left.active) +
               static_cast<std::size_t>(right.active);
    }
};

class ReplaySubsystem {
public:
    static PaneBinding replayCameraBinding(sim::PaneSlot slot) {
        return PaneBinding{
            PaneBindingKind::ReplayCamera,
            0u,
            -1,
            slot == sim::PaneSlot::Right ? "Replay Camera B" : "Replay Camera A"
        };
    }

    static ReplayPaneLayout defaultReplayPaneLayout(std::uint8_t localParticipantCount) {
        ReplayPaneLayout layout;
        layout.available = true;
        layout.focusedSlot = sim::PaneSlot::Left;

        layout.left.slot = sim::PaneSlot::Left;
        layout.left.active = true;
        layout.left.focused = true;
        layout.left.binding = replayCameraBinding(sim::PaneSlot::Left);

        layout.right.slot = sim::PaneSlot::Right;
        layout.right.active = localParticipantCount > 1u;
        layout.right.focused = false;
        layout.right.binding =
            layout.right.active ? replayCameraBinding(sim::PaneSlot::Right) : PaneBinding{};
        return layout;
    }

    static std::vector<ReplayPaneBindingChange> defaultPaneBindingTimeline(
        std::uint8_t localParticipantCount,
        float timestamp = 0.0f) {
        const ReplayPaneLayout layout = defaultReplayPaneLayout(localParticipantCount);
        std::vector<ReplayPaneBindingChange> changes;
        changes.reserve(layout.activePaneCount());

        const auto appendSlot = [&](const ReplayPaneSlotLayout& slot) {
            if (!slot.active || slot.binding.kind == PaneBindingKind::None) {
                return;
            }

            changes.push_back(ReplayPaneBindingChange{
                timestamp,
                slot.slot,
                sim::PaneViewMode::ReplayCamera,
                slot.binding.actorId,
                slot.binding.label
            });
        };

        appendSlot(layout.left);
        appendSlot(layout.right);
        return changes;
    }

    void reset() {
        timeline_.reset();
    }

    void startRecording() {
        shotStrategyTimeline_.clear();
        spectatorTimeline_.clear();
        paneBindingTimeline_.clear();
        timeline_.startRecording();
    }

    void stopRecording() {
        timeline_.stopRecording();
    }

    void advanceRecording(float dt) {
        timeline_.advanceRecording(dt);
    }

    void stopPlayback() {
        timeline_.stopPlayback();
    }

    template <typename FrameContainer>
    bool startPlayback(const FrameContainer& frames, bool autoPlay) {
        return timeline_.startPlayback(frames, autoPlay);
    }

    template <typename FrameContainer>
    bool resetPlayback(const FrameContainer& frames) {
        return timeline_.resetPlayback(frames);
    }

    template <typename FrameContainer>
    bool seekBy(const FrameContainer& frames, int delta) {
        return timeline_.seekBy(frames, delta);
    }

    template <typename FrameContainer>
    bool updatePlayback(const FrameContainer& frames, float dt) {
        return timeline_.updatePlayback(frames, dt);
    }

    ReplayStatusView statusView(std::size_t recordedFrameCount = 0u) const {
        ReplayStatusView view;
        view.recordingActive = timeline_.isRecording;
        view.playbackActive = timeline_.isPlayback;
        const ReplayShotStrategyTimelineView strategyView = shotStrategyTimelineView();

        if (timeline_.isRecording) {
            view.statusLine = "Recording";
            if (recordedFrameCount > 0u) {
                view.statusLine += " (" + std::to_string(recordedFrameCount) + " frames)";
            }
            return view;
        }

        if (timeline_.isPlayback) {
            view.statusLine = timeline_.playbackPlaying
                ? "Playback (playing)"
                : "Playback (paused)";
            if (recordedFrameCount > 0u) {
                view.statusLine += " @" +
                    std::to_string(timeline_.playbackIndex + 1u) + "/" +
                    std::to_string(recordedFrameCount);
            }
            if (strategyView.available) {
                view.statusLine += " | Rule " + strategyView.activeRuleLabel;
            }
            const ReplayPaneBindingTimelineView bindingView = paneBindingTimelineView();
            if (bindingView.available) {
                view.statusLine += " | " + bindingView.activeSlotLabel +
                    " " + bindingView.activeBindingLabel;
            }
            return view;
        }

        view.statusLine = recordedFrameCount > 0u ? "Replay ready" : "Replay idle";
        if (strategyView.available) {
            view.statusLine += " | Rule " + strategyView.activeRuleLabel;
        }
        const ReplayPaneBindingTimelineView bindingView = paneBindingTimelineView();
        if (bindingView.available) {
            view.statusLine += " | " + bindingView.activeSlotLabel +
                " " + bindingView.activeBindingLabel;
        }
        return view;
    }

    void setShotStrategyTimeline(std::vector<ReplayShotStrategyChange> changes) {
        std::sort(changes.begin(),
                  changes.end(),
                  [](const ReplayShotStrategyChange& lhs, const ReplayShotStrategyChange& rhs) {
                      return lhs.timestamp < rhs.timestamp;
                  });
        shotStrategyTimeline_ = std::move(changes);
    }

    const std::vector<ReplayShotStrategyChange>& shotStrategyTimeline() const {
        return shotStrategyTimeline_;
    }

    ReplayShotStrategyTimelineView shotStrategyTimelineView() const {
        ReplayShotStrategyTimelineView view;
        if (shotStrategyTimeline_.empty()) {
            return view;
        }

        view.available = true;
        view.entries.reserve(shotStrategyTimeline_.size());
        for (const auto& change : shotStrategyTimeline_) {
            view.entries.push_back(ReplayShotStrategyChangeView{
                change.timestamp,
                change.mode,
                net::toString(change.mode),
                net::shotEvaluationModeExplanation(change.mode)
            });
        }

        const std::size_t activeIndex = app::ReplayTimeline::activeEntryIndexAt(
            shotStrategyTimeline_,
            playbackTimestamp(),
            [](const ReplayShotStrategyChange& change) { return change.timestamp; });

        view.activeEntryIndex = activeIndex;
        view.activeTimestamp = shotStrategyTimeline_[activeIndex].timestamp;
        view.activeRuleLabel = net::toString(shotStrategyTimeline_[activeIndex].mode);
        view.activeRuleExplanation =
            net::shotEvaluationModeExplanation(shotStrategyTimeline_[activeIndex].mode);
        return view;
    }

    void setSpectatorTimeline(std::vector<ReplaySpectatorContextChange> changes) {
        std::sort(changes.begin(),
                  changes.end(),
                  [](const ReplaySpectatorContextChange& lhs,
                     const ReplaySpectatorContextChange& rhs) {
                      return lhs.timestamp < rhs.timestamp;
                  });
        spectatorTimeline_ = std::move(changes);
    }

    const std::vector<ReplaySpectatorContextChange>& spectatorTimeline() const {
        return spectatorTimeline_;
    }

    ReplaySpectatorTimelineView spectatorTimelineView() const {
        ReplaySpectatorTimelineView view;
        if (spectatorTimeline_.empty()) {
            return view;
        }

        view.available = true;
        view.entries.reserve(spectatorTimeline_.size());
        for (const auto& change : spectatorTimeline_) {
            view.entries.push_back(ReplaySpectatorContextChangeView{
                change.timestamp,
                change.mode,
                paneModeLabel(change.mode),
                change.followTargetActorId,
                change.followTargetLabel,
                change.sessionSpectator,
                change.canReturnToCharacter
            });
        }

        const std::size_t activeIndex = app::ReplayTimeline::activeEntryIndexAt(
            spectatorTimeline_,
            playbackTimestamp(),
            [](const ReplaySpectatorContextChange& change) { return change.timestamp; });
        const auto& active = spectatorTimeline_[activeIndex];
        view.activeEntryIndex = activeIndex;
        view.activeTimestamp = active.timestamp;
        view.activeModeLabel = paneModeLabel(active.mode);
        view.activeFollowTargetLabel = active.followTargetLabel;
        view.sessionSpectator = active.sessionSpectator;
        view.canReturnToCharacter = active.canReturnToCharacter;
        return view;
    }

    void setPaneBindingTimeline(std::vector<ReplayPaneBindingChange> changes) {
        std::sort(changes.begin(),
                  changes.end(),
                  [](const ReplayPaneBindingChange& lhs, const ReplayPaneBindingChange& rhs) {
                      return lhs.timestamp < rhs.timestamp;
                  });
        paneBindingTimeline_ = std::move(changes);
    }

    const std::vector<ReplayPaneBindingChange>& paneBindingTimeline() const {
        return paneBindingTimeline_;
    }

    ReplayPaneBindingTimelineView paneBindingTimelineView() const {
        ReplayPaneBindingTimelineView view;
        if (paneBindingTimeline_.empty()) {
            return view;
        }

        view.available = true;
        view.entries.reserve(paneBindingTimeline_.size());
        for (const auto& change : paneBindingTimeline_) {
            view.entries.push_back(ReplayPaneBindingChangeView{
                change.timestamp,
                change.slot,
                slotLabel(change.slot),
                change.mode,
                paneModeLabel(change.mode),
                change.actorId,
                change.bindingLabel
            });
        }

        const std::size_t activeIndex = app::ReplayTimeline::activeEntryIndexAt(
            paneBindingTimeline_,
            playbackTimestamp(),
            [](const ReplayPaneBindingChange& change) { return change.timestamp; });
        const auto& active = paneBindingTimeline_[activeIndex];
        view.activeEntryIndex = activeIndex;
        view.activeTimestamp = active.timestamp;
        view.activeSlot = active.slot;
        view.activeSlotLabel = slotLabel(active.slot);
        view.activeModeLabel = paneModeLabel(active.mode);
        view.activeBindingLabel = active.bindingLabel;
        return view;
    }

    const app::ReplayTimeline& timeline() const {
        return timeline_;
    }

    app::ReplayTimeline& timeline() {
        return timeline_;
    }

private:
    float playbackTimestamp() const {
        return timeline_.isPlayback ? timeline_.playbackTimer : 0.0f;
    }

    static const char* slotLabel(sim::PaneSlot slot) {
        return slot == sim::PaneSlot::Right ? "Right Pane" : "Left Pane";
    }

    static const char* paneModeLabel(sim::PaneViewMode mode) {
        switch (mode) {
            case sim::PaneViewMode::PlayerControlled:
                return "Player Controlled";
            case sim::PaneViewMode::SpectatorFreeFly:
                return "Spectator Free-Fly";
            case sim::PaneViewMode::SpectatorFollowFirstPerson:
                return "Spectator Follow First Person";
            case sim::PaneViewMode::SpectatorFollowThirdPerson:
                return "Spectator Follow Third Person";
            case sim::PaneViewMode::ReplayCamera:
                return "Replay Camera";
        }
        return "Replay Camera";
    }

    app::ReplayTimeline timeline_{};
    std::vector<ReplayShotStrategyChange> shotStrategyTimeline_{};
    std::vector<ReplaySpectatorContextChange> spectatorTimeline_{};
    std::vector<ReplayPaneBindingChange> paneBindingTimeline_{};
};

}  // namespace client
