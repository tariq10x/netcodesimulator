#include "app/AppFlow.hpp"
#include "CharacterEditorScreen.hpp"
#include "DisplayManager.hpp"
#include "LevelSelectMenu.hpp"
#include "LevelEditor.hpp"
#include "MainMenu.hpp"
#include "MultiplayerSessionMenu.hpp"
#include "ReplayStudio.hpp"
#include "SettingsMenu.hpp"
#include "TypographyService.hpp"
#include "app/UserSettings.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/SessionFlowController.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct LaunchOptions {
    bool joinMode{false};
    std::string host{"127.0.0.1"};
    std::uint16_t serverPort{net::kDefaultServerPort};
    std::uint16_t localPort{0};
    std::string playerName{"player"};
    std::uint16_t protocolVersion{net::kProtocolVersion};
};

constexpr std::uint16_t kStudyModeAttackerBotCount = 1u;
constexpr std::uint16_t kStudyModeDefenderBotCount = 1u;

net::SessionLaunchConfig makeLabStudyLaunchConfig(int levelSlot) {
    return net::makeStudySessionLaunchConfig(levelSlot,
                                             "player",
                                             net::kDefaultServerPort,
                                             kStudyModeAttackerBotCount,
                                             kStudyModeDefenderBotCount);
}

const char* urlForExternalLinkTarget(MainMenu::ExternalLinkTarget target) {
    switch (target) {
        case MainMenu::ExternalLinkTarget::YouTube:
            return "https://www.youtube.com/@tariq10x";
        case MainMenu::ExternalLinkTarget::Patreon:
            return "https://www.patreon.com/c/tariq10x";
        case MainMenu::ExternalLinkTarget::GitHub:
            return "https://github.com/tariq10x/netcodesimulator";
        case MainMenu::ExternalLinkTarget::None:
            break;
    }
    return nullptr;
}

void openExternalLinkTarget(MainMenu::ExternalLinkTarget target) {
    const char* url = urlForExternalLinkTarget(target);
    if (url != nullptr) {
        OpenURL(url);
    }
}

bool parsePort(const char* text, std::uint16_t* portOut) {
    if (text == nullptr || portOut == nullptr) {
        return false;
    }

    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }

    *portOut = static_cast<std::uint16_t>(value);
    return true;
}

bool parseLaunchOptions(int argc, char** argv, LaunchOptions* optionsOut) {
    if (optionsOut == nullptr) {
        return false;
    }

    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--join") {
            if (index + 2 >= argc) {
                return false;
            }
            options.joinMode = true;
            options.host = argv[++index];
            if (!parsePort(argv[++index], &options.serverPort)) {
                return false;
            }
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                options.playerName = argv[++index];
            }
        } else if (arg == "--local-port") {
            if (index + 1 >= argc || !parsePort(argv[++index], &options.localPort)) {
                return false;
            }
        } else if (arg == "--protocol-version") {
            if (index + 1 >= argc || !parsePort(argv[++index], &options.protocolVersion)) {
                return false;
            }
        } else {
            return false;
        }
    }

    *optionsOut = options;
    return true;
}

bool shouldSuppressMainMenuStatusMessage(const std::string& statusMessage) {
    return statusMessage.empty() || statusMessage == "session ended";
}

int runDirectJoinClient(const LaunchOptions& options) {
    const app::UserSettings userSettings = app::UserSettingsStore().load().settings;
    display::initWindow("Netcode Simulator - Network Client");
    TypographyService::shared().initialize();
    display::disableCursorForCapture();

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig(
            options.host,
            options.serverPort,
            options.playerName,
            options.protocolVersion);
    joinConfig.clientLocalPort = options.localPort;

    app::AppFlow::SessionStartResult sessionStart = app::AppFlow::startSession(joinConfig);
    if (!sessionStart.started || sessionStart.sessionFlow == nullptr) {
        std::cerr << "Failed to start network client: " << sessionStart.statusMessage << std::endl;
        display::enableCursorPreservingPosition();
        display::shutdownWindow();
        return 1;
    }

    std::unique_ptr<net::SessionFlowController> sessionFlow =
        std::move(sessionStart.sessionFlow);
    int exitCode = 0;
    while (!WindowShouldClose()) {
        display::updateWindowPolicy();
        const float dt = GetFrameTime();
        const InputHandler3D::InputState input = InputHandler3D::poll(userSettings.controls);

        sessionFlow->update(dt, &input);

        display::beginFrame();
        sessionFlow->render();
        display::endFrame();

        if (sessionFlow->shouldReturnToMainMenu()) {
            break;
        }
        if (sessionFlow->state() == net::SessionFlowState::Failed) {
            exitCode = 1;
            break;
        }
    }

    display::enableCursorPreservingPosition();
    display::shutdownWindow();
    return exitCode;
}

void printUsage() {
    std::cerr << "Usage: netcodesim [--join <host> <port> [player-name]] "
                 "[--local-port <port>] [--protocol-version <version>]" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "  Netcode Simulator - Network Visualizer" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    LaunchOptions launchOptions;
    if (!parseLaunchOptions(argc, argv, &launchOptions)) {
        printUsage();
        return 1;
    }
    if (launchOptions.joinMode) {
        return runDirectJoinClient(launchOptions);
    }

    try {
        // Initialize window for menus
        display::initWindow("Netcode Simulator");
        TypographyService::shared().initialize();
        display::enableCursorPreservingPosition();  // Start with cursor enabled for menus
        app::UserSettingsStore userSettingsStore;
        app::UserSettings userSettings = userSettingsStore.load().settings;

        GameMode currentMode = GameMode::MAIN_MENU;
        int selectedLevel = -1;
        std::unique_ptr<MainMenu> mainMenu;
        std::unique_ptr<SettingsMenu> settingsMenu;
        std::unique_ptr<LevelSelectMenu> levelSelect;
        std::unique_ptr<MultiplayerSessionMenu> multiplayerMenu;
        std::unique_ptr<LevelEditor> levelEditor;
        std::unique_ptr<ReplayStudio> replayStudio;
        std::unique_ptr<CharacterEditorScreen> characterEditor;
        std::unique_ptr<net::SessionFlowController> sessionFlow;
        std::string menuStatusMessage;
        bool sessionCursorCaptured = false;
        std::vector<GameMode> navigationHistory;
        bool suppressBackShortcutUntilRelease = false;

        bool running = true;
        bool labStudyTarget = false;
        bool editorTarget = false;
        bool hostGameTarget = false;
        auto createMainMenu = [&](bool suppressQuitShortcut) {
            mainMenu = std::make_unique<MainMenu>();
            if (suppressQuitShortcut) {
                mainMenu->suppressQuitShortcutUntilReleased();
            }
            if (!menuStatusMessage.empty()) {
                mainMenu->setStatusMessage(menuStatusMessage);
            }
        };
        auto renderFrame = [](const auto& renderFn) {
            display::beginFrame();
            renderFn();
            display::endFrame();
        };
        auto discardScreenForMode = [&](GameMode mode) {
            switch (mode) {
                case GameMode::SETTINGS:
                    settingsMenu.reset();
                    return;
                case GameMode::LEVEL_SELECT:
                    levelSelect.reset();
                    hostGameTarget = false;
                    labStudyTarget = false;
                    editorTarget = false;
                    return;
                case GameMode::MULTIPLAYER_SESSION:
                    multiplayerMenu.reset();
                    if (sessionFlow != nullptr &&
                        sessionFlow->state() != net::SessionFlowState::Running) {
                        sessionFlow.reset();
                    }
                    return;
                case GameMode::LEVEL_EDITOR:
                    levelEditor.reset();
                    return;
                case GameMode::REPLAY_STUDIO:
                    replayStudio.reset();
                    return;
                case GameMode::CHARACTER_EDITOR:
                    characterEditor.reset();
                    return;
                default:
                    return;
            }
        };
        auto armBackShortcutSuppression = [&]() {
            suppressBackShortcutUntilRelease = true;
            if (mainMenu) {
                mainMenu->suppressQuitShortcutUntilReleased();
            }
        };
        auto popMenuMode = [&]() {
            const GameMode poppedMode = currentMode;
            discardScreenForMode(poppedMode);

            if (navigationHistory.empty()) {
                currentMode = GameMode::MAIN_MENU;
                if (!mainMenu) {
                    createMainMenu(false);
                }
            } else {
                currentMode = navigationHistory.back();
                navigationHistory.pop_back();
            }

            sessionCursorCaptured = false;
            display::enableCursorPreservingPosition();
            armBackShortcutSuppression();
        };
        auto pushMenuMode = [&](GameMode nextMode) {
            navigationHistory.push_back(currentMode);
            currentMode = nextMode;
        };
        auto restoreMenuShell = [&]() {
            SetWindowTitle("Netcode Simulator");
            display::enableCursorPreservingPosition();
            sessionCursorCaptured = false;
            navigationHistory.clear();
            settingsMenu.reset();
            levelSelect.reset();
            multiplayerMenu.reset();
            levelEditor.reset();
            replayStudio.reset();
            characterEditor.reset();
            hostGameTarget = false;
            labStudyTarget = false;
            editorTarget = false;
            createMainMenu(true);
            suppressBackShortcutUntilRelease = true;
        };
        auto restoreRetryableMultiplayerSetup = [&](const std::string& statusMessage) {
            if (multiplayerMenu) {
                multiplayerMenu->setStatusMessage(statusMessage, false);
            }
            sessionFlow.reset();
            sessionCursorCaptured = false;
            display::enableCursorPreservingPosition();
        };
        auto returnToMainMenuFromSession = [&](const std::string& statusMessage) {
            menuStatusMessage = shouldSuppressMainMenuStatusMessage(statusMessage)
                ? std::string{}
                : statusMessage;
            sessionFlow.reset();
            currentMode = GameMode::MAIN_MENU;
            restoreMenuShell();
        };
        auto adoptSessionStart = [&](app::AppFlow::SessionStartResult& sessionStart) {
            sessionFlow = std::move(sessionStart.sessionFlow);
            sessionCursorCaptured = false;

            switch (app::AppFlow::routeFor(sessionStart)) {
                case app::AppFlow::ShellRoute::RuntimeSession: {
                    const GameMode launchedMode =
                        sessionStart.launchConfig.surface == net::SessionProductSurface::LabStudy
                            ? GameMode::FREE_GAME
                            : GameMode::MULTIPLAYER_SESSION;
                    if (launchedMode != currentMode) {
                        pushMenuMode(launchedMode);
                    } else {
                        currentMode = launchedMode;
                    }
                    return true;
                }
                case app::AppFlow::ShellRoute::LevelEditor:
                    if (currentMode != GameMode::LEVEL_EDITOR) {
                        pushMenuMode(GameMode::LEVEL_EDITOR);
                    }
                    sessionFlow.reset();
                    levelEditor =
                        std::make_unique<LevelEditor>(sessionStart.launchConfig.levelSlot);
                    return true;
                case app::AppFlow::ShellRoute::Replay:
                case app::AppFlow::ShellRoute::LevelSelect:
                case app::AppFlow::ShellRoute::MultiplayerSetup:
                case app::AppFlow::ShellRoute::None:
                    sessionFlow.reset();
                    return false;
            }
            sessionFlow.reset();
            return false;
        };
        auto startSession = [&](const net::SessionLaunchConfig& launchConfig) {
            app::AppFlow::SessionStartResult sessionStart =
                app::AppFlow::startSession(launchConfig);
            if (!sessionStart.started || !adoptSessionStart(sessionStart)) {
                returnToMainMenuFromSession(sessionStart.statusMessage);
                return false;
            }
            return true;
        };
        auto startLabStudySession = [&](int levelSlot) {
            return startSession(makeLabStudyLaunchConfig(levelSlot));
        };
        createMainMenu(false);
        while (running && !WindowShouldClose()) {
            display::updateWindowPolicy();
            const bool backShortcutDown = IsKeyDown(KEY_Q) || IsKeyDown(KEY_ESCAPE);
            if (suppressBackShortcutUntilRelease && !backShortcutDown) {
                suppressBackShortcutUntilRelease = false;
            }
            const bool allowBackShortcut = !suppressBackShortcutUntilRelease;
            float dt = GetFrameTime();

            switch (currentMode) {
                case GameMode::MAIN_MENU: {
                    if (!mainMenu) {
                        createMainMenu(false);
                    }

                    GameMode nextMode = mainMenu->update(allowBackShortcut);
                    if (mainMenu->requestedExternalLink() != MainMenu::ExternalLinkTarget::None) {
                        openExternalLinkTarget(mainMenu->requestedExternalLink());
                        mainMenu->clearRequestedExternalLink();
                    }
                    if (nextMode != GameMode::MAIN_MENU) {
                        const MainMenu::AppShellSurface requestedSurface =
                            mainMenu->requestedSurface();
                        const net::SessionEntryPoint requestedEntryPoint =
                            mainMenu->requestedEntryPoint();
                        const app::AppFlow::ShellRoute requestedRoute =
                            app::AppFlow::routeForRequestedEntryPoint(requestedEntryPoint);
                        const net::SessionLaunchMode requestedSessionMode =
                            mainMenu->requestedSessionMode();
                        hostGameTarget = false;
                        labStudyTarget = false;
                        editorTarget = false;
                        if (requestedRoute == app::AppFlow::ShellRoute::LevelSelect &&
                            requestedEntryPoint == net::SessionEntryPoint::Host &&
                            requestedSessionMode == net::SessionLaunchMode::Host &&
                            nextMode == GameMode::LEVEL_SELECT) {
                            hostGameTarget = true;
                            labStudyTarget = false;
                            editorTarget = false;
                            pushMenuMode(GameMode::LEVEL_SELECT);
                        } else if (requestedRoute == app::AppFlow::ShellRoute::LevelSelect &&
                                   requestedSurface == MainMenu::AppShellSurface::LabStudy &&
                                   requestedEntryPoint == net::SessionEntryPoint::LabStudy &&
                                   nextMode == GameMode::LEVEL_SELECT) {
                            labStudyTarget = true;
                            hostGameTarget = false;
                            editorTarget = false;
                            pushMenuMode(GameMode::LEVEL_SELECT);
                        } else if (requestedRoute == app::AppFlow::ShellRoute::LevelSelect &&
                                   requestedSurface == MainMenu::AppShellSurface::LevelEditor &&
                                   requestedEntryPoint == net::SessionEntryPoint::LevelEditor &&
                                   nextMode == GameMode::LEVEL_SELECT) {
                            editorTarget = true;
                            hostGameTarget = false;
                            labStudyTarget = false;
                            pushMenuMode(GameMode::LEVEL_SELECT);
                        } else if (nextMode == GameMode::MULTIPLAYER_SESSION) {
                            hostGameTarget = false;
                            labStudyTarget = false;
                            editorTarget = false;
                            pushMenuMode(GameMode::MULTIPLAYER_SESSION);
                        } else if (requestedRoute == app::AppFlow::ShellRoute::Replay ||
                                   nextMode == GameMode::REPLAY_STUDIO) {
                            hostGameTarget = false;
                            labStudyTarget = false;
                            editorTarget = false;
                            pushMenuMode(GameMode::REPLAY_STUDIO);
                        } else if (nextMode == GameMode::CHARACTER_EDITOR) {
                            hostGameTarget = false;
                            labStudyTarget = false;
                            editorTarget = false;
                            pushMenuMode(GameMode::CHARACTER_EDITOR);
                        } else if (nextMode == GameMode::SETTINGS) {
                            pushMenuMode(GameMode::SETTINGS);
                        } else {
                            currentMode = nextMode;
                        }

                        mainMenu->clearRequestedNavigation();
                        mainMenu->clearRequestedSessionMode();
                        mainMenu->clearRequestedExternalLink();

                        // Initialize next state
                        if (currentMode == GameMode::LEVEL_SELECT) {
                            if (hostGameTarget) {
                                levelSelect = std::make_unique<LevelSelectMenu>(
                                    "SELECT HOST LEVEL",
                                    "");
                            } else if (labStudyTarget) {
                                levelSelect = std::make_unique<LevelSelectMenu>(
                                    "SELECT LAB STUDY LEVEL",
                                    "Pick a lab study map | Number keys 1-9 | Q/ESC to return");
                            } else if (editorTarget) {
                                levelSelect = std::make_unique<LevelSelectMenu>(
                                    "SELECT EDITOR SLOT",
                                    "Pick a slot to edit | Number keys 1-9 | Q/ESC to return");
                            } else {
                                levelSelect = std::make_unique<LevelSelectMenu>();
                            }
                        } else if (currentMode == GameMode::MULTIPLAYER_SESSION) {
                            multiplayerMenu = std::make_unique<MultiplayerSessionMenu>(
                                requestedSessionMode == net::SessionLaunchMode::Join
                                    ? net::SessionLaunchMode::Join
                                    : net::SessionLaunchMode::Host);
                            if (requestedSessionMode == net::SessionLaunchMode::Host) {
                                multiplayerMenu->setSelectedLevelSlot(selectedLevel);
                            }
                        } else if (currentMode == GameMode::SETTINGS) {
                            settingsMenu = std::make_unique<SettingsMenu>(userSettings);
                        } else if (currentMode == GameMode::REPLAY_STUDIO) {
                            replayStudio = std::make_unique<ReplayStudio>();
                        } else if (currentMode == GameMode::CHARACTER_EDITOR) {
                            characterEditor = std::make_unique<CharacterEditorScreen>();
                        } else if (currentMode == GameMode::QUIT) {
                            running = false;
                        }
                    } else {
                        renderFrame([&]() { mainMenu->render(); });
                    }
                    break;
                }

                case GameMode::SETTINGS: {
                    if (!settingsMenu) {
                        settingsMenu = std::make_unique<SettingsMenu>(userSettings);
                    }

                    const GameMode nextMode = settingsMenu->update(allowBackShortcut);
                    if (settingsMenu->consumeApplyRequested()) {
                        userSettings = settingsMenu->userSettings();
                        if (!userSettingsStore.save(userSettings)) {
                            settingsMenu->setStatusMessage(
                                "Applied settings, but failed to persist controls to repo .netcodesim.");
                        }
                    }
                    if (nextMode != GameMode::SETTINGS) {
                        if (nextMode == GameMode::MAIN_MENU) {
                            popMenuMode();
                        } else {
                            currentMode = nextMode;
                        }
                    } else {
                        renderFrame([&]() { settingsMenu->render(); });
                    }
                    break;
                }

                case GameMode::LEVEL_SELECT: {
                    if (!levelSelect) {
                        levelSelect = std::make_unique<LevelSelectMenu>();
                    }

                    auto result = levelSelect->update(allowBackShortcut);
                    if (result.mode != GameMode::LEVEL_SELECT) {
                        selectedLevel = result.levelSlot;
                        if (result.mode == GameMode::MAIN_MENU) {
                            popMenuMode();
                        } else if (hostGameTarget &&
                                   result.mode == GameMode::GAMEPLAY &&
                                   selectedLevel > 0) {
                            pushMenuMode(GameMode::MULTIPLAYER_SESSION);
                            multiplayerMenu = std::make_unique<MultiplayerSessionMenu>(
                                net::SessionLaunchMode::Host);
                            multiplayerMenu->setSelectedLevelSlot(selectedLevel);
                        } else if (labStudyTarget &&
                                   result.mode == GameMode::GAMEPLAY &&
                                   selectedLevel > 0) {
                            if (!startLabStudySession(selectedLevel)) {
                                break;
                            }
                        } else if (editorTarget &&
                                   result.mode == GameMode::GAMEPLAY &&
                                   selectedLevel > 0) {
                            if (!startSession(net::makeEditorSessionLaunchConfig(selectedLevel))) {
                                break;
                            }
                        } else {
                            currentMode = result.mode;
                        }
                    } else {
                        renderFrame([&]() { levelSelect->render(); });
                    }
                    break;
                }

                case GameMode::MULTIPLAYER_SESSION: {
                    if (!multiplayerMenu) {
                        multiplayerMenu = std::make_unique<MultiplayerSessionMenu>(
                            net::SessionLaunchMode::Join);
                        if (!menuStatusMessage.empty()) {
                            multiplayerMenu->setStatusMessage(menuStatusMessage, false);
                        }
                    }

                    if (sessionFlow) {
                        if (sessionFlow->state() == net::SessionFlowState::Running) {
                            if (!sessionCursorCaptured) {
                                display::disableCursorForCapture();
                                sessionCursorCaptured = true;
                            }

                            InputHandler3D::InputState input =
                                InputHandler3D::poll(userSettings.controls);
                            if (!allowBackShortcut) {
                                input.quit = false;
                            }
                            sessionFlow->update(dt, &input);

                            if (sessionFlow->shouldReturnToMainMenu()) {
                                returnToMainMenuFromSession(sessionFlow->statusMessage());
                            } else {
                                renderFrame([&]() { sessionFlow->render(); });
                            }
                            break;
                        }

                        multiplayerMenu->setStatusMessage(sessionFlow->statusMessage(), true);
                        const MultiplayerSessionMenu::Result pendingSetupResult =
                            multiplayerMenu->update(allowBackShortcut);
                        if (pendingSetupResult.mode == GameMode::MAIN_MENU) {
                            sessionFlow.reset();
                            popMenuMode();
                            break;
                        }

                        sessionFlow->update(dt, nullptr);
                        if (sessionFlow->state() == net::SessionFlowState::Failed) {
                            restoreRetryableMultiplayerSetup(sessionFlow->statusMessage());
                        } else if (sessionFlow->shouldReturnToMainMenu()) {
                            returnToMainMenuFromSession(sessionFlow->statusMessage());
                        } else if (sessionFlow->state() == net::SessionFlowState::Running) {
                            if (!sessionCursorCaptured) {
                                display::disableCursorForCapture();
                                sessionCursorCaptured = true;
                            }
                            renderFrame([&]() { sessionFlow->render(); });
                        } else {
                            multiplayerMenu->setStatusMessage(sessionFlow->statusMessage(), true);
                            renderFrame([&]() { multiplayerMenu->render(); });
                        }
                    } else {
                        auto result = multiplayerMenu->update(allowBackShortcut);
                        if (result.mode != GameMode::MULTIPLAYER_SESSION || result.submitted) {
                            if (result.submitted) {
                                app::AppFlow::SessionStartResult sessionStart =
                                    app::AppFlow::startSession(result.launchConfig);
                                if (!sessionStart.started || !adoptSessionStart(sessionStart)) {
                                    restoreRetryableMultiplayerSetup(sessionStart.statusMessage);
                                } else {
                                    multiplayerMenu->setStatusMessage(sessionStart.statusMessage, true);
                                }
                                break;
                            }

                            if (result.mode == GameMode::MAIN_MENU) {
                                popMenuMode();
                            } else {
                                currentMode = result.mode;
                            }
                        } else {
                            renderFrame([&]() { multiplayerMenu->render(); });
                        }
                    }
                    break;
                }

                case GameMode::LEVEL_EDITOR: {
                    GameMode nextMode = levelEditor->update(dt, allowBackShortcut);
                    if (nextMode != GameMode::LEVEL_EDITOR) {
                        if (nextMode == GameMode::MAIN_MENU) {
                            popMenuMode();
                        } else {
                            currentMode = nextMode;
                        }
                    } else {
                        renderFrame([&]() { levelEditor->render(); });
                    }
                    break;
                }

                case GameMode::REPLAY_STUDIO: {
                    if (!replayStudio) {
                        replayStudio = std::make_unique<ReplayStudio>();
                    }

                    const GameMode nextMode = replayStudio->update(dt, allowBackShortcut);
                    if (nextMode != GameMode::REPLAY_STUDIO) {
                        if (nextMode == GameMode::MAIN_MENU) {
                            popMenuMode();
                        } else {
                            currentMode = nextMode;
                        }
                    } else {
                        renderFrame([&]() { replayStudio->render(); });
                    }
                    break;
                }

                case GameMode::CHARACTER_EDITOR: {
                    if (!characterEditor) {
                        characterEditor = std::make_unique<CharacterEditorScreen>();
                    }

                    const GameMode nextMode = characterEditor->update(dt, allowBackShortcut);
                    if (nextMode != GameMode::CHARACTER_EDITOR) {
                        if (nextMode == GameMode::MAIN_MENU) {
                            popMenuMode();
                        } else {
                            currentMode = nextMode;
                        }
                    } else {
                        renderFrame([&]() { characterEditor->render(); });
                    }
                    break;
                }

                case GameMode::FREE_GAME: {
                    if (!sessionFlow) {
                        if (!navigationHistory.empty()) {
                            popMenuMode();
                        } else {
                            currentMode = GameMode::MAIN_MENU;
                            restoreMenuShell();
                        }
                        break;
                    }

                    if (sessionFlow->state() == net::SessionFlowState::Running) {
                        if (!sessionCursorCaptured) {
                            display::disableCursorForCapture();
                            sessionCursorCaptured = true;
                        }

                        InputHandler3D::InputState input =
                            InputHandler3D::poll(userSettings.controls);
                        if (!allowBackShortcut) {
                            input.quit = false;
                        }
                        sessionFlow->update(dt, &input);

                        if (sessionFlow->shouldReturnToMainMenu()) {
                            returnToMainMenuFromSession(sessionFlow->statusMessage());
                        } else {
                            renderFrame([&]() { sessionFlow->render(); });
                        }
                        break;
                    }

                    const bool backPressed =
                        allowBackShortcut &&
                        (IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_ESCAPE));
                    if (backPressed) {
                        sessionFlow.reset();
                        popMenuMode();
                        break;
                    }

                    sessionFlow->update(dt, nullptr);
                    if (sessionFlow->state() == net::SessionFlowState::Failed ||
                        sessionFlow->shouldReturnToMainMenu()) {
                        returnToMainMenuFromSession(sessionFlow->statusMessage());
                    } else if (sessionFlow->state() == net::SessionFlowState::Running) {
                        if (!sessionCursorCaptured) {
                            display::disableCursorForCapture();
                            sessionCursorCaptured = true;
                        }
                        renderFrame([&]() { sessionFlow->render(); });
                    } else {
                        renderFrame([&]() { sessionFlow->render(); });
                    }
                    break;
                }

                case GameMode::QUIT: {
                    running = false;
                    break;
                }

                default: {
                    currentMode = GameMode::MAIN_MENU;
                    restoreMenuShell();
                    break;
                }
            }
        }

        // Cleanup
        display::shutdownWindow();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Thank you for using Netcode Simulator!" << std::endl;
    return 0;
}
