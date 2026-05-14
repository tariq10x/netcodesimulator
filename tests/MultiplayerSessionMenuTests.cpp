#include "DisplayManager.hpp"
#include "MainMenu.hpp"
#include "LevelSelectMenu.hpp"
#include "MultiplayerSessionMenu.hpp"
#include "SettingsMenu.hpp"
#include "LevelData.hpp"
#include "TestDataRoot.hpp"
#include "app/CharacterPresetStore.hpp"
#include "net/SessionFlowController.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kMenuDiscoveryPortBase = 48100u;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

    throw std::runtime_error("failed to locate repository root for menu characterization tests");
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

void ensureTestLevelExists(int slot) {
    LevelData::Level level("Menu Test Level " + std::to_string(slot));
    level.obstacles.push_back(LevelData::Obstacle{
        static_cast<float>(slot),
        -static_cast<float>(slot),
        6.0f,
        3.0f,
        2.5f,
        Color{120, 200, 120, 255}
    });
    expect(LevelData::saveLevel(level, slot),
           "multiplayer session menu test level fixture should save successfully");
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
           context + " should keep the selected team highlighted until the primary launch action is confirmed");

    const MultiplayerSessionMenu::Result confirmed = menu->submitForTest();
    expect(confirmed.submitted && confirmed.launchConfig.preferredTeam == selectedTeam,
           context + " should submit successfully when the primary launch action confirms the selected team");
    return confirmed.launchConfig;
}

void testMainMenuExposesHostAndJoinFlow() {
    MainMenu menu;

    expect(menu.optionCount() == 6, "main menu should expose the consolidated top-level mode set");
    expect(std::string(menu.optionLabel(0)) == "Multiplayer",
           "first main menu option should expose the multiplayer submenu");
    expect(std::string(menu.optionLabel(1)) == "Lab Study",
           "second main menu option should expose the consolidated single-player study mode");
    expect(std::string(menu.optionLabel(3)) == "Character Editor",
           "main menu should expose a character editor entry");
    expect(std::string(menu.optionLabel(4)) == "Replay Studio",
           "main menu should expose a replay studio entry");
    expect(std::string(menu.optionLabel(5)) == "Settings", "main menu should expose a settings entry");

    const GameMode multiplayerMode = menu.triggerOptionForTest(0);
    expect(multiplayerMode == GameMode::MAIN_MENU,
           "multiplayer root option should open its host/join submenu in-place");
    expect(menu.showingMultiplayerSubmenu(),
           "multiplayer root option should activate the multiplayer submenu");
    expect(menu.optionCount() == 2,
           "multiplayer submenu should only expose Host and Join as centered options");
    expect(std::string(menu.optionLabel(0)) == "Host", "first multiplayer submenu option should be Host");
    expect(std::string(menu.optionLabel(1)) == "Join", "second multiplayer submenu option should be Join");

    const Rectangle submenuBackButton = menu.multiplayerBackButtonRectForTest();
    expect(submenuBackButton.width > 0.0f && submenuBackButton.height > 0.0f,
           "multiplayer submenu should expose its Back action as a visible top-left button");
    expect(std::string(menu.optionLabel(2)).empty(),
           "multiplayer submenu should not keep a centered Back option");

    expect(menu.triggerSubmenuBackForTest() == GameMode::MAIN_MENU,
           "top-left multiplayer submenu Back should return to the root main menu");
    expect(!menu.showingMultiplayerSubmenu(),
           "top-left multiplayer submenu Back should restore the root menu view");
    expect(menu.triggerOptionForTest(0) == GameMode::MAIN_MENU,
           "multiplayer root option should reopen the submenu after testing top-left Back");

    const GameMode hostMode = menu.triggerOptionForTest(0);
    expect(hostMode == GameMode::LEVEL_SELECT,
           "host flow should route through level selection before multiplayer setup");
    expect(menu.requestedSessionMode() == net::SessionLaunchMode::Host,
           "host option should mark the requested multiplayer mode as host");
    expect(menu.showingMultiplayerSubmenu(),
           "forward navigation should preserve the multiplayer submenu state so shell back navigation can restore it");

    menu.clearRequestedSessionMode();
    const GameMode joinMode = menu.triggerOptionForTest(1);
    expect(joinMode == GameMode::MULTIPLAYER_SESSION,
           "join flow should transition into multiplayer session setup without CLI flags");
    expect(menu.requestedSessionMode() == net::SessionLaunchMode::Join,
           "join option should mark the requested multiplayer mode as join");

    menu.resetToRootView();
    const GameMode characterMode = menu.triggerOptionForTest(3);
    expect(characterMode == GameMode::CHARACTER_EDITOR,
           "character editor option should transition into the character editor screen");

    menu.resetToRootView();
    const GameMode replayMode = menu.triggerOptionForTest(4);
    expect(replayMode == GameMode::REPLAY_STUDIO,
           "replay studio option should transition into the saved replay browser");

    menu.resetToRootView();
    const GameMode settingsMode = menu.triggerOptionForTest(5);
    expect(settingsMode == GameMode::SETTINGS,
           "settings option should transition into the display settings screen");

    LevelSelectMenu hostLevelMenu("SELECT HOST LEVEL", "Pick the map for the host session");
    expect(hostLevelMenu.titleText() == "SELECT HOST LEVEL",
           "host flow should be able to present a dedicated level-selection title");
}

void testMainMenuBackShortcutDoesNotCascadeIntoQuit() {
    MainMenu menu;

    expect(menu.triggerOptionForTest(0) == GameMode::MAIN_MENU,
           "multiplayer root option should open its submenu before testing back-shortcut handling");
    expect(menu.showingMultiplayerSubmenu(),
           "back-shortcut regression test requires the multiplayer submenu to be open");

    expect(menu.triggerQuitShortcutForTest() == GameMode::MAIN_MENU,
           "back from the multiplayer submenu should return to the root menu instead of quitting");
    expect(!menu.showingMultiplayerSubmenu(),
           "back from the multiplayer submenu should restore the root menu view");
    expect(menu.quitShortcutSuppressedForTest(),
           "returning to the root menu via the back shortcut should suppress the next quit edge until release");

    expect(menu.triggerQuitShortcutForTest() == GameMode::MAIN_MENU,
           "the same held back shortcut should not immediately cascade into a root-menu quit");

    menu.releaseQuitShortcutSuppressionForTest();
    expect(!menu.quitShortcutSuppressedForTest(),
           "releasing the shortcut should clear the temporary quit suppression");
    expect(menu.triggerQuitShortcutForTest() == GameMode::QUIT,
           "after the shortcut is released, a new quit shortcut on the root menu should still exit");
}

void testMainMenuCanSuppressQuitShortcutAfterShellReturn() {
    MainMenu menu;
    menu.suppressQuitShortcutUntilReleased();

    expect(menu.triggerQuitShortcutForTest() == GameMode::MAIN_MENU,
           "shell-level quit suppression should ignore the first carried-over quit shortcut");

    menu.releaseQuitShortcutSuppressionForTest();
    expect(menu.triggerQuitShortcutForTest() == GameMode::QUIT,
           "once the carried-over shortcut is released, the root menu should still allow quitting");
}

void testMainMenuSurfacesExternalLinkIntentsWithoutChangingNavigationState() {
    MainMenu menu;

    expect(menu.requestedExternalLink() == MainMenu::ExternalLinkTarget::None,
           "main menu should start without any pending external-link intent");

    expect(menu.triggerExternalLinkForTest(MainMenu::ExternalLinkTarget::YouTube) ==
               GameMode::MAIN_MENU,
           "YouTube button activation should not leave the main menu");
    expect(menu.requestedExternalLink() == MainMenu::ExternalLinkTarget::YouTube,
           "YouTube button activation should surface an explicit external-link intent");
    expect(menu.requestedSessionMode() == net::SessionLaunchMode::None &&
               menu.requestedNavigation().surface == MainMenu::AppShellSurface::None,
           "external-link activation should not mutate gameplay navigation state");

    menu.clearRequestedExternalLink();
    expect(menu.triggerExternalLinkForTest(MainMenu::ExternalLinkTarget::Patreon) ==
               GameMode::MAIN_MENU,
           "Patreon button activation should not leave the main menu");
    expect(menu.requestedExternalLink() == MainMenu::ExternalLinkTarget::Patreon,
           "Patreon button activation should surface an explicit external-link intent");

    menu.clearRequestedExternalLink();
    expect(menu.triggerExternalLinkForTest(MainMenu::ExternalLinkTarget::GitHub) ==
               GameMode::MAIN_MENU,
           "GitHub button activation should not leave the main menu");
    expect(menu.requestedExternalLink() == MainMenu::ExternalLinkTarget::GitHub,
           "GitHub button activation should surface an explicit external-link intent");
}

void testMainMenuRendersPlatformGlyphsForExternalLinks() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string menuSource = readTextFile(repoRoot / "include/MainMenu.hpp");
    const std::filesystem::path githubMarkAsset =
        repoRoot / "assets/icons/GitHub_Invertocat_White.png";

    expect(menuSource.find("drawYouTubeGlyph(") != std::string::npos &&
               menuSource.find("drawPatreonGlyph(") != std::string::npos &&
               menuSource.find("drawGitHubGlyph(") != std::string::npos,
           "main menu should render social actions through explicit YouTube, Patreon, and GitHub glyph helpers");
    expect(menuSource.find("assets/icons/GitHub_Invertocat_White.png") != std::string::npos &&
               std::filesystem::exists(githubMarkAsset),
           "main menu should render the GitHub social action with the official Invertocat asset from the checked-in assets directory");
    expect(menuSource.find("button.text") == std::string::npos,
           "main menu social buttons should no longer draw raw text labels inside the external-link buttons");
    expect(menuSource.find("renderExternalButton(") != std::string::npos,
           "main menu should own a dedicated social-button render path instead of drawing the platform buttons like ordinary text buttons");
    expect(menuSource.find("shortcutBadgeRect(") == std::string::npos &&
               menuSource.find("std::to_string(i + 1)") == std::string::npos,
           "main menu should no longer render visible numeric shortcut badges beside the option labels");
}

void testMainMenuUsesAnimatedGameplayBackdrop() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string menuSource = readTextFile(repoRoot / "include/MainMenu.hpp");

    expect(menuSource.find("renderGameplayBackdrop(") != std::string::npos &&
               menuSource.find("drawBackdropArena(") != std::string::npos &&
               menuSource.find("drawBackdropActor(") != std::string::npos &&
               menuSource.find("drawBackdropTracer(") != std::string::npos,
           "main menu should own a dedicated animated gameplay backdrop instead of relying on a flat static clear");
    expect(menuSource.find("BeginMode3D(") != std::string::npos &&
               menuSource.find("GetTime()") != std::string::npos &&
               menuSource.find("DrawRectangleGradientH(") != std::string::npos,
           "main menu backdrop should animate a 3D scene and composite it for readable foreground UI");
    expect(menuSource.find("DrawCylinder(rootPosition") != std::string::npos &&
               menuSource.find("Config::ENEMY_BODY_HEIGHT") != std::string::npos &&
               menuSource.find("Config::ENEMY_HEAD_OFFSET") != std::string::npos,
           "main menu backdrop actors should reuse the in-game cylinder-and-head proportions instead of a custom menu-only character silhouette");
}

void testMainMenuUsesSquareSocialButtons() {
    MainMenu menu;

    const float titleY = menu.rootTitleYForTest();
    const float subtitleY = menu.rootSubtitleYForTest();
    const Rectangle firstOption = menu.optionRectForTest(0);
    const Rectangle secondOption = menu.optionRectForTest(1);
    const Rectangle lastOption = menu.optionRectForTest(5);
    const Rectangle youtubeButton =
        menu.externalButtonRectForTest(MainMenu::ExternalLinkTarget::YouTube);
    const Rectangle patreonButton =
        menu.externalButtonRectForTest(MainMenu::ExternalLinkTarget::Patreon);
    const Rectangle githubButton =
        menu.externalButtonRectForTest(MainMenu::ExternalLinkTarget::GitHub);

    expect(youtubeButton.width > 0.0f && youtubeButton.width == youtubeButton.height,
           "YouTube main-menu button should use a square icon tile instead of a wide text pill");
    expect(patreonButton.width > 0.0f && patreonButton.width == patreonButton.height,
           "Patreon main-menu button should use a square icon tile instead of a wide text pill");
    expect(githubButton.width > 0.0f && githubButton.width == githubButton.height,
           "GitHub main-menu button should use a square icon tile instead of a wide text pill");

    const float youtubeCenterX = youtubeButton.x + youtubeButton.width * 0.5f;
    const float patreonCenterX = patreonButton.x + patreonButton.width * 0.5f;
    const float githubCenterX = githubButton.x + githubButton.width * 0.5f;
    const float centeredPairMidpoint = (youtubeCenterX + githubCenterX) * 0.5f;
    const float screenCenterX = Config::SCREEN_WIDTH * 0.5f;

    expect(std::fabs(patreonCenterX - screenCenterX) < 0.01f,
           "the middle Patreon social button should sit exactly on the menu centerline");
    expect(std::fabs(centeredPairMidpoint - screenCenterX) < 0.01f,
           "the outer social buttons should stay symmetrically centered around the menu midpoint");
    expect(std::fabs((patreonCenterX - youtubeCenterX) - (githubCenterX - patreonCenterX)) < 0.01f,
           "social buttons should keep equal spacing so the three-icon row remains visually balanced");

    const float menuGap = secondOption.y - (firstOption.y + firstOption.height);
    const float socialGap = youtubeButton.y - (lastOption.y + lastOption.height);
    const float titleToSubtitleGap =
        subtitleY - (titleY + TypographyTheme::style(TypographyStyleId::MenuTitle).lineHeight);
    const float subtitleToActionsGap =
        firstOption.y -
        (subtitleY + TypographyTheme::style(TypographyStyleId::AppSubtitle).lineHeight);
    expect(std::fabs(titleY - 150.0f) < 0.01f,
           "root menu title should sit lower so the full menu composition occupies the screen instead of leaving a disconnected top band");
    expect(std::fabs(titleToSubtitleGap - 10.0f) < 0.01f,
           "root menu subtitle should sit tightly under the title as one typographic block");
    expect(std::fabs(subtitleToActionsGap - 38.0f) < 0.01f,
           "root menu actions should sit noticeably closer to the title block so the screen reads as one ordered stack");
    expect(std::fabs(menuGap - 16.0f) < 0.01f,
           "main menu buttons should keep a tight internal gap so the primary actions read as a single group");
    expect(std::fabs(socialGap - 36.0f) < 0.01f,
           "the social button row should sit on a deliberate secondary gap below the primary action stack");
}

void testMainMenuExternalLinksUsePublicUrls() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string mainSource = readTextFile(repoRoot / "src/main_3d.cpp");

    expect(mainSource.find("case MainMenu::ExternalLinkTarget::YouTube:") != std::string::npos &&
               mainSource.find("https://www.youtube.com/@tariq10x") != std::string::npos &&
               mainSource.find("case MainMenu::ExternalLinkTarget::Patreon:") != std::string::npos &&
               mainSource.find("https://www.patreon.com/c/tariq10x") != std::string::npos &&
               mainSource.find("case MainMenu::ExternalLinkTarget::GitHub:") != std::string::npos &&
               mainSource.find("https://github.com/tariq10x/netcodesimulator") != std::string::npos,
           "main-menu social buttons should resolve to the public YouTube, Patreon, and GitHub URLs");
}

void testLevelSelectExposesVisibleBackAction() {
    LevelSelectMenu menu("SELECT HOST LEVEL", "Pick the map for the host session");

    const Rectangle backButton = menu.backButtonRectForTest();
    expect(backButton.width > 0.0f && backButton.height > 0.0f,
           "level-selection menus should expose a visible back button instead of relying on hidden shortcuts only");

    const LevelSelectMenu::SelectResult backResult = menu.triggerBackForTest();
    expect(backResult.mode == GameMode::MAIN_MENU && backResult.levelSlot == -1,
           "level-selection back action should return to the main menu without selecting a slot");
}

void testJoinSetupBackShortcutIgnoresQWhileEditingTextFields() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    menu.setActiveFieldForTest("Player Name");

    expect(menu.hasActiveFieldForTest(),
           "join-setup regression test requires a focused text field before exercising Q handling");

    const MultiplayerSessionMenu::Result qResult = menu.handleBackShortcutForTest(KEY_Q);
    expect(qResult.mode == GameMode::MULTIPLAYER_SESSION && !qResult.submitted,
           "typing Q into an active text field should not navigate away from the setup screen");
    expect(menu.hasActiveFieldForTest(),
           "the focused text field should remain active when Q is treated as plain text input");

    const MultiplayerSessionMenu::Result escapeResult = menu.handleBackShortcutForTest(KEY_ESCAPE);
    expect(escapeResult.mode == GameMode::MULTIPLAYER_SESSION && !escapeResult.submitted,
           "Escape should clear field focus before the multiplayer setup treats it as a back shortcut");
    expect(!menu.hasActiveFieldForTest(),
           "Escape should clear the active text field instead of immediately leaving the menu");
}

void testBusySetupClearsFieldFocusSoBackCanWin() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    expect(menu.hasActiveFieldForTest(),
           "busy-setup regression test requires the default host field to start focused");

    menu.setStatusMessage("starting session", true);
    expect(menu.busy(), "busy setup regression test requires the menu to enter its busy state");
    expect(!menu.hasActiveFieldForTest(),
           "entering the busy startup state should clear text-field focus so back input is not trapped by the player-name field");
}

void testBusyHostSetupBackActionCancelsPendingStateAndReturns() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setStatusMessage("starting session", true);

    const MultiplayerSessionMenu::Result backResult = menu.triggerBackForTest();
    expect(backResult.mode == GameMode::MAIN_MENU && !backResult.submitted,
           "busy host setup back actions should still navigate back instead of trapping the user in startup");
    expect(!menu.busy() && menu.statusMessage().empty(),
           "busy host setup back actions should clear the pending startup state as they return");
    expect(!menu.hasActiveFieldForTest(),
           "busy host setup back actions should leave no focused text field behind");
}

void testHostSetupExposesVisibleUpperLeftBackAction() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);

    const Rectangle cornerBackButton = menu.cornerBackButtonRectForTest();
    expect(cornerBackButton.width > 0.0f && cornerBackButton.height > 0.0f,
           "host setup should expose a visible upper-left back button like the other menus");

    const MultiplayerSessionMenu::Result backResult = menu.triggerBackForTest();
    expect(backResult.mode == GameMode::MAIN_MENU && !backResult.submitted,
           "host setup back actions should return to the previous shell screen without launching");
}

void testHostSetupBackShortcutWinsEvenWithFocusedField() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setActiveFieldForTest("Player Name");

    expect(menu.hasActiveFieldForTest(),
           "host-setup back-shortcut regression test requires a focused text field");

    const MultiplayerSessionMenu::Result qResult = menu.handleBackShortcutForTest(KEY_Q);
    expect(qResult.mode == GameMode::MAIN_MENU && !qResult.submitted,
           "host setup should let Q navigate back immediately even when a text field is focused");
    expect(!menu.hasActiveFieldForTest(),
           "host setup back shortcuts should clear any focused text field as they navigate back");
}

void testHostSetupBuildsPublicJoinLaunchConfig() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setPreferredTeam(sim::TeamId::Defender);
    menu.setSessionLabel("Player LAN Match");
    menu.setTickRateHz(120u);
    menu.setSnapshotRateHz(60u);
    menu.setMaxHumanPlayersText("4");
    menu.setShotEvaluationMode(net::ShotEvaluationMode::LivePosition);
    menu.setServerPortText("41000");
    menu.setTotalBotCountText("4");

    std::string error;
    expect(menu.validate(&error), "host setup with selected level and valid port should validate");

    const net::SessionLaunchConfig config = menu.buildLaunchConfig();
    expect(config.mode == net::SessionLaunchMode::Host, "host launch config should preserve host mode");
    expect(config.levelSlot == 4, "host launch config should preserve selected level slot");
    expect(config.preferredTeam == sim::TeamId::Defender,
           "host launch config should preserve the selected team");
    expect(config.startLocalServer, "host launch config should start a local authoritative server");
    expect(config.startLocalProxy, "host launch config should start a local impairment proxy");
    expect(config.sessionLabel == "Player LAN Match",
           "host launch config should preserve the configured session label");
    expect(config.tickRateHz == 120u && config.snapshotRateHz == 60u,
           "host launch config should preserve the configured authoritative tick and snapshot cadence");
    expect(config.maxHumanPlayers == 4u,
           "host launch config should preserve the configured human-player cap");
    expect(config.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "host launch config should preserve the configured shot-evaluation rule");
    expect(config.attackerBotCount == 2u && config.defenderBotCount == 2u,
           "host launch config should split the requested total bot count evenly across teams");
    expect(config.clientConnectHost == "127.0.0.1",
           "host local client should connect to loopback rather than bypassing the proxy");
    expect(config.proxyClientListenPort == 41000u,
           "the user-entered host port should be the proxy's public client-facing port");
    expect(config.clientConnectPort == config.proxyClientListenPort,
           "host local client should connect to the same public join port");
    expect(config.serverListenPort == 0u && config.proxyUpstreamServerPort == 0u,
           "internal server upstream ports should be assigned during startup rather than exposed in the menu");

    const std::string previewLine = menu.previewLine();
    const std::string expectedRuleSummary =
        net::shotEvaluationModeSummary(config.shotEvaluationMode);
    expect(previewLine.find("Join on 127.0.0.1:41000") != std::string::npos,
           "host preview should advertise the selected public join port");
    expect(previewLine.find("Session Player LAN Match") != std::string::npos,
           "host preview should include the configured session label");
    expect(previewLine.find("Tick 120") != std::string::npos &&
               previewLine.find("Snap 60") != std::string::npos,
           "host preview should include the configured authoritative cadence");
    expect(previewLine.find("Humans 4") != std::string::npos,
           "host preview should include the configured human-player cap");
    expect(previewLine.find("Rule Live Position") != std::string::npos,
           "host preview should include the configured shot-evaluation rule");
    expect(previewLine.find(expectedRuleSummary) != std::string::npos,
           "host preview should expose the same canonical shot-rule explanation later used by LAN discovery");
    expect(previewLine.find("Bots 4") != std::string::npos &&
               previewLine.find("2 attackers / 2 defenders") != std::string::npos,
           "host preview should include the configured total bot count and balanced team split");
    expect(previewLine.find("42000") == std::string::npos,
           "host preview should not imply a hidden derived proxy port");
}

void testHostSetupCarriesSelectedCharacterProfile() {
    app::CharacterPresetStore store;
    std::vector<std::string> profileIds;
    for (int index = 0; index < 7; ++index) {
        character::CharacterProfile profile;
        profile.id = "wide-shoulders-" + std::to_string(index);
        profile.name = index == 0 ? "Wide Shoulders" : "Wide Shoulders " + std::to_string(index);
        profile.appearance = character::CharacterAppearance{
            1.70f + static_cast<float>(index) * 0.10f,
            0.20f,
            18.0f
        };
        profile.builtIn = false;
        expect(store.save(profile), "host character-selection test profile should save");
        profileIds.push_back(profile.id);
    }

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setServerPortText("41000");
    expect(menu.characterProfileCountForTest() >= 8u,
           "host setup should load saved character profiles alongside the default");
    menu.openCharacterDropdownForTest();
    const Rectangle characterField = menu.fieldRectForTest("Character Preset");
    const Rectangle dropdown = menu.characterDropdownRectForTest();
    expect(menu.characterDropdownOpenForTest() &&
               dropdown.y > characterField.y + characterField.height &&
               dropdown.width == characterField.width,
           "host character selection should open as a dropdown below the field");
    expect(menu.visibleCharacterDropdownRowCountForTest() <= 5u &&
               dropdown.height <= 5.0f * 56.0f,
           "host character dropdown should stay compact instead of expanding the setup layout");

    expect(menu.selectCharacterProfileForTest("wide-shoulders-0"),
           "host setup should allow selecting a saved character profile");
    expect(menu.characterProfileTextForTest() == "Wide Shoulders",
           "host setup should display the selected character profile name");

    const net::SessionLaunchConfig config = menu.buildLaunchConfig();
    expect(config.characterProfileName == "Wide Shoulders" &&
               std::fabs(config.characterAppearance.shoulderWidth - 1.70f) < 0.0001f &&
               std::fabs(config.characterAppearance.shoulderHeight - 0.20f) < 0.0001f &&
               std::fabs(config.characterAppearance.shoulderAngleDeg - 18.0f) < 0.0001f,
           "host launch config should carry the selected authoritative character appearance");
    expect(menu.previewLine().find("Character Wide Shoulders") != std::string::npos,
           "host preview should include the selected character profile");
    for (const std::string& profileId : profileIds) {
        expect(store.remove(profileId), "host character-selection test profile should clean up");
    }
}

void testHostSetupCanLaunchAsSpectator() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-spectator");
    menu.setServerPortText("41001");

    const MultiplayerSessionMenu::Result firstStep = menu.submitForTest();
    expect(!firstStep.submitted &&
               menu.step() == MultiplayerSessionMenu::Step::TeamChoice,
           "host spectator launch should still advance into explicit team choice");

    const MultiplayerSessionMenu::Result selection =
        menu.chooseTeamForTest(sim::TeamId::Spectator);
    expect(!selection.submitted,
           "clicking spectator in the team-choice step should only change the selection");

    const MultiplayerSessionMenu::Result submitted = menu.submitForTest();
    expect(submitted.submitted,
           "host spectator launch should occur only after confirming the primary Start Game action");
    expect(submitted.launchConfig.preferredTeam == sim::TeamId::Spectator,
           "host spectator launch should preserve the explicit spectator choice");
}

void testHostSetupStartsWithAdvancedCollapsedAndCanExpand() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setServerPortText("41000");

    expect(!menu.hostAdvancedExpanded(),
           "host setup should start with advanced settings collapsed");
    expect(std::string(menu.submitButtonLabelForTest()) == "Start Game",
           "host setup should expose a start-game primary action before team choice");
    expect(std::string(menu.stepSubtitleForTest()).empty(),
           "host setup should avoid explanatory subtitle copy above the streamlined setup panel");
    expect(menu.fieldRectForTest("Bots").height > 0.0f,
           "host setup should expose total bot count as an essential editable field");

    std::string error;
    expect(menu.validate(&error),
           "host essentials should validate without expanding advanced settings");

    menu.setHostAdvancedExpandedForTest(true);
    menu.setSessionLabel("Advanced Host");
    menu.setTotalBotCountText("4");
    expect(menu.hostAdvancedExpanded(),
           "host setup should allow explicit expansion of advanced settings");
    expect(menu.validate(&error),
           "expanding host advanced settings should preserve valid launch semantics");
}

void testHostSetupCentersPrimaryStartAction() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);

    const Rectangle startButton = menu.launchButtonRectForTest();
    const float startCenterX = startButton.x + startButton.width * 0.5f;

    expect(startButton.width > 0.0f && startButton.height > 0.0f,
           "host setup should expose a visible primary Start Game button");
    expect(std::fabs(startCenterX - 960.0f) < 0.01f,
           "host setup should center the Start Game button in the 1920-wide virtual canvas");
}

void testTeamChoiceCentersLaunchActionAndUsesColoredTeamAvatars() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setServerPortText("41000");

    const MultiplayerSessionMenu::Result firstStep = menu.submitForTest();
    expect(!firstStep.submitted &&
               menu.step() == MultiplayerSessionMenu::Step::TeamChoice,
           "team-choice layout regression test requires the host flow to advance into explicit team choice");

    const Rectangle startButton = menu.launchButtonRectForTest();
    expect(std::fabs((startButton.x + startButton.width * 0.5f) - 960.0f) < 0.01f,
           "team-choice start action should remain centered after removing the duplicate lower back button");
    expect(std::string(menu.stepSubtitleForTest()).empty(),
           "team-choice screen should no longer show a redundant explanatory subtitle above the cards");

    const Rectangle attackersCard = menu.teamChoiceRectForTest(sim::TeamId::Attacker);
    const Rectangle defendersCard = menu.teamChoiceRectForTest(sim::TeamId::Defender);
    const Rectangle spectatorCard = menu.teamChoiceRectForTest(sim::TeamId::Spectator);
    expect(attackersCard.width > 0.0f && defendersCard.width > 0.0f && spectatorCard.width > 0.0f,
           "team-choice screen should expose visible cards for attackers, defenders, and spectator");
    expect(std::fabs(attackersCard.y - defendersCard.y) < 0.01f &&
               std::fabs(defendersCard.y - spectatorCard.y) < 0.01f,
           "team-choice cards should stay aligned on one shared horizontal row");

    const Color attackerAccent = menu.teamChoiceAccentColorForTest(sim::TeamId::Attacker);
    const Color defenderAccent = menu.teamChoiceAccentColorForTest(sim::TeamId::Defender);
    expect(attackerAccent.r > attackerAccent.g && attackerAccent.r > attackerAccent.b,
           "attackers team card should use a red-accented character treatment");
    expect(defenderAccent.b > defenderAccent.r && defenderAccent.b > defenderAccent.g,
           "defenders team card should use a blue-accented character treatment");
}

void testTeamChoiceSourceDropsHelperCopyAndDuplicateLowerBackButton() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string menuSource = readTextFile(repoRoot / "include/MultiplayerSessionMenu.hpp");

    expect(menuSource.find("Pick attackers, defenders, or spectator before launch. Clicking a choice starts immediately.") == std::string::npos,
           "team-choice screen should no longer render a top explanatory subtitle");
    expect(menuSource.find("Attackers and defenders switch immediately and respawn you.") == std::string::npos &&
               menuSource.find("Spectator launches directly into observation mode and can be toggled in-session.") == std::string::npos,
           "team-choice screen should no longer render the lower explanatory helper copy");
    expect(menuSource.find("Click a team to start | LEFT/RIGHT changes team choice | ENTER confirms | Q/ESC returns to setup") == std::string::npos,
           "team-choice screen should no longer render the bottom instruction footer");
    expect(menuSource.find("backButton_") == std::string::npos,
           "team-choice screen should use only the upper-left back action instead of a duplicate lower back button");
}

void testHostSetupSourceDropsHelperCopyAndPreview() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string menuSource = readTextFile(repoRoot / "include/MultiplayerSessionMenu.hpp");

    expect(menuSource.find("Start from the essentials first.") == std::string::npos,
           "host setup should no longer render a long explanatory subtitle above the panel");
    expect(menuSource.find("Selected Level Slot:") == std::string::npos &&
               menuSource.find("Host routing:") == std::string::npos &&
               menuSource.find("Cadence:") == std::string::npos &&
               menuSource.find("Human slots:") == std::string::npos,
           "host setup should avoid stacked detail copy above the form");
    expect(menuSource.find("Advanced host options stay available") == std::string::npos &&
               menuSource.find("Next: ") == std::string::npos &&
               menuSource.find("Top-left Back returns") == std::string::npos,
           "host setup should not render helper, preview, or shortcut footer copy");
    expect(menuSource.find("\"Players\"") == std::string::npos,
           "host setup should no longer render the old Players summary category");
    expect(menuSource.find("Show Advanced") == std::string::npos &&
               menuSource.find("Hide Advanced") == std::string::npos,
           "host setup advanced toggle should use shorter labels");
    expect(menuSource.find("drawHostMetricCard(") == std::string::npos &&
               menuSource.find("kHostSummary") == std::string::npos,
           "host setup should avoid duplicate summary cards and keep the editable form as the source of truth");
    expect(menuSource.find("Level Slot ") != std::string::npos,
           "host setup should keep selected level context as a single non-editable header");
}

void testTeamChoiceRequiresExplicitLaunchConfirmation() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setServerPortText("41000");

    const MultiplayerSessionMenu::Result firstStep = menu.submitForTest();
    expect(!firstStep.submitted &&
               menu.step() == MultiplayerSessionMenu::Step::TeamChoice,
           "host start action should advance into explicit team choice before launch");

    const MultiplayerSessionMenu::Result selection = menu.chooseTeamForTest(sim::TeamId::Defender);
    expect(!selection.submitted,
           "clicking a team in the team-choice step should only change the selection, not launch immediately");
    expect(menu.preferredTeam() == sim::TeamId::Defender,
           "team-choice clicks should update the selected team before launch confirmation");

    const MultiplayerSessionMenu::Result submitted = menu.submitForTest();
    expect(submitted.submitted,
           "team-choice launch should occur only when the primary Start Game action is confirmed");
    expect(submitted.launchConfig.preferredTeam == sim::TeamId::Defender,
           "launch confirmation should preserve the explicitly selected team");
}

void testHostAdvancedLayoutSeparatesSummaryFieldsAndPreview() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setHostAdvancedExpandedForTest(true);

    const Rectangle portRect = menu.fieldRectForTest("Port");
    const Rectangle playerNameRect = menu.fieldRectForTest("Player Name");
    const Rectangle botsRect = menu.fieldRectForTest("Bots");
    const float previewY = menu.setupPreviewYForTest();

    expect(portRect.height > 0.0f && playerNameRect.height > 0.0f && botsRect.height > 0.0f,
           "expanded host setup should expose the advanced field layout for regression checks");
    expect(portRect.y >= 430.0f,
           "host setup fields should start below the header summary and advanced-toggle band");
    expect(playerNameRect.x > portRect.x && playerNameRect.y == portRect.y,
           "expanded host setup should use a multi-column grid so advanced fields do not stack into one crowded column");
    expect(botsRect.y > portRect.y,
           "expanded host setup should stack advanced fields in increasing vertical order");
    expect(previewY >= botsRect.y + botsRect.height + 20.0f,
           "host setup preview text should render below the final advanced field rather than through it");
}

void testHostSetupFallsBackToPlayerNameWhenSessionLabelIsEmpty() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(2);
    menu.setPlayerName("fallback-host");
    menu.setSessionLabel("   ");
    menu.setShotEvaluationMode(net::ShotEvaluationMode::SeenPosition);
    menu.setServerPortText("41000");

    std::string error;
    expect(menu.validate(&error), "empty session labels should not block host setup");

    const net::SessionLaunchConfig config = menu.buildLaunchConfig();
    expect(config.sessionLabel.empty(),
           "host launch config should preserve an empty stored session label when the host leaves it blank");

    const std::string previewLine = menu.previewLine();
    expect(previewLine.find("Session fallback-host") != std::string::npos,
           "host preview should fall back to the host player name when the session label is blank");
    expect(previewLine.find("Rule Seen Position") != std::string::npos,
           "host preview fallback should still show the selected shot-evaluation rule");
    expect(previewLine.find(net::shotEvaluationModeSummary(net::ShotEvaluationMode::SeenPosition)) !=
               std::string::npos,
           "host preview fallback should preserve the canonical shot-rule explanation text");
}

void testHostSetupRejectsInvalidBotCounts() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setServerPortText("41000");
    menu.setTotalBotCountText("17");

    std::string error;
    expect(!menu.validate(&error),
           "host setup should reject out-of-range bot counts");
    expect(error.find("bot count") != std::string::npos,
           "host setup should surface an explicit bot-count validation error");
}

void testHostSetupRejectsHumanCapBelowLocalParticipants() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host);
    menu.setSelectedLevelSlot(4);
    menu.setPlayerName("host-player");
    menu.setLocalParticipantCountForTest(2u);
    menu.setServerPortText("41000");
    menu.setMaxHumanPlayersText("1");

    std::string error;
    expect(!menu.validate(&error),
           "host setup should reject human-player caps below the local participant count");
    expect(error.find("local participant count") != std::string::npos,
           "host setup should surface an explicit validation error for undersized human-player caps");
}

void testStudySetupBuildsLaunchConfigWithLocalParticipants() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Host,
                                net::SessionProductSurface::LabStudy);
    menu.setSelectedLevelSlot(5);
    menu.setPlayerName("study-host");
    menu.setLocalParticipantCountForTest(2u);
    menu.setShotEvaluationMode(net::ShotEvaluationMode::LivePosition);
    menu.setServerPortText("41010");

    std::string error;
    expect(menu.validate(&error),
           "lab-study setup should validate through the shared multiplayer menu form");

    const net::SessionLaunchConfig config = menu.buildLaunchConfig();
    expect(config.surface == net::SessionProductSurface::LabStudy &&
               config.entryPoint == net::SessionEntryPoint::LabStudy,
           "lab-study setup should map into the dedicated shared launch surface and entry point");
    expect(config.levelSlot == 5 &&
               config.levelHash == net::makeLevelIdentityHash(5) &&
               config.localParticipantCount == 2u,
           "lab-study setup should carry the selected level slot and local participant count through the shared launch contract");
    expect(config.shotEvaluationMode == net::ShotEvaluationMode::LivePosition &&
               config.studyOptions.enablePredictionToggle &&
               config.studyOptions.enableShotStrategyToggle &&
               config.studyOptions.enableReplayCapture &&
               !config.studyOptions.enableEventLogging,
           "lab-study setup should preserve authoritative shot strategy and keep event logging opt-in in the shared launch contract");
}

void testJoinSetupBlocksInvalidAddressAndPort() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    menu.setPlayerName("joiner");
    menu.setHostAddress("bad host");
    menu.setServerPortText("41000");

    auto invalidHostResult = menu.submitForTest();
    expect(!invalidHostResult.submitted, "join setup should block launch when the address is invalid");
    expect(menu.hasValidationError(), "join setup should surface a validation error for invalid addresses");

    menu.setHostAddress("192.168.0.24");
    menu.setServerPortText("70000");
    auto invalidPortResult = menu.submitForTest();
    expect(!invalidPortResult.submitted, "join setup should block launch when the port is invalid");
    expect(menu.hasValidationError(), "join setup should surface a validation error for invalid ports");
}

void testJoinSetupAcceptsCombinedHostAndPortInput() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    menu.setPlayerName("joiner");
    menu.setPreferredTeam(sim::TeamId::Attacker);
    menu.setHostAddress("192.168.0.24:42000");
    menu.setServerPortText("41000");

    const net::SessionLaunchConfig config =
        submitAfterTeamChoice(&menu, "direct-connect join");
    expect(menu.hostAddress() == "192.168.0.24",
           "join setup should normalize the host field after parsing a pasted host:port");
    expect(menu.serverPortText() == "42000",
           "join setup should normalize the port field after parsing a pasted host:port");
    expect(config.clientConnectHost == "192.168.0.24" &&
               config.clientConnectPort == 42000u,
           "join setup should launch against the parsed pasted endpoint rather than the stale port field");
    expect(config.preferredTeam == sim::TeamId::Attacker,
           "join setup should preserve the selected team");
}

void testJoinSetupPreservesSpectatorChoice() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    menu.setPlayerName("spectator-joiner");
    menu.setHostAddress("192.168.0.24");
    menu.setServerPortText("42000");

    const MultiplayerSessionMenu::Result firstStep = menu.submitForTest();
    expect(!firstStep.submitted &&
               menu.step() == MultiplayerSessionMenu::Step::TeamChoice,
           "join spectator launch should still advance into explicit team choice");

    const MultiplayerSessionMenu::Result selection =
        menu.chooseTeamForTest(sim::TeamId::Spectator);
    expect(!selection.submitted,
           "clicking spectator in the join team-choice step should only change the selection");
    const MultiplayerSessionMenu::Result submitted = menu.submitForTest();
    expect(submitted.submitted,
           "join spectator launch should occur only after confirming the primary Join Game action");
    expect(submitted.launchConfig.preferredTeam == sim::TeamId::Spectator,
           "join spectator launch should preserve the explicit spectator choice");
}

void testJoinMenuDefaultsToBrowserSubviewAndCanSwitchToDirectConnect() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);

    expect(menu.joinSubview() == MultiplayerSessionMenu::JoinSubview::Browser,
           "join setup should open on the browser-first subview by default");
    expect(std::string(menu.submitButtonLabelForTest()) == "Join Game",
           "join setup should expose a join-game primary action before team choice");

    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    expect(menu.joinSubview() == MultiplayerSessionMenu::JoinSubview::DirectConnect,
           "join setup should allow an explicit switch to direct connect");
}

void testJoinBrowserLayoutKeepsPlayerFieldBelowDiscoveryRows() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);

    const Rectangle playerNameRect = menu.fieldRectForTest("Player Name");
    const Rectangle lastDiscoveryRow = menu.discoveryRowRectForTest(3u);
    const float previewY = menu.setupPreviewYForTest();

    expect(playerNameRect.height > 0.0f,
           "join browser should still expose the player-name field for layout regression checks");
    expect(playerNameRect.y >= lastDiscoveryRow.y + lastDiscoveryRow.height + 40.0f,
           "join browser should place the player-name field below the discovery list instead of on top of it");
    expect(previewY >= playerNameRect.y + playerNameRect.height + 20.0f,
           "join browser preview text should render below the player-name field");
}

void testJoinMenuAutoScanPopulatesCompatibleSessions() {
    ensureTestLevelExists(9);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(9, "browser-host", 45200u, 0u, 0u);
    config.sessionLabel = "Browser Match";
    config.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    config.discoveryPort = kMenuDiscoveryPortBase;
    config.clientSessionId = 0x29000001u;
    config.clientConnectTimeoutUs = 350'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before join-menu auto-scan checks");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setDiscoveryPortForTest(config.discoveryPort);
    const bool discovered = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu]() {
            return menu.discoveryScanCount() == 1u &&
                   menu.visibleDiscoveryEntries().size() == 1u;
        },
        std::chrono::milliseconds(750));

    expect(discovered,
           "entering the join menu should trigger a single initial LAN scan that discovers the hosted session");

    const auto entries = menu.visibleDiscoveryEntries();
    const net::HostedSessionMetadata hostedMetadata = controller.hostedSessionMetadata();
    expect(entries.size() == 1u, "auto-scan should populate one compatible discovery row for the hosted session");
    expect(entries.front().compatibility == net::BrowserCompatibilityState::Compatible,
           "join-menu LAN browser should surface only compatible discovery rows");
    expect(entries.front().advertisement.sessionLabel == "Browser Match" &&
               entries.front().advertisement.joinPort == 45200u,
           "discovered browser rows should preserve the authoritative session metadata");
    expect(net::detailLineForSessionAdvertisement(entries.front().advertisement).find(
               net::shotEvaluationModeSummary(hostedMetadata.shotEvaluationMode)) != std::string::npos,
           "join-menu LAN browser should expose the same shot-rule explanation carried by hosted session metadata");
}

void testJoinMenuRefreshUpdatesWithoutDuplicatingRows() {
    ensureTestLevelExists(7);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(7, "refresh-host", 45210u, 0u, 0u);
    config.sessionLabel = "Refresh Match";
    config.discoveryPort = kMenuDiscoveryPortBase + 1u;
    config.clientSessionId = 0x2A000001u;
    config.clientConnectTimeoutUs = 350'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before join-menu refresh checks");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setDiscoveryPortForTest(config.discoveryPort);
    const bool initialDiscovery = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu]() {
            return menu.discoveryScanCount() == 1u &&
                   menu.visibleDiscoveryEntries().size() == 1u;
        },
        std::chrono::milliseconds(750));
    expect(initialDiscovery, "join-menu refresh test requires an initial discovered browser row");

    const std::uint64_t initialLastSeenUs = menu.visibleDiscoveryEntries().front().lastSeenUs;
    menu.refreshDiscoveryForTest();

    const bool refreshed = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu, initialLastSeenUs]() {
            const auto entries = menu.visibleDiscoveryEntries();
            return menu.discoveryScanCount() == 2u &&
                   entries.size() == 1u &&
                   entries.front().lastSeenUs > initialLastSeenUs;
        },
        std::chrono::milliseconds(750));

    expect(refreshed,
           "manual LAN browser refresh should requery the host and update the existing row without duplicates");
}

void testJoinMenuPeriodicRefreshKeepsDiscoveredRowsStable() {
    ensureTestLevelExists(5);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(5, "stable-host", 45215u, 0u, 0u);
    config.sessionLabel = "Stable Match";
    config.discoveryPort = kMenuDiscoveryPortBase + 2u;
    config.clientSessionId = 0x2A000101u;
    config.clientConnectTimeoutUs = 350'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before stable browser checks");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setDiscoveryPortForTest(config.discoveryPort);
    const bool stayedVisible = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu]() {
            const auto entries = menu.visibleDiscoveryEntries();
            return menu.discoveryScanCount() >= 4u &&
                   entries.size() == 1u &&
                   entries.front().lastSeenUs >= 2'500'000u;
        },
        std::chrono::milliseconds(4200));

    expect(stayedVisible,
           "active join-browser mode should keep re-querying before stale expiry so discovered rows do not disappear");
}

void testJoinMenuSelectionResolvesLaunchEndpoint() {
    ensureTestLevelExists(8);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(8, "selection-host", 45220u, 0u, 0u);
    config.sessionLabel = "Selection Match";
    config.discoveryPort = kMenuDiscoveryPortBase + 3u;
    config.clientSessionId = 0x2B000001u;
    config.clientConnectTimeoutUs = 350'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(), "hosted session should start before join-menu selection checks");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setDiscoveryPortForTest(config.discoveryPort);
    menu.setPlayerName("browser-joiner");
    const bool discovered = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu]() {
            return menu.visibleDiscoveryEntries().size() == 1u;
        },
        std::chrono::milliseconds(750));
    expect(discovered, "join-menu selection test requires a discovered LAN browser row");

    expect(menu.selectDiscoveryEntryForTest(0u),
           "selecting the discovered LAN browser row should populate the join endpoint fields");
    expect(menu.hostAddress() == "127.0.0.1" &&
               menu.serverPortText() == "45220",
           "selecting a discovery row should resolve the advertised host and public join port");

    const net::SessionLaunchConfig configOut =
        submitAfterTeamChoice(&menu, "browser-selected join");
    expect(configOut.clientConnectHost == "127.0.0.1" &&
               configOut.clientConnectPort == 45220u,
           "browser-selected joins should launch against the discovered host endpoint");
}

void testJoinMenuSelectionSurvivesAdvertisementRefreshes() {
    ensureTestLevelExists(6);

    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(6, "persistent-host", 45221u, 0u, 0u);
    config.sessionLabel = "Persistent Match";
    config.discoveryPort = kMenuDiscoveryPortBase + 4u;
    config.clientSessionId = 0x2B000101u;
    config.clientConnectTimeoutUs = 350'000u;

    net::SessionFlowController controller(config);
    expect(controller.start(),
           "hosted session should start before selection-preservation checks");

    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);
    menu.setDiscoveryPortForTest(config.discoveryPort);
    menu.setPlayerName("browser-joiner");
    const bool discovered = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu]() {
            return menu.visibleDiscoveryEntries().size() == 1u;
        },
        std::chrono::milliseconds(750));
    expect(discovered,
           "selection-preservation test requires an initial discovered LAN browser row");
    expect(menu.selectDiscoveryEntryForTest(0u),
           "selection-preservation test requires the discovered browser row to be selectable");

    const bool refreshed = waitForDiscoveryBrowserPredicate(
        &controller,
        &menu,
        [&menu]() {
            return menu.discoveryScanCount() >= 2u &&
                   menu.visibleDiscoveryEntries().size() == 1u;
        },
        std::chrono::milliseconds(2200));
    expect(refreshed,
           "selection-preservation test requires at least one browser refresh after the row is selected");

    const net::SessionLaunchConfig configOut =
        submitAfterTeamChoice(&menu, "browser-selected join after refresh");
    expect(configOut.clientConnectHost == "127.0.0.1" &&
               configOut.clientConnectPort == 45221u,
           "browser-selected joins should keep the selected discovery row valid across advertisement refreshes");
}

void testJoinMenuHidesIncompatibleRowsAndKeepsManualJoinAvailable() {
    MultiplayerSessionMenu menu(net::SessionLaunchMode::Join);

    net::SessionAdvertisement incompatible;
    incompatible.sessionLabel = "Old Build";
    incompatible.hostPlayerName = "legacy-host";
    incompatible.levelSlot = 4;
    incompatible.levelHash = net::makeLevelIdentityHash(4);
    incompatible.joinHost = "127.0.0.1";
    incompatible.joinPort = 45230u;
    incompatible.humanPlayers = 1u;
    incompatible.maxHumanPlayers = 2u;
    incompatible.protocolVersion = net::kProtocolVersion - 1u;
    incompatible.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    menu.injectDiscoveryAdvertisementForTest(incompatible, 16'667u);

    expect(menu.visibleDiscoveryEntries().empty(),
           "incompatible discovery advertisements should be filtered out of the selectable LAN browser rows");
    expect(menu.unavailableDiscoveryEntries().size() == 1u,
           "incompatible discovery advertisements should still be tracked as unavailable browser rows");
    expect(menu.discoveryBrowserSummaryLine().find("hidden") != std::string::npos,
           "join-menu browser summary should mention hidden incompatible or unavailable sessions");

    menu.setJoinSubviewForTest(MultiplayerSessionMenu::JoinSubview::DirectConnect);
    menu.setPlayerName("manual-joiner");
    menu.setHostAddress("127.0.0.1");
    menu.setServerPortText("45231");
    const net::SessionLaunchConfig manualRetry =
        submitAfterTeamChoice(&menu, "manual retry join");
    expect(manualRetry.clientConnectHost == "127.0.0.1" &&
               manualRetry.clientConnectPort == 45231u,
           "manual join should remain available even when incompatible discovery rows are hidden");
}

void testLevelSelectMapsSurfaceSelectionIntoLaunchContract() {
    const MainMenu::NavigationSelection studySelection{
        MainMenu::AppShellSurface::LabStudy,
        net::SessionEntryPoint::LabStudy};
    const net::SessionLaunchConfig studyConfig =
        LevelSelectMenu::buildLaunchConfigForSelection(
            studySelection,
            6,
            "study-player",
            2u,
            net::ShotEvaluationMode::LivePosition);
    expect(studyConfig.surface == net::SessionProductSurface::LabStudy &&
               studyConfig.entryPoint == net::SessionEntryPoint::LabStudy,
           "level selection should compose lab-study surface choices into the shared launch contract");
    expect(studyConfig.levelSlot == 6 &&
               studyConfig.levelHash == net::makeLevelIdentityHash(6) &&
               studyConfig.localParticipantCount == 2u &&
               studyConfig.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "level selection should preserve slot identity or local participants or shot strategy when composing a study launch");

    const MainMenu::NavigationSelection editorSelection{
        MainMenu::AppShellSurface::LevelEditor,
        net::SessionEntryPoint::LevelEditor};
    const net::SessionLaunchConfig editorConfig =
        LevelSelectMenu::buildLaunchConfigForSelection(editorSelection, 7);
    expect(editorConfig.surface == net::SessionProductSurface::LevelEditor &&
               editorConfig.entryPoint == net::SessionEntryPoint::LevelEditor &&
               editorConfig.levelSlot == 7,
           "level selection should also compose editor slot choices through the shared launch contract");
}

void testDisplayManagerDefaultsToCompactPreset() {
    expect(display::currentResolutionPreset() == display::ResolutionPreset::HD540,
           "display manager should default to the compact 960x540 preset");
    expect(display::currentWindowWidth() == 960 && display::currentWindowHeight() == 540,
           "default display preset should expose the compact 16:9 window size");
    expect(display::currentWindowLabel() == "960x540",
           "default display state should report the current native window size through the shared display adapter");
}

void testDisplayManagerKeepsLogicalAndFramebufferMetricsSeparate() {
    const display::DisplayMetrics retinaMetrics =
        display::displayMetricsForTest(960, 540, 1920, 1080);
    expect(retinaMetrics.logicalWindowWidth == 960 &&
               retinaMetrics.logicalWindowHeight == 540,
           "display metrics should keep the raylib logical window size separate");
    expect(retinaMetrics.framebufferWidth == 1920 &&
               retinaMetrics.framebufferHeight == 1080,
           "display metrics should keep the physical framebuffer size separate");
    expect(std::fabs(retinaMetrics.framebufferScaleX - 2.0f) < 0.001f &&
               std::fabs(retinaMetrics.framebufferScaleY - 2.0f) < 0.001f,
           "display metrics should expose the framebuffer scale used by HighDPI displays");
    expect(std::fabs(retinaMetrics.logicalDestination.width - 960.0f) < 0.001f &&
               std::fabs(retinaMetrics.logicalDestination.height - 540.0f) < 0.001f,
           "final presentation should still target the logical window rectangle");
    expect(std::fabs(retinaMetrics.framebufferDestination.width - 1920.0f) < 0.001f &&
               std::fabs(retinaMetrics.framebufferDestination.height - 1080.0f) < 0.001f,
           "direct fallback rendering should target the physical framebuffer rectangle");

    const display::DisplayMetrics letterboxedMetrics =
        display::displayMetricsForTest(1280, 800, 2560, 1600);
    expect(std::fabs(letterboxedMetrics.logicalDestination.width - 1280.0f) < 0.001f &&
               std::fabs(letterboxedMetrics.logicalDestination.height - 720.0f) < 0.001f &&
               std::fabs(letterboxedMetrics.logicalDestination.y - 40.0f) < 0.001f,
           "logical presentation should letterbox against the logical window size");
    expect(std::fabs(letterboxedMetrics.framebufferDestination.width - 2560.0f) < 0.001f &&
               std::fabs(letterboxedMetrics.framebufferDestination.height - 1440.0f) < 0.001f &&
               std::fabs(letterboxedMetrics.framebufferDestination.y - 80.0f) < 0.001f,
           "framebuffer fallback presentation should letterbox against the physical framebuffer size");
}

void testSettingsMenuSwitchesResolutionPreset() {
    SettingsMenu menu;
    expect(menu.selectedPreset() == display::ResolutionPreset::HD540,
           "settings menu should seed its selection from the active resolution preset");

    menu.selectNextPresetForTest();
    expect(menu.selectedPreset() == display::ResolutionPreset::HD1080,
           "settings menu should still allow switching to the HD preset");

    menu.applySelectedPresetForTest();
    expect(display::currentResolutionPreset() == display::ResolutionPreset::HD1080,
           "settings menu should still apply the HD preset when explicitly selected");
    expect(display::currentWindowWidth() == 1920 && display::currentWindowHeight() == 1080,
           "the HD preset should still expose the 1920x1080 window size");
}

void testSettingsMenuDebouncesEntryClickUntilMouseRelease() {
    SettingsMenu menu;
    expect(!menu.clickReadyForTest(),
           "settings menu should start with mouse clicks disarmed so the entry click cannot click through");

    menu.markClickReadyForTest();
    expect(menu.clickReadyForTest(),
           "settings menu should arm mouse clicks only after an explicit release/update path");

    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string source = readTextFile(repoRoot / "include/SettingsMenu.hpp");
    expect(source.find("clickReady_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)") !=
               std::string::npos,
           "settings mouse actions should be gated behind the release-before-click guard");
    expect(countOccurrences(source, "IsMouseButtonPressed(MOUSE_BUTTON_LEFT)") == 1u,
           "settings should not bypass the debounced leftClickPressed value with direct click checks");
}

void testSettingsDefaultsRemainLocalUntilAppliedToLaunchConfig() {
    SettingsMenu menu;
    menu.setSessionLocalParticipantCountForTest(2u);
    menu.setSessionShotEvaluationModeForTest(net::ShotEvaluationMode::LivePosition);
    const SettingsMenu::SessionLaunchDefaults defaults = menu.sessionLaunchDefaults();

    menu.selectNextPresetForTest();
    menu.applySelectedPresetForTest();
    expect(menu.sessionLaunchDefaults().localParticipantCount == defaults.localParticipantCount &&
               menu.sessionLaunchDefaults().preferredShotEvaluationMode ==
                   defaults.preferredShotEvaluationMode,
           "display resolution changes should remain local UI state until settings are explicitly mapped into a launch config");

    MultiplayerSessionMenu hostMenu(net::SessionLaunchMode::Host);
    hostMenu.setSelectedLevelSlot(4);
    hostMenu.setPlayerName("settings-host");
    hostMenu.setServerPortText("41020");

    std::string error;
    expect(hostMenu.validate(&error),
           "host setup should still validate before settings defaults are applied");

    net::SessionLaunchConfig config = hostMenu.buildLaunchConfig();
    expect(config.localParticipantCount == net::kDefaultLocalParticipantCount &&
               config.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "host launch config should keep menu-local defaults until settings explicitly feed the shared launch contract");

    SettingsMenu::applySessionLaunchDefaults(menu.sessionLaunchDefaults(), &config);
    expect(config.localParticipantCount == 2u &&
               config.shotEvaluationMode == net::ShotEvaluationMode::LivePosition,
           "settings defaults should update only the explicit shared launch fields when applied");
}

void testMenuSurfacesRouteTypographyThroughSharedService() {
    const std::filesystem::path repoRoot = findRepoRoot();

    for (const char* relativePath : {
             "include/MainMenu.hpp",
             "include/LevelSelectMenu.hpp",
             "include/MultiplayerSessionMenu.hpp",
             "include/CharacterEditorScreen.hpp",
             "include/SettingsMenu.hpp",
         }) {
        const std::string source = readTextFile(repoRoot / relativePath);
        expect(source.find("TypographyService") != std::string::npos,
               std::string(relativePath) +
                   " should include the shared typography service instead of owning raw font rendering");
        expect(countOccurrences(source, "DrawText(") == 0u,
               std::string(relativePath) +
                   " should no longer draw app-shell text through raw DrawText calls");
        expect(countOccurrences(source, "MeasureText(") == 0u,
               std::string(relativePath) +
                   " should no longer measure app-shell text through raw MeasureText calls");
    }

    const std::string mainSource = readTextFile(repoRoot / "src/main_3d.cpp");
    expect(countOccurrences(mainSource, "TypographyService::shared().initialize();") >= 2u,
           "app-shell startup should initialize the shared typography registry before menu surfaces render");
}

}  // namespace

int main() {
    try {
        const testsupport::ScopedTestDataRoot scopedDataRoot("netcodesim-multiplayer-menu");
        (void)scopedDataRoot;

        testDisplayManagerDefaultsToCompactPreset();
        testDisplayManagerKeepsLogicalAndFramebufferMetricsSeparate();
        testMainMenuExposesHostAndJoinFlow();
        testMainMenuBackShortcutDoesNotCascadeIntoQuit();
        testMainMenuCanSuppressQuitShortcutAfterShellReturn();
        testMainMenuSurfacesExternalLinkIntentsWithoutChangingNavigationState();
        testMainMenuRendersPlatformGlyphsForExternalLinks();
        testMainMenuUsesAnimatedGameplayBackdrop();
        testMainMenuUsesSquareSocialButtons();
        testMainMenuExternalLinksUsePublicUrls();
        testLevelSelectExposesVisibleBackAction();
        testJoinSetupBackShortcutIgnoresQWhileEditingTextFields();
        testBusySetupClearsFieldFocusSoBackCanWin();
        testBusyHostSetupBackActionCancelsPendingStateAndReturns();
        testHostSetupExposesVisibleUpperLeftBackAction();
        testHostSetupBackShortcutWinsEvenWithFocusedField();
        testHostSetupBuildsPublicJoinLaunchConfig();
        testHostSetupCarriesSelectedCharacterProfile();
        testHostSetupCanLaunchAsSpectator();
        testHostSetupStartsWithAdvancedCollapsedAndCanExpand();
        testHostSetupCentersPrimaryStartAction();
        testTeamChoiceCentersLaunchActionAndUsesColoredTeamAvatars();
        testTeamChoiceSourceDropsHelperCopyAndDuplicateLowerBackButton();
        testHostSetupSourceDropsHelperCopyAndPreview();
        testTeamChoiceRequiresExplicitLaunchConfirmation();
        testHostAdvancedLayoutSeparatesSummaryFieldsAndPreview();
        testHostSetupFallsBackToPlayerNameWhenSessionLabelIsEmpty();
        testHostSetupRejectsInvalidBotCounts();
        testHostSetupRejectsHumanCapBelowLocalParticipants();
        testStudySetupBuildsLaunchConfigWithLocalParticipants();
        testJoinSetupBlocksInvalidAddressAndPort();
        testJoinSetupAcceptsCombinedHostAndPortInput();
        testJoinSetupPreservesSpectatorChoice();
        testJoinMenuDefaultsToBrowserSubviewAndCanSwitchToDirectConnect();
        testJoinBrowserLayoutKeepsPlayerFieldBelowDiscoveryRows();
        testJoinMenuAutoScanPopulatesCompatibleSessions();
        testJoinMenuRefreshUpdatesWithoutDuplicatingRows();
        testJoinMenuPeriodicRefreshKeepsDiscoveredRowsStable();
        testJoinMenuSelectionResolvesLaunchEndpoint();
        testJoinMenuSelectionSurvivesAdvertisementRefreshes();
        testJoinMenuHidesIncompatibleRowsAndKeepsManualJoinAvailable();
        testLevelSelectMapsSurfaceSelectionIntoLaunchContract();
        testSettingsMenuSwitchesResolutionPreset();
        testSettingsMenuDebouncesEntryClickUntilMouseRelease();
        testSettingsDefaultsRemainLocalUntilAppliedToLaunchConfig();
        testMenuSurfacesRouteTypographyThroughSharedService();
        std::cout << "MultiplayerSessionMenuTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "MultiplayerSessionMenuTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
