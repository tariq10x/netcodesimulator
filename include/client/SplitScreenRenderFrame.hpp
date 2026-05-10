#pragma once

#include "client/PaneViewState.hpp"
#include "client/RenderFrame.hpp"

namespace client {

struct PaneRenderFrame {
    sim::PaneSlot slot{sim::PaneSlot::Left};
    bool focused{true};
    PaneBinding binding{};
    RenderWorldFrame world{};
};

struct SplitScreenRenderFrame {
    PaneRenderFrame left{};
    PaneRenderFrame right{};
    sim::PaneSlot focusedSlot{sim::PaneSlot::Left};
    SharedOverlayRenderFrame sharedOverlays{};

    const PaneRenderFrame* paneFor(sim::PaneSlot slot) const {
        return slot == sim::PaneSlot::Left ? &left : &right;
    }

    PaneRenderFrame* paneFor(sim::PaneSlot slot) {
        return slot == sim::PaneSlot::Left ? &left : &right;
    }
};

inline SplitScreenRenderFrame buildSplitScreenRenderFrame(const PaneRenderFrame& left,
                                                          const PaneRenderFrame& right,
                                                          const SharedOverlayRenderFrame& sharedOverlays,
                                                          sim::PaneSlot focus) {
    SplitScreenRenderFrame frame;
    frame.left = left;
    frame.right = right;
    frame.focusedSlot = focus;
    frame.sharedOverlays = sharedOverlays;
    return frame;
}

}  // namespace client
