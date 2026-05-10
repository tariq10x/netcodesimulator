#pragma once

#include <string>
#include <vector>

#include "client/PaneViewState.hpp"

namespace client {

struct SharedOverlayViewState {
    std::string hostedSessionLine{};
    std::vector<std::string> hudLines{};
    CompactScoreView compactScore{};
    std::vector<KillFeedEntryView> killFeed{};
    std::vector<ScoreboardSectionView> scoreboardSections{};
    ReplayStatusView replay{};
    bool scoreboardVisible{false};
    bool localNetworkPanelVisible{false};
    bool diagnosticsPanelVisible{false};
};

struct SplitScreenViewState {
    PaneViewState left{};
    PaneViewState right{};
    sim::PaneSlot focusedSlot{sim::PaneSlot::Left};
    SharedOverlayViewState sharedOverlays{};

    const PaneViewState* paneFor(sim::PaneSlot slot) const {
        return slot == sim::PaneSlot::Left ? &left : &right;
    }

    PaneViewState* paneFor(sim::PaneSlot slot) {
        return slot == sim::PaneSlot::Left ? &left : &right;
    }
};

inline SplitScreenViewState buildSplitScreenView(const PaneViewState& left,
                                                 const PaneViewState& right,
                                                 sim::PaneSlot focus) {
    SplitScreenViewState view;
    view.left = left;
    view.right = right;
    view.focusedSlot = focus;
    return view;
}

}  // namespace client
