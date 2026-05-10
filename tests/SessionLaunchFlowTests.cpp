#include "app/AppFlow.hpp"
#include "app/SessionComposer.hpp"
#include "MultiplayerSessionMenu.hpp"
#include "LevelData.hpp"
#include "TestDataRoot.hpp"
#include "net/Protocol.hpp"
#include "net/SessionDiscovery.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/SessionFlowController.hpp"
#include "net/ServerRuntime.hpp"
#include "net/UdpSocket.hpp"

#include <cmath>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::uint16_t kSessionFlowDiscoveryPortBase = 48200u;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool containsLine(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::filesystem::path findRepoRoot() {
    std::filesystem::path probe = std::filesystem::current_path();
    while (!probe.empty()) {
        if (std::filesystem::exists(probe / "CMakeLists.txt") &&
            std::filesystem::exists(probe / "src/main_3d.cpp")) {
            return probe;
        }
        if (probe == probe.root_path()) {
            break;
        }
        probe = probe.parent_path();
    }

    throw std::runtime_error("failed to locate repository root for lifecycle source assertions");
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    expect(file.is_open(), "expected to open source fixture: " + path.string());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0u;
    }

    std::size_t count = 0u;
    std::size_t cursor = 0u;
    while ((cursor = text.find(needle, cursor)) != std::string::npos) {
        ++count;
        cursor += needle.size();
    }
    return count;
}

const sim::RosterEntry& requireRosterEntry(const std::vector<sim::RosterEntry>& roster,
                                           int actorId) {
    for (const auto& entry : roster) {
        if (entry.actorId == actorId) {
            return entry;
        }
    }
    throw std::runtime_error("roster entry missing from session-flow snapshot");
}

template <typename Predicate>
bool waitForPredicate(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

template <typename Predicate>
bool waitForHostedClientPredicate(net::SessionFlowController* host,
                                  net::ClientRuntime* client,
                                  Predicate predicate,
                                  std::chrono::milliseconds timeout) {
    expect(host != nullptr, "host session controller is required");
    expect(client != nullptr, "client runtime is required");

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        host->update(1.0f / 60.0f, nullptr);
        client->update(1.0f / 60.0f, nullptr);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

template <typename Predicate>
bool waitForSessionFlowPredicate(net::SessionFlowController* host,
                                 net::SessionFlowController* join,
                                 Predicate predicate,
                                 std::chrono::milliseconds timeout) {
    expect(host != nullptr, "host session controller is required");
    expect(join != nullptr, "join session controller is required");

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        host->update(1.0f / 60.0f, nullptr);
        join->update(1.0f / 60.0f, nullptr);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return predicate();
}

template <typename Predicate>
bool waitForDiscoveryBrowserPredicate(net::SessionFlowController* host,
                                      MultiplayerSessionMenu* menu,
                                      Predicate predicate,
                                      std::chrono::milliseconds timeout) {
    expect(host != nullptr, "host session controller is required");
    expect(menu != nullptr, "multiplayer session menu is required");

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        menu->tickDiscoveryForTest();
        host->update(1.0f / 60.0f, nullptr);
        menu->tickDiscoveryForTest();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return predicate();
}

net::SessionLaunchConfig submitAfterTeamChoice(MultiplayerSessionMenu* menu,
                                               const std::string& context) {
    expect(menu != nullptr, "multiplayer session menu is required");

    const MultiplayerSessionMenu::Result firstStep = menu->submitForTest();
    expect(!firstStep.submitted &&
               menu->step() == MultiplayerSessionMenu::Step::TeamChoice,
           context + " should advance to explicit team choice before launch");

    const sim::TeamId selectedTeam = menu->preferredTeam();
    const MultiplayerSessionMenu::Result selectionResult =
        menu->chooseTeamForTest(selectedTeam);
    expect(!selectionResult.submitted &&
               menu->preferredTeam() == selectedTeam &&
               menu->step() == MultiplayerSessionMenu::Step::TeamChoice,
           context + " should preserve the selected team until the launch action is confirmed");

    const MultiplayerSessionMenu::Result confirmed = menu->submitForTest();
    expect(confirmed.submitted && confirmed.launchConfig.preferredTeam == selectedTeam,
           context + " should submit after the primary launch action confirms the selected team");
    return confirmed.launchConfig;
}

net::ProxyStats deltaStats(const net::ProxyStats& after, const net::ProxyStats& before);

bool waitForDiscoveryAdvertisement(net::SessionFlowController* host,
                                   net::UdpSocket* socket,
                                   net::SessionAdvertisement* advertisementOut,
                                   std::chrono::milliseconds timeout) {
    expect(host != nullptr, "host session controller is required");
    expect(socket != nullptr, "discovery socket is required");
    expect(advertisementOut != nullptr, "advertisement output is required");

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        host->update(1.0f / 60.0f, nullptr);

        net::ReceivedDatagram datagram;
        if (socket->receive(&datagram) != net::ReceiveStatus::Received) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        const net::SessionAdvertisementParseResult parsed =
            net::deserializeSessionAdvertisement(datagram.payload);
        if (!parsed.ok) {
            continue;
        }

        *advertisementOut = parsed.advertisement;
        return true;
    }

    return false;
}

void ensureTestLevelExists(int slot) {
    LevelData::Level level("Session Flow Level " + std::to_string(slot));
    level.obstacles.push_back(LevelData::Obstacle{
        static_cast<float>(slot),
        -static_cast<float>(slot),
        6.0f,
        3.0f,
        2.5f,
        Color{120, 200, 120, 255}
    });
    expect(LevelData::saveLevel(level, slot),
           "session-flow test level fixture should save successfully");
}

LevelData::LevelDefinition makePortableLevelFixture(int slot) {
    LevelData::LevelDefinition level("Portable Level " + std::to_string(slot));
    level.floorColor = Color{45, 55, 65, 255};
    level.obstacles.push_back(LevelData::Obstacle{
        static_cast<float>(slot),
        -static_cast<float>(slot),
        6.0f,
        3.0f,
        2.5f,
        Color{120, 200, 120, 255}
    });
    level.areas.push_back(LevelData::Area{
        static_cast<float>(slot) * 0.5f,
        static_cast<float>(slot) * 0.25f,
        5.0f,
        7.0f,
        Color{80, 90, 160, 180}
    });
    level.enemies.push_back(LevelData::EnemySpawn{
        -static_cast<float>(slot) * 0.75f,
        static_cast<float>(slot) * 0.5f,
        Color{200, 80, 80, 255}
    });
    return level;
}

void testLoadingSameSavedLevelSlotYieldsDeterministicSchema() {
    constexpr int kSlot = 3;
    const LevelData::LevelDefinition expected = makePortableLevelFixture(kSlot);
    expect(LevelData::saveLevel(expected, kSlot),
           "portable level fixture should save to the shared slot path");

    LevelData::LevelDefinition firstLoad;
    LevelData::LevelDefinition secondLoad;
    expect(LevelData::loadLevel(firstLoad, kSlot),
           "shared level data should load the first copy from the saved slot");
    expect(LevelData::loadLevel(secondLoad, kSlot),
           "shared level data should load the second copy from the saved slot");
    expect(firstLoad == expected && secondLoad == expected,
           "loading the same saved slot should preserve the full portable level schema");
    expect(LevelData::schemaFingerprint(firstLoad) == LevelData::schemaFingerprint(secondLoad) &&
               LevelData::schemaFingerprint(firstLoad) == LevelData::schemaFingerprint(expected),
           "loading the same saved slot should yield the same deterministic schema fingerprint every time");
}

void testHostedLevelLoadingExposesEditorEnemiesAsAuthoritativeBotSpawns() {
    constexpr int kSlot = 8;
    LevelData::LevelDefinition level("Hosted Bot Spawn Fixture");
    level.obstacles.push_back(LevelData::Obstacle{
        2.0f,
        -2.0f,
        4.0f,
        3.0f,
        2.0f,
        Color{120, 200, 120, 255}
    });
    level.enemies.push_back(LevelData::EnemySpawn{-9.0f, -7.0f, Color{255, 80, 80, 255}});
    level.enemies.push_back(LevelData::EnemySpawn{0.0f, 4.0f, Color{80, 160, 255, 255}});
    level.enemies.push_back(LevelData::EnemySpawn{9.0f, 8.0f, Color{120, 255, 120, 255}});
    expect(LevelData::saveLevel(level, kSlot),
           "hosted bot spawn fixture should save successfully");

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(kSlot, "editor-bot-host", 45280u, 0u, 0u);
    sim::MovementEnvironment environment;
    std::vector<sim::Vec3> authoredBotSpawns;
    std::string error;
    expect(app::buildHostedMovementEnvironment(config,
                                               &environment,
                                               &error,
                                               &authoredBotSpawns),
           "hosted level loading should succeed for a level-editor bot fixture");

    expect(environment.collisionBoxes.size() == 1u,
           "hosted level loading should still expose authored obstacle collision");
    expect(authoredBotSpawns.size() == 3u &&
               authoredBotSpawns[0].x == -9.0f &&
               authoredBotSpawns[1].z == 4.0f &&
               authoredBotSpawns[2].x == 9.0f,
           "hosted level loading should pass level-editor enemies to the authoritative bot spawn path");
}

void testHostedEditorPlacedCharactersBecomeReplicatedTeamBots() {
    constexpr int kSlot = 8;
    LevelData::LevelDefinition level("Hosted Editor Bot Runtime Fixture");
    level.enemies.push_back(LevelData::EnemySpawn{-7.0f, -4.0f, Color{255, 80, 80, 255}});
    level.enemies.push_back(LevelData::EnemySpawn{5.0f, 6.0f, Color{80, 160, 255, 255}});
    expect(LevelData::saveLevel(level, kSlot),
           "hosted editor bot runtime fixture should save successfully");

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(kSlot,
                                         "editor-bot-host",
                                         45281u,
                                         0u,
                                         0u,
                                         0u,
                                         net::kDefaultProxyServerPort,
                                         net::kProtocolVersion,
                                         sim::TeamId::Attacker);
    config.clientSessionId = 0x5A000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted editor bot runtime should start successfully");

    const bool replicated = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        const net::ServerRuntime* server = controller.hostedServer();
        const net::ClientRuntime* client = controller.clientRuntime();
        if (controller.state() != net::SessionFlowState::Running ||
            server == nullptr ||
            client == nullptr ||
            !client->hasSnapshot()) {
            return false;
        }

        std::size_t serverBotCount = 0u;
        std::size_t clientBotCount = 0u;
        for (const auto& entry : server->worldState().roster) {
            if (entry.isBot && sim::isPlayableTeam(entry.team)) {
                ++serverBotCount;
            }
        }
        for (const auto& entry : client->roster()) {
            if (entry.isBot && sim::isPlayableTeam(entry.team)) {
                ++clientBotCount;
            }
        }

        const client::ClientViewState viewState = client->clientViewState();
        std::size_t replicatedBotViews = 0u;
        for (const auto& remote : viewState.remotePlayers) {
            const sim::RosterEntry& rosterEntry =
                requireRosterEntry(client->roster(), remote.actorId);
            if (rosterEntry.isBot && sim::isPlayableTeam(remote.team)) {
                ++replicatedBotViews;
            }
        }

        return serverBotCount == 2u &&
               clientBotCount == 2u &&
               replicatedBotViews == 2u &&
               server->worldState().enemies.empty() &&
               client->latestSnapshot() != nullptr &&
               client->latestSnapshot()->remoteEnemies.empty();
    }, std::chrono::milliseconds(1200));
    expect(replicated,
           "editor-placed characters should become replicated authoritative bots with playable teams");

    const net::ServerRuntime* server = controller.hostedServer();
    expect(server != nullptr &&
               requireRosterEntry(server->worldState().roster, 1000).team == sim::TeamId::Defender &&
               requireRosterEntry(server->worldState().roster, 1001).team == sim::TeamId::Attacker,
           "hosted editor bots should balance against the attack-side host instead of remaining unassigned");
}

void testHostedEditorLevelWithoutCharactersDoesNotKeepLegacyEnemy() {
    constexpr int kSlot = 1;
    LevelData::LevelDefinition level("Hosted Empty Editor Runtime Fixture");
    level.obstacles.push_back(LevelData::Obstacle{
        1.0f,
        -1.0f,
        6.0f,
        3.0f,
        2.5f,
        Color{120, 200, 120, 255}
    });
    expect(LevelData::saveLevel(level, kSlot),
           "hosted empty editor runtime fixture should save successfully");

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(kSlot, "empty-editor-host", 45282u, 0u, 0u);
    config.clientSessionId = 0x5A000002u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted empty editor runtime should start successfully");

    const bool replicated = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        const net::ServerRuntime* server = controller.hostedServer();
        const net::ClientRuntime* client = controller.clientRuntime();
        return controller.state() == net::SessionFlowState::Running &&
               server != nullptr &&
               client != nullptr &&
               client->hasSnapshot() &&
               server->worldState().enemies.empty() &&
               client->latestSnapshot() != nullptr &&
               client->latestSnapshot()->remoteEnemies.empty() &&
               client->roster().size() == 1u;
    }, std::chrono::milliseconds(1200));
    expect(replicated,
           "hosted editor levels without placed characters should not keep the legacy unassigned enemy actor");
}

void testEditorOwnedLevelSaveAndLoadRemainPortableToSessionStartup() {
    constexpr int kSlot = 4;
    const LevelData::LevelDefinition expected = makePortableLevelFixture(kSlot);
    expect(LevelData::saveLevelDefinition(expected, LevelData::getLevelPath(kSlot)),
           "editor-style explicit-path level save should persist through the shared LevelDefinition seam");

    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string editorSource = readTextFile(repoRoot / "include/LevelEditor.hpp");
    const std::string clientRuntimeSource = readTextFile(repoRoot / "src/ClientRuntime.cpp");
    expect(editorSource.find("const LevelData::LevelDefinition& portableLevelDefinition() const") !=
               std::string::npos &&
               editorSource.find("void applyPortableLevelDefinition(const LevelData::LevelDefinition& level,") !=
                   std::string::npos &&
               editorSource.find("bool savePortableLevelDefinition(int slot)") != std::string::npos &&
               editorSource.find("bool loadPortableLevelDefinition(int slot)") != std::string::npos,
           "level editor should expose explicit portable LevelDefinition seams instead of hiding editor state behind a second schema path");
    expect(clientRuntimeSource.find("LevelData::loadLevel(level, levelSlot)") != std::string::npos &&
               clientRuntimeSource.find("arena->loadLevel(level);") != std::string::npos,
           "shared runtime session startup should continue loading authored level slots through the shared LevelData seam");

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(kSlot, "portable-host", 45270u, 0u, 0u);
    config.clientSessionId = 0x1D000001u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(),
           "shared runtime session startup should begin successfully from the authored level slot");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running,
           "shared runtime session startup should reach running state from the authored level slot");
    expect(controller.clientRuntime() != nullptr &&
               controller.clientRuntime()->authoritativeLevelSlot() == kSlot &&
               controller.clientRuntime()->authoritativeLevelHash() == net::makeLevelIdentityHash(kSlot),
           "shared runtime session startup should preserve the authored level slot identity for the connected client");

    LevelData::LevelDefinition reloaded;
    expect(LevelData::loadLevelDefinition(reloaded, LevelData::getLevelPath(kSlot)),
           "shared LevelDefinition loads should still succeed after session startup uses the same slot");
    expect(reloaded == expected,
           "editor-owned explicit-path persistence should remain portable to shared runtime startup without schema drift");
}

void testMainMenuRoutesFirstClassSurfacesThroughTypedSelection() {
    MainMenu menu;

    expect(menu.requestedSurface() == MainMenu::AppShellSurface::None &&
               menu.requestedEntryPoint() == net::SessionEntryPoint::None,
           "main menu should start without a pending product-surface selection");

    expect(menu.triggerOptionForTest(1) == GameMode::LEVEL_SELECT,
           "lab-study surface selection should route through level selection instead of entering runtime directly");
    expect(menu.requestedSurface() == MainMenu::AppShellSurface::LabStudy &&
               menu.requestedEntryPoint() == net::SessionEntryPoint::LabStudy,
           "lab-study root selection should preserve a typed lab-study surface and entry point");
    menu.clearRequestedNavigation();

    expect(menu.triggerOptionForTest(2) == GameMode::LEVEL_SELECT,
           "level-editor surface selection should route through level selection instead of entering the editor directly");
    expect(menu.requestedSurface() == MainMenu::AppShellSurface::LevelEditor &&
               menu.requestedEntryPoint() == net::SessionEntryPoint::LevelEditor,
           "level-editor root selection should preserve a typed editor surface and entry point");
    menu.clearRequestedNavigation();

    expect(menu.triggerOptionForTest(3) == GameMode::REPLAY_STUDIO,
           "replay studio should be a first-class top-level shell surface");
    expect(menu.requestedSurface() == MainMenu::AppShellSurface::ReplayStudio &&
               menu.requestedEntryPoint() == net::SessionEntryPoint::Replay,
           "replay-studio root selection should preserve a typed replay surface and entry point");
    menu.clearRequestedNavigation();

    expect(menu.triggerOptionForTest(4) == GameMode::SETTINGS,
           "settings should remain a first-class top-level shell surface");
    expect(menu.requestedSurface() == MainMenu::AppShellSurface::Settings &&
               menu.requestedEntryPoint() == net::SessionEntryPoint::None,
           "settings selection should remain distinct from session entry-point routing");

    expect(menu.triggerOptionForTest(0) == GameMode::MAIN_MENU,
           "multiplayer should still open its submenu in place");
    expect(menu.requestedSurface() == MainMenu::AppShellSurface::Multiplayer &&
               menu.requestedEntryPoint() == net::SessionEntryPoint::None,
           "multiplayer root selection should preserve a typed multiplayer surface before host or join selection");
}

void testAppFlowRoutesTopLevelShellSelections() {
    expect(app::AppFlow::routeForRequestedEntryPoint(net::SessionEntryPoint::Host) ==
               app::AppFlow::ShellRoute::LevelSelect &&
               app::AppFlow::routeForRequestedEntryPoint(net::SessionEntryPoint::LabStudy) ==
               app::AppFlow::ShellRoute::LevelSelect &&
               app::AppFlow::routeForRequestedEntryPoint(net::SessionEntryPoint::LevelEditor) ==
               app::AppFlow::ShellRoute::LevelSelect,
           "AppFlow should route hosted, study, and editor entry points through level selection in the top-level shell");
    expect(app::AppFlow::routeForRequestedEntryPoint(net::SessionEntryPoint::Join) ==
               app::AppFlow::ShellRoute::MultiplayerSetup &&
               app::AppFlow::routeForRequestedEntryPoint(net::SessionEntryPoint::Replay) ==
               app::AppFlow::ShellRoute::Replay,
           "AppFlow should route join and replay entry points through typed shell routes");

    app::AppFlow::SessionStartResult runtimeStart;
    runtimeStart.target = app::AppFlow::LaunchTarget::RuntimeSession;
    expect(app::AppFlow::routeFor(runtimeStart) == app::AppFlow::ShellRoute::RuntimeSession,
           "AppFlow session results should expose runtime sessions through a shared shell route");

    app::AppFlow::SessionStartResult editorStart;
    editorStart.target = app::AppFlow::LaunchTarget::LevelEditor;
    expect(app::AppFlow::routeFor(editorStart) == app::AppFlow::ShellRoute::LevelEditor,
           "AppFlow session results should expose editor entry through a shared shell route");
}

void testAppShellOwnsWindowLifecycleCalls() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string editorSource = readTextFile(repoRoot / "include/LevelEditor.hpp");
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");

    expect(editorSource.find("display::initWindow(") == std::string::npos,
           "level editor should not initialize the display window directly");
    expect(editorSource.find("display::shutdownWindow(") == std::string::npos,
           "level editor should not shut down the display window directly");

    expect(countOccurrences(shellSource, "display::initWindow(") == 2u,
           "main app shell should be the only remaining owner of display initialization calls");
    expect(countOccurrences(shellSource, "display::shutdownWindow(") == 3u,
           "main app shell should be the only remaining owner of display shutdown calls");
    expect(shellSource.find("auto restoreRetryableMultiplayerSetup = [&](const std::string& statusMessage)") != std::string::npos,
           "main app shell should define an explicit retryable multiplayer-setup recovery helper");
    expect(shellSource.find("auto returnToMainMenuFromSession = [&](const std::string& statusMessage)") != std::string::npos,
           "main app shell should define an explicit session-end return-to-menu helper");
    expect(countOccurrences(shellSource, "restoreRetryableMultiplayerSetup(") == 2u,
           "failed session startup and failed join-runtime paths should restore the retryable multiplayer setup");
    expect(shellSource.find("restoreRetryableMultiplayerSetup(\"session cancelled\")") == std::string::npos,
           "startup-pending cancel should no longer strand the user on the same multiplayer setup screen");
    expect(shellSource.find("if (pendingSetupResult.mode == GameMode::MAIN_MENU) {\n                            sessionFlow.reset();\n                            popMenuMode();") != std::string::npos,
           "startup-pending cancel should tear down the pending session flow and pop back to the previous menu");
    expect(countOccurrences(shellSource, "multiplayerMenu->update(allowBackShortcut)") == 2u,
           "main app shell should keep the multiplayer setup interactive in both idle and startup-pending states so the visible Back action remains clickable");
    expect(countOccurrences(shellSource, "returnToMainMenuFromSession(") == 5u,
           "shared session shutdown paths should still funnel terminal runtime exits back through the main-menu recovery helper");
}

void testMainShellComposesStudyModeThroughSharedSessionFlow() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");
    const std::string sessionFlowSource = readTextFile(repoRoot / "src/SessionFlowController.cpp");

    expect(shellSource.find("labStudyTarget = true;") != std::string::npos &&
               shellSource.find("? GameMode::FREE_GAME") != std::string::npos &&
               shellSource.find("pushMenuMode(launchedMode);") != std::string::npos,
           "main app shell should keep routing the level-select study path into FREE_GAME mode while now pushing that runtime onto the shared menu history stack");
    expect(shellSource.find("makeLabStudyLaunchConfig(levelSlot)") != std::string::npos,
           "main app shell should derive the study launch from an app-shell helper");
    expect(shellSource.find("net::makeStudySessionLaunchConfig(levelSlot") != std::string::npos &&
               shellSource.find("net::makeHostSessionLaunchConfig(levelSlot") == std::string::npos,
           "main app shell helper should build study sessions through the dedicated Lab Study launch config instead of a generic host config");
    expect(shellSource.find("case GameMode::FREE_GAME:") != std::string::npos &&
               shellSource.find("sessionFlow->render();") != std::string::npos,
           "main app shell should drive study mode through the shared session controller");
    expect(sessionFlowSource.find("GameMode::FREE_GAME") == std::string::npos,
           "session flow controller should remain launch-agnostic rather than owning study-mode policy directly");
}

void testSessionFlowControllerDependsOnTransportArtifactBoundary() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string headerSource = readTextFile(repoRoot / "include/net/SessionFlowController.hpp");
    const std::string implementationSource = readTextFile(repoRoot / "src/SessionFlowController.cpp");
    const std::string composerSource = readTextFile(repoRoot / "include/app/SessionComposer.hpp");

    expect(headerSource.find("#include \"net/ProxyRuntime.hpp\"") == std::string::npos &&
               headerSource.find("#include \"net/TransportArtifactAdapter.hpp\"") != std::string::npos,
           "SessionFlowController.hpp should depend on the named transport boundary rather than directly including ProxyRuntime");
    expect(headerSource.find("std::unique_ptr<TransportArtifactAdapter> proxy_") != std::string::npos &&
               headerSource.find("std::unique_ptr<ProxyRuntime> proxy_") == std::string::npos,
           "SessionFlowController should store the hosted transport through TransportArtifactAdapter");
    expect(implementationSource.find("std::make_unique<ProxyRuntime>(proxyConfig)") == std::string::npos &&
               composerSource.find("std::make_unique<net::ProxyRuntime>(proxyConfig)") != std::string::npos,
           "SessionComposer should compose the existing ProxyRuntime implementation behind the transport boundary");
}

void testSessionComposerDescribesHostAndJoinRuntimeComposition() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(6, "composer-host", 45140, 1u, 2u);
    hostConfig.sessionLabel = "Composer Host";
    app::SessionComposer hostComposer(hostConfig);
    const app::SessionComposer::Result hostResult = hostComposer.compose();
    expect(hostResult.ok,
           "host session composition should succeed through SessionComposer without direct legacy-mode construction");
    expect(hostResult.composition.startupSequence_.size() == 3u &&
               hostResult.composition.startupSequence_[0] == "server" &&
               hostResult.composition.startupSequence_[1] == "proxy" &&
               hostResult.composition.startupSequence_[2] == "client",
           "host SessionComposer composition should describe server, proxy, and client startup deterministically");
    expect(hostResult.composition.hostedServer_ != nullptr &&
               hostResult.composition.proxy_ != nullptr &&
               hostResult.composition.client_ != nullptr,
           "host SessionComposer composition should include the composed runtime stack");
    expect(hostResult.composition.hostedServer_->config().authoredBotTeamBias ==
               hostConfig.preferredTeam,
           "host SessionComposer should pass the host team preference into authored bot balancing");
    expect(hostComposer.hostedSessionMetadata().sessionLabel == "Composer Host",
           "host SessionComposer should expose authoritative hosted-session metadata from launch configuration");

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1", 45140, "composer-join");
    app::SessionComposer joinComposer(joinConfig);
    const app::SessionComposer::Result joinResult = joinComposer.compose();
    expect(joinResult.ok,
           "join session composition should succeed through SessionComposer without direct legacy-mode construction");
    expect(joinResult.composition.startupSequence_.size() == 2u &&
               joinResult.composition.startupSequence_[0] == "proxy" &&
               joinResult.composition.startupSequence_[1] == "client",
           "join SessionComposer composition should describe proxy and client startup deterministically");
    expect(joinResult.composition.hostedServer_ == nullptr &&
               joinResult.composition.proxy_ != nullptr &&
               joinResult.composition.client_ != nullptr,
           "join SessionComposer composition should omit hosted server ownership while still composing proxy and client");
}

void testSessionLaunchConfigCapturesSurfaceParticipantCountAndStudyOptions() {
    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(4,
                                         "surface-host",
                                         45145u,
                                         1u,
                                         1u,
                                         0u,
                                         net::kDefaultProxyServerPort,
                                         net::kProtocolVersion,
                                         sim::TeamId::Attacker,
                                         2u,
                                         net::ShotEvaluationMode::LivePosition);
    expect(hostConfig.surface == net::SessionProductSurface::Multiplayer &&
               hostConfig.entryPoint == net::SessionEntryPoint::Host,
           "host launch config should preserve the multiplayer surface and explicit host entry point");
    expect(hostConfig.levelSlot == 4 &&
               hostConfig.levelHash == net::makeLevelIdentityHash(4) &&
               hostConfig.localParticipantCount == 2u &&
               hostConfig.tickRateHz == net::kDefaultSessionTickRateHz &&
               hostConfig.snapshotRateHz == net::kDefaultHostedSnapshotRateHz &&
               hostConfig.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "host launch config should preserve level identity, cadence defaults, local participant count, and shot strategy through deterministic construction");

    net::SessionLaunchConfig hostSpectatorConfig =
        net::makeHostSessionLaunchConfig(4, "surface-host-spectator", 45147u, 1u, 1u, 0u,
                                         net::kDefaultProxyServerPort, net::kProtocolVersion,
                                         sim::TeamId::Spectator);
    net::SessionLaunchConfig joinSpectatorConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1",
                                         45148u,
                                         "surface-join-spectator",
                                         net::kProtocolVersion,
                                         sim::TeamId::Spectator);
    expect(hostSpectatorConfig.preferredTeam == sim::TeamId::Spectator &&
               joinSpectatorConfig.preferredTeam == sim::TeamId::Spectator,
           "session launch helpers should preserve explicit spectator startup choices");

    net::SessionLaunchConfig studyConfig =
        net::makeStudySessionLaunchConfig(5, "study-host", 45146u, 1u, 1u);
    expect(studyConfig.surface == net::SessionProductSurface::LabStudy &&
               studyConfig.entryPoint == net::SessionEntryPoint::LabStudy &&
               studyConfig.mode == net::SessionLaunchMode::Host,
           "lab-study launch config should preserve a dedicated lab-study surface while still targeting the shared hosted runtime mode");
    expect(studyConfig.studyOptions.enablePredictionToggle &&
               studyConfig.studyOptions.enableShotStrategyToggle &&
               studyConfig.studyOptions.enableReplayCapture &&
               !studyConfig.studyOptions.enableEventLogging,
           "lab-study launch config should enable study controls while leaving event logging opt-in by default");

    net::SessionLaunchConfig replayConfig = net::makeReplaySessionLaunchConfig(6);
    net::SessionLaunchConfig editorConfig = net::makeEditorSessionLaunchConfig(7);
    expect(replayConfig.surface == net::SessionProductSurface::Replay &&
               replayConfig.entryPoint == net::SessionEntryPoint::Replay &&
               replayConfig.mode == net::SessionLaunchMode::None &&
               !replayConfig.startLocalServer &&
               !replayConfig.startLocalProxy,
           "replay launch config should preserve a non-runtime replay entry without reviving direct network ownership");
    expect(editorConfig.surface == net::SessionProductSurface::LevelEditor &&
               editorConfig.entryPoint == net::SessionEntryPoint::LevelEditor &&
               editorConfig.mode == net::SessionLaunchMode::None &&
               !editorConfig.startLocalServer &&
               !editorConfig.startLocalProxy,
           "editor launch config should preserve a non-runtime editor entry without reviving direct network ownership");
}

void testSessionComposerBuildsStudyReplayAndEditorEntryPoints() {
    ensureTestLevelExists(5);

    net::SessionLaunchConfig studyConfig =
        net::makeStudySessionLaunchConfig(5, "study-host", 45147u, 1u, 1u);
    app::SessionComposer studyComposer(studyConfig);
    const app::SessionComposer::Result studyResult = studyComposer.compose();
    expect(studyResult.ok,
           "lab-study launch composition should succeed through the shared SessionComposer seam");
    expect(studyResult.composition.startupSequence_.size() == 3u &&
               studyResult.composition.hostedServer_ != nullptr &&
               studyResult.composition.proxy_ != nullptr &&
               studyResult.composition.client_ != nullptr,
           "lab-study launch composition should still build server or proxy or client through the shared runtime seam");
    expect(studyResult.composition.config.surface == net::SessionProductSurface::LabStudy &&
               studyResult.composition.config.localParticipantCount == 1u &&
               studyResult.composition.config.studyOptions.enableReplayCapture &&
               !studyResult.composition.config.studyOptions.enableEventLogging &&
               studyResult.composition.config.studyEventRunId.empty(),
           "lab-study launch composition should preserve the shared surface or participant or study-option contract without pre-arming event logs");
    expect(studyResult.composition.hostedServer_->config().studyActionsEnabled &&
               studyResult.composition.client_->config().studyActionsEnabled &&
               !studyResult.composition.hostedServer_->config().studyEventLoggingEnabled &&
               !studyResult.composition.client_->config().studyEventLoggingEnabled,
           "lab-study launch composition should enable study actions while keeping event logging disabled until the host toggles it");

    net::SessionLaunchConfig replayConfig = net::makeReplaySessionLaunchConfig(6);
    app::SessionComposer replayComposer(replayConfig);
    const app::SessionComposer::Result replayResult = replayComposer.compose();
    expect(replayResult.ok,
           "replay launch composition should succeed through SessionComposer");
    expect(replayResult.composition.startupSequence_.empty() &&
               replayResult.composition.hostedServer_ == nullptr &&
               replayResult.composition.proxy_ == nullptr &&
               replayResult.composition.client_ == nullptr,
           "replay launch composition should route through the shared seam without composing a runtime stack");
    expect(replayResult.replayLayout.available &&
               replayResult.replayLayout.activePaneCount() == 1u &&
               replayResult.replayLayout.focusedSlot == sim::PaneSlot::Left &&
               replayResult.replayLayout.left.active &&
               replayResult.replayLayout.left.binding.kind == client::PaneBindingKind::ReplayCamera &&
               replayResult.replayLayout.left.binding.label == "Replay Camera A" &&
               !replayResult.replayLayout.right.active,
           "single-view replay launch composition should bind the default replay camera into the shared left slot model");
    expect(replayResult.replayPaneBindingTimeline.size() == 1u &&
               replayResult.replayPaneBindingTimeline.front().timestamp == 0.0f &&
               replayResult.replayPaneBindingTimeline.front().slot == sim::PaneSlot::Left &&
               replayResult.replayPaneBindingTimeline.front().bindingLabel == "Replay Camera A",
           "single-view replay launch composition should seed pane-binding metadata through the shared replay binding seam");

    net::SessionLaunchConfig splitReplayConfig = net::makeReplaySessionLaunchConfig(6, 2u);
    app::SessionComposer splitReplayComposer(splitReplayConfig);
    const app::SessionComposer::Result splitReplayResult = splitReplayComposer.compose();
    expect(splitReplayResult.ok,
           "split replay launch composition should succeed through SessionComposer");
    expect(splitReplayResult.replayLayout.available &&
               splitReplayResult.replayLayout.activePaneCount() == 2u &&
               splitReplayResult.replayLayout.left.slot == sim::PaneSlot::Left &&
               splitReplayResult.replayLayout.right.slot == sim::PaneSlot::Right &&
               splitReplayResult.replayLayout.left.binding.kind == client::PaneBindingKind::ReplayCamera &&
               splitReplayResult.replayLayout.right.binding.kind == client::PaneBindingKind::ReplayCamera &&
               splitReplayResult.replayLayout.left.binding.label == "Replay Camera A" &&
               splitReplayResult.replayLayout.right.binding.label == "Replay Camera B",
           "split replay launch composition should bind two replay cameras into the same left or right slot model used by live split-screen");
    expect(splitReplayResult.replayPaneBindingTimeline.size() == 2u &&
               splitReplayResult.replayPaneBindingTimeline[0].slot == sim::PaneSlot::Left &&
               splitReplayResult.replayPaneBindingTimeline[1].slot == sim::PaneSlot::Right,
           "split replay launch composition should preserve left and right replay bindings in shared pane-binding metadata");

    net::SessionLaunchConfig editorConfig = net::makeEditorSessionLaunchConfig(7);
    app::SessionComposer editorComposer(editorConfig);
    const app::SessionComposer::Result editorResult = editorComposer.compose();
    expect(editorResult.ok,
           "editor launch composition should succeed through SessionComposer");
    expect(editorResult.composition.startupSequence_.empty() &&
               editorResult.composition.hostedServer_ == nullptr &&
               editorResult.composition.proxy_ == nullptr &&
               editorResult.composition.client_ == nullptr,
           "editor launch composition should route through the shared seam without composing a runtime stack");
}

void testAppFlowStartsSessionThroughComposerSeam() {
    ensureTestLevelExists(7);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(7, "appflow-host", 45150, 1u, 1u);
    config.clientSessionId = 0x70000001u;
    config.clientConnectTimeoutUs = 300'000u;

    app::AppFlow::SessionStartResult sessionStart = app::AppFlow::startSession(config);
    expect(sessionStart.sessionFlow != nullptr && sessionStart.started,
           "AppFlow should compose and start a session through the SessionComposer seam");
    expect(sessionStart.sessionFlow->startupSequence().size() == 3u,
           "AppFlow-started sessions should preserve the composed startup sequence");

    const bool running = waitForPredicate([&]() {
        sessionStart.sessionFlow->update(1.0f / 60.0f, nullptr);
        return sessionStart.sessionFlow->state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));
    expect(running,
           "AppFlow-started sessions should still reach the running state deterministically");
}

void testAppFlowRoutesReplayAndEditorWithoutRuntimeOwnership() {
    net::SessionLaunchConfig replayConfig = net::makeReplaySessionLaunchConfig(6);
    app::AppFlow::SessionStartResult replayStart = app::AppFlow::startSession(replayConfig);
    expect(replayStart.started &&
               replayStart.target == app::AppFlow::LaunchTarget::Replay &&
               replayStart.sessionFlow == nullptr &&
               app::AppFlow::routeFor(replayStart) == app::AppFlow::ShellRoute::Replay,
           "AppFlow should route replay entry through the shared launch seam without constructing a runtime session");
    expect(replayStart.replayLayout.available &&
               replayStart.replayLayout.activePaneCount() == 1u &&
               replayStart.replayLayout.left.binding.label == "Replay Camera A" &&
               replayStart.replayPaneBindingTimeline.size() == 1u,
           "AppFlow replay entry should preserve the shared replay-pane binding contracts without reviving a separate gameplay owner");

    net::SessionLaunchConfig splitReplayConfig = net::makeReplaySessionLaunchConfig(6, 2u);
    app::AppFlow::SessionStartResult splitReplayStart = app::AppFlow::startSession(splitReplayConfig);
    expect(splitReplayStart.started &&
               splitReplayStart.replayLayout.available &&
               splitReplayStart.replayLayout.activePaneCount() == 2u &&
               splitReplayStart.replayLayout.left.binding.label == "Replay Camera A" &&
               splitReplayStart.replayLayout.right.binding.label == "Replay Camera B" &&
               splitReplayStart.replayPaneBindingTimeline.size() == 2u,
           "AppFlow replay entry should keep two replay cameras in the shared left or right slot model instead of inventing a replay-only pane path");

    net::SessionLaunchConfig editorConfig = net::makeEditorSessionLaunchConfig(7);
    app::AppFlow::SessionStartResult editorStart = app::AppFlow::startSession(editorConfig);
    expect(editorStart.started &&
               editorStart.target == app::AppFlow::LaunchTarget::LevelEditor &&
               editorStart.sessionFlow == nullptr,
           "AppFlow should route editor entry through the shared launch seam without constructing a runtime session");
}

void testSplitScreenSessionControlWaitsForReadyRightParticipantBeforePlayerBinding() {
    net::SessionFlowController controller(
        net::makeHostSessionLaunchConfig(4, "split-host", 45170u, 0u, 0u));
    controller.bindPrimaryLocalParticipant(7u, 7, "Left Player");
    controller.requestRightLocalParticipant();

    expect(controller.config().localParticipantCount == 1u &&
               controller.splitScreenController().awaitingRightLocalParticipant() &&
               !controller.splitScreenController().rightSlot().active,
           "enabling split-screen for player ownership should wait to activate the right slot until a true second local participant is ready");

    expect(controller.bindRightLocalParticipant(8u, 8, "Right Player"),
           "split-screen session control should bind the right slot once the second local participant is ready");
    controller.setFocusedPane(sim::PaneSlot::Right);

    expect(controller.config().localParticipantCount == 2u &&
               controller.splitScreenController().splitScreenActive() &&
               controller.splitScreenController().rightSlot().active &&
               controller.splitScreenController().rightSlot().binding.kind == client::PaneBindingKind::LocalParticipant &&
               controller.splitScreenController().rightSlot().focused,
           "split-screen session control should route focus and player ownership through the right slot only after the participant is bound");
}

void testSplitScreenSessionControlDisablesRightSlotAndTemporaryParticipant() {
    net::SessionFlowController controller(
        net::makeHostSessionLaunchConfig(5, "split-host", 45171u, 0u, 0u));
    controller.bindPrimaryLocalParticipant(7u, 7, "Left Player");
    controller.requestRightLocalParticipant();
    expect(controller.bindRightLocalParticipant(9u, 9, "Right Player"),
           "split-screen teardown test requires a bound temporary right-side participant");

    controller.disableSplitScreen();

    expect(controller.config().localParticipantCount == 1u &&
               !controller.splitScreenController().splitScreenActive() &&
               controller.splitScreenController().temporaryLocalParticipantId() == 0u &&
               !controller.splitScreenController().rightSlot().active &&
               controller.splitScreenController().rightSlot().binding.kind == client::PaneBindingKind::None &&
               controller.splitScreenController().leftSlot().focused,
           "disabling split-screen should tear down the temporary right-side participant and discard the right slot cleanly");
}

void testAppFlowAndSessionComposerOwnLaunchStructure() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");
    const std::string controllerHeader = readTextFile(repoRoot / "include/net/SessionFlowController.hpp");
    const std::string controllerSource = readTextFile(repoRoot / "src/SessionFlowController.cpp");

    expect(shellSource.find("#include \"app/AppFlow.hpp\"") != std::string::npos &&
               shellSource.find("app::AppFlow::startSession(") != std::string::npos,
           "main app shell should launch multiplayer sessions through the AppFlow seam");
    expect(shellSource.find("std::make_unique<net::SessionFlowController>(pendingSessionLaunch)") == std::string::npos,
           "main app shell should no longer construct SessionFlowController directly from raw launch config");
    expect(shellSource.find("net::ClientRuntime client(clientConfig);") == std::string::npos &&
               shellSource.find("app::AppFlow::startSession(joinConfig)") != std::string::npos,
           "the direct join path should also launch through the composed AppFlow session stack instead of constructing ClientRuntime directly");
    expect(controllerHeader.find("struct SessionRuntimeComposition") != std::string::npos,
           "SessionFlowController should expose an explicit runtime-composition seam");
    expect(controllerSource.find("#include \"app/SessionComposer.hpp\"") != std::string::npos,
           "SessionFlowController should delegate startup composition to SessionComposer");
    expect(controllerSource.find("SessionFlowController::startLocalServer(") == std::string::npos &&
               controllerSource.find("SessionFlowController::startLocalProxy(") == std::string::npos &&
               controllerSource.find("SessionFlowController::startClientRuntime(") == std::string::npos,
           "SessionFlowController should stop owning the low-level startup composition helpers after the seam extraction");
}

void testMainShellRoutesTopLevelSelectionsThroughAppFlow() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string menuSource = readTextFile(repoRoot / "include/MainMenu.hpp");
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");

    expect(menuSource.find("enum class AppShellSurface") != std::string::npos &&
               menuSource.find("NavigationSelection") != std::string::npos &&
               menuSource.find("requestedEntryPoint() const") != std::string::npos,
           "MainMenu should expose a typed app-shell surface and entry-point contract rather than only raw GameMode transitions");
    expect(shellSource.find("mainMenu->requestedSurface()") != std::string::npos &&
               shellSource.find("mainMenu->requestedEntryPoint()") != std::string::npos &&
               shellSource.find("app::AppFlow::routeForRequestedEntryPoint(requestedEntryPoint)") != std::string::npos,
           "main app shell should route top-level menu selections through typed surface and entry-point state before starting runtime composition");
    expect(shellSource.find("nextMode == GameMode::FREE_GAME") == std::string::npos &&
               shellSource.find("nextMode == GameMode::LEVEL_EDITOR") == std::string::npos,
           "main app shell should stop treating root-menu study or editor selections as direct legacy runtime mode ownership");
    expect(shellSource.find("switch (app::AppFlow::routeFor(sessionStart))") != std::string::npos &&
               shellSource.find("startSession(net::makeEditorSessionLaunchConfig(selectedLevel))") != std::string::npos,
           "main app shell should route shared AppFlow start results, including editor entry, through one typed shell handoff");
}

void testMainShellMaintainsExplicitMenuBackStack() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");
    const std::string menuSource = readTextFile(repoRoot / "include/MainMenu.hpp");
    const std::string editorSource = readTextFile(repoRoot / "include/LevelEditor.hpp");

    expect(shellSource.find("std::vector<GameMode> navigationHistory;") != std::string::npos &&
               shellSource.find("auto popMenuMode = [&]() {") != std::string::npos &&
               shellSource.find("auto pushMenuMode = [&](GameMode nextMode) {") != std::string::npos,
           "main app shell should own an explicit menu history stack instead of collapsing every back action into a root-menu reset");
    expect(shellSource.find("mainMenu.reset();") == std::string::npos &&
               shellSource.find("popMenuMode();") != std::string::npos,
           "main app shell should preserve previous menu instances for back navigation and pop them explicitly when Q returns to an earlier menu");
    expect(shellSource.find("display::enableCursorPreservingPosition();") != std::string::npos &&
               shellSource.find("display::disableCursorForCapture();") != std::string::npos &&
               shellSource.find("EnableCursor();") == std::string::npos &&
               shellSource.find("DisableCursor();") == std::string::npos,
           "main app shell should route cursor capture/release through DisplayManager so menu back navigation does not recenter the pointer");
    expect(menuSource.find("selectedOption = index;") != std::string::npos &&
               menuSource.find("currentView_ = View::Root;") != std::string::npos,
           "MainMenu should preserve multiplayer-submenu context during forward navigation while still allowing explicit in-menu back to the root surface");
    expect(editorSource.find("IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q)") != std::string::npos,
           "LevelEditor should honor Q alongside Escape for shell-level back navigation");
}

void testHostLaunchStartsServerProxyClientAndEntersRunning() {
    ensureTestLevelExists(2);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(2, "host-player", 45100, 2u, 1u);
    config.sessionLabel = "Player LAN Match";
    config.tickRateHz = 120u;
    config.snapshotRateHz = 60u;
    config.maxHumanPlayers = 4u;
    config.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    config.clientSessionId = 0x10010001u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "host session should start successfully");

    const auto& sequence = controller.startupSequence();
    expect(sequence.size() == 3u, "host session should record a three-step startup sequence");
    expect(sequence[0] == "server" && sequence[1] == "proxy" && sequence[2] == "client",
           "host session should start server, then proxy, then local client");
    expect(controller.config().proxyClientListenPort == 45100u &&
           controller.config().clientConnectPort == 45100u,
           "the selected host port should be both the public proxy listen port and the host client's connect port");
    expect(controller.config().attackerBotCount == 2u &&
               controller.config().defenderBotCount == 1u,
           "host session configuration should preserve the requested attacker and defender bot counts");
    expect(controller.config().tickRateHz == 120u &&
               controller.config().snapshotRateHz == 60u &&
               controller.config().maxHumanPlayers == 4u,
           "host session configuration should preserve the selected cadence and human-player cap");
    expect(controller.config().sessionLabel == "Player LAN Match" &&
               controller.config().shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "host session configuration should preserve the selected session label and shot rule");
    expect(controller.config().serverListenPort != 0u &&
           controller.config().proxyUpstreamServerPort == controller.config().serverListenPort,
           "host startup should bind the internal server first and point the proxy upstream to that actual port");
    expect(controller.config().levelSlot == 2 &&
           controller.config().levelHash == net::makeLevelIdentityHash(2),
           "host startup should preserve the selected level identity in session configuration");
    expect(controller.config().publicJoinPort == 45100u,
           "host startup should preserve the selected public join port in launch configuration");
    const net::HostedSessionMetadata hostedMetadata = controller.hostedSessionMetadata();
    expect(hostedMetadata.sessionLabel == "Player LAN Match" &&
               hostedMetadata.hostPlayerName == "host-player",
           "hosted startup should expose authoritative hosted session metadata derived from launch configuration");
    expect(hostedMetadata.publicJoinPort == 45100u &&
               hostedMetadata.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "hosted startup metadata should include the public join port and selected shot rule");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));

    expect(running, "host session should enter running state after startup succeeds");
    expect(controller.clientRuntime() != nullptr && controller.clientRuntime()->hasSnapshot(),
           "host session should expose a connected client with an authoritative snapshot");
    expect(controller.clientRuntime() != nullptr &&
           controller.clientRuntime()->authoritativeLevelSlot() == 2 &&
           controller.clientRuntime()->authoritativeLevelHash() == net::makeLevelIdentityHash(2),
           "host local client should learn the authoritative selected level identity from the welcome path");
    expect(controller.hostedServer() != nullptr &&
               controller.hostedServer()->config().attackerBotCount == 2u &&
               controller.hostedServer()->config().defenderBotCount == 1u,
           "hosted startup should expose the requested bot counts to the authoritative server startup path");
}

void testHostedMultiplayerReplayHotkeysStopRecordingBeforePlayback() {
    ensureTestLevelExists(1);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(1, "recording-host", 45310u, 0u, 0u);
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted recording hotkey session should start successfully");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running &&
               controller.clientRuntime() != nullptr &&
               controller.clientRuntime()->hasSnapshot();
    }, std::chrono::milliseconds(750));
    expect(running, "hosted recording hotkey session should reach connected gameplay");

    InputHandler3D::InputState recordInput;
    recordInput.toggleRecording = true;
    controller.update(1.0f / 60.0f, &recordInput);
    expect(controller.clientRuntime()->clientViewState().replay.recordingActive,
           "pressing 5 in hosted multiplayer should start the local in-game recording");

    for (int frame = 0; frame < 4; ++frame) {
        controller.update(1.0f / 60.0f, nullptr);
    }

    InputHandler3D::InputState playbackInput;
    playbackInput.togglePlayback = true;
    controller.update(1.0f / 60.0f, &playbackInput);
    client::ReplayStatusView recordingReplay = controller.clientRuntime()->clientViewState().replay;
    expect(recordingReplay.recordingActive && !recordingReplay.playbackActive,
           "pressing 6 while hosted recording is active should not enter playback");
    expect(controller.hostedServer() != nullptr &&
               controller.hostedServer()->commandReplayRecordingEnabled(),
           "ignoring 6 during active recording should leave hosted command replay capture consistent");

    controller.update(1.0f / 60.0f, &recordInput);
    client::ReplayStatusView stoppedReplay = controller.clientRuntime()->clientViewState().replay;
    expect(!stoppedReplay.recordingActive && !stoppedReplay.playbackActive,
           "pressing 5 again in hosted multiplayer should stop recording without entering replay playback");
    expect(stoppedReplay.statusLine.find("Replay ready") != std::string::npos,
           "host command replay status should not mask the local recording that 6 will play");
    expect(controller.hostedServer() != nullptr &&
               !controller.hostedServer()->commandReplayRecordingEnabled(),
           "stopping hosted in-game recording should also leave command replay capture stopped");

    controller.update(1.0f / 60.0f, &playbackInput);
    client::ReplayStatusView playbackReplay = controller.clientRuntime()->clientViewState().replay;
    expect(playbackReplay.playbackActive &&
               playbackReplay.statusLine.find("Playback (playing)") != std::string::npos,
           "pressing 6 after stopping hosted recording should explicitly play the last recording");

    InputHandler3D::InputState resetReplayInput;
    resetReplayInput.resetPlayback = true;
    controller.update(1.0f / 60.0f, &resetReplayInput);
    playbackReplay = controller.clientRuntime()->clientViewState().replay;
    expect(playbackReplay.playbackActive &&
               playbackReplay.statusLine.find("Playback (paused) @1/") != std::string::npos,
           "pressing 0 in hosted multiplayer should reset to the first replay frame without exiting replay mode");

    InputHandler3D::InputState exitReplayInput;
    exitReplayInput.stopReplayPlayback = true;
    controller.update(1.0f / 60.0f, &exitReplayInput);
    playbackReplay = controller.clientRuntime()->clientViewState().replay;
    expect(!playbackReplay.recordingActive &&
               !playbackReplay.playbackActive &&
               playbackReplay.statusLine.find("Replay ready") != std::string::npos,
           "pressing Backspace in hosted multiplayer should exit replay playback without discarding the recording");

    controller.update(1.0f / 60.0f, &playbackInput);
    playbackReplay = controller.clientRuntime()->clientViewState().replay;
    expect(playbackReplay.playbackActive &&
               playbackReplay.statusLine.find("Playback (playing)") != std::string::npos,
           "pressing 6 after hosted replay exit should play the last completed recording again");

    controller.update(1.0f / 60.0f, &playbackInput);
    playbackReplay = controller.clientRuntime()->clientViewState().replay;
    expect(playbackReplay.playbackActive &&
               playbackReplay.statusLine.find("Playback (paused)") != std::string::npos,
           "pressing 6 while hosted replay playback is active should pause it");
}

void testHostedWelcomeCarriesSelectedSessionMetadata() {
    ensureTestLevelExists(8);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(8, "host-player", 45180u);
    config.sessionLabel = "Metadata Match";
    config.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    config.clientSessionId = 0x18000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start successfully before welcome metadata checks");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before serving welcome metadata");

    net::UdpSocket probeSocket;
    expect(probeSocket.bind({"127.0.0.1", 0u}), "metadata probe socket should bind locally");

    net::Packet helloPacket;
    helloPacket.header.channel = net::Channel::Control;
    helloPacket.header.kind = net::PacketKind::Hello;
    helloPacket.payload = net::HelloMessage{0x18000002u, 0u, "metadata-probe"};
    expect(probeSocket.sendTo({"127.0.0.1", controller.config().proxyClientListenPort},
                              net::serializePacket(helloPacket)),
           "metadata probe should send a hello packet to the hosted public join port");

    net::ParseResult welcomeResult;
    const bool receivedWelcome = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);

        net::ReceivedDatagram datagram;
        if (probeSocket.receive(&datagram) != net::ReceiveStatus::Received) {
            return false;
        }

        welcomeResult = net::deserializePacket(datagram.payload);
        return welcomeResult.ok && welcomeResult.packet.header.kind == net::PacketKind::Welcome;
    }, std::chrono::milliseconds(900));

    expect(receivedWelcome, "metadata probe should receive a welcome packet from the hosted session");
    const auto& welcome = std::get<net::WelcomeMessage>(welcomeResult.packet.payload);
    expect(welcome.sessionMetadata.sessionLabel == "Metadata Match",
           "welcome packet should preserve the selected session label");
    expect(welcome.sessionMetadata.hostPlayerName == "host-player",
           "welcome packet should preserve the host player name");
    expect(welcome.sessionMetadata.publicJoinPort == 45180u,
           "welcome packet should preserve the authoritative public join port");
    expect(welcome.sessionMetadata.levelSlot == 8 &&
               welcome.sessionMetadata.levelHash == net::makeLevelIdentityHash(8),
           "welcome packet should preserve the authoritative level identity inside hosted metadata");
    expect(welcome.sessionMetadata.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "welcome packet should preserve the selected shot-evaluation rule");
}

void testHostedSessionAdvertisesAuthoritativeMetadataAfterStartup() {
    ensureTestLevelExists(9);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(9, "host-player", 45190u);
    config.sessionLabel = "Discovery Match";
    config.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    config.discoveryPort = kSessionFlowDiscoveryPortBase;
    config.clientSessionId = 0x19000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before discovery advertisement checks");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before advertising");

    net::UdpSocket browserSocket;
    expect(browserSocket.bind({"127.0.0.1", 0u}), "browser discovery socket should bind locally");
    expect(browserSocket.sendTo({"127.0.0.1", config.discoveryPort},
                                net::serializeSessionDiscoveryQuery(net::SessionDiscoveryQuery{})),
           "browser discovery socket should send a discovery query");

    net::SessionAdvertisement advertisement;
    const bool receivedAdvertisement = waitForDiscoveryAdvertisement(&controller,
                                                                     &browserSocket,
                                                                     &advertisement,
                                                                     std::chrono::milliseconds(900));
    expect(receivedAdvertisement,
           "running hosted sessions should answer local discovery queries with an advertisement");
    expect(advertisement.sessionLabel == "Discovery Match",
           "discovery advertisement should preserve the authoritative session label");
    expect(advertisement.hostPlayerName == "host-player",
           "discovery advertisement should preserve the authoritative host player name");
    expect(advertisement.levelSlot == 9 &&
               advertisement.levelHash == net::makeLevelIdentityHash(9),
           "discovery advertisement should preserve the authoritative level identity");
    expect(advertisement.joinPort == 45190u,
           "discovery advertisement should preserve the authoritative public join port");
    expect(advertisement.humanPlayers == 1u && advertisement.maxHumanPlayers == 2u,
           "discovery advertisement should preserve authoritative human occupancy");
    expect(advertisement.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "discovery advertisement should preserve the authoritative shot-evaluation rule");
}

void testHostedSessionShutdownStopsDiscoveryAdvertisementAndAllowsBrowserExpiry() {
    ensureTestLevelExists(7);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(7, "host-player", 45200u);
    config.sessionLabel = "Expiry Match";
    config.discoveryPort = kSessionFlowDiscoveryPortBase + 1u;
    config.clientSessionId = 0x1A000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before discovery shutdown checks");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before shutdown checks");

    net::UdpSocket browserSocket;
    expect(browserSocket.bind({"127.0.0.1", 0u}), "browser discovery socket should bind locally");
    expect(browserSocket.sendTo({"127.0.0.1", config.discoveryPort},
                                net::serializeSessionDiscoveryQuery(net::SessionDiscoveryQuery{})),
           "browser discovery socket should send an initial discovery query");

    net::SessionAdvertisement advertisement;
    const bool receivedAdvertisement = waitForDiscoveryAdvertisement(&controller,
                                                                     &browserSocket,
                                                                     &advertisement,
                                                                     std::chrono::milliseconds(900));
    expect(receivedAdvertisement, "initial hosted discovery query should receive an advertisement");

    net::SessionBrowserCache cache;
    cache.upsert(advertisement, 1'000'000u);
    expect(cache.entries().size() == 1u,
           "browser cache should contain the advertised hosted session before shutdown");

    controller.shutdown("session ended");

    expect(browserSocket.sendTo({"127.0.0.1", config.discoveryPort},
                                net::serializeSessionDiscoveryQuery(net::SessionDiscoveryQuery{})),
           "browser discovery socket should be able to send another query after shutdown");

    bool receivedAfterShutdown = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        controller.update(1.0f / 60.0f, nullptr);
        net::ReceivedDatagram datagram;
        if (browserSocket.receive(&datagram) == net::ReceiveStatus::Received) {
            receivedAfterShutdown = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    expect(!receivedAfterShutdown,
           "ended hosted sessions should stop answering discovery queries");

    cache.expireStale(4'000'000u);
    expect(cache.entries().empty(),
           "browser cache entries should expire deterministically after advertisement shutdown");
}

void testJoinLaunchStartsLocalProxyAndRoutesClientThroughLoopback() {
    net::SessionLaunchConfig config =
        net::makeJoinSessionLaunchConfig("127.0.0.1", 49999u, "joiner");
    config.clientSessionId = 0x1B000001u;
    config.clientConnectTimeoutUs = 200'000u;
    config.clientHelloRetryIntervalUs = 20'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "join launch should start successfully with a local proxy path");

    const auto& sequence = controller.startupSequence();
    expect(sequence.size() == 2u && sequence[0] == "proxy" && sequence[1] == "client",
           "join launch should start a local proxy before the local client runtime");
    expect(controller.config().startLocalProxy,
           "join launch config should preserve the request to start a local proxy");
    expect(controller.config().proxyUpstreamServerHost == "127.0.0.1" &&
               controller.config().proxyUpstreamServerPort == 49999u,
           "join launch config should preserve the selected remote host endpoint as the proxy upstream target");
    expect(controller.config().proxyClientListenPort != 0u &&
               controller.config().proxyServerListenPort != 0u,
           "join launch should allocate local client and server proxy sockets");
    expect(controller.config().clientConnectHost == "127.0.0.1" &&
               controller.config().clientConnectPort == controller.config().proxyClientListenPort,
           "join launch should rewrite the local client endpoint to the loopback proxy listen socket");
    expect(controller.clientRuntime() != nullptr &&
               controller.clientRuntime()->config().serverHost == "127.0.0.1" &&
               controller.clientRuntime()->config().serverPort == controller.config().proxyClientListenPort,
           "join client runtime should connect to the local proxy rather than directly to the remote host");

    const bool relayedHello = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.aggregateProxyStats(true).receivedPackets > 0u &&
               controller.aggregateProxyStats(true).forwardedPackets > 0u;
    }, std::chrono::milliseconds(250));

    expect(relayedHello,
           "join-local proxy startup should relay the client's initial hello traffic upstream");
}

void testHostLaunchSpawnsConfiguredBotsIntoHostedRoster() {
    ensureTestLevelExists(5);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(5, "host-player", 45230, 2u, 1u);
    config.clientSessionId = 0x10010010u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "host session should start successfully before bot roster validation");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));

    expect(running, "host session should enter running state before exposing the hosted bot roster");
    expect(controller.hostedServer() != nullptr,
           "running hosted sessions should expose the authoritative server for roster inspection");

    std::size_t botCount = 0u;
    std::size_t attackerBots = 0u;
    std::size_t defenderBots = 0u;
    std::size_t humanCount = 0u;
    for (const auto& entry : controller.hostedServer()->worldState().roster) {
        if (entry.isBot) {
            ++botCount;
            if (entry.team == sim::TeamId::Attacker) {
                ++attackerBots;
            } else if (entry.team == sim::TeamId::Defender) {
                ++defenderBots;
            }
        } else {
            ++humanCount;
        }
    }

    expect(botCount == 3u && attackerBots == 2u && defenderBots == 1u,
           "hosted startup should spawn the configured attacker and defender bot counts into the authoritative roster");
    expect(humanCount == 1u && controller.hostedServer()->worldState().roster.size() == 4u,
           "hosted startup should preserve the local host player alongside the configured bots");
}

void testJoinClientCanConnectAndPlayThroughLocalProxyPath() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(6, "host-player", 45155u);
    hostConfig.clientSessionId = 0x1C000001u;
    hostConfig.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController host(hostConfig);
    expect(host.start(), "hosted session should start before join-local proxy checks");

    const bool hostRunning = waitForPredicate([&]() {
        host.update(1.0f / 60.0f, nullptr);
        return host.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(hostRunning, "hosted session should reach running state before the proxied join starts");

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1", host.config().proxyClientListenPort, "proxy-joiner");
    joinConfig.clientSessionId = 0x1C000002u;
    joinConfig.clientConnectTimeoutUs = 400'000u;
    joinConfig.clientHelloRetryIntervalUs = 20'000u;

    net::SessionFlowController join(joinConfig);
    expect(join.start(), "join-local proxy session should start successfully");
    expect(join.startupSequence().size() == 2u &&
               join.startupSequence()[0] == "proxy" &&
               join.startupSequence()[1] == "client",
           "join-local proxy startup should record proxy then client startup ordering");

    const bool connected = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return join.state() == net::SessionFlowState::Running &&
                   join.clientRuntime() != nullptr &&
                   join.clientRuntime()->hasSnapshot() &&
                   host.hostedSessionCount() == 2u;
        },
        std::chrono::milliseconds(1200));
    expect(connected,
           "join client should still connect to the hosted session while routed through a local proxy path");

    expect(join.config().clientConnectHost == "127.0.0.1" &&
               join.config().clientConnectPort == join.config().proxyClientListenPort,
           "connected join sessions should keep the local client routed through the proxy listen socket");
    expect(join.config().proxyUpstreamServerPort == host.config().proxyClientListenPort,
           "join-local proxy should target the host's public join port as its upstream endpoint");

    const net::ProxyStats baselineUpstream = join.aggregateProxyStats(true);
    const net::ProxyStats baselineDownstream = join.aggregateProxyStats(false);

    InputHandler3D::InputState joinInput;
    joinInput.moveInput = Vector2{-1.0f, 0.0f};
    for (int frame = 0; frame < 30; ++frame) {
        host.update(1.0f / 60.0f, nullptr);
        join.update(1.0f / 60.0f, &joinInput);
    }

    const net::ProxyStats upstreamDelta =
        deltaStats(join.aggregateProxyStats(true), baselineUpstream);
    const net::ProxyStats downstreamDelta =
        deltaStats(join.aggregateProxyStats(false), baselineDownstream);
    expect(upstreamDelta.receivedPackets > 0u && upstreamDelta.forwardedPackets > 0u,
           "join-local proxy should continue forwarding local client traffic upstream during active play");
    expect(downstreamDelta.forwardedPackets > 0u,
           "join-local proxy should continue forwarding authoritative server traffic downstream during active play");
    expect(host.state() == net::SessionFlowState::Running &&
               join.state() == net::SessionFlowState::Running &&
               join.clientRuntime() != nullptr &&
               join.clientRuntime()->hasSnapshot(),
           "host and join sessions should remain stable while the joiner plays through the local proxy path");
}

void testJoinSessionFlowExposesGhostTrackForDelayedRemotePlayers() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(6, "ghost-host", 45156u);
    hostConfig.clientSessionId = 0x1C100001u;
    hostConfig.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController host(hostConfig);
    expect(host.start(), "hosted session should start before the ghost-track flow test");

    const bool hostRunning = waitForPredicate([&]() {
        host.update(1.0f / 60.0f, nullptr);
        return host.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(hostRunning, "hosted session should reach running state before the delayed join starts");

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1", host.config().proxyClientListenPort, "ghost-joiner");
    joinConfig.clientSessionId = 0x1C100002u;
    joinConfig.clientConnectTimeoutUs = 400'000u;
    joinConfig.clientHelloRetryIntervalUs = 20'000u;

    net::SessionFlowController join(joinConfig);
    expect(join.start(), "delayed join session should start successfully");

    const bool connected = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return join.state() == net::SessionFlowState::Running &&
                   join.clientRuntime() != nullptr &&
                   join.clientRuntime()->hasSnapshot() &&
                   host.hostedSessionCount() == 2u;
        },
        std::chrono::milliseconds(1400));
    expect(connected,
           "join session should connect through the local proxy before the ghost-track flow test continues");

    host.clientRuntime()->setLocalNetworkSettingsForTest(180.0f, 0.0f);

    InputHandler3D::InputState hostInput;
    hostInput.moveInput = Vector2{1.0f, 0.0f};
    for (int frame = 0; frame < 60; ++frame) {
        host.update(1.0f / 60.0f, &hostInput);
        join.update(1.0f / 60.0f, nullptr);
    }

    const client::ClientViewState joinView = join.clientRuntime()->clientViewState();
    expect(joinView.remotePlayers.size() == 1u &&
               joinView.remotePlayerGhosts.size() == 1u,
           "delayed join sessions should still publish both the manipulated remote player and its clean ghost track");

    const float ghostLeadX =
        joinView.remotePlayerGhosts.front().eyePosition.x -
        joinView.remotePlayers.front().eyePosition.x;
    expect(ghostLeadX > 0.5f,
           "delayed join sessions should expose a visibly leading ghost-control track in the real session flow");
}

void testHostSessionFlowExposesGhostTrackForLaggedJoinPlayer() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(6, "ghost-host", 45166u);
    hostConfig.clientSessionId = 0x1C200001u;
    hostConfig.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController host(hostConfig);
    expect(host.start(), "hosted session should start before the host ghost-track flow test");

    const bool hostRunning = waitForPredicate([&]() {
        host.update(1.0f / 60.0f, nullptr);
        return host.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(hostRunning, "hosted session should reach running state before the delayed join starts");

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1", host.config().proxyClientListenPort, "lagged-joiner");
    joinConfig.clientSessionId = 0x1C200002u;
    joinConfig.clientConnectTimeoutUs = 400'000u;
    joinConfig.clientHelloRetryIntervalUs = 20'000u;

    net::SessionFlowController join(joinConfig);
    expect(join.start(), "lagged join session should start successfully");

    const bool connected = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return host.state() == net::SessionFlowState::Running &&
                   join.state() == net::SessionFlowState::Running &&
                   host.clientRuntime() != nullptr &&
                   join.clientRuntime() != nullptr &&
                   host.clientRuntime()->hasSnapshot() &&
                   join.clientRuntime()->hasSnapshot() &&
                   host.hostedSessionCount() == 2u;
        },
        std::chrono::milliseconds(1400));
    expect(connected,
           "host and join sessions should connect before applying lagged-player ghost assertions");

    InputHandler3D::InputState joinInput;
    joinInput.moveInput = Vector2{1.0f, 0.0f};
    for (int frame = 0; frame < 90; ++frame) {
        host.update(1.0f / 60.0f, nullptr);
        join.update(1.0f / 60.0f, &joinInput);
    }

    const client::ClientViewState hostViewNoLag = host.clientRuntime()->clientViewState();
    expect(hostViewNoLag.remotePlayers.size() == 1u &&
               hostViewNoLag.remotePlayerGhosts.size() == 1u,
           "host clients should already expose a control ghost track before any additional lag is introduced");

    const float ghostNoLagDx =
        hostViewNoLag.remotePlayerGhosts.front().eyePosition.x -
        hostViewNoLag.remotePlayers.front().eyePosition.x;
    const float ghostNoLagDz =
        hostViewNoLag.remotePlayerGhosts.front().eyePosition.z -
        hostViewNoLag.remotePlayers.front().eyePosition.z;
    const float ghostNoLagOffset =
        std::sqrt((ghostNoLagDx * ghostNoLagDx) + (ghostNoLagDz * ghostNoLagDz));
    expect(ghostNoLagOffset < 0.15f,
           "without added latency the host should see the ghost-control track overlap the manipulated actor");

    join.clientRuntime()->setLocalNetworkSettingsForTest(180.0f, 0.0f);

    for (int frame = 0; frame < 90; ++frame) {
        host.update(1.0f / 60.0f, nullptr);
        join.update(1.0f / 60.0f, &joinInput);
    }

    const client::ClientViewState hostView = host.clientRuntime()->clientViewState();
    expect(hostView.remotePlayers.size() == 1u &&
               hostView.remotePlayerGhosts.size() == 1u,
           "host clients should publish both the manipulated remote player and the clean ghost for a lagged join participant");

    const float ghostLeadX =
        hostView.remotePlayerGhosts.front().eyePosition.x -
        hostView.remotePlayers.front().eyePosition.x;
    expect(ghostLeadX > 0.5f,
           "host clients should observe the join participant's ghost-control track leading the manipulated actor under added latency");

    const std::vector<std::string> hudLines = host.clientRuntime()->compactHudLines();
    expect(containsLine(hudLines, "Ghosts 1 | Matched 1 | Offset"),
           "host compact hud should expose ghost-track debug state for lagged join participants");
}

void testFailedJoinReturnsToMenuWithErrorAndRetryPath() {
    net::SessionLaunchConfig config =
        net::makeJoinSessionLaunchConfig("127.0.0.1", 49999, "joiner");
    config.clientSessionId = 0x20020002u;
    config.clientConnectTimeoutUs = 60'000u;
    config.clientHelloRetryIntervalUs = 10'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "join session should start its client runtime even if the host is absent");

    const bool failed = waitForPredicate([&]() {
        controller.update(1.0f / 120.0f, nullptr);
        return controller.state() == net::SessionFlowState::Failed;
    }, std::chrono::milliseconds(750));

    expect(failed, "join session should fail after the configured timeout when no host responds");
    expect(controller.statusMessage().find("timed out") != std::string::npos,
           "failed manual join should expose a specific timeout error message");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    menu.setHostAddress("127.0.0.1");
    menu.setServerPortText("41000");
    menu.setPlayerName("joiner");
    menu.setStatusMessage(controller.statusMessage(), false);

    expect(!menu.statusMessage().empty() && !menu.busy(),
           "failed join should return to the setup UI with a visible non-busy error state");
    const net::SessionLaunchConfig retry =
        submitAfterTeamChoice(&menu, "manual retry join");
    expect(retry.clientConnectHost == "127.0.0.1" &&
               retry.clientConnectPort == 41000u,
           "after a failed join the setup UI should still allow a retry submission");
}

void testDiscoverySelectedJoinPreservesBaselineGameplayRosterAndTeamScore() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(6, "browser-host", 45250u, 0u, 0u);
    hostConfig.sessionLabel = "Discovery Baseline";
    hostConfig.discoveryPort = kSessionFlowDiscoveryPortBase + 2u;
    hostConfig.clientSessionId = 0x47000001u;
    hostConfig.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController host(hostConfig);
    expect(host.start(), "hosted session should start before the browser-selected join regression");

    const bool hostRunning = waitForPredicate([&]() {
        host.update(1.0f / 60.0f, nullptr);
        return host.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(hostRunning, "hosted session should reach running state before LAN discovery selection");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setDiscoveryPortForTest(hostConfig.discoveryPort);
    menu.setPlayerName("browser-joiner");
    const bool discovered = waitForDiscoveryBrowserPredicate(
        &host,
        &menu,
        [&menu]() {
            return menu.visibleDiscoveryEntries().size() == 1u;
        },
        std::chrono::milliseconds(900));
    expect(discovered, "browser-selected baseline regression requires one compatible discovery row");
    expect(menu.selectDiscoveryEntryForTest(0u),
           "browser-selected baseline regression requires selecting the discovered LAN row");

    net::SessionLaunchConfig joinConfig =
        submitAfterTeamChoice(&menu, "browser-selected baseline regression");
    joinConfig.clientSessionId = 0x47000002u;
    joinConfig.clientConnectTimeoutUs = 400'000u;
    joinConfig.clientHelloRetryIntervalUs = 20'000u;

    net::SessionFlowController join(joinConfig);
    expect(join.start(), "browser-selected join session should start successfully");

    const bool connected = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return join.state() == net::SessionFlowState::Running &&
                   join.clientRuntime() != nullptr &&
                   join.clientRuntime()->hasSnapshot() &&
                   host.hostedSessionCount() == 2u;
        },
        std::chrono::milliseconds(1200));
    expect(connected, "browser-selected join should connect to the hosted session");

    const net::ClientRuntime* joinClient = join.clientRuntime();
    expect(joinClient != nullptr && joinClient->hasAuthoritativeSessionMetadata(),
           "browser-selected join should preserve authoritative hosted session metadata after connect");
    expect(joinClient->authoritativeSessionMetadata().sessionLabel == "Discovery Baseline" &&
               joinClient->authoritativeSessionMetadata().hostPlayerName == "browser-host",
           "browser-selected join should preserve the advertised host metadata");
    expect(joinClient->roster().size() == 2u,
           "browser-selected join should expose the authoritative two-player roster");
    expect(joinClient->teamScores().attackers == 0u && joinClient->teamScores().defenders == 0u,
           "browser-selected join should preserve the baseline zeroed team score");
    expect(requireRosterEntry(joinClient->roster(), 1).team == sim::TeamId::Attacker,
           "browser-selected join should preserve the host's attacker team assignment");
    expect(requireRosterEntry(joinClient->roster(), 2).team == sim::TeamId::Defender,
           "browser-selected join should preserve the joiner's defender team assignment");

    const sim::Vec3 baselineJoinPosition = joinClient->localPlayerState().position;
    const std::uint32_t baselineAck =
        joinClient->latestSnapshot() != nullptr ? joinClient->latestSnapshot()->ackedInputSeq : 0u;

    InputHandler3D::InputState joinInput;
    joinInput.moveInput = Vector2{-1.0f, 0.0f};
    for (int frame = 0; frame < 24; ++frame) {
        host.update(1.0f / 60.0f, nullptr);
        join.update(1.0f / 60.0f, &joinInput);
    }

    joinClient = join.clientRuntime();
    expect(joinClient != nullptr &&
               joinClient->localPlayerState().position.x < baselineJoinPosition.x,
           "browser-selected join should preserve baseline local movement during active play");
    expect(joinClient != nullptr &&
               joinClient->latestSnapshot() != nullptr &&
               joinClient->latestSnapshot()->ackedInputSeq > baselineAck,
           "browser-selected join should keep advancing authoritative input acknowledgements during movement");

    net::ServerRuntime* hostedServer = const_cast<net::ServerRuntime*>(host.hostedServer());
    if (hostedServer != nullptr && hostedServer->worldState().enemies.empty()) {
        sim::RemoteActorState target;
        target.entityId = 900;
        target.position = sim::Vec3{0.0f, 0.0f, -10.0f};
        hostedServer->worldState().enemies.push_back(target);
    }
    expect(hostedServer != nullptr && !hostedServer->worldState().enemies.empty(),
           "browser-selected baseline regression requires an authoritative enemy target");
    hostedServer->worldState().enemies.front().position = sim::Vec3{0.0f, 0.0f, -10.0f};
    hostedServer->worldState().enemies.front().velocity = sim::Vec3{};
    const float baselineEnemyHealth = hostedServer->worldState().enemies.front().health;
    const std::size_t baselineTraceCount =
        host.clientRuntime() != nullptr ? host.clientRuntime()->combatTraceCount() : 0u;

    const bool combatResolved = waitForPredicate([&]() {
        net::ServerRuntime* liveHostedServer = const_cast<net::ServerRuntime*>(host.hostedServer());
        if (liveHostedServer == nullptr || liveHostedServer->worldState().enemies.empty()) {
            return false;
        }

        liveHostedServer->worldState().enemies.front().position = sim::Vec3{0.0f, 0.0f, -10.0f};
        liveHostedServer->worldState().enemies.front().velocity = sim::Vec3{};

        InputHandler3D::InputState fireInput;
        fireInput.firePressed = true;
        host.update(1.0f / 60.0f, &fireInput);
        join.update(1.0f / 60.0f, nullptr);
        return host.hostedServer() != nullptr &&
               !host.hostedServer()->worldState().enemies.empty() &&
               host.hostedServer()->worldState().enemies.front().health < baselineEnemyHealth &&
               host.clientRuntime() != nullptr &&
               host.clientRuntime()->combatTraceCount() > baselineTraceCount;
    }, std::chrono::milliseconds(900));
    expect(combatResolved,
           "browser-selected join should preserve baseline combat resolution and local fire feedback");

    const bool replicatedRosterAndScoreState = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            const net::ClientRuntime* currentJoinClient = join.clientRuntime();
            const net::ServerRuntime* currentHostedServer = host.hostedServer();
            return currentJoinClient != nullptr &&
                   currentHostedServer != nullptr &&
                   currentJoinClient->roster().size() == 2u &&
                   currentJoinClient->teamScores().attackers ==
                       currentHostedServer->worldState().teamScores.attackers &&
                   currentJoinClient->teamScores().defenders ==
                       currentHostedServer->worldState().teamScores.defenders;
        },
        std::chrono::milliseconds(300));
    joinClient = join.clientRuntime();
    expect(replicatedRosterAndScoreState,
           "browser-selected join combat should preserve authoritative roster size and replicated team-score state");
    expect(joinClient != nullptr &&
               joinClient->latestSnapshot() != nullptr &&
               joinClient->latestSnapshot()->sessionMetadata.sessionLabel == "Discovery Baseline" &&
               joinClient->latestSnapshot()->sessionMetadata.hostPlayerName == "browser-host" &&
               joinClient->latestSnapshot()->sessionMetadata.publicJoinPort == 45250u &&
               joinClient->latestSnapshot()->sessionMetadata.levelSlot == 6 &&
               joinClient->latestSnapshot()->sessionMetadata.levelHash == net::makeLevelIdentityHash(6) &&
               joinClient->latestSnapshot()->sessionMetadata.shotEvaluationMode ==
                   net::ShotEvaluationMode::SeenPosition,
           "browser-selected join snapshots should preserve authoritative hosted-session metadata after connect");
}

void testLateJoinReceivesCurrentAuthoritativeSnapshotStateOnHostedFlow() {
    ensureTestLevelExists(5);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(5, "host-player", 45260u, 0u, 0u);
    hostConfig.sessionLabel = "Late Join Snapshot";
    hostConfig.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    hostConfig.clientSessionId = 0x48000001u;
    hostConfig.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController host(hostConfig);
    expect(host.start(), "late-join host session should start successfully");

    const bool hostRunning = waitForPredicate([&]() {
        host.update(1.0f / 60.0f, nullptr);
        return host.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(hostRunning, "late-join host session should reach running state");

    net::ServerRuntime* hostedServer = const_cast<net::ServerRuntime*>(host.hostedServer());
    expect(hostedServer != nullptr, "late-join host flow should expose the authoritative hosted server");

    hostedServer->worldState().teamScores.attackers = 2u;
    if (sim::RosterEntry* hostEntry = sim::findRosterEntry(&hostedServer->worldState(), 1)) {
        hostEntry->kills = 2u;
        hostEntry->displayName = "host-player";
    }

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1", host.config().proxyClientListenPort, "late-joiner");
    joinConfig.clientSessionId = 0x48000002u;
    joinConfig.clientConnectTimeoutUs = 400'000u;
    joinConfig.clientHelloRetryIntervalUs = 20'000u;

    net::SessionFlowController join(joinConfig);
    expect(join.start(), "late-join session should start successfully");

    const bool connected = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return join.state() == net::SessionFlowState::Running &&
                   join.clientRuntime() != nullptr &&
                   join.clientRuntime()->hasSnapshot() &&
                   join.clientRuntime()->latestSnapshot() != nullptr &&
                   join.clientRuntime()->latestSnapshot()->sessionMetadata.sessionLabel ==
                       "Late Join Snapshot";
        },
        std::chrono::milliseconds(1200));
    expect(connected,
           "late join should connect through the hosted flow and receive a full authoritative snapshot");

    const net::ClientRuntime* joinClient = join.clientRuntime();
    const net::WorldSnapshot* snapshot =
        joinClient != nullptr ? joinClient->latestSnapshot() : nullptr;
    expect(snapshot != nullptr &&
               snapshot->teamScores.attackers == 2u &&
               snapshot->teamScores.defenders == 0u,
           "late join should reconstruct the current authoritative score state before active play begins");
    expect(joinClient != nullptr &&
               requireRosterEntry(joinClient->roster(), 1).kills == 2u,
           "late join should reconstruct the current authoritative roster stats before active play begins");
    expect(snapshot != nullptr &&
               snapshot->sessionMetadata.sessionLabel == "Late Join Snapshot" &&
               snapshot->sessionMetadata.hostPlayerName == "host-player" &&
               snapshot->sessionMetadata.publicJoinPort == 45260u &&
               snapshot->sessionMetadata.levelSlot == 5 &&
               snapshot->sessionMetadata.levelHash == net::makeLevelIdentityHash(5) &&
               snapshot->sessionMetadata.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "late join snapshots should preserve hosted-session metadata on the real hosted flow");
    expect(snapshot != nullptr &&
               snapshot->authoritativeTime.serverTick > 0u &&
               snapshot->cadence.authoritativeTickHz == 60u,
           "late join snapshots should carry authoritative cadence and timing on the real hosted flow");
}

void testFailedBrowserSelectedJoinReturnsToMenuWithErrorAndRetryPath() {
    net::SessionLaunchConfig config =
        net::makeJoinSessionLaunchConfig("127.0.0.1", 49998, "browser-joiner");
    config.clientSessionId = 0x20020003u;
    config.clientConnectTimeoutUs = 60'000u;
    config.clientHelloRetryIntervalUs = 10'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "browser-selected join should start its client runtime even if the host is absent");

    const bool failed = waitForPredicate([&]() {
        controller.update(1.0f / 120.0f, nullptr);
        return controller.state() == net::SessionFlowState::Failed;
    }, std::chrono::milliseconds(750));

    expect(failed, "browser-selected join should fail after the configured timeout when no host responds");
    expect(controller.statusMessage().find("timed out") != std::string::npos,
           "failed browser-selected join should surface a specific timeout error message");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setPlayerName("browser-joiner");

    net::SessionAdvertisement advertisement;
    advertisement.sessionLabel = "Offline Match";
    advertisement.hostPlayerName = "offline-host";
    advertisement.levelSlot = 4;
    advertisement.levelHash = net::makeLevelIdentityHash(4);
    advertisement.joinHost = "127.0.0.1";
    advertisement.joinPort = 49998u;
    advertisement.humanPlayers = 1u;
    advertisement.maxHumanPlayers = 2u;
    advertisement.protocolVersion = net::kProtocolVersion;
    advertisement.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    menu.injectDiscoveryAdvertisementForTest(advertisement, 16'667u);

    expect(menu.selectDiscoveryEntryForTest(0u),
           "browser-selected join retry test requires a selectable compatible discovery row");
    menu.setStatusMessage(controller.statusMessage(), false);

    expect(menu.statusMessage().find("timed out") != std::string::npos && !menu.busy(),
           "failed browser-selected join should return to the setup UI with a specific visible non-busy error");
    const net::SessionLaunchConfig retry =
        submitAfterTeamChoice(&menu, "browser-selected retry join");
    expect(retry.clientConnectHost == "127.0.0.1" &&
               retry.clientConnectPort == 49998u,
           "after a failed browser-selected join the setup UI should preserve the discovered endpoint for retry");
}

void testSessionEndSignalsReturnToMainMenuWithoutRestart() {
    ensureTestLevelExists(3);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(3, "host-player", 45110);
    config.clientSessionId = 0x30030003u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "host session should start successfully before shutdown");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));

    expect(running, "session should reach running state before testing end-of-session handling");
    controller.shutdown("session ended");

    expect(controller.state() == net::SessionFlowState::Ended,
           "shutdown should move the session flow into the terminal ended state");
    expect(controller.shouldReturnToMainMenu(),
           "ended sessions should signal the outer UI flow to return to the main menu");
}

void testSessionBackClosesUiModeBeforeEndingSession() {
    ensureTestLevelExists(3);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(3, "host-player", 45111u);
    config.clientSessionId = 0x30030011u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "host session should start successfully before testing ui back handling");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));
    expect(running, "session should reach running state before testing ui back handling");

    net::ClientRuntime* client = controller.clientRuntime();
    expect(client != nullptr, "running session should expose a client runtime");

    InputHandler3D::InputState uiInput;
    uiInput.toggleUIMode = true;
    controller.update(1.0f / 60.0f, &uiInput);
    expect(client->uiModeActive(),
           "ui back-handling test requires ui mode to open first");

    InputHandler3D::InputState backInput;
    backInput.quit = true;
    controller.update(1.0f / 60.0f, &backInput);

    expect(controller.state() == net::SessionFlowState::Running &&
               !controller.shouldReturnToMainMenu(),
           "back should close ui mode before it ends the running session");
    expect(!client->uiModeActive(),
           "back should leave the client runtime in gameplay mode when ui mode was open");

    controller.update(1.0f / 60.0f, &backInput);
    expect(controller.state() == net::SessionFlowState::Ended &&
               controller.shouldReturnToMainMenu(),
           "a second back action without a local overlay should end the running session");
}

void testSessionEnterReleasesMouseWithoutOpeningRuntimeSettings() {
    ensureTestLevelExists(3);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(3, "host-player", 45115u);
    config.clientSessionId = 0x30030015u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(),
           "host session should start successfully before testing enter-based mouse release");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));
    expect(running,
           "session should reach running state before testing enter-based mouse release");

    net::ClientRuntime* client = controller.clientRuntime();
    expect(client != nullptr, "running session should expose a client runtime");
    expect(!client->uiModeActive(),
           "enter-based mouse release test requires gameplay mode to start captured");

    InputHandler3D::InputState enterInput;
    enterInput.menuConfirm = true;
    controller.update(1.0f / 60.0f, &enterInput);

    expect(client->uiModeActive(),
           "pressing Enter during the running session should still release the mouse");
    expect(!client->localNetworkPanelVisible(),
           "pressing Enter during the running session should not open the runtime settings overlay");

    controller.update(1.0f / 60.0f, &enterInput);
    expect(!client->uiModeActive(),
           "pressing Enter again during the running session should recapture the mouse without opening runtime settings");
    expect(!client->localNetworkPanelVisible(),
           "pressing Enter again during the running session should still keep the runtime settings overlay closed");
    expect(controller.state() == net::SessionFlowState::Running &&
               !controller.shouldReturnToMainMenu(),
           "pressing Enter during gameplay should not immediately end the running session");
}

void testSessionBackCancelsTeamMenuBeforeEndingSession() {
    ensureTestLevelExists(3);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(3, "host-player", 45112u);
    config.clientSessionId = 0x30030012u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "host session should start successfully before testing team-menu back handling");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(750));
    expect(running, "session should reach running state before testing team-menu back handling");

    net::ClientRuntime* client = controller.clientRuntime();
    expect(client != nullptr, "running session should expose a client runtime");

    InputHandler3D::InputState teamMenuInput;
    teamMenuInput.switchTeam = true;
    controller.update(1.0f / 60.0f, &teamMenuInput);
    expect(client->teamMenuVisible(),
           "team-menu back-handling test requires the explicit team menu to open first");

    InputHandler3D::InputState backInput;
    backInput.quit = true;
    controller.update(1.0f / 60.0f, &backInput);

    expect(controller.state() == net::SessionFlowState::Running &&
               !controller.shouldReturnToMainMenu(),
           "back should cancel the explicit team menu before it ends the running session");
    expect(!client->teamMenuVisible(),
           "back should close the explicit team menu immediately");
}

void testHostedSessionCanStartAndReturnToMenuAcrossRepeatedCycles() {
    ensureTestLevelExists(3);

    for (int cycle = 0; cycle < 3; ++cycle) {
        net::SessionLaunchConfig config =
            net::makeHostSessionLaunchConfig(3, "host-player", static_cast<std::uint16_t>(45220 + cycle));
        config.clientSessionId = static_cast<std::uint32_t>(0x33000010u + cycle);
        config.clientConnectTimeoutUs = 300'000u;

        net::SessionFlowController controller(config);
        expect(controller.start(), "host session should start successfully on each repeated cycle");

        const bool running = waitForPredicate([&]() {
            controller.update(1.0f / 60.0f, nullptr);
            return controller.state() == net::SessionFlowState::Running;
        }, std::chrono::milliseconds(750));

        expect(running, "host session should reach running state on each repeated cycle");
        controller.shutdown("cycle complete");

        expect(controller.state() == net::SessionFlowState::Ended,
               "repeated host cycles should still end cleanly");
        expect(controller.shouldReturnToMainMenu(),
               "repeated host cycles should still signal a return to the main menu");
    }
}

void testHostedSessionAcceptsSecondClientOnPublicPort() {
    ensureTestLevelExists(4);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(4, "host-player", 45140);
    config.clientSessionId = 0x41000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start successfully");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before accepting a joiner");

    net::ClientConfig joinConfig;
    joinConfig.serverHost = "127.0.0.1";
    joinConfig.serverPort = controller.config().proxyClientListenPort;
    joinConfig.playerName = "joiner";
    joinConfig.sessionId = 0x41000002u;
    joinConfig.connectTimeoutUs = 400'000u;

    net::ClientRuntime joiner(joinConfig);
    expect(joiner.start(), "public-port joiner should start its local UDP socket");

    const bool connected = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        return joiner.state() == net::ClientConnectionState::Connected && joiner.hasSnapshot();
    }, std::chrono::milliseconds(900));
    expect(connected, "second client should connect through the public host port");

    for (int frame = 0; frame < 12; ++frame) {
        controller.update(1.0f / 60.0f, nullptr);
        joiner.update(1.0f / 60.0f, nullptr);
    }

    expect(controller.state() == net::SessionFlowState::Running,
           "host session should remain running after a second client joins");
    expect(controller.clientRuntime() != nullptr && controller.clientRuntime()->peerId() == 1u,
           "host local client should stay bound to peer id 1");
    expect(joiner.peerId() == 2u,
           "second public-port joiner should be assigned peer id 2");
    expect(controller.hostedSessionCount() == 2u,
           "hosted session should expose exactly the host and joiner sessions after the second client connects");
    expect(joiner.hasAuthoritativeLevelIdentity() &&
           joiner.authoritativeLevelSlot() == 4 &&
           joiner.authoritativeLevelHash() == net::makeLevelIdentityHash(4),
           "join startup should expose the host-selected level identity to the joining client");
}

void testHostedSessionStaysStableWhenJoinerTargetsInternalServerPort() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(6, "host-player", 45150);
    config.clientSessionId = 0x42000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start successfully");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before testing the internal-port misjoin");

    const std::uint16_t hostPeerId = controller.clientRuntime() != nullptr
        ? controller.clientRuntime()->peerId()
        : 0u;
    expect(hostPeerId == 1u, "host local client should own peer id 1 before the misdirected join");

    net::ClientConfig joinConfig;
    joinConfig.serverHost = "127.0.0.1";
    joinConfig.serverPort = controller.config().serverListenPort;
    joinConfig.playerName = "wrong-port";
    joinConfig.sessionId = 0x42000002u;
    joinConfig.connectTimeoutUs = 80'000u;
    joinConfig.helloRetryIntervalUs = 10'000u;

    net::ClientRuntime joiner(joinConfig);
    expect(joiner.start(), "internal-port joiner should start its local UDP socket");

    const bool failed = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        return joiner.state() == net::ClientConnectionState::TimedOut ||
               joiner.state() == net::ClientConnectionState::Rejected;
    }, std::chrono::milliseconds(900));
    expect(failed, "joiner targeting the internal authoritative port should fail to connect");

    for (int frame = 0; frame < 24; ++frame) {
        controller.update(1.0f / 60.0f, nullptr);
    }

    expect(controller.state() == net::SessionFlowState::Running,
           "host session should remain running after an internal-port misjoin attempt");
    expect(controller.clientRuntime() != nullptr && controller.clientRuntime()->peerId() == hostPeerId,
           "host local client should keep its original peer identity after an internal-port misjoin");
    expect(controller.clientRuntime() != nullptr && controller.clientRuntime()->hasSnapshot(),
           "host local client should keep receiving authoritative snapshots after an internal-port misjoin");
}

void testHostedSessionJoinConvergesUnderDelayedDuplicateHelloRetries() {
    ensureTestLevelExists(7);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(7, "host-player", 45160);
    config.clientSessionId = 0x43000001u;
    config.clientConnectTimeoutUs = 500'000u;
    config.proxyUpstreamLink.baseDelayMs = 120.0f;
    config.proxyUpstreamLink.duplicatePct = 100.0f;
    config.proxyUpstreamLink.seed = 0x2468ACE0u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "delayed-duplicate host session should start successfully");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(1200));
    expect(running, "delayed-duplicate host session should reach running state");

    const net::ProxyStats baselineUpstream = controller.aggregateProxyStats(true);

    net::ClientConfig joinConfig;
    joinConfig.serverHost = "127.0.0.1";
    joinConfig.serverPort = controller.config().proxyClientListenPort;
    joinConfig.playerName = "delayed-joiner";
    joinConfig.sessionId = 0x43000002u;
    joinConfig.connectTimeoutUs = 900'000u;
    joinConfig.helloRetryIntervalUs = 20'000u;

    net::ClientRuntime joiner(joinConfig);
    expect(joiner.start(), "delayed-duplicate joiner should start its local UDP socket");

    const bool connected = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        return joiner.state() == net::ClientConnectionState::Connected && joiner.hasSnapshot();
    }, std::chrono::milliseconds(1500));
    expect(connected, "joiner should still connect when hello packets are delayed, duplicated, and retried");

    const net::ProxyStats upstreamDelta =
        deltaStats(controller.aggregateProxyStats(true), baselineUpstream);
    expect(upstreamDelta.duplicatedPackets > 0u,
           "delayed-duplicate join path should exercise upstream packet duplication");
    expect(controller.clientRuntime() != nullptr && controller.clientRuntime()->peerId() == 1u,
           "host local client should stay on peer id 1 after delayed duplicate join retries");
    expect(joiner.peerId() == 2u,
           "joiner should still receive peer id 2 after delayed duplicate hello retries");
    expect(controller.hostedSessionCount() == 2u,
           "duplicate hello retries should still converge to exactly two hosted sessions");
}

void testHostedSessionLargeFrameSpikeDoesNotTimeoutHostClient() {
    ensureTestLevelExists(8);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(8, "host-player", 45170);
    config.clientSessionId = 0x44000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before testing a large frame spike");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before the large frame spike");

    InputHandler3D::InputState hostInput;
    hostInput.moveInput = Vector2{1.0f, 0.0f};

    for (int frame = 0; frame < 6; ++frame) {
        controller.update(1.0f / 60.0f, &hostInput);
    }

    const net::WorldSnapshot* beforeSpike =
        controller.clientRuntime() != nullptr ? controller.clientRuntime()->latestSnapshot() : nullptr;
    expect(beforeSpike != nullptr, "host client should have a snapshot before the large frame spike");
    const std::uint32_t baselineServerTick = beforeSpike->serverTick;

    controller.update(6.0f, &hostInput);
    controller.update(1.0f / 60.0f, &hostInput);
    controller.update(1.0f / 60.0f, &hostInput);

    std::uint32_t recoveredServerTick = baselineServerTick;
    const bool recovered = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, &hostInput);
        const net::WorldSnapshot* snapshot =
            controller.clientRuntime() != nullptr ? controller.clientRuntime()->latestSnapshot() : nullptr;
        if (snapshot == nullptr || snapshot->serverTick <= baselineServerTick) {
            return false;
        }
        recoveredServerTick = snapshot->serverTick;
        return true;
    }, std::chrono::milliseconds(900));
    expect(controller.state() == net::SessionFlowState::Running,
           "a single oversized host frame should not immediately end the hosted session");
    expect(controller.clientRuntime() != nullptr &&
               controller.clientRuntime()->state() == net::ClientConnectionState::Connected,
           "the host local client should remain connected after a large frame spike");
    expect(recovered,
           "host client should continue receiving authoritative snapshots after the frame spike");
    expect((recoveredServerTick - baselineServerTick) <= 30u,
           "hosted server progression should stay bounded instead of simulating the full multi-second frame gap");
}

void testHostedSessionShortHostStallResumesWithoutEndingSession() {
    ensureTestLevelExists(9);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(9, "host-player", 45180);
    config.clientSessionId = 0x45000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before testing a short host stall");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before testing a short host stall");

    net::ClientConfig joinConfig;
    joinConfig.serverHost = "127.0.0.1";
    joinConfig.serverPort = controller.config().proxyClientListenPort;
    joinConfig.playerName = "stall-joiner";
    joinConfig.sessionId = 0x45000002u;
    joinConfig.connectTimeoutUs = 400'000u;

    net::ClientRuntime joiner(joinConfig);
    expect(joiner.start(), "short-stall joiner should start its local UDP socket");

    const bool connected = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        return joiner.state() == net::ClientConnectionState::Connected && joiner.hasSnapshot();
    }, std::chrono::milliseconds(900));
    expect(connected, "short-stall joiner should connect before the host stall");

    InputHandler3D::InputState hostInput;
    hostInput.moveInput = Vector2{1.0f, 0.0f};

    InputHandler3D::InputState joinInput;
    joinInput.moveInput = Vector2{-1.0f, 0.0f};

    for (int frame = 0; frame < 6; ++frame) {
        controller.update(1.0f / 60.0f, &hostInput);
        joiner.update(1.0f / 60.0f, &joinInput);
    }

    const net::WorldSnapshot* baselineSnapshot = joiner.latestSnapshot();
    expect(baselineSnapshot != nullptr, "joiner should have a baseline snapshot before the host stall");
    const std::uint32_t baselineServerTick = baselineSnapshot->serverTick;

    for (int frame = 0; frame < 240; ++frame) {
        joiner.update(1.0f / 60.0f, &joinInput);
    }

    expect(joiner.state() == net::ClientConnectionState::Connected,
           "a host stall shorter than the silence timeout should not disconnect the joiner by itself");

    const bool recovered = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        const net::WorldSnapshot* snapshot = joiner.latestSnapshot();
        return controller.state() == net::SessionFlowState::Running &&
               snapshot != nullptr &&
               snapshot->serverTick > baselineServerTick;
    }, std::chrono::milliseconds(900));
    expect(recovered, "hosted session should resume and continue publishing snapshots after a short host stall");
}

void testHostedSessionLargeFrameSpikeDoesNotPruneConnectedJoiner() {
    ensureTestLevelExists(1);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(1, "host-player", 45190);
    config.clientSessionId = 0x46000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before testing joiner stability under a large frame spike");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before testing joiner stability");

    net::ClientConfig joinConfig;
    joinConfig.serverHost = "127.0.0.1";
    joinConfig.serverPort = controller.config().proxyClientListenPort;
    joinConfig.playerName = "frame-spike-joiner";
    joinConfig.sessionId = 0x46000002u;
    joinConfig.connectTimeoutUs = 400'000u;

    net::ClientRuntime joiner(joinConfig);
    expect(joiner.start(), "frame-spike joiner should start its local UDP socket");

    const bool connected = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        return joiner.state() == net::ClientConnectionState::Connected && joiner.hasSnapshot();
    }, std::chrono::milliseconds(900));
    expect(connected, "frame-spike joiner should connect before the host frame spike");

    InputHandler3D::InputState hostInput;
    hostInput.moveInput = Vector2{1.0f, 0.0f};

    InputHandler3D::InputState joinInput;
    joinInput.moveInput = Vector2{-1.0f, 0.0f};

    for (int frame = 0; frame < 6; ++frame) {
        controller.update(1.0f / 60.0f, &hostInput);
        joiner.update(1.0f / 60.0f, &joinInput);
    }

    const net::WorldSnapshot* baselineSnapshot = joiner.latestSnapshot();
    expect(baselineSnapshot != nullptr, "joiner should have a snapshot before the host frame spike");
    const std::uint32_t baselineServerTick = baselineSnapshot->serverTick;

    controller.update(6.0f, &hostInput);
    joiner.update(1.0f / 60.0f, &joinInput);
    controller.update(1.0f / 60.0f, &hostInput);
    joiner.update(1.0f / 60.0f, &joinInput);
    controller.update(1.0f / 60.0f, &hostInput);
    joiner.update(1.0f / 60.0f, &joinInput);

    std::uint32_t recoveredServerTick = baselineServerTick;
    const bool recovered = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        const net::WorldSnapshot* snapshot = joiner.latestSnapshot();
        if (snapshot == nullptr || snapshot->serverTick <= baselineServerTick) {
            return false;
        }
        recoveredServerTick = snapshot->serverTick;
        return true;
    }, std::chrono::milliseconds(900));
    expect(controller.state() == net::SessionFlowState::Running,
           "a large host frame spike should not end a healthy hosted session with a connected joiner");
    expect(joiner.state() == net::ClientConnectionState::Connected,
           "joiner should remain connected after the host frame spike");
    expect(controller.hostedSessionCount() == 2u,
           "hosted server should keep both peer sessions after the host frame spike");
    expect(recovered,
           "joiner should continue receiving snapshots after the host frame spike");
    expect((recoveredServerTick - baselineServerTick) <= 30u,
           "hosted server progression should stay bounded for the connected joiner after a large frame spike");
}

void testHostedSessionIdleJoinRemainsStablePastPreviousTimeoutWindow() {
    ensureTestLevelExists(2);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(2, "host-player", 45200);
    config.clientSessionId = 0x47000001u;
    config.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before testing idle keepalive stability");

    const bool running = waitForPredicate([&]() {
        controller.update(1.0f / 60.0f, nullptr);
        return controller.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(running, "hosted session should reach running state before testing idle keepalive stability");

    net::ClientConfig joinConfig;
    joinConfig.serverHost = "127.0.0.1";
    joinConfig.serverPort = controller.config().proxyClientListenPort;
    joinConfig.playerName = "idle-joiner";
    joinConfig.sessionId = 0x47000002u;
    joinConfig.connectTimeoutUs = 400'000u;

    net::ClientRuntime joiner(joinConfig);
    expect(joiner.start(), "idle joiner should start its local UDP socket");

    const bool connected = waitForHostedClientPredicate(&controller, &joiner, [&]() {
        return joiner.state() == net::ClientConnectionState::Connected && joiner.hasSnapshot();
    }, std::chrono::milliseconds(900));
    expect(connected, "idle joiner should connect before the long idle window");

    const net::WorldSnapshot* baselineSnapshot = joiner.latestSnapshot();
    expect(baselineSnapshot != nullptr, "idle joiner should have a baseline snapshot before the long idle window");
    const std::uint32_t baselineServerTick = baselineSnapshot->serverTick;

    for (int frame = 0; frame < 360; ++frame) {
        controller.update(1.0f / 60.0f, nullptr);
        joiner.update(1.0f / 60.0f, nullptr);
    }

    const net::WorldSnapshot* afterIdle = joiner.latestSnapshot();
    expect(controller.state() == net::SessionFlowState::Running,
           "hosted session should remain running while both localhost peers idle past the previous repro window");
    expect(joiner.state() == net::ClientConnectionState::Connected,
           "idle joiner should remain connected past the previous repro window");
    expect(controller.hostedSessionCount() == 2u,
           "hosted server should keep both localhost peers alive while they idle");
    expect(afterIdle != nullptr && afterIdle->serverTick > baselineServerTick,
           "idle localhost peers should keep receiving authoritative snapshots after several seconds of inactivity");
}

void testJoinLocalControlsDoNotBreakTimeoutReturnToMenuStability() {
    ensureTestLevelExists(4);

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(4, "timeout-host", 45260u);
    hostConfig.clientSessionId = 0x48000001u;
    hostConfig.clientConnectTimeoutUs = 400'000u;

    net::SessionFlowController host(hostConfig);
    expect(host.start(), "hosted session should start before local-toggle timeout checks");

    const bool hostRunning = waitForPredicate([&]() {
        host.update(1.0f / 60.0f, nullptr);
        return host.state() == net::SessionFlowState::Running;
    }, std::chrono::milliseconds(900));
    expect(hostRunning, "hosted session should reach running state before starting the timeout join");

    net::SessionLaunchConfig joinConfig =
        net::makeJoinSessionLaunchConfig("127.0.0.1", host.config().proxyClientListenPort, "toggle-joiner");
    joinConfig.clientSessionId = 0x48000002u;
    joinConfig.clientConnectTimeoutUs = 400'000u;
    joinConfig.clientHelloRetryIntervalUs = 20'000u;
    joinConfig.clientServerSilenceTimeoutUs = 120'000u;

    net::SessionFlowController join(joinConfig);
    expect(join.start(), "join session should start before local-toggle timeout checks");

    const bool connected = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return join.state() == net::SessionFlowState::Running &&
                   join.clientRuntime() != nullptr &&
                   join.clientRuntime()->hasSnapshot();
        },
        std::chrono::milliseconds(1200));
    expect(connected, "join session should connect before local-toggle timeout checks");

    InputHandler3D::InputState panelInput;
    panelInput.toggleUIPanel = true;
    host.update(1.0f / 60.0f, nullptr);
    join.update(1.0f / 60.0f, &panelInput);

    InputHandler3D::InputState interpInput;
    interpInput.toggleInterp = true;
    host.update(1.0f / 60.0f, nullptr);
    join.update(1.0f / 60.0f, &interpInput);

    InputHandler3D::InputState predictionInput;
    predictionInput.togglePrediction = true;
    host.update(1.0f / 60.0f, nullptr);
    join.update(1.0f / 60.0f, &predictionInput);

    InputHandler3D::InputState closePanelInput;
    closePanelInput.toggleUIPanel = true;
    host.update(1.0f / 60.0f, nullptr);
    join.update(1.0f / 60.0f, &closePanelInput);

    InputHandler3D::InputState scoreboardInput;
    scoreboardInput.toggleScoreboard = true;
    host.update(1.0f / 60.0f, nullptr);
    join.update(1.0f / 60.0f, &scoreboardInput);

    expect(join.clientRuntime() != nullptr &&
               join.clientRuntime()->hasLocalNetworkControls() &&
               !join.clientRuntime()->localNetworkPanelVisible() &&
               join.clientRuntime()->interpolationEnabled() &&
               join.clientRuntime()->predictionEnabled() &&
               join.clientRuntime()->scoreboardVisible(),
           "join clients should keep their own transport overlay and scoreboard behavior while host-managed sync toggles remain locked");

    const net::WorldSnapshot* baselineSnapshot =
        join.clientRuntime() != nullptr ? join.clientRuntime()->latestSnapshot() : nullptr;
    const std::uint32_t baselineServerTick =
        baselineSnapshot != nullptr ? baselineSnapshot->serverTick : 0u;
    const bool snapshotAdvanced = waitForSessionFlowPredicate(
        &host,
        &join,
        [&]() {
            return join.clientRuntime() != nullptr &&
                   join.clientRuntime()->latestSnapshot() != nullptr &&
                   join.clientRuntime()->latestSnapshot()->serverTick > baselineServerTick;
        },
        std::chrono::milliseconds(900));

    expect(snapshotAdvanced,
           "local-only control toggles should not stop snapshot advancement before the timeout path");

    host.shutdown("host ended");

    const bool ended = waitForPredicate([&]() {
        join.update(1.0f / 60.0f, nullptr);
        return join.state() == net::SessionFlowState::Ended;
    }, std::chrono::milliseconds(900));
    expect(ended,
           "join session should still time out into the terminal ended state after local-only toggles");
    expect(join.shouldReturnToMainMenu(),
           "timed-out join sessions should still signal a return to the main menu after local-only toggles");
    expect(join.statusMessage().find("timed out") != std::string::npos,
           "timed-out join sessions should preserve the timeout reason after local-only toggles");
}

struct SeededImpairmentResult {
    net::ProxyStats upstream{};
    std::uint32_t ackedInputSeq{0u};
};

net::ProxyStats deltaStats(const net::ProxyStats& after, const net::ProxyStats& before) {
    net::ProxyStats delta;
    delta.receivedPackets = after.receivedPackets - before.receivedPackets;
    delta.forwardedPackets = after.forwardedPackets - before.forwardedPackets;
    delta.droppedPackets = after.droppedPackets - before.droppedPackets;
    delta.duplicatedPackets = after.duplicatedPackets - before.duplicatedPackets;
    delta.reorderedPackets = after.reorderedPackets - before.reorderedPackets;
    delta.queuedPackets = after.queuedPackets;
    return delta;
}

SeededImpairmentResult runSeededImpairmentCase(std::uint16_t publicPort) {
    ensureTestLevelExists(5);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(5, "seeded-host", publicPort);
    config.clientSessionId = 0x51515151u;
    config.discoveryPort = 0u;
    config.proxyServerListenPort = 0u;
    config.clientConnectTimeoutUs = 6'000'000u;
    config.clientHelloRetryIntervalUs = 50'000u;
    config.proxyUpstreamLink.duplicatePct = 45.0f;
    config.proxyUpstreamLink.seed = 0x12345678u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "seeded impairment host session should start successfully");

    constexpr int kStartupFrames = 300;
    bool running = false;
    for (int frame = 0; frame < kStartupFrames; ++frame) {
        controller.update(1.0f / 60.0f, nullptr);
        if (controller.state() == net::SessionFlowState::Running &&
            controller.clientRuntime() != nullptr &&
            controller.clientRuntime()->hasSnapshot()) {
            running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(running,
           "seeded impairment host session should reach running state");
    expect(controller.clientRuntime() != nullptr && controller.clientRuntime()->hasSnapshot(),
           "seeded impairment host session should expose an authoritative snapshot before the command burst");

    const net::ProxyStats baselineStats = controller.aggregateProxyStats(true);
    const std::uint32_t baselineAck = controller.clientRuntime() != nullptr
        ? controller.clientRuntime()->lastAckedInputSeq()
        : 0u;

    InputHandler3D::InputState input;
    input.moveInput = Vector2{1.0f, 0.0f};
    constexpr std::uint64_t kCommandFrames = 24u;
    for (int frame = 0; frame < static_cast<int>(kCommandFrames); ++frame) {
        controller.update(1.0f / 60.0f, &input);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    constexpr int kDrainFrames = 180;
    bool commandBurstAcked = false;
    for (int frame = 0; frame < kDrainFrames; ++frame) {
        controller.update(1.0f / 60.0f, nullptr);
        const std::uint32_t ackedInputSeq = controller.clientRuntime() != nullptr
            ? controller.clientRuntime()->lastAckedInputSeq() - baselineAck
            : 0u;
        if (ackedInputSeq >= kCommandFrames) {
            commandBurstAcked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    SeededImpairmentResult result;
    result.upstream = deltaStats(controller.aggregateProxyStats(true), baselineStats);
    result.ackedInputSeq = controller.clientRuntime() != nullptr
        ? controller.clientRuntime()->lastAckedInputSeq() - baselineAck
        : 0u;
    expect(result.upstream.receivedPackets >= kCommandFrames,
           "seeded impairment run should receive the scripted upstream command burst");
    expect(commandBurstAcked && result.ackedInputSeq >= kCommandFrames,
           "seeded impairment run should authoritatively acknowledge the scripted command burst");
    return result;
}

void testSeededLocalhostImpairmentRunAppliesConfiguredLink() {
    const SeededImpairmentResult first = runSeededImpairmentCase(0u);
    const SeededImpairmentResult second = runSeededImpairmentCase(0u);

    expect(first.upstream.duplicatedPackets > 0u &&
               second.upstream.duplicatedPackets > 0u,
           "seeded impairment runs should apply the configured duplicate policy");
    expect(first.upstream.droppedPackets == 0u &&
               second.upstream.droppedPackets == 0u,
           "seeded impairment runs should not drop packets when loss is disabled");
    expect(first.ackedInputSeq > 0u && second.ackedInputSeq > 0u,
           "seeded impairment runs should continue to advance authoritative command acks");
}

void testHostSessionFailsWhenSelectedLevelCannotLoad() {
    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(99, "invalid-level-host", 45210);
    config.clientSessionId = 0x99000001u;
    config.clientConnectTimeoutUs = 300'000u;

    net::SessionFlowController controller(config);
    expect(!controller.start(),
           "host session startup should fail immediately when the selected authoritative level cannot be loaded");

    expect(controller.state() == net::SessionFlowState::Failed,
           "host session should enter the failed state when the selected authoritative level cannot be loaded locally");
    expect(controller.statusMessage().find("level_load_failed") != std::string::npos,
           "failed host startup should expose an explicit level load failure reason");
}

void testServerRuntimeSpectatorParticipationIsAuthoritative() {
    net::ServerConfig config;
    config.levelSlot = 4;
    config.levelHash = net::makeLevelIdentityHash(4);
    config.respawnDelaySeconds = 0.25f;

    net::ServerRuntime server(config);
    net::WelcomeMessage welcome;
    expect(server.acceptClient(net::HelloMessage{0x21000001u, 0u, "spectator-player", sim::TeamId::Attacker},
                               1'000'000u,
                               &welcome),
           "server runtime should accept a playable client before spectator assertions run");
    expect(welcome.participantState.team == sim::TeamId::Attacker &&
               welcome.participantState.participation == sim::ParticipationState::Playing,
           "initial welcome should expose the requested playable participation state");
    server.takePendingPackets();

    expect(server.handleControlPayload(1u, net::TeamChangeRequest{sim::TeamId::Spectator}, 1'010'000u),
           "server runtime should accept an explicit authoritative spectator request");
    const net::ClientSession* spectatorSession = server.findSession(1u);
    expect(spectatorSession != nullptr &&
               spectatorSession->participantState().team == sim::TeamId::Spectator &&
               spectatorSession->participantState().participation == sim::ParticipationState::Spectating &&
               !spectatorSession->participantState().control.controlsActor(),
           "spectator entry should update authoritative participant state to spectating without a controlled actor");

    const sim::RosterEntry& rosterEntry = requireRosterEntry(server.worldState().roster, 1);
    expect(rosterEntry.team == sim::TeamId::Spectator &&
               rosterEntry.participation == sim::ParticipationState::Spectating &&
               !rosterEntry.control.controlsActor() &&
               !rosterEntry.alive,
           "spectator entry should update authoritative roster participation and team state");
    expect(sim::findPlayer(server.worldState(), 1) == nullptr,
           "spectator entry should remove the authoritative playable actor");

    server.tickOnce(1'400'000u);
    expect(server.findSession(1u) != nullptr &&
               server.findSession(1u)->participantState().participation == sim::ParticipationState::Spectating &&
               sim::findPlayer(server.worldState(), 1) == nullptr,
           "spectator participation should not auto-return through the respawn path after time advances");
}

void testServerRuntimeSpectatorExitRestoresPlayableActorOrTeamSelection() {
    net::ServerConfig restoreConfig;
    restoreConfig.levelSlot = 5;
    restoreConfig.levelHash = net::makeLevelIdentityHash(5);

    net::ServerRuntime restoreServer(restoreConfig);
    expect(restoreServer.acceptClient(
               net::HelloMessage{0x22000001u, 0u, "restore-player", sim::TeamId::Defender},
               2'000'000u),
           "restore-path server should accept the initial playable client");
    restoreServer.takePendingPackets();
    expect(restoreServer.handleControlPayload(1u, net::TeamChangeRequest{sim::TeamId::Spectator}, 2'010'000u),
           "restore-path server should enter authoritative spectator mode");
    expect(restoreServer.handleControlPayload(1u, net::TeamChangeRequest{sim::TeamId::None}, 2'020'000u),
           "spectator exit should accept an explicit return request");
    const net::ClientSession* restoredSession = restoreServer.findSession(1u);
    expect(restoredSession != nullptr &&
               restoredSession->participantState().team == sim::TeamId::Defender &&
               restoredSession->participantState().participation == sim::ParticipationState::Playing &&
               restoredSession->participantState().control.controlsActor(),
           "spectator exit should restore the previous playable actor when a valid team is available");
    expect(sim::findPlayer(restoreServer.worldState(), 1) != nullptr,
           "restoring from spectator should recreate the authoritative playable actor");

    net::ServerConfig selectionConfig;
    selectionConfig.levelSlot = 6;
    selectionConfig.levelHash = net::makeLevelIdentityHash(6);

    net::ServerRuntime selectionServer(selectionConfig);
    net::WelcomeMessage spectatorWelcome;
    expect(selectionServer.acceptClient(
               net::HelloMessage{0x22000002u, 0u, "selection-player", sim::TeamId::Spectator},
               3'000'000u,
               &spectatorWelcome),
           "team-selection fallback server should accept an initial spectator join");
    expect(spectatorWelcome.participantState.team == sim::TeamId::Spectator &&
               spectatorWelcome.participantState.participation == sim::ParticipationState::Spectating,
           "spectator joins should start in authoritative spectator participation state");
    selectionServer.takePendingPackets();

    expect(selectionServer.handleControlPayload(1u, net::TeamChangeRequest{sim::TeamId::None}, 3'010'000u),
           "spectator exit should also accept a return request when no prior playable team exists");
    const net::ClientSession* selectionSession = selectionServer.findSession(1u);
    expect(selectionSession != nullptr &&
               selectionSession->participantState().team == sim::TeamId::None &&
               selectionSession->participantState().participation == sim::ParticipationState::TeamSelection &&
               !selectionSession->participantState().control.controlsActor(),
           "spectator exit should route back through authoritative team selection when no prior playable team exists");
    expect(sim::findPlayer(selectionServer.worldState(), 1) == nullptr,
           "team-selection fallback should preserve the absence of an authoritative playable actor");
}

}  // namespace

int main() {
    try {
        const testsupport::ScopedTestDataRoot scopedDataRoot("netcodesim-session-launch-flow");
        (void)scopedDataRoot;

        testAppShellOwnsWindowLifecycleCalls();
        testLoadingSameSavedLevelSlotYieldsDeterministicSchema();
        testHostedLevelLoadingExposesEditorEnemiesAsAuthoritativeBotSpawns();
        testHostedEditorPlacedCharactersBecomeReplicatedTeamBots();
        testHostedEditorLevelWithoutCharactersDoesNotKeepLegacyEnemy();
        testEditorOwnedLevelSaveAndLoadRemainPortableToSessionStartup();
        testMainMenuRoutesFirstClassSurfacesThroughTypedSelection();
        testAppFlowRoutesTopLevelShellSelections();
        testMainShellComposesStudyModeThroughSharedSessionFlow();
        testSessionFlowControllerDependsOnTransportArtifactBoundary();
        testSessionComposerDescribesHostAndJoinRuntimeComposition();
        testSessionLaunchConfigCapturesSurfaceParticipantCountAndStudyOptions();
        testSessionComposerBuildsStudyReplayAndEditorEntryPoints();
        testAppFlowStartsSessionThroughComposerSeam();
        testAppFlowRoutesReplayAndEditorWithoutRuntimeOwnership();
        testSplitScreenSessionControlWaitsForReadyRightParticipantBeforePlayerBinding();
        testSplitScreenSessionControlDisablesRightSlotAndTemporaryParticipant();
        testAppFlowAndSessionComposerOwnLaunchStructure();
        testMainShellRoutesTopLevelSelectionsThroughAppFlow();
        testMainShellMaintainsExplicitMenuBackStack();
        testHostLaunchStartsServerProxyClientAndEntersRunning();
        testHostedMultiplayerReplayHotkeysStopRecordingBeforePlayback();
        testHostedWelcomeCarriesSelectedSessionMetadata();
        testHostedSessionAdvertisesAuthoritativeMetadataAfterStartup();
        testHostedSessionShutdownStopsDiscoveryAdvertisementAndAllowsBrowserExpiry();
        testJoinLaunchStartsLocalProxyAndRoutesClientThroughLoopback();
        testHostLaunchSpawnsConfiguredBotsIntoHostedRoster();
        testJoinClientCanConnectAndPlayThroughLocalProxyPath();
        testJoinSessionFlowExposesGhostTrackForDelayedRemotePlayers();
        testDiscoverySelectedJoinPreservesBaselineGameplayRosterAndTeamScore();
        testLateJoinReceivesCurrentAuthoritativeSnapshotStateOnHostedFlow();
        testFailedJoinReturnsToMenuWithErrorAndRetryPath();
        testFailedBrowserSelectedJoinReturnsToMenuWithErrorAndRetryPath();
        testSessionEndSignalsReturnToMainMenuWithoutRestart();
        testSessionBackClosesUiModeBeforeEndingSession();
        testSessionEnterReleasesMouseWithoutOpeningRuntimeSettings();
        testSessionBackCancelsTeamMenuBeforeEndingSession();
        testHostedSessionCanStartAndReturnToMenuAcrossRepeatedCycles();
        testHostedSessionAcceptsSecondClientOnPublicPort();
        testHostedSessionStaysStableWhenJoinerTargetsInternalServerPort();
        testHostedSessionJoinConvergesUnderDelayedDuplicateHelloRetries();
        testHostedSessionLargeFrameSpikeDoesNotTimeoutHostClient();
        testHostedSessionShortHostStallResumesWithoutEndingSession();
        testHostedSessionLargeFrameSpikeDoesNotPruneConnectedJoiner();
        testHostedSessionIdleJoinRemainsStablePastPreviousTimeoutWindow();
        testHostSessionFlowExposesGhostTrackForLaggedJoinPlayer();
        testJoinLocalControlsDoNotBreakTimeoutReturnToMenuStability();
        testSeededLocalhostImpairmentRunAppliesConfiguredLink();
        testHostSessionFailsWhenSelectedLevelCannotLoad();
        testServerRuntimeSpectatorParticipationIsAuthoritative();
        testServerRuntimeSpectatorExitRestoresPlayableActorOrTeamSelection();
        std::cout << "SessionLaunchFlowTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "SessionLaunchFlowTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
