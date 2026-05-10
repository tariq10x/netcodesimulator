# Release Regression Matrix

Use [`run_release_regression.sh`](./run_release_regression.sh) for the repeatable release-prep smoke pass:

```bash
./tests/run_release_regression.sh build
```

Coverage ownership:

- `LevelEditorTests`
  Covers editor placement, toolbar affordances, save visibility, and drag/move behavior.
- `MultiplayerSessionMenuTests`, `SessionDiscoveryTests`, `SessionLaunchFlowTests`
  Cover JOIN discovery stability, back-navigation/menu flow, external menu actions, and host/join/lab-study launch seams.
- `PlayerPresentationTests`, `ClientRuntimeJoinTests`, `CombatParityTests`
  Cover compact score presentation, kill feed behavior, study presentation toggles, and HUD/runtime parity.
- `ProtocolTests`, `ServerRuntimeTests`, `ClientRuntimeJoinTests`
  Cover session-action protocol wiring, frozen-bot study actions, and staged/live tick-rate behavior.
- `CheckpointStoreTests`, `ClientRuntimeJoinTests`
  Cover recording-checkpoint defaults and persisted transition timing.
- `ArchitectureCharacterizationTests`
  Guards the higher-level shared-runtime and ownership boundaries used by the release-prep refactors.
