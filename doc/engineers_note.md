# Engineer's Notes

## Build System Architecture & Release Lifecycle

### 1. Mode Segregation (`build/debug/` vs `build/release/`)
- **Debug Mode (`MODE=debug`, Default)**:
  - Output: `build/debug/bin/engine`
  - Flags: `-O0 -g3 -DENGINE_DEBUG=1`
  - Version: Dynamically embeds Git branch and commit hash (`0.1.0-dev+<hash>[-dirty]`).
- **Release Mode (`MODE=release`)**:
  - Output: `build/release/bin/engine`
  - Flags: `-O3 -DNDEBUG -march=native -flto -DENGINE_RELEASE=1`
  - Linker: `-flto -Wl,--gc-sections`
  - Version: Clean semantic version (`0.1.0`).

### 2. Version Management & GitHub Releases
- Single source of truth is [VERSION](VERSION).
- Intentional release targets:
  - `make release-patch` (e.g., `0.1.0` $\rightarrow$ `0.1.1`)
  - `make release-minor` (e.g., `0.1.0` $\rightarrow$ `0.2.0`)
  - `make release-major` (e.g., `0.1.0` $\rightarrow$ `1.0.0`)
- Automatically updates `VERSION`, generates Git tag `vX.Y.Z`, and produces the release binary.

### 3. Git Isolation & `.gitignore`
- Dedicated repository initialized inside `projects/antigravity/engine/` on branch `main`.
- Clean `.gitignore` ignores `build/`, `.dependencies/`, object files, and IDE artifacts.
