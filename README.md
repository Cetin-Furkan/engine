# Modular C23 Engine

A modern C23 Vulkan & Wayland engine with self-configuring build system, mode segregation, and dead-code elimination.

## Directory Structure

```
├── build/
│   ├── debug/            # Debug build (obj, deps, bin)
│   └── release/          # Release build (obj, deps, bin)
├── .dependencies/        # Persistent system requirements cache
├── doc/                  # Documentation & engineer notes
├── include/              # Public engine headers
├── scripts/              # Environment verification & version scripts
├── src/                  # Subsystem source implementations
├── .gitignore            # Git ignore specification
├── Makefile              # Self-configuring build script
├── main.c                # Engine entry point
├── README.md             # Project readme
└── VERSION               # Single source of truth for SemVer (e.g. 0.1.0)
```

## Quick Start

### Development & Debugging
```bash
# Build in debug mode (default, includes -g3 -O0 and local git metadata)
make

# Run debug binary
./build/debug/bin/engine
```

### Production Release
```bash
# Build a portable release binary (-O3 -flto -DNDEBUG, baseline x86-64)
make release

# Optional: tune for this CPU only. Do not copy that binary to other PCs.
# make release NATIVE=1

# Run release binary
./build/release/bin/engine
```

### Release Bumping for GitHub
```bash
# Bump patch (0.1.0 -> 0.1.1), creates git tag v0.1.1, and builds release
make release-patch

# Bump minor (0.1.0 -> 0.2.0), creates git tag v0.2.0, and builds release
make release-minor

# Bump major (0.1.0 -> 1.0.0), creates git tag v1.0.0, and builds release
make release-major
```

## Runtime

The binary requires Vulkan 1.4 on the loader and on the selected device. The instance is WSI-free. A frame leaves the GPU as a Linux DMA-BUF.

Boot order is system, Wayland, Vulkan, ring, then engine. A failed stage skips every stage after it. Shutdown runs in reverse, and only for stages that initialized.

```bash
# Pick a device by the probe index printed at startup
ENGINE_GPU_INDEX=0 ./build/debug/bin/engine

# Or by a case-insensitive ASCII substring of the vendor, device type, or device name
ENGINE_GPU=NVIDIA ./build/debug/bin/engine
```
