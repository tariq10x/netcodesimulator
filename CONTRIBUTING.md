# Contributing

NetcodeSim is a local/LAN educational simulator for FPS networking concepts. Contributions should keep the project understandable as a learning resource, not just functional as a game prototype.

## Development Setup

Install CMake, a C++17 compiler, and raylib. Static-analysis runs also require `clang-tidy`.

On macOS:

```bash
brew install cmake raylib
```

On Debian/Ubuntu and derivatives:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  libasound2-dev \
  libgl1-mesa-dev \
  libglu1-mesa-dev \
  libx11-dev \
  libxcursor-dev \
  libxi-dev \
  libxinerama-dev \
  libxrandr-dev \
  pkg-config
```

For `clang-tidy` on macOS:

```bash
brew install llvm
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

Configure, build, and test:

```bash
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

If your CMake version does not support presets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Pull Request Expectations

- Keep runtime behavior testable where practical.
- Add or update tests for protocol, simulation, networking, and menu-flow changes.
- Keep the README and architecture notes aligned with user-visible behavior.
- Run the full CTest suite before opening a pull request.
- Keep the CI quality presets clean: `ci`, `dev-tidy`, and `dev-sanitize`.
- Avoid committing generated build output, local IDE files, private agent artifacts, or prompt/spec work files.

## Code Quality

The repository includes `.clang-format`, `.clang-tidy`, target-scoped compiler warnings, and sanitizer presets. The normal development preset keeps warnings visible without blocking local iteration. The CI preset treats compiler warnings as errors, the `dev-tidy` preset treats production-target `clang-tidy` diagnostics as errors, and the `dev-sanitize` preset runs tests with AddressSanitizer and UndefinedBehaviorSanitizer.

Recommended local pre-PR checks:

```bash
cmake --preset ci
cmake --build --preset ci --parallel
ctest --preset ci

cmake --preset dev-tidy
cmake --build --preset dev-tidy --parallel

cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize
```

## Reporting Issues

When reporting a bug, include:

- Your platform and compiler.
- The build command you used.
- Steps to reproduce the issue.
- Whether the failure occurs in Multiplayer, Lab Study, Level Editor, or Settings.
- Any relevant console output.

## Citation

If you use NetcodeSim in academic work, please cite it using the metadata in `CITATION.cff`.
