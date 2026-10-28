# Modular C23 Engine

One C23 process. `src/` is the platform: a hand-written Wayland client, its own `io_uring`, and a Vulkan 1.4 device that presents a Linux DMA-BUF. `examples/demo/` is the program that process hosts today.

## Why

The client does not use SDL, GLFW, `libwayland`, `liburing`, or `VK_KHR_swapchain`. It writes the Wayland wire protocol on one fixed socket, submits I/O on a ring it set up itself, and hands the compositor a DMA-BUF whose acquire and release points are DRM syncobj timelines.

A `wl_surface.frame` callback stays armed while the window is up. Each stamp steps a fixed 8 ms clock, at most five steps. The engine records a new DMA-BUF when the picture changed, on the first stamp, or while a compute pulse still has work. When the demo goes idle the callback chain stays up, and the GPU is not submitted again until input or the simulation changes the picture. Between those events the thread waits in `io_uring`. The loop leaves on the toplevel close, or on `SIGINT`, `SIGTERM`, or `SIGHUP`. If the compositor stops sending stamps, an absolute timeout re-arms the callback.

## What the demo does

`examples/demo` is the hosted program. Boot reads `examples/demo/world/opening.world` when that file is present. This tree places three fillets on the metric grid, at `(100, 0, 0)`, `(1000, 0, 0)`, and `(-1000, 0, 0)`. A missing file boots the grid with no objects. `k_startup_objects[]` in `examples/demo/world/load.c` is empty.

Coordinates are world pixels. `+X` is right, `+Y` is up, `+Z` comes out of the ground. The ground is the plane `z = 0`, and it is on from boot. It is not a line in the world file.

`Tab` switches the live workspace between `Blender-3D` and `Matplotlib-2D`. Both profiles ship unsealed, so the switch works. The 3D view stores yaw and pitch as `Ry(yaw) * Rx(-pitch)`. The flat profile locks the identity quaternion and `z = 0`, and it does not grant yaw, pitch, or object spawn.

The opening fillets run one revolution on the compositor clock, 5000 ms, then hold. A click on a cube plants heat. That field steps for 5000 ms and then holds. After that the loop prints `Loop idle` and stops recording until something changes.

Keys, on the 3D profile:

- Arrows pan on the screen axes. `Q` and `E` yaw. `C` and `V` pitch. The wheel and the touchpad zoom. `=` , numpad plus, and shift+`4` zoom in. `-` and numpad minus zoom out. Zoom runs from `0.0001` to `10000`. At zoom 1, one world pixel is one screen pixel.
- Numpad `8`, `2`, `4`, `6`, and `5` snap to top, bottom, left, right, and front. Numpad `5` again, from the front, sets the back. `Home` or numpad `1` restores the opening pose, including position and zoom. Numpad `7` shows or hides the view ball.
- Numpad `9` grants and revokes `PERM_DRAW`. It is off in both compiled profiles, so boot places no corners. With it on, a click on empty ground places a point. `F` closes that list into a face. A crossing outline is refused. A valid outline is filled by an ear clip. `Esc` clears the selection. Backspace, Delete, or `X` removes the selection.
- `1` through `6` raise a tetrahedron, an octahedron, an icosahedron, a cylinder, a cone, and a torus. `4` with shift up is the cylinder. `S` toggles faceted and smooth edges. `G` toggles glow. `R` toggles a continuous spin. `F5` runs one revolution again. A click selects. A click on a cube also plants heat.
- `M` cycles the math curve through sin, sinc, gaussian, and off. `L` reloads `plots/data.csv` and `plots/mesh.obj`.
- Hold `T`, click, click again, then type. `Esc` leaves the box. Glyphs are HarfBuzz outlines, rasterized by FreeType as a signed-distance field into an append-only R8 device-address atlas. The corner mark is the embedded Inter face. A `dynamic.faces` line cannot retarget that face.

The cube samples a 128³ fp16 lattice. `shaders/grid.comp` steps that field. `shaders/heat.comp` steps the per-vertex fillet heat.

## How it works

**Ring and wire.** `src/ring/ring.c` registers the ring with `SINGLE_ISSUER`, `DEFER_TASKRUN`, and `NO_MMAP`, plus a registered wait and a provided-buffer ring. The bring-up log on this machine is SQ 64, CQ 256, 32 buffers of 8192 bytes. `src/wayland/wayland.c` encodes and parses the wire itself. It asks the surface for a DMA-BUF tranche and binds `wp_linux_drm_syncobj_v1` acquire and release timelines. The color image is `DRM_FORMAT_ARGB8888`. The modifier is the one the compositor's tranche and the device agree on.

**Clock and handles.** `FrameClock` in `include/frame/clock.h` is 64 bytes: the last stamp, a fixed-point refresh period, missed refreshes, stall count, and the fixed-step remainder. The instance in the frame state is `alignas(64)`. A presented slot is named by `(generation << 8) | (index + 1)`. Generation 0 never resolves, and blanking the slot bumps the generation so the old handle fails the match. The world table stores the generation it selected. A render-target entry stores its own, so a copied `EngineTargets` cannot pick up a recycled image.

**Device addresses.** Shaders that carry bulk data read it through `GL_EXT_buffer_reference` and push constants. `vulkan_cmd_push` is `vkCmdPushConstants2`. Geometry, heat, materials, plot samples, mesh corners, and text quads each have their own `DaBuffer`. The volume lattice is the one `BdaArena`. Host buffers ask for device-local, host-visible, coherent memory. There is no descriptor set in the tree.

Shape, plot, mesh, and text each keep 12 slices, enough for both frame slots and the orphan ring. A slice is claimed only after `vkGetSemaphoreCounterValue` shows its acquire point has retired. A reload writes the new slice and does not call `vkQueueWaitIdle`. The cube mesh bake, the solid bake, the lattice seed, and the plot math bake do wait once, at init, before the first frame.

**One pass.** `src/vulkan/pipe.c` builds graphics pipelines from SPIR-V chained in as `VkShaderModuleCreateInfo` under maintenance5. The cache file is magic `AGPC`, then vendor, device, and `pipelineCacheUUID`. The path is `$XDG_CACHE_HOME/antigravity/engine.pipeline`, else `$HOME/.cache/antigravity/engine.pipeline`, else `/tmp/antigravity-engine.pipeline`. A mismatch starts empty.

`src/frame/present/record.c` puts color, coverage, and depth into one `vkCmdPipelineBarrier2`, sets the viewport and scissor once, and draws the world, the window chrome, the HUD, and the text in one dynamic rendering pass. The resolve is the end of that pass. Sample shading is off, so one fragment covers every sample. The pass sample count is the highest of 8, 4, 2, and 1 that both the color format and the depth format support. Depth prefers `D32_SFLOAT`, then `D24_UNORM_S8_UINT`, then `D16_UNORM`.

Depth and coverage images are created transient when the device allows it, and as ordinary depth or color attachments when it does not. Their memory comes from a pool of 16 slots, each rounded up to 16 MiB. The pool asks for lazily allocated device-local memory first, then device-local, then any matching type. The presented color image is not in that pool. It is an exportable DMA-BUF.

**Ground.** The grid is six vertices on `z = 0`. It tests depth with `VK_COMPARE_OP_LESS_OR_EQUAL` and does not write depth, so geometry on `z = 0` is not sliced by its own ground. `span = span_steps * coarse + span_mod` rebuilds the grid phase from the camera, so a pan of several thousand world units is not added to a quad corner in float32. `pixel_footprint` in `shaders/plane.vert` inverts the 2×2 of screen-per-world partials taken from the quaternion. Line width stays in screen pixels across the zoom range.

## How to build on it

`include/engine/app.h` is the seam. `src/` does not name a cube, a face, a plot, a font, or a key action. It calls `engine_app()`.

What is in the tree today is `examples/demo`: the two workspaces, the fillets, the 128³ fp16 lattice, the six solids, ear-clipped faces, the plot, and the SDF text. That program was brought up on Intel Iris Xe (TGL GT2), Mesa 26.2.4, Vulkan 1.4.354, one queue family shared by graphics, compute, and transfer.

The same seam is where a different program goes. Replacing `examples/demo` keeps the ring, the wire protocol, and the DMA-BUF present path. Draw and compute are Vulkan 1.4, not a vendor compute API. A plotter, a modeling viewport, a game, or a live compute view is another `EngineApp` on that path. An AMD or NVIDIA driver is that same path when it passes the startup probe. This tree has not been run on those devices. The probe prints the device it accepted and names the feature or extension it refused.

Three places to change the demo without replacing it:

**Policy.** `WorkspacePolicy` in `examples/demo/include/frame/policy.h` is a name, a permission mask, `sealed`, `lock_2d_quat`, and up to 48 key binds. An action runs only when its bit is set. `sealed` makes `policy_grant`, `policy_revoke`, `policy_rebind_key`, and `policy_switch` fail. `lock_2d_quat` holds the identity quaternion and `z = 0`. The compiled profiles live in `examples/demo/world/policy.c`. Shift matches exactly: shift+`4` is zoom, and `4` alone is the cylinder.

**Objects.** A kind is a `WorldOps` table in `examples/demo/world/world_int.h`: `hit`, `on_click`, `draw`, `advance`, `commit`, `record_compute`, `simulate`, and the record outcome calls. Kinds register with `table_kind_listen` from a `[[gnu::constructor]]`. `k_startup_objects[]` places C objects at boot. `opening.world` places `ground`, `cube`, and `mesh` lines. The table holds 48 objects. `alive` is the picture being built. `shown_alive` is the picture on screen.

**A new program.** Implement `const EngineApp *engine_app(void)` and stop compiling `examples/demo`. The hooks that record a frame, in order, are `record_compute`, `claim_frame`, `prepare_draw`, `draw`, and `draw_overlay`. `claim_frame` runs even when the pass has no depth. `release_frame` runs only when the submit does not. `click`, `scroll`, `text`, and `key` are the input hooks. `route_key` returns ignore, hold, or tap. The engine does not know which.

## Rules

The language is C23 (`-std=c23`). The code uses `#embed`, `nullptr`, `alignas`, and `static_assert`. The platform does not link `libwayland` or `liburing`, and it does not create a swapchain. The binary does link `libvulkan`, `libm`, FreeType, HarfBuzz, fontconfig, and libxkbcommon.

Hooks run on the one `io_uring` thread and return. A plot reload and a chrome-spec reload go through `ring_read_file_all`. `opening.world` is read with `fopen` at boot. A runtime edit of shape, plot, mesh, or text claims a retired slice. It does not call `vkQueueWaitIdle`. The init bakes do.

The camera, the spin, the object table, the text runs, and the view ball keep a shown copy of what is already on screen. A resize draws that copy. It does not advance the clock.

The embedded Inter face and a `WorkspacePolicy` with `sealed` set ignore later retargeting. Both shipped profiles are unsealed. These files are meant to be edited: `chrome/window.chrome`, `examples/demo/world/opening.world`, `examples/demo/text/face/dynamic.faces`, `examples/demo/plots/data.csv`, and `examples/demo/plots/mesh.obj`. `ENGINE_CHROME` overrides the chrome spec path.

## Build and run

`scripts/check_env.sh` requires Linux 7.2 or newer, a compiler that accepts `-std=c23`, and a Vulkan header that defines `VK_API_VERSION_1_4`. The shader build uses `glslangValidator` for buffer-reference shaders and `slangc` for four others. The link also needs `freetype2`, `harfbuzz`, `fontconfig`, and `xkbcommon`. A Wayland session has to be running. `wayland-protocols` is not used.

```bash
make
./build/debug/bin/engine

make release
./build/release/bin/engine

make test
```

`make release` is `-O3 -flto -DNDEBUG` for baseline x86-64. `make release NATIVE=1` tunes for this CPU. Do not copy that binary to another machine.

```bash
make release-patch    # 0.1.0 -> 0.1.1, tag v0.1.1
make release-minor
make release-major
```

Boot order is system, ring, Wayland, Vulkan, then engine. A failed stage skips every stage after it. Shutdown runs in reverse, and only for stages that came up.

```bash
ENGINE_GPU_INDEX=0 ./build/release/bin/engine
ENGINE_GPU=Intel ./build/release/bin/engine
```

`ENGINE_GPU_INDEX` is the probe index printed at startup. `ENGINE_GPU` is a case-insensitive substring of the vendor, the device type, or the device name. Either one is a filter. It is not a portability test.

## Layout

```
include/          public headers. engine/app.h is the seam.
src/              ring, wayland, vulkan, chrome, present. No demo types.
examples/demo/    the hosted workspace, its shaders' C, and its data.
shaders/          GLSL, compiled to SPIR-V at build time.
chrome/           window.chrome, the chrome spec.
tests/            handles, clock, world, policy, text.
scripts/          check_env.sh and the version bump.
Makefile
VERSION           0.1.0
```
