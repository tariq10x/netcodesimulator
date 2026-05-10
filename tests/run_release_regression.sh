#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"

targets=(
  LevelEditorTests
  MultiplayerSessionMenuTests
  SessionDiscoveryTests
  SessionLaunchFlowTests
  PlayerPresentationTests
  ProtocolTests
  ServerRuntimeTests
  ClientRuntimeJoinTests
  CheckpointStoreTests
  CombatParityTests
  ArchitectureCharacterizationTests
)

cmake --build "${build_dir}" --target "${targets[@]}" --parallel 4

ctest --test-dir "${build_dir}" --output-on-failure -R \
  "LevelEditorTests|MultiplayerSessionMenuTests|SessionDiscoveryTests|SessionLaunchFlowTests|PlayerPresentationTests|ProtocolTests|ServerRuntimeTests|ClientRuntimeJoinTests|CheckpointStoreTests|CombatParityTests|ArchitectureCharacterizationTests"
