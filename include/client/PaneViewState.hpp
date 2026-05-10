#pragma once

#include <cstdint>
#include <string>

#include "client/ClientViewState.hpp"

namespace client {

enum class PaneBindingKind : std::uint8_t {
    None = 0,
    LocalParticipant = 1,
    SpectatorTarget = 2,
    ReplayCamera = 3
};

struct PaneBinding {
    PaneBindingKind kind{PaneBindingKind::None};
    std::uint16_t participantId{0u};
    int actorId{-1};
    std::string label{};
};

struct PaneViewState {
    sim::PaneSlot slot{sim::PaneSlot::Left};
    bool focused{true};
    PaneBinding binding{};
    ClientViewState viewState{};
    bool predictionEnabled{false};
    bool authoritativeOnly{false};
};

}  // namespace client
