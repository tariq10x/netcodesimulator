cmake_minimum_required(VERSION 3.16)

set(NETCODESIM_BUILD_CONFIGURATIONS Release Debug RelWithDebInfo MinSizeRel)
get_property(NETCODESIM_GENERATOR_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NETCODESIM_GENERATOR_IS_MULTI_CONFIG AND NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${NETCODESIM_BUILD_CONFIGURATIONS}" CACHE STRING "Available build configurations")
endif()

project(netcodesim VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_DEFAULT_BUILD_TYPE)
        set(CMAKE_DEFAULT_BUILD_TYPE Release CACHE STRING "Default build type")
    endif()
    set_property(CACHE CMAKE_DEFAULT_BUILD_TYPE PROPERTY STRINGS ${NETCODESIM_BUILD_CONFIGURATIONS})
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${NETCODESIM_BUILD_CONFIGURATIONS})
endif()

include(CTest)

message(STATUS "Building netcodesim with raylib")

if(WIN32)
    set(NETCODESIM_DEFAULT_USE_SYSTEM_RAYLIB OFF)
else()
    set(NETCODESIM_DEFAULT_USE_SYSTEM_RAYLIB ON)
endif()

option(NETCODESIM_USE_SYSTEM_RAYLIB "Use an installed raylib package when available" ${NETCODESIM_DEFAULT_USE_SYSTEM_RAYLIB})
option(NETCODESIM_FETCH_RAYLIB "Fetch raylib with CMake when no installed raylib package is found" ON)
option(NETCODESIM_ENABLE_HIGHDPI "Enable raylib HighDPI window support" ON)
set(NETCODESIM_RAYLIB_TAG "6.0" CACHE STRING "raylib git tag used when CMake fetches raylib")
set(NETCODESIM_RAYLIB_PROVIDER "")

if(NETCODESIM_USE_SYSTEM_RAYLIB)
    find_package(raylib QUIET)
endif()

if(NETCODESIM_USE_SYSTEM_RAYLIB AND raylib_FOUND)
    set(NETCODESIM_RAYLIB_TARGET raylib)
    set(NETCODESIM_RAYLIB_PROVIDER "system CMake package")
elseif(NETCODESIM_USE_SYSTEM_RAYLIB)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RAYLIB QUIET raylib)
    endif()

    if(RAYLIB_FOUND)
        add_library(raylib_external INTERFACE)
        target_include_directories(raylib_external SYSTEM INTERFACE ${RAYLIB_INCLUDE_DIRS})
        target_link_directories(raylib_external INTERFACE ${RAYLIB_LIBRARY_DIRS})
        target_link_libraries(raylib_external INTERFACE ${RAYLIB_LIBRARIES})
        target_compile_options(raylib_external INTERFACE ${RAYLIB_CFLAGS_OTHER})
        set(NETCODESIM_RAYLIB_TARGET raylib_external)
        set(NETCODESIM_RAYLIB_PROVIDER "system pkg-config package")
    endif()
endif()

if(NOT NETCODESIM_RAYLIB_TARGET AND APPLE AND NETCODESIM_USE_SYSTEM_RAYLIB)
    foreach(_raylib_prefix IN ITEMS /opt/homebrew/opt/raylib /usr/local/opt/raylib)
        if(EXISTS "${_raylib_prefix}/include/raylib.h")
            add_library(raylib_external INTERFACE)
            target_include_directories(raylib_external SYSTEM INTERFACE "${_raylib_prefix}/include")
            target_link_directories(raylib_external INTERFACE "${_raylib_prefix}/lib")
            target_link_libraries(raylib_external INTERFACE raylib)
            set(NETCODESIM_RAYLIB_TARGET raylib_external)
            set(NETCODESIM_RAYLIB_PROVIDER "Homebrew package at ${_raylib_prefix}")
            break()
        endif()
    endforeach()
endif()

if(NOT NETCODESIM_RAYLIB_TARGET AND NETCODESIM_FETCH_RAYLIB)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG ${NETCODESIM_RAYLIB_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(raylib)
    if(TARGET raylib)
        set(NETCODESIM_RAYLIB_TARGET raylib)
        set(NETCODESIM_RAYLIB_PROVIDER "CMake FetchContent tag ${NETCODESIM_RAYLIB_TAG}")
        set(NETCODESIM_RAYLIB_SYSTEM_INCLUDE_DIRS
            "${raylib_SOURCE_DIR}/src"
            "${raylib_SOURCE_DIR}/src/external"
        )
    endif()
endif()

if(NOT NETCODESIM_RAYLIB_TARGET)
    message(FATAL_ERROR "raylib was not found. Install raylib or configure with NETCODESIM_FETCH_RAYLIB=ON")
endif()

message(STATUS "raylib provider: ${NETCODESIM_RAYLIB_PROVIDER}")
message(STATUS "raylib target: ${NETCODESIM_RAYLIB_TARGET}")

set(NETCODESIM_RAYLIB_EXTRA_LIBS)
if(UNIX AND NOT APPLE)
    find_package(glfw3 CONFIG QUIET)
    if(TARGET glfw)
        list(APPEND NETCODESIM_RAYLIB_EXTRA_LIBS glfw)
    endif()
endif()

include(cmake/NetcodeSimQuality.cmake)

set(SOURCES
    src/main_3d.cpp
)

add_library(NetcodeSimSim STATIC
    src/SimulationRules.cpp
    src/WorldState.cpp
)

target_include_directories(NetcodeSimSim PUBLIC ${CMAKE_SOURCE_DIR}/include)
netcodesim_apply_project_quality(NetcodeSimSim)

add_library(NetcodeSimNet STATIC
    src/Codec.cpp
    src/CommandReplayArchive.cpp
    src/LagCompensation.cpp
    src/Protocol.cpp
    src/ProxyRuntime.cpp
    src/ReplayPlaybackRuntime.cpp
    src/SessionDiscovery.cpp
    src/StudyEventLog.cpp
    src/UserDataPaths.cpp
    src/UdpSocket.cpp
    src/ServerRuntime.cpp
)

target_include_directories(NetcodeSimNet PUBLIC ${CMAKE_SOURCE_DIR}/include)
netcodesim_apply_project_quality(NetcodeSimNet)
target_compile_definitions(NetcodeSimNet PUBLIC
    NETCODESIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    NETCODESIM_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(NetcodeSimNet PUBLIC NetcodeSimSim)

if(WIN32)
    target_link_libraries(NetcodeSimNet PUBLIC ws2_32)
endif()

add_library(NetcodeSimClient STATIC
    src/Arena3D.cpp
    src/CheckpointStore.cpp
    src/ClientRuntime.cpp
    src/ControlBindings.cpp
    src/DisplayManager.cpp
    src/DiagnosticsModel.cpp
    src/InterpolationBuffer.cpp
    src/LevelData.cpp
    src/PredictionBuffer.cpp
    src/ReplayArchive.cpp
    src/ReplayStudio.cpp
    src/ReplayTimeline.cpp
    src/SessionFlowController.cpp
    src/TypographyService.cpp
    src/UserSettings.cpp
)

target_include_directories(NetcodeSimClient PUBLIC ${CMAKE_SOURCE_DIR}/include)
if(NETCODESIM_RAYLIB_SYSTEM_INCLUDE_DIRS)
    target_include_directories(NetcodeSimClient SYSTEM PUBLIC ${NETCODESIM_RAYLIB_SYSTEM_INCLUDE_DIRS})
endif()
netcodesim_apply_project_quality(NetcodeSimClient)
target_link_libraries(NetcodeSimClient PUBLIC NetcodeSimNet ${NETCODESIM_RAYLIB_TARGET} ${NETCODESIM_RAYLIB_EXTRA_LIBS})
target_compile_definitions(NetcodeSimClient PUBLIC
    NETCODESIM_ENABLE_HIGHDPI=$<BOOL:${NETCODESIM_ENABLE_HIGHDPI}>
)

if(APPLE)
    target_link_libraries(NetcodeSimClient PUBLIC "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

add_executable(netcodesim ${SOURCES})
netcodesim_apply_project_quality(netcodesim)
target_link_libraries(netcodesim PRIVATE NetcodeSimClient)

if(APPLE)
    target_link_libraries(netcodesim PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

# Copy assets to build directory
add_custom_command(TARGET netcodesim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:netcodesim>/assets
    COMMENT "Copying assets to build directory"
)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
