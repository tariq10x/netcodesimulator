add_executable(SimulationRulesTests
    SimulationRulesTests.cpp
)

target_link_libraries(SimulationRulesTests PRIVATE NetcodeSimSim)

add_test(NAME SimulationRulesTests COMMAND SimulationRulesTests)

add_executable(ProtocolTests
    ProtocolTests.cpp
)

target_link_libraries(ProtocolTests PRIVATE NetcodeSimNet)

add_test(NAME ProtocolTests COMMAND ProtocolTests)

add_executable(UdpSocketLoopbackTests
    UdpSocketLoopbackTests.cpp
)

target_link_libraries(UdpSocketLoopbackTests PRIVATE NetcodeSimNet)

add_test(NAME UdpSocketLoopbackTests COMMAND UdpSocketLoopbackTests)

add_executable(ServerRuntimeTests
    ServerRuntimeTests.cpp
)

target_link_libraries(ServerRuntimeTests PRIVATE NetcodeSimNet)

add_test(NAME ServerRuntimeTests COMMAND ServerRuntimeTests)

add_executable(ClientRuntimeJoinTests
    ClientRuntimeJoinTests.cpp
)

target_link_libraries(ClientRuntimeJoinTests PRIVATE NetcodeSimClient)
target_compile_definitions(ClientRuntimeJoinTests PRIVATE NETCODESIM_ENABLE_TEST_HOOKS)

add_test(NAME ClientRuntimeJoinTests COMMAND ClientRuntimeJoinTests)

add_executable(ClientPredictionTests
    ClientPredictionTests.cpp
)

target_link_libraries(ClientPredictionTests PRIVATE NetcodeSimClient)

add_test(NAME ClientPredictionTests COMMAND ClientPredictionTests)

add_executable(InterpolationTests
    InterpolationTests.cpp
)

target_link_libraries(InterpolationTests PRIVATE NetcodeSimClient)

add_test(NAME InterpolationTests COMMAND InterpolationTests)

add_executable(LagCompensationTests
    LagCompensationTests.cpp
)

target_link_libraries(LagCompensationTests PRIVATE NetcodeSimNet)

add_test(NAME LagCompensationTests COMMAND LagCompensationTests)

add_executable(ProxyRuntimeTests
    ProxyRuntimeTests.cpp
)

target_link_libraries(ProxyRuntimeTests PRIVATE NetcodeSimNet)

add_test(NAME ProxyRuntimeTests COMMAND ProxyRuntimeTests)

add_executable(DiagnosticsModelTests
    DiagnosticsModelTests.cpp
)

target_link_libraries(DiagnosticsModelTests PRIVATE NetcodeSimClient)

add_test(NAME DiagnosticsModelTests COMMAND DiagnosticsModelTests)

add_executable(SessionDiscoveryTests
    SessionDiscoveryTests.cpp
)

target_link_libraries(SessionDiscoveryTests PRIVATE NetcodeSimNet)

add_test(NAME SessionDiscoveryTests COMMAND SessionDiscoveryTests)

add_executable(MultiplayerSessionMenuTests
    MultiplayerSessionMenuTests.cpp
)

target_link_libraries(MultiplayerSessionMenuTests PRIVATE NetcodeSimClient)

add_test(NAME MultiplayerSessionMenuTests COMMAND MultiplayerSessionMenuTests)

add_executable(SessionLaunchFlowTests
    SessionLaunchFlowTests.cpp
)

target_link_libraries(SessionLaunchFlowTests PRIVATE NetcodeSimClient)

add_test(NAME SessionLaunchFlowTests COMMAND SessionLaunchFlowTests)

add_executable(PlayerActionParityTests
    PlayerActionParityTests.cpp
)

target_link_libraries(PlayerActionParityTests PRIVATE NetcodeSimClient)

add_test(NAME PlayerActionParityTests COMMAND PlayerActionParityTests)

add_executable(CombatParityTests
    CombatParityTests.cpp
)

target_link_libraries(CombatParityTests PRIVATE NetcodeSimClient)
target_compile_definitions(CombatParityTests PRIVATE NETCODESIM_ENABLE_TEST_HOOKS)

add_test(NAME CombatParityTests COMMAND CombatParityTests)

add_executable(PlayerPresentationTests
    PlayerPresentationTests.cpp
)

target_link_libraries(PlayerPresentationTests PRIVATE NetcodeSimClient)

add_test(NAME PlayerPresentationTests COMMAND PlayerPresentationTests)

add_executable(TypographyServiceTests
    TypographyServiceTests.cpp
)

target_link_libraries(TypographyServiceTests PRIVATE NetcodeSimClient)
target_compile_definitions(TypographyServiceTests PRIVATE
    NETCODESIM_RUNTIME_ASSET_DIR="$<TARGET_FILE_DIR:netcodesim>/assets"
)
add_dependencies(TypographyServiceTests netcodesim)

add_test(NAME TypographyServiceTests COMMAND TypographyServiceTests)

add_executable(ArchitectureCharacterizationTests
    ArchitectureCharacterizationTests.cpp
)

target_link_libraries(ArchitectureCharacterizationTests PRIVATE NetcodeSimClient)

add_test(NAME ArchitectureCharacterizationTests COMMAND ArchitectureCharacterizationTests)

add_executable(LevelEditorTests
    LevelEditorTests.cpp
)

target_link_libraries(LevelEditorTests PRIVATE NetcodeSimClient)

add_test(NAME LevelEditorTests COMMAND LevelEditorTests)

add_executable(CheckpointStoreTests
    CheckpointStoreTests.cpp
)

target_link_libraries(CheckpointStoreTests PRIVATE NetcodeSimClient)

add_test(NAME CheckpointStoreTests COMMAND CheckpointStoreTests)

add_executable(CharacterProfileTests
    CharacterProfileTests.cpp
)

target_link_libraries(CharacterProfileTests PRIVATE NetcodeSimClient)

add_test(NAME CharacterProfileTests COMMAND CharacterProfileTests)

add_executable(ReplayTimelineTests
    ReplayTimelineTests.cpp
)

target_link_libraries(ReplayTimelineTests PRIVATE NetcodeSimClient)

add_test(NAME ReplayTimelineTests COMMAND ReplayTimelineTests)

add_executable(ReplayArchiveTests
    ReplayArchiveTests.cpp
)

target_link_libraries(ReplayArchiveTests PRIVATE NetcodeSimClient)

add_test(NAME ReplayArchiveTests COMMAND ReplayArchiveTests)

add_executable(ReplayCommandArchiveTests
    ReplayCommandArchiveTests.cpp
)

target_link_libraries(ReplayCommandArchiveTests PRIVATE NetcodeSimNet)

add_test(NAME ReplayCommandArchiveTests COMMAND ReplayCommandArchiveTests)

add_executable(UserSettingsTests
    UserSettingsTests.cpp
)

target_link_libraries(UserSettingsTests PRIVATE NetcodeSimClient)

add_test(NAME UserSettingsTests COMMAND UserSettingsTests)

add_executable(StudyEventLogTests
    StudyEventLogTests.cpp
)

target_link_libraries(StudyEventLogTests PRIVATE NetcodeSimNet)

add_test(NAME StudyEventLogTests COMMAND StudyEventLogTests)

get_property(NETCODESIM_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(NETCODESIM_TEST_TARGET IN LISTS NETCODESIM_TEST_TARGETS)
    netcodesim_apply_project_quality(${NETCODESIM_TEST_TARGET})
endforeach()
