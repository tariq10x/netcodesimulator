#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/SessionComposer.hpp"

namespace app {

class AppFlow {
public:
    enum class ShellRoute : std::uint8_t {
        None = 0,
        LevelSelect = 1,
        MultiplayerSetup = 2,
        RuntimeSession = 3,
        Replay = 4,
        LevelEditor = 5
    };

    enum class LaunchTarget : std::uint8_t {
        None = 0,
        RuntimeSession = 1,
        Replay = 2,
        LevelEditor = 3
    };

    enum class ObservationContext : std::uint8_t {
        Gameplay = 0,
        PaneLocalObservation = 1,
        SessionSpectator = 2
    };

    struct PaneRoute {
        ShellRoute shellRoute{ShellRoute::None};
        LaunchTarget launchTarget{LaunchTarget::None};
        ObservationContext observationContext{ObservationContext::Gameplay};
        bool usesRuntimeSession{false};
    };

    struct SessionStartResult {
        bool started{false};
        std::string statusMessage{};
        LaunchTarget target{LaunchTarget::None};
        net::SessionLaunchConfig launchConfig{};
        client::ReplayPaneLayout replayLayout{};
        std::vector<client::ReplayPaneBindingChange> replayPaneBindingTimeline{};
        std::unique_ptr<net::SessionFlowController> sessionFlow{};
    };

    static ShellRoute routeForRequestedEntryPoint(net::SessionEntryPoint entryPoint) {
        switch (entryPoint) {
            case net::SessionEntryPoint::Host:
            case net::SessionEntryPoint::LabStudy:
            case net::SessionEntryPoint::LevelEditor:
                return ShellRoute::LevelSelect;
            case net::SessionEntryPoint::Join:
                return ShellRoute::MultiplayerSetup;
            case net::SessionEntryPoint::Replay:
                return ShellRoute::Replay;
            case net::SessionEntryPoint::None:
                return ShellRoute::None;
        }
        return ShellRoute::None;
    }

    static LaunchTarget targetFor(const net::SessionLaunchConfig& launchConfig) {
        switch (launchConfig.entryPoint) {
            case net::SessionEntryPoint::Replay:
                return LaunchTarget::Replay;
            case net::SessionEntryPoint::LevelEditor:
                return LaunchTarget::LevelEditor;
            case net::SessionEntryPoint::Host:
            case net::SessionEntryPoint::Join:
            case net::SessionEntryPoint::LabStudy:
                return LaunchTarget::RuntimeSession;
            case net::SessionEntryPoint::None:
                return LaunchTarget::None;
        }
        return LaunchTarget::None;
    }

    static ShellRoute routeFor(const SessionStartResult& sessionStart) {
        switch (sessionStart.target) {
            case LaunchTarget::RuntimeSession:
                return ShellRoute::RuntimeSession;
            case LaunchTarget::Replay:
                return ShellRoute::Replay;
            case LaunchTarget::LevelEditor:
                return ShellRoute::LevelEditor;
            case LaunchTarget::None:
                return ShellRoute::None;
        }
        return ShellRoute::None;
    }

    static ObservationContext observationContextFor(const sim::ParticipantState& participantState,
                                                    const sim::PaneViewState& paneView) {
        switch (paneView.mode) {
            case sim::PaneViewMode::SpectatorFreeFly:
            case sim::PaneViewMode::SpectatorFollowFirstPerson:
            case sim::PaneViewMode::SpectatorFollowThirdPerson:
            case sim::PaneViewMode::ReplayCamera:
                if (participantState.participation == sim::ParticipationState::Spectating ||
                    participantState.team == sim::TeamId::Spectator) {
                    return ObservationContext::SessionSpectator;
                }
                return ObservationContext::PaneLocalObservation;
            case sim::PaneViewMode::PlayerControlled:
                break;
        }
        return ObservationContext::Gameplay;
    }

    static PaneRoute paneRouteFor(const net::SessionLaunchConfig& launchConfig,
                                  const sim::ParticipantState& participantState = {},
                                  const sim::PaneViewState& paneView = {}) {
        PaneRoute route;
        route.shellRoute = routeForRequestedEntryPoint(launchConfig.entryPoint);
        route.launchTarget = targetFor(launchConfig);
        route.usesRuntimeSession = route.launchTarget == LaunchTarget::RuntimeSession;
        route.observationContext = observationContextFor(participantState, paneView);
        return route;
    }

    static SessionStartResult startSession(const net::SessionLaunchConfig& launchConfig) {
        SessionComposer composer(launchConfig);
        SessionComposer::Result composition = composer.compose();

        SessionStartResult result;
        result.target = targetFor(composition.composition.config);
        result.launchConfig = composition.composition.config;
        result.replayLayout = composition.replayLayout;
        result.replayPaneBindingTimeline = composition.replayPaneBindingTimeline;
        if (!composition.ok) {
            result.statusMessage = composition.error;
            result.sessionFlow = std::make_unique<net::SessionFlowController>(launchConfig);
            result.sessionFlow->shutdown(result.statusMessage);
            return result;
        }

        if (result.target == LaunchTarget::Replay) {
            result.started = result.replayLayout.available;
            result.statusMessage = result.started ? "replay ready" : "replay unavailable";
            return result;
        }
        if (result.target == LaunchTarget::LevelEditor) {
            result.started = true;
            result.statusMessage = "editor ready";
            return result;
        }

        result.sessionFlow =
            std::make_unique<net::SessionFlowController>(std::move(composition.composition));
        result.started = result.sessionFlow->start();
        result.statusMessage = result.sessionFlow->statusMessage();
        result.launchConfig = result.sessionFlow->config();
        return result;
    }
};

}  // namespace app
