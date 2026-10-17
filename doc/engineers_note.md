# Engineer's Notes

## Build System Architecture & Release Lifecycle

### 1. Mode Segregation (`build/debug/` vs `build/release/`)
- **Debug Mode (`MODE=debug`, Default)**:
  - Output: `build/debug/bin/engine`
  - Flags: `-O0 -g3 -DENGINE_DEBUG=1`
  - Version: `0.1.0-dev+<short-hash>[-dirty]`. The hash is `git rev-parse --short HEAD`. A dirty worktree appends `-dirty`. The branch name is not part of the string.
- **Release Mode (`MODE=release`)**:
  - Output: `build/release/bin/engine`
  - Flags: `-O3 -DNDEBUG -flto -DENGINE_RELEASE=1` (baseline x86-64; `NATIVE=1` or `ARCH=` opts into `-march`)
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

## Runtime Contract

- The loader and the selected device must support Vulkan 1.4.
- `ENGINE_GPU_INDEX` forces a probe index. `ENGINE_GPU` matches vendor, device type, or device name. An ineligible device is not selected.
- `maintenance5`, `maintenance6`, `pushDescriptor`, and `samplerAnisotropy` are enabled on the logical device when the physical device reports them. A true `supports_*` flag means that feature was turned on.
- Transfer prefers a pure transfer queue family, then another non-graphics family that can transfer, then the graphics family.
- A shared family gets one `VkQueue` per role when it has enough queues. `VulkanQueueInfo.submission_aliased` is set when two roles still share one queue. Those submits must not run at the same time.
- `vulkan_create_exportable_image()` takes the exact image usage. `VULKAN_EXPORT_TILING_AUTO` tries a single-plane DRM format modifier, then linear tiling, then optimal tiling. Optimal export does not carry a DRM modifier.
- `vulkan_create_exportable_image_with_modifiers()` intersects the physical device's single-plane DRM format modifiers with an explicit list and rejects a result that has no offered DRM modifier.
- Boot order is system, ring, Wayland, Vulkan, engine. Shutdown is the reverse and skips stages that did not initialize.
- A presented buffer carries two DRM syncobj timelines. The GPU signals the acquire point in the submit. The compositor signals the release point. The CPU sleeps in io_uring until that release eventfd is readable. The present path does not call `vkWaitForFences`.
