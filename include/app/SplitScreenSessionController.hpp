#pragma once

#include <cstdint>
#include <string>

#include "client/PaneViewState.hpp"

namespace app {

class SplitScreenSessionController {
public:
    struct SlotState {
        sim::PaneSlot slot{sim::PaneSlot::Left};
        bool active{false};
        bool focused{false};
        client::PaneBinding binding{};
    };

    SplitScreenSessionController() {
        left_.slot = sim::PaneSlot::Left;
        left_.active = true;
        left_.focused = true;
        right_.slot = sim::PaneSlot::Right;
    }

    void bindPrimaryLocalParticipant(std::uint16_t participantId,
                                     int actorId,
                                     const std::string& label) {
        left_.active = true;
        left_.binding.kind = client::PaneBindingKind::LocalParticipant;
        left_.binding.participantId = participantId;
        left_.binding.actorId = actorId;
        left_.binding.label = label;
    }

    void requestRightLocalParticipant() {
        awaitingRightLocalParticipant_ = true;
        right_.active = false;
        right_.binding = client::PaneBinding{};
    }

    bool bindRightLocalParticipant(std::uint16_t participantId,
                                   int actorId,
                                   const std::string& label) {
        if (!awaitingRightLocalParticipant_) {
            return false;
        }

        right_.active = true;
        right_.binding.kind = client::PaneBindingKind::LocalParticipant;
        right_.binding.participantId = participantId;
        right_.binding.actorId = actorId;
        right_.binding.label = label;
        right_.focused = false;
        awaitingRightLocalParticipant_ = false;
        temporaryLocalParticipantId_ = participantId;
        return true;
    }

    void bindRightObservation(const client::PaneBinding& binding) {
        awaitingRightLocalParticipant_ = false;
        temporaryLocalParticipantId_ = 0u;
        right_.active = true;
        right_.binding = binding;
        right_.focused = false;
    }

    void setFocusedSlot(sim::PaneSlot slot) {
        left_.focused = slot == sim::PaneSlot::Left;
        right_.focused = slot == sim::PaneSlot::Right && right_.active;
    }

    void disableSplitScreen() {
        awaitingRightLocalParticipant_ = false;
        temporaryLocalParticipantId_ = 0u;
        right_.active = false;
        right_.focused = false;
        right_.binding = client::PaneBinding{};
        left_.focused = true;
    }

    bool splitScreenActive() const {
        return right_.active;
    }

    bool awaitingRightLocalParticipant() const {
        return awaitingRightLocalParticipant_;
    }

    std::uint16_t temporaryLocalParticipantId() const {
        return temporaryLocalParticipantId_;
    }

    std::uint8_t activeLocalParticipantCount() const {
        return temporaryLocalParticipantId_ != 0u ? 2u : 1u;
    }

    const SlotState& leftSlot() const {
        return left_;
    }

    const SlotState& rightSlot() const {
        return right_;
    }

private:
    SlotState left_{};
    SlotState right_{};
    bool awaitingRightLocalParticipant_{false};
    std::uint16_t temporaryLocalParticipantId_{0u};
};

}  // namespace app
