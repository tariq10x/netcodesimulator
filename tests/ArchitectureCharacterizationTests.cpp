#include "LevelData.hpp"
#include "sim/SimulationDefaults.hpp"
#include "sim/SimulationTypes.hpp"
#include "sim/WorldState.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

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

    throw std::runtime_error("failed to locate repository root for architecture characterization tests");
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

bool containsExactSourceToken(const std::string& text, const std::string& token) {
    std::size_t cursor = 0u;
    while ((cursor = text.find(token, cursor)) != std::string::npos) {
        const std::size_t after = cursor + token.size();
        if (after >= text.size()) {
            return true;
        }
        const unsigned char next = static_cast<unsigned char>(text[after]);
        if (std::isalnum(next) == 0 && text[after] != '_') {
            return true;
        }
        cursor = after;
    }
    return false;
}

void expectSharedTypographySource(const std::string& source,
                                  const std::string& subject,
                                  bool forbidDrawFps = false,
                                  bool requireTypographyReference = true) {
    if (requireTypographyReference) {
        expect(source.find("TypographyService") != std::string::npos,
               subject + " should route through TypographyService after the typography migration");
    }
    expect(countOccurrences(source, "DrawText(") == 0u &&
               countOccurrences(source, "MeasureText(") == 0u &&
               countOccurrences(source, "DrawTextEx(") == 0u &&
               countOccurrences(source, "MeasureTextEx(") == 0u,
           subject + " should not bypass the shared typography service with raw backend text calls");
    if (forbidDrawFps) {
        expect(countOccurrences(source, "DrawFPS(") == 0u,
               subject + " should not bypass the shared typography service with DrawFPS");
    }
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
#ifdef _WIN32
        char* existing = nullptr;
        std::size_t existingLength = 0u;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
            std::free(existing);
        }
#else
        const char* existing = std::getenv(name_.c_str());
        if (existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
        }
#endif
        set(value);
    }

    ~ScopedEnvVar() {
        if (hadOriginal_) {
            set(originalValue_);
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    bool hadOriginal_{false};
    std::string originalValue_{};
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(original_);
    }

private:
    std::filesystem::path original_;
};

void testCheckpointContractsMoveIntoSharedStore() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string checkpointStoreSource = readTextFile(repoRoot / "src/CheckpointStore.cpp");

    expect(countOccurrences(checkpointStoreSource, "\"checkpoints.json\"") == 2u,
           "the shared checkpoint store should own the latency-mode checkpoint filename and legacy asset path");
    expect(countOccurrences(checkpointStoreSource, "\"checkpoints_replay.json\"") == 1u,
           "the shared checkpoint store should own the replay-studio checkpoint filename");
}

void testAppDataRootStaysInRepoNetcodeSimDirectory() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string userDataSource = readTextFile(repoRoot / "src/UserDataPaths.cpp");
    const std::string cmakeSource = readTextFile(repoRoot / "CMakeLists.txt");

    expect(userDataSource.find("NETCODESIM_SOURCE_DIR") != std::string::npos &&
               userDataSource.find("\".netcodesim\"") != std::string::npos &&
               userDataSource.find("NETCODESIM_DATA_ROOT") != std::string::npos,
           "the shared app-data root should default to the repo source directory under .netcodesim while preserving an explicit override");
    expect(userDataSource.find("copyLegacyDataDirectory") == std::string::npos &&
               userDataSource.find("copy_options::skip_existing") == std::string::npos,
           "the app-data root should avoid copying home data into the repository data directory");
    expect(cmakeSource.find("target_compile_definitions(NetcodeSimNet PUBLIC") != std::string::npos &&
               cmakeSource.find("NETCODESIM_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"") != std::string::npos,
           "the repo-root data path must be available to both server-side logging and client-side data stores");
}

void testStudyModeCompositionMovesOntoSharedRuntimeStack() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");
    const std::string sessionFlowSource = readTextFile(repoRoot / "src/SessionFlowController.cpp");

    expect(shellSource.find("labStudyTarget = true;") != std::string::npos &&
               shellSource.find("? GameMode::FREE_GAME") != std::string::npos &&
               shellSource.find("pushMenuMode(launchedMode);") != std::string::npos,
           "main app shell should keep routing level selection into the explicit FREE_GAME study path while now expressing that runtime transition through the shared menu stack");
    expect(shellSource.find("makeLabStudyLaunchConfig(levelSlot)") != std::string::npos,
           "main app shell should build study-mode sessions through an app-shell helper");
    expect(shellSource.find("case GameMode::FREE_GAME:") != std::string::npos &&
               shellSource.find("sessionFlow->render();") != std::string::npos,
           "main app shell should render study mode through the shared session-flow runtime");
    expect(sessionFlowSource.find("GameMode::FREE_GAME") == std::string::npos,
           "session flow controller should stay generic and free of study-mode shell policy");
}

void testLevelPersistenceMovesIntoTranslationUnitWithoutChangingSlotRules() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string headerSource = readTextFile(repoRoot / "include/LevelData.hpp");
    const std::string implementationSource = readTextFile(repoRoot / "src/LevelData.cpp");

    expect(headerSource.find("inline bool saveLevel(") == std::string::npos &&
               headerSource.find("inline bool loadLevel(") == std::string::npos,
           "LevelData.hpp should no longer embed save or load implementations after extraction");
    expect(headerSource.find("bool saveLevel(const LevelDefinition& level, int slot);") != std::string::npos &&
               headerSource.find("bool loadLevel(LevelDefinition& level, int slot);") != std::string::npos &&
               implementationSource.find("bool saveLevel(const LevelDefinition& level, int slot)") != std::string::npos &&
               implementationSource.find("bool loadLevel(LevelDefinition& level, int slot)") != std::string::npos,
           "LevelData slot wrappers should stay schema-first while LevelData.cpp owns their persistence policy");
    expect(headerSource.find("struct LevelDefinition {") != std::string::npos &&
               headerSource.find("using Level = LevelDefinition;") != std::string::npos,
           "LevelData.hpp should expose a portable LevelDefinition schema while keeping the legacy Level alias transitional");
    expect(implementationSource.find("bool saveLevelDefinition(") != std::string::npos &&
               implementationSource.find("bool loadLevelDefinition(") != std::string::npos,
           "LevelData.cpp should provide explicit path-based persistence helpers around the portable level schema");

    const std::size_t definitionStart = headerSource.find("struct LevelDefinition {");
    const std::size_t definitionEnd = headerSource.find("};", definitionStart);
    expect(definitionStart != std::string::npos && definitionEnd != std::string::npos,
           "LevelData.hpp should keep the portable LevelDefinition block visible");
    const std::string definitionBlock = headerSource.substr(definitionStart, definitionEnd - definitionStart);
    expect(definitionBlock.find("getLevelsDirectory") == std::string::npos &&
               definitionBlock.find("getLevelPath") == std::string::npos &&
               definitionBlock.find("saveLevel(") == std::string::npos &&
               definitionBlock.find("loadLevel(") == std::string::npos,
           "LevelDefinition should remain free of slot and filesystem policy declarations");

    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-leveldata-paths";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    const std::filesystem::path expectedDirectory =
        std::filesystem::weakly_canonical(tempRoot / "repo" / ".netcodesim" / "levels");
    expect(std::filesystem::weakly_canonical(LevelData::getLevelsDirectory()) == expectedDirectory,
           "LevelData should resolve the levels directory under the application run root");
    expect(std::filesystem::weakly_canonical(LevelData::getLevelPath(3)) ==
               expectedDirectory / "level_3.json",
           "LevelData should keep resolving slot paths as level_<slot>.json inside the levels directory");

    LevelData::LevelDefinition sample("Slot Validation");
    expect(!LevelData::saveLevel(sample, 0),
           "slot zero should remain invalid for saving");
    expect(!LevelData::saveLevel(sample, 10),
           "slot ten should remain invalid for saving");
    expect(!LevelData::levelExists(0) && !LevelData::levelExists(10),
           "invalid slots should still report as non-existent");
    expect(!LevelData::deleteLevel(0) && !LevelData::deleteLevel(10),
           "invalid slots should still refuse deletion");

    std::filesystem::current_path(tempRoot.parent_path());
    std::filesystem::remove_all(tempRoot);
}

void testLevelEditorConsumesSharedLevelDefinitionSchema() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string editorSource = readTextFile(repoRoot / "include/LevelEditor.hpp");

    expect(editorSource.find("LevelData::LevelDefinition currentLevel;") != std::string::npos,
           "level editor should keep the shared LevelDefinition schema as its active document model");
    expect(editorSource.find("LevelData::saveLevel(currentLevel, slot)") == std::string::npos &&
               editorSource.find("LevelData::loadLevel(currentLevel, slot)") == std::string::npos,
           "level editor should stop depending on the legacy slot-wrapper alias path directly");
    expect(editorSource.find("LevelData::saveLevelDefinition(currentLevel, LevelData::getLevelPath(slot))") != std::string::npos &&
               editorSource.find("LevelData::loadLevelDefinition(loadedLevel, LevelData::getLevelPath(slot))") != std::string::npos,
           "level editor should compose shared LevelDefinition persistence with explicit slot-path policy");
    expect(editorSource.find("GetScreenToWorldRayEx(display::mousePosition(),") != std::string::npos &&
               editorSource.find("Config::SCREEN_WIDTH") != std::string::npos &&
               editorSource.find("Config::SCREEN_HEIGHT") != std::string::npos,
           "level editor 3D picking should use virtual render-target dimensions so mouse-to-world mapping stays resolution independent");
    expect(editorSource.find("kEdgePanMarginPx") != std::string::npos &&
               editorSource.find("edgePanInput(mouseScreen)") != std::string::npos &&
               editorSource.find("IsCursorOnScreen()") != std::string::npos,
           "level editor should support mouse edge-panning against the virtual render bounds");
    expect(editorSource.find("kZoomStepFovy") != std::string::npos &&
               editorSource.find("camera.fovy = std::clamp(camera.fovy - wheelMove * kZoomStepFovy") != std::string::npos &&
               editorSource.find("Ctrl+wheel height") != std::string::npos,
           "level editor should use plain mouse wheel for camera zoom and keep height edits on a modifier");
    expect(editorSource.find("drawToolIcon(") != std::string::npos &&
               editorSource.find("drawSaveIcon(") != std::string::npos &&
               editorSource.find("drawUiText(\"LEVEL EDITOR\"") == std::string::npos &&
               editorSource.find("drawUiText(currentLevel.name") == std::string::npos &&
               editorSource.find("toolHint(") == std::string::npos &&
               editorSource.find("Click a swatch or press C") == std::string::npos &&
               editorSource.find("Active swatch updates the selected object") == std::string::npos &&
               editorSource.find("for (int row = 0; row < 3; ++row)") != std::string::npos &&
               editorSource.find("for (int column = 0; column < 3; ++column)") != std::string::npos &&
               editorSource.find("drawUiText(spec.label") == std::string::npos &&
               editorSource.find("drawUiText(\"SAVE\", saveRect") == std::string::npos,
           "level editor should drop the redundant top-left summary overlay, remove helper hint copy, and render compact icon-based tools including a top-down 3x3 area icon");
    expect(editorSource.find("enum class SlotSelectorAction") != std::string::npos &&
               editorSource.find("openSlotSelector(SlotSelectorAction::SAVE)") != std::string::npos &&
               editorSource.find("openSlotSelector(SlotSelectorAction::LOAD)") != std::string::npos &&
               editorSource.find("slotSelectorAction == SlotSelectorAction::LOAD") != std::string::npos &&
               editorSource.find("app_shell::level_slots::renderPreview(slot, rect);") != std::string::npos &&
               editorSource.find("slot.slotNumber == currentSlot") != std::string::npos,
           "level editor should preserve distinct save and load slot actions while rendering slot minimaps and highlighting the currently edited slot");
}

void testEditorTextUsesSharedTypography() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string editorSource = readTextFile(repoRoot / "include/LevelEditor.hpp");

    expect(editorSource.find("#include \"TypographyService.hpp\"") != std::string::npos &&
               editorSource.find("TypographyService::shared().initialize();") != std::string::npos,
           "level editor should initialize and consume the shared typography service");
    expect(countOccurrences(editorSource, "DrawText(") == 0u &&
               countOccurrences(editorSource, "MeasureText(") == 0u &&
               countOccurrences(editorSource, "DrawTextEx(") == 0u &&
               countOccurrences(editorSource, "MeasureTextEx(") == 0u,
           "level editor should stop drawing or measuring text through raw backend calls");
}

void testMigratedPresentationSourcesStayOnSharedTypographyPath() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");

    expect(countOccurrences(shellSource, "TypographyService::shared().initialize();") >= 2u,
           "main app shell should initialize the shared typography service before rendering active UI and runtime surfaces");

    expectSharedTypographySource(readTextFile(repoRoot / "include/MainMenu.hpp"), "MainMenu");
    expectSharedTypographySource(readTextFile(repoRoot / "include/LevelSelectMenu.hpp"), "LevelSelectMenu");
    expectSharedTypographySource(readTextFile(repoRoot / "include/MultiplayerSessionMenu.hpp"), "MultiplayerSessionMenu");
    expectSharedTypographySource(readTextFile(repoRoot / "include/SettingsMenu.hpp"), "SettingsMenu");
    expectSharedTypographySource(readTextFile(repoRoot / "include/RuntimeSettingsOverlay.hpp"), "RuntimeSettingsOverlay");
    expectSharedTypographySource(readTextFile(repoRoot / "src/ClientRuntime.cpp"), "ClientRuntime");
    expectSharedTypographySource(readTextFile(repoRoot / "include/LevelEditor.hpp"), "LevelEditor");
}

void testLevelSaveLoadRoundTripPreservesRepresentativeData() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-architecture-characterization";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    LevelData::LevelDefinition source("Architecture Characterization Level");
    source.floorColor = Color{12, 34, 56, 78};
    source.obstacles.push_back(LevelData::Obstacle{
        4.0f,
        -3.0f,
        6.5f,
        2.5f,
        3.5f,
        Color{120, 150, 200, 255}
    });
    source.areas.push_back(LevelData::Area{
        -2.0f,
        7.0f,
        5.0f,
        4.0f,
        Color{10, 220, 30, 140}
    });
    source.enemies.push_back(LevelData::EnemySpawn{
        9.0f,
        -8.0f,
        Color{200, 10, 20, 255}
    });

    const std::filesystem::path explicitPath = tempRoot / "portable-level.json";
    expect(LevelData::saveLevelDefinition(source, explicitPath),
           "portable level schema should save successfully through the explicit path-based persistence helper");
    LevelData::LevelDefinition loadedFromExplicitPath;
    expect(LevelData::loadLevelDefinition(loadedFromExplicitPath, explicitPath),
           "portable level schema should load successfully through the explicit path-based persistence helper");
    expect(loadedFromExplicitPath.name == source.name &&
               loadedFromExplicitPath.obstacles.size() == source.obstacles.size() &&
               loadedFromExplicitPath.areas.size() == source.areas.size() &&
               loadedFromExplicitPath.enemies.size() == source.enemies.size(),
           "portable level schema should round-trip through explicit-path persistence without relying on slot policy");

    constexpr int kSlot = 8;
    expect(LevelData::saveLevel(source, kSlot),
           "representative characterization level should save successfully");
    expect(LevelData::levelExists(kSlot),
           "saved characterization level should exist on disk");

    LevelData::LevelDefinition loaded;
    expect(LevelData::loadLevel(loaded, kSlot),
           "representative characterization level should load successfully");

    expect(loaded.name == source.name, "loaded level name should round-trip");
    expect(loaded.floorColor.r == source.floorColor.r &&
               loaded.floorColor.g == source.floorColor.g &&
               loaded.floorColor.b == source.floorColor.b &&
               loaded.floorColor.a == source.floorColor.a,
           "loaded floor color should round-trip");
    expect(loaded.obstacles.size() == source.obstacles.size(),
           "loaded obstacle count should round-trip");
    expect(loaded.areas.size() == source.areas.size(),
           "loaded area count should round-trip");
    expect(loaded.enemies.size() == source.enemies.size(),
           "loaded enemy count should round-trip");
    expect(loaded.obstacles.front().width == source.obstacles.front().width &&
               loaded.obstacles.front().height == source.obstacles.front().height,
           "loaded obstacle dimensions should round-trip");
    expect(loaded.areas.front().color.a == source.areas.front().color.a,
           "loaded area color alpha should round-trip");
    expect(loaded.enemies.front().x == source.enemies.front().x &&
               loaded.enemies.front().z == source.enemies.front().z,
           "loaded enemy spawn position should round-trip");

    std::filesystem::current_path(tempRoot.parent_path());
    std::filesystem::remove_all(tempRoot);
}

void testSharedSimulationDefaultsOwnCurrentNumericBaselines() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string simulationTypesHeader = readTextFile(repoRoot / "include/sim/SimulationTypes.hpp");
    const std::string defaultsHeader = readTextFile(repoRoot / "include/sim/SimulationDefaults.hpp");

    expect(simulationTypesHeader.find("#include \"Config3D.hpp\"") == std::string::npos,
           "SimulationTypes.hpp should no longer include Config3D.hpp directly");
    expect(simulationTypesHeader.find("#include \"sim/SimulationDefaults.hpp\"") != std::string::npos,
           "SimulationTypes.hpp should include the shared simulation defaults header");
    expect(defaultsHeader.find("namespace sim::defaults") != std::string::npos,
           "SimulationDefaults.hpp should define the shared simulation defaults namespace");

    const sim::SimConfig config;
    expect(config.playerEyeHeight == sim::defaults::kPlayerEyeHeight,
           "SimConfig should keep the shared default player eye height");
    expect(config.playerMoveSpeed == sim::defaults::kPlayerSpeed,
           "SimConfig should keep the shared default player speed");
    expect(config.weaponRange == sim::defaults::kWeaponRange,
           "SimConfig should keep the shared default weapon range");

    const sim::PlayerState playerState;
    expect(playerState.position.y == sim::defaults::kPlayerEyeHeight,
           "PlayerState should keep the shared default player eye height");

    const sim::RemoteActorState enemyState;
    expect(enemyState.radius == sim::defaults::kEnemyRadius,
           "RemoteActorState should keep the shared default enemy radius");
}

void testGameplayUserCmdAndTimingVocabularyRemainSeparatedFromControlOwnership() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string simulationTypesHeader = readTextFile(repoRoot / "include/sim/SimulationTypes.hpp");
    const std::string protocolHeader = readTextFile(repoRoot / "include/net/Protocol.hpp");

    expect(simulationTypesHeader.find("struct TimingCadence {") != std::string::npos &&
               simulationTypesHeader.find("struct CommandTiming {") != std::string::npos &&
               simulationTypesHeader.find("enum class StagedApplyBoundary") != std::string::npos,
           "shared simulation types should define explicit cadence and staged-apply timing vocabulary");

    const std::size_t userCmdStart = simulationTypesHeader.find("struct UserCmd {");
    const std::size_t userCmdEnd = simulationTypesHeader.find("};", userCmdStart);
    expect(userCmdStart != std::string::npos && userCmdEnd != std::string::npos,
           "SimulationTypes.hpp should define a gameplay-only UserCmd contract");
    const std::string userCmdBlock = simulationTypesHeader.substr(userCmdStart, userCmdEnd - userCmdStart);
    expect(userCmdBlock.find("requestedTeam") == std::string::npos &&
               userCmdBlock.find("reportedLatencyMs") == std::string::npos &&
               userCmdBlock.find("reportedLossPct") == std::string::npos,
           "UserCmd should not own team or transport-metric semantics as primary fields");

    expect(simulationTypesHeader.find("UserCmd toUserCmd() const") != std::string::npos &&
               simulationTypesHeader.find("static PlayerCommand fromUserCmd(") != std::string::npos &&
               simulationTypesHeader.find("CommandTiming toCommandTiming() const") != std::string::npos,
           "PlayerCommand should expose transitional adapters around the gameplay-only UserCmd and timing vocabulary");
    expect(protocolHeader.find("sim::TimingCadence cadence{};") != std::string::npos &&
               protocolHeader.find("sim::StagedApplyBoundary stagedApplyBoundary") != std::string::npos,
           "protocol contracts should carry cadence and staged-apply timing semantics explicitly");
}

void testSharedParticipationAndPaneVocabularyStayExplicit() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string simulationTypesHeader = readTextFile(repoRoot / "include/sim/SimulationTypes.hpp");
    const std::string worldStateHeader = readTextFile(repoRoot / "include/sim/WorldState.hpp");

    expect(simulationTypesHeader.find("TeamId::Spectator") != std::string::npos &&
               simulationTypesHeader.find("enum class ParticipationState") != std::string::npos &&
               simulationTypesHeader.find("struct ControlBinding") != std::string::npos &&
               simulationTypesHeader.find("enum class PaneViewMode") != std::string::npos &&
               simulationTypesHeader.find("struct AuthoritativeTime") != std::string::npos,
           "shared simulation contracts should define spectator team, participation, control, pane, and authoritative timing vocabulary explicitly");
    expect(worldStateHeader.find("TimingCadence cadence{};") != std::string::npos &&
               worldStateHeader.find("AuthoritativeTime authoritativeTime{};") != std::string::npos,
           "WorldState should expose shared cadence and authoritative timing surfaces instead of leaving time semantics implicit");

    expect(!sim::isPlayableTeam(sim::TeamId::Spectator),
           "spectator team identity should stay separate from playable-team rules");

    sim::ParticipantState participant;
    participant.presence = sim::SessionPresence::Connected;
    participant.team = sim::TeamId::Spectator;
    participant.participation = sim::ParticipationState::Spectating;
    participant.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 14};

    expect(participant.team == sim::TeamId::Spectator &&
               participant.participation == sim::ParticipationState::Spectating &&
               participant.control.controlsActor() &&
               participant.control.actorId == 14,
           "participant state should preserve team identity, participation state, and control binding as distinct typed fields");

    sim::PaneViewState pane;
    pane.slot = sim::PaneSlot::Right;
    pane.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    pane.focused = false;
    pane.followTargetActorId = 22;

    expect(pane.slot == sim::PaneSlot::Right &&
               pane.mode == sim::PaneViewMode::SpectatorFollowThirdPerson &&
               pane.followTargetActorId == 22,
           "pane view state should expose slot, mode, focus, and follow target without inferring them from camera state");

    sim::WorldState world;
    world.cadence.authoritativeTickHz = 60u;
    world.authoritativeTime.serverTick = 18244u;
    world.authoritativeTime.serverTimeUs = 304066666u;
    world.authoritativeTime.viewedServerTimeUs = 303900000u;

    expect(world.cadence.authoritativeTickHz == 60u &&
               world.authoritativeTime.serverTick == 18244u &&
               world.authoritativeTime.viewedServerTimeUs == 303900000u,
           "world state should carry authoritative cadence and timing contracts directly");

    sim::RosterEntry rosterEntry;
    rosterEntry.team = sim::TeamId::Attacker;
    rosterEntry.participation = sim::ParticipationState::Playing;
    rosterEntry.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};

    expect(rosterEntry.team == sim::TeamId::Attacker &&
               rosterEntry.participation == sim::ParticipationState::Playing &&
               rosterEntry.control.controlsActor(),
           "roster entries should keep typed participation and control fields separate from legacy bot or score booleans");
}

void testReplicationContractsRemainDistinctFromPresentationBundles() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string simulationTypesHeader = readTextFile(repoRoot / "include/sim/SimulationTypes.hpp");
    const std::string protocolHeader = readTextFile(repoRoot / "include/net/Protocol.hpp");

    expect(protocolHeader.find("struct ReplicationSnapshot {") != std::string::npos &&
               protocolHeader.find("struct SessionSummary {") != std::string::npos &&
               protocolHeader.find("struct GameplayEventBatch {") != std::string::npos,
           "protocol contracts should define explicit replication, summary, and gameplay-event batch families");
    expect(protocolHeader.find("using SnapshotEvent = GameplayEvent;") != std::string::npos,
           "the transitional snapshot-event name should remain an alias around the canonical gameplay-event contract");
    expect(protocolHeader.find("struct WorldSnapshot : ReplicationSnapshot, SessionSummary, GameplayEventBatch {") != std::string::npos,
           "WorldSnapshot should remain only a transitional bundle over the split replication contracts");
    expect(protocolHeader.find("ClientViewState") == std::string::npos &&
               protocolHeader.find("RenderFrame") == std::string::npos &&
               simulationTypesHeader.find("ClientViewState") == std::string::npos &&
               simulationTypesHeader.find("RenderFrame") == std::string::npos,
           "shared replication and simulation headers should remain distinct from client presentation contracts");
}

void testPlatformAdaptersRemainNarrowlyScoped() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string udpHeader = readTextFile(repoRoot / "include/net/UdpSocket.hpp");
    const std::string udpSource = readTextFile(repoRoot / "src/UdpSocket.cpp");
    const std::string displayHeader = readTextFile(repoRoot / "include/DisplayManager.hpp");
    const std::string displaySource = readTextFile(repoRoot / "src/DisplayManager.cpp");
    const std::string mainSource = readTextFile(repoRoot / "src/main_3d.cpp");

    expect(udpHeader.find("Platform adapter for non-blocking UDP datagram I/O only.") != std::string::npos &&
               udpHeader.find("It must not own gameplay, study-policy, or session semantics.") != std::string::npos,
           "UdpSocket should document its role as a platform-only UDP adapter");
    expect(udpHeader.find("sim/") == std::string::npos &&
               udpHeader.find("PlayerCommand") == std::string::npos &&
               udpHeader.find("WorldSnapshot") == std::string::npos,
           "UdpSocket should not depend on gameplay contracts directly");
    expect(udpSource.find("TeamChangeRequest") == std::string::npos &&
               udpSource.find("RuntimeParamChangeRequest") == std::string::npos &&
               udpSource.find("SessionFlowController") == std::string::npos,
           "UdpSocket implementation should stay limited to socket concerns rather than control or session policy");

    expect(displayHeader.find("Platform adapter for window, render-target, and input-transform lifecycle only.") != std::string::npos &&
               displayHeader.find("It must not own gameplay, networking, or study-policy semantics.") != std::string::npos,
           "DisplayManager should document its role as a platform-only window and render adapter");
    expect(displayHeader.find("ClientRuntime") == std::string::npos &&
               displayHeader.find("ServerRuntime") == std::string::npos &&
               displayHeader.find("WorldSnapshot") == std::string::npos,
           "DisplayManager declarations should not own networking or gameplay contracts");
    expect(displayHeader.find("void updateWindowPolicy();") != std::string::npos &&
               displaySource.find("FLAG_WINDOW_RESIZABLE") != std::string::npos &&
               displaySource.find("SetWindowMinSize(") != std::string::npos,
           "DisplayManager should own ratio-locked resizable window policy in the platform adapter layer");
    expect(displaySource.find("SessionLaunchConfig") == std::string::npos &&
               displaySource.find("TeamChangeRequest") == std::string::npos &&
               displaySource.find("PlayerCommand") == std::string::npos,
           "DisplayManager implementation should stay limited to render and window lifecycle behavior");
    expect(mainSource.find("display::updateWindowPolicy();") != std::string::npos &&
               mainSource.find("display::syncInputTransform();") == std::string::npos,
           "app-shell frame loops should route resize enforcement through DisplayManager's window policy update hook");
}

void testInputPollingRemainsSeparatedFromGameplayCommandShaping() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string inputHeader = readTextFile(repoRoot / "include/InputHandler3D.hpp");
    const std::string clientRuntimeSource = readTextFile(repoRoot / "src/ClientRuntime.cpp");
    const std::string clientSyncHeader = readTextFile(repoRoot / "include/client/ClientSyncRuntime.hpp");

    expect(inputHeader.find("struct InputFrame {") != std::string::npos &&
               inputHeader.find("static InputFrame toInputFrame(") != std::string::npos &&
               inputHeader.find("static InputFrame toPredictionFrame(") != std::string::npos,
           "InputHandler3D should expose an explicit gameplay InputFrame seam between raw polling and command shaping");

    const std::size_t frameStart = inputHeader.find("struct InputFrame {");
    const std::size_t frameEnd = inputHeader.find("};", frameStart);
    expect(frameStart != std::string::npos && frameEnd != std::string::npos,
           "InputHandler3D should keep the gameplay InputFrame block visible");
    const std::string frameBlock = inputHeader.substr(frameStart, frameEnd - frameStart);
    expect(frameBlock.find("toggleUIMode") == std::string::npos &&
               frameBlock.find("toggleScoreboard") == std::string::npos &&
               frameBlock.find("switchTeam") == std::string::npos,
           "InputFrame should stay limited to gameplay command fields rather than menu or platform toggles");

    expect(clientRuntimeSource.find("syncRuntime_.buildCommand(syncContext(),") != std::string::npos &&
               clientRuntimeSource.find("syncRuntime_.applyLocalPrediction(syncContext(),") != std::string::npos &&
               clientRuntimeSource.find("syncRuntime_.shouldPredictFireAttempt(syncContext(), *input)") != std::string::npos &&
               clientSyncHeader.find("InputHandler3D::toInputFrame(input)") != std::string::npos &&
               clientSyncHeader.find("InputHandler3D::toPlayerCommand(inputFrame,") != std::string::npos,
           "ClientRuntime should delegate gameplay command shaping and immediate prediction policy through ClientSyncRuntime");
}

void testRenderPresentationConsumesClientViewStateHandoff() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string clientViewHeader = readTextFile(repoRoot / "include/client/ClientViewState.hpp");
    const std::string clientPresentationHeader =
        readTextFile(repoRoot / "include/client/ClientPresentation.hpp");
    const std::string clientRuntimeHeader = readTextFile(repoRoot / "include/net/ClientRuntime.hpp");
    const std::string clientRuntimeSource = readTextFile(repoRoot / "src/ClientRuntime.cpp");

    expect(clientViewHeader.find("struct CameraViewState {") != std::string::npos &&
               clientViewHeader.find("struct RemotePlayerView {") != std::string::npos &&
               clientViewHeader.find("struct RemoteEnemyView {") != std::string::npos &&
               clientViewHeader.find("struct TeamMenuView {") != std::string::npos,
           "ClientViewState should expose explicit camera, remote-actor, and team-menu presentation seams");
    expect(clientPresentationHeader.find("struct ClientPresentationInputs {") != std::string::npos &&
               clientPresentationHeader.find("const ClientViewState& viewState;") != std::string::npos &&
               clientPresentationHeader.find("RenderFrame build(const ClientPresentationInputs& inputs) const") != std::string::npos,
           "ClientPresentation should consume ClientViewState and produce an explicit RenderFrame contract");
    expect(clientRuntimeHeader.find("client::ClientPresentation clientPresentation_{};") != std::string::npos,
           "ClientRuntime should keep ClientPresentation as the render-facing handoff owner");

    const std::size_t renderStart = clientRuntimeSource.find("void ClientRuntime::render() const {");
    const std::size_t renderEnd =
        clientRuntimeSource.find("ClientConnectionState ClientRuntime::state() const {", renderStart);
    expect(renderStart != std::string::npos && renderEnd != std::string::npos,
           "ClientRuntime render block should remain visible for architecture characterization");
    const std::string renderBlock = clientRuntimeSource.substr(renderStart, renderEnd - renderStart);

    const std::size_t liveFrameStart =
        clientRuntimeSource.find("client::RenderFrame ClientRuntime::buildLiveRenderFrame(");
    const std::size_t liveFrameEnd =
        clientRuntimeSource.find("void ClientRuntime::toggleReplayRecording()", liveFrameStart);
    expect(liveFrameStart != std::string::npos && liveFrameEnd != std::string::npos,
           "ClientRuntime should keep the live presentation handoff in a dedicated RenderFrame builder");
    const std::string liveFrameBlock =
        clientRuntimeSource.substr(liveFrameStart, liveFrameEnd - liveFrameStart);

    expect(liveFrameBlock.find("clientPresentation_.build(") != std::string::npos &&
               liveFrameBlock.find("client::ClientPresentationInputs{viewState, arena_.get(), &combatTraceBeams}") != std::string::npos,
           "ClientRuntime live-frame building should delegate presentation state construction to ClientPresentation");
    expect(renderBlock.find("client::RenderFrame frame = buildRenderFrame(viewState);") != std::string::npos &&
               renderBlock.find("frame.remoteEnemies") != std::string::npos &&
               renderBlock.find("frame.remotePlayers") != std::string::npos &&
               renderBlock.find("frame.hud.lines") != std::string::npos &&
               renderBlock.find("frame.scoreboard") != std::string::npos,
           "ClientRuntime render should consume a RenderFrame rather than rebuilding render state from sync-owned collections inline");
    expect(renderBlock.find("Player3D::renderRootFromSimState(") == std::string::npos &&
               renderBlock.find("client::PresentationStateSubsystem::compactHudLines(viewState)") == std::string::npos &&
               renderBlock.find("const auto& actor = remoteEnemies_[index];") == std::string::npos,
           "ClientRuntime render should stop reaching into raw player and enemy sync state after the RenderFrame handoff");
}

void testHostJoinModeUsesComposedRuntime() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");
    const std::string sessionFlowSource = readTextFile(repoRoot / "src/SessionFlowController.cpp");
    const std::string clientRuntimeHeader = readTextFile(repoRoot / "include/net/ClientRuntime.hpp");

    expect(shellSource.find("app::AppFlow::startSession(result.launchConfig)") != std::string::npos &&
               shellSource.find("app::AppFlow::startSession(joinConfig)") != std::string::npos,
           "host and join mode should both launch through the composed AppFlow session stack");
    expect(shellSource.find("net::ClientRuntime client(clientConfig);") == std::string::npos,
           "the direct join path should no longer construct a standalone ClientRuntime outside the shared host-join stack");
    expect(shellSource.find("case GameMode::MULTIPLAYER_SESSION:") != std::string::npos &&
               shellSource.find("startLabStudySession(selectedLevel)") != std::string::npos &&
               shellSource.find("sessionFlow->render();") != std::string::npos,
           "host, join, and lab-study flows should render through SessionFlowController");
    expect(sessionFlowSource.find("GameMode::FREE_GAME") == std::string::npos,
           "SessionFlowController should remain on the composed multiplayer runtime path without owning app-shell mode policy");
    expect(clientRuntimeHeader.find("client::ReplaySubsystem replaySubsystem_{};") != std::string::npos &&
               !containsExactSourceToken(shellSource, "GameMode::REPLAY") &&
               !containsExactSourceToken(shellSource, "GameMode::RECORDING"),
           "Host/Join mode should keep recording on the shared client runtime path without requiring a dedicated replay or recording mode identity");
}

void testRecordingLifecycleRemainsOnSharedClientRuntimePath() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string clientRuntimeHeader =
        readTextFile(repoRoot / "include/net/ClientRuntime.hpp");
    const std::string shellSource =
        readTextFile(repoRoot / "src/main_3d.cpp");

    expect(clientRuntimeHeader.find("client::ReplaySubsystem replaySubsystem_{};") != std::string::npos &&
               !containsExactSourceToken(shellSource, "GameMode::REPLAY") &&
               !containsExactSourceToken(shellSource, "GameMode::RECORDING"),
           "recording should remain a cross-mode session feature surfaced through client replay state rather than a dedicated mode identity");
}

void testReplayOverlayRemainsOnSharedClientRuntimePath() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string clientViewStateHeader =
        readTextFile(repoRoot / "include/client/ClientViewState.hpp");
    const std::string clientRuntimeSource =
        readTextFile(repoRoot / "src/ClientRuntime.cpp");

    expect(clientViewStateHeader.find("bool overlayVisible{false};") != std::string::npos,
           "typed replay presentation state should carry explicit replay-overlay visibility");
    expect(clientRuntimeSource.find("input->toggleRecordingOverlay") != std::string::npos &&
               clientRuntimeSource.find("replayOverlayVisible_ = !replayOverlayVisible_;") != std::string::npos &&
               clientRuntimeSource.find("drawReplayOverlay(frame.replay);") != std::string::npos &&
               clientRuntimeSource.find("frame.replay = replayStatusView();") != std::string::npos,
           "ClientRuntime should consume the O hotkey, preserve replay overlay visibility locally, and render that overlay on the shared runtime path");
}

void testToggleableOverlayRenderingStaysCentralized() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string replayTransportHeader =
        readTextFile(repoRoot / "include/client/ReplayTransportControls.hpp");
    const std::string runtimeSettingsHeader =
        readTextFile(repoRoot / "include/RuntimeSettingsOverlay.hpp");
    const std::string hudOverlayHeader =
        readTextFile(repoRoot / "include/client/HudOverlayRenderer.hpp");
    const std::string clientRuntimeSource =
        readTextFile(repoRoot / "src/ClientRuntime.cpp");
    const std::string replayStudioSource =
        readTextFile(repoRoot / "src/ReplayStudio.cpp");

    expect(replayTransportHeader.find("struct ReplayTransportOverlayState") != std::string::npos &&
               replayTransportHeader.find("struct ReplayCheckpointOverlayState") != std::string::npos &&
               replayTransportHeader.find("renderReplayTransportOverlay") != std::string::npos &&
               replayTransportHeader.find("makeReplayRecordingTransportButtons") != std::string::npos,
           "recording and replay transport overlays should have one shared visual implementation");
    expect(clientRuntimeSource.find("client::renderReplayTransportOverlay(overlay, buttons);") != std::string::npos &&
               replayStudioSource.find("client::renderReplayTransportOverlay(overlay, buttons);") != std::string::npos,
           "runtime and replay studio should only adapt state into the shared replay transport renderer");
    expect(clientRuntimeSource.find("overlay.checkpoint = client::ReplayCheckpointOverlayState{") != std::string::npos &&
               replayStudioSource.find("overlay.checkpoint = client::ReplayCheckpointOverlayState{") != std::string::npos,
           "runtime and Replay Studio transport overlays should both feed checkpoint state into the shared checkpoint panel");
    expect(clientRuntimeSource.find("void drawReplayCheckpointOverlay") == std::string::npos,
           "client runtime should not reimplement replay checkpoint overlay drawing locally");

    expect(runtimeSettingsHeader.find("class RuntimeSettingsOverlay") != std::string::npos &&
               clientRuntimeSource.find("RuntimeSettingsOverlay::State ClientRuntime::buildRuntimeSettingsOverlayState()") != std::string::npos &&
               replayStudioSource.find("RuntimeSettingsOverlay::State ReplayStudio::buildReplaySettingsOverlayState()") != std::string::npos,
           "the U settings overlay should remain one shared renderer with mode-specific state builders");
    expect(hudOverlayHeader.find("static void renderScoreboard") != std::string::npos &&
               clientRuntimeSource.find("client::HudOverlayRenderer::renderScoreboard(frame.scoreboard);") != std::string::npos &&
               replayStudioSource.find("client::HudOverlayRenderer::renderScoreboard(frame.scoreboard);") != std::string::npos,
	           "scoreboard rendering should stay centralized in HudOverlayRenderer across runtime and replay views");
}

void testReplayStudioLibrarySelectionIsExplicit() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string replayStudioSource =
        readTextFile(repoRoot / "src/ReplayStudio.cpp");

    const std::size_t rowClick = replayStudioSource.find("rows_[index].hovered && mousePressed");
    const std::size_t actionHit =
        replayStudioSource.find("const LibraryActionButton hoveredAction");
    const std::size_t rowLoop =
        replayStudioSource.find("for (std::size_t index = 0u; index < rows_.size(); ++index)");
    const std::size_t immersiveUpdate =
        replayStudioSource.find("void ReplayStudio::updateImmersiveCommandReplay");
    const std::size_t immersiveRender =
        replayStudioSource.find("void ReplayStudio::renderImmersiveOverlay");
    const std::size_t transportRender =
        replayStudioSource.find("void ReplayStudio::renderReplayTransportOverlay", immersiveRender);

    expect(rowClick != std::string::npos &&
               replayStudioSource.find("previewSelectedReplay();", rowClick) != std::string::npos &&
               replayStudioSource.find("libraryActionButtonAt(Vector2 mouse)") != std::string::npos &&
               replayStudioSource.find("IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)") != std::string::npos,
           "Replay Studio library row clicks should select and preview only; opening should stay on the explicit Open button or Enter key");
    expect(actionHit != std::string::npos &&
               rowLoop != std::string::npos &&
               actionHit < rowLoop &&
               replayStudioSource.find("!libraryActionHovered && CheckCollisionPointRec(mouse, rows_[index].bounds)") !=
                   std::string::npos &&
               replayStudioSource.find("rows_[index].hovered && mousePressed && !mouseClickConsumed") !=
                   std::string::npos,
           "Replay Studio action buttons should consume overlapping clicks before library rows can select the replay underneath");
    expect(replayStudioSource.find("updateCommandPlayback(dtSeconds, true);") != std::string::npos &&
               replayStudioSource.find("updateLegacyPlayback(dtSeconds, true);") != std::string::npos,
           "Replay Studio previews should loop through the same command and legacy playback paths used for rendering");
    expect(immersiveUpdate != std::string::npos &&
               replayStudioSource.find("if (!replaySettingsVisible_) {\n        display::disableCursorForCapture();",
                                       immersiveUpdate) == std::string::npos,
           "Replay Studio spectator mouse look should not re-apply cursor capture every frame because that can consume mouse deltas");
    expect(immersiveRender != std::string::npos &&
               transportRender != std::string::npos &&
               replayStudioSource.find("O Transport | U Settings", immersiveRender) > transportRender &&
               replayStudioSource.find("DrawRectangle(18, 18, 640, 136", immersiveRender) > transportRender,
           "Replay Studio immersive replay should not draw the always-on top-left title/help text block; detailed controls belong behind O/U overlays");
}

void testEnterReleaseCapturedMouseUsesSharedDisplayManagerPath() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string displayHeader = readTextFile(repoRoot / "include/DisplayManager.hpp");
    const std::string displaySource = readTextFile(repoRoot / "src/DisplayManager.cpp");
    const std::string clientRuntimeSource = readTextFile(repoRoot / "src/ClientRuntime.cpp");
    const std::string replayStudioSource = readTextFile(repoRoot / "src/ReplayStudio.cpp");

    expect(displayHeader.find("bool isCursorCaptured();") != std::string::npos &&
               displayHeader.find("bool releaseCursorIfCaptured();") != std::string::npos &&
               displaySource.find("bool releaseCursorIfCaptured()") != std::string::npos,
           "DisplayManager should own the shared captured-cursor query/release helper");
    expect(clientRuntimeSource.find("confirmPressed && display::isCursorCaptured()") != std::string::npos &&
               clientRuntimeSource.find("if (releaseCapturedMouseWithConfirm)") != std::string::npos &&
               clientRuntimeSource.find("setUiMode(true);") != std::string::npos,
           "normal gameplay sessions should make Enter release the captured mouse through the shared UI-mode path");
    expect(replayStudioSource.find("input.menuConfirm && display::releaseCursorIfCaptured()") != std::string::npos &&
               replayStudioSource.find("!replaySettingsVisible_ && display::isCursorCaptured()") != std::string::npos,
           "Replay Studio immersive playback should release the mouse on Enter and only drive free-look while captured");
}

void testFinalRuntimeSourceBoundariesRemoveTransitionalAdapters() {
    const std::filesystem::path repoRoot = findRepoRoot();
    const std::string shellSource = readTextFile(repoRoot / "src/main_3d.cpp");
    const std::string controllerSource = readTextFile(repoRoot / "src/SessionFlowController.cpp");
    const std::string clientSource = readTextFile(repoRoot / "src/ClientRuntime.cpp");
    const std::string serverSource = readTextFile(repoRoot / "src/ServerRuntime.cpp");

    expect(shellSource.find("case GameMode::GAMEPLAY:") == std::string::npos,
           "main app shell should keep primary runtime paths on the shared session stack");
    expect(shellSource.find("makeLabStudyLaunchConfig(int levelSlot)") != std::string::npos &&
               shellSource.find("labStudyTarget") != std::string::npos &&
               shellSource.find("startLabStudySession(selectedLevel)") != std::string::npos,
           "main app shell should compose the consolidated lab-study mode through an explicit shared-session launch helper");

    expect(controllerSource.find("hasComposedRuntime(") != std::string::npos &&
               controllerSource.find("advanceHostedNetworking(dtUs);") != std::string::npos,
           "SessionFlowController should treat runtime composition presence and hosted-step timing as explicit orchestration concerns");

    expect(clientSource.find("#include \"net/ProxyRuntime.hpp\"") == std::string::npos &&
               clientSource.find("#include \"net/TransportArtifactAdapter.hpp\"") != std::string::npos &&
               clientSource.find("LevelData::LevelDefinition level;") != std::string::npos,
           "ClientRuntime should depend on the transport adapter boundary and shared LevelDefinition schema rather than transitional proxy or alias seams");

    expect(serverSource.find("applyTransportArtifactParamValue(") != std::string::npos &&
               serverSource.find("isBotTransportTargetId(") != std::string::npos &&
               serverSource.find("request.targetId < static_cast<std::int32_t>(kFirstBotTransportTargetId)") == std::string::npos,
           "ServerRuntime should normalize bot transport overrides around the transport adapter helpers instead of proxy-era target checks");
}

}  // namespace

int main() {
    try {
        testCheckpointContractsMoveIntoSharedStore();
        testAppDataRootStaysInRepoNetcodeSimDirectory();
        testStudyModeCompositionMovesOntoSharedRuntimeStack();
        testLevelPersistenceMovesIntoTranslationUnitWithoutChangingSlotRules();
        testLevelEditorConsumesSharedLevelDefinitionSchema();
        testEditorTextUsesSharedTypography();
        testMigratedPresentationSourcesStayOnSharedTypographyPath();
        testLevelSaveLoadRoundTripPreservesRepresentativeData();
        testSharedSimulationDefaultsOwnCurrentNumericBaselines();
        testGameplayUserCmdAndTimingVocabularyRemainSeparatedFromControlOwnership();
        testSharedParticipationAndPaneVocabularyStayExplicit();
        testReplicationContractsRemainDistinctFromPresentationBundles();
        testPlatformAdaptersRemainNarrowlyScoped();
        testInputPollingRemainsSeparatedFromGameplayCommandShaping();
        testRenderPresentationConsumesClientViewStateHandoff();
        testHostJoinModeUsesComposedRuntime();
        testRecordingLifecycleRemainsOnSharedClientRuntimePath();
        testReplayOverlayRemainsOnSharedClientRuntimePath();
        testToggleableOverlayRenderingStaysCentralized();
        testReplayStudioLibrarySelectionIsExplicit();
        testEnterReleaseCapturedMouseUsesSharedDisplayManagerPath();
        testFinalRuntimeSourceBoundariesRemoveTransitionalAdapters();
    } catch (const std::exception& ex) {
        std::cerr << "ArchitectureCharacterizationTests failure: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "ArchitectureCharacterizationTests passed\n";
    return 0;
}
