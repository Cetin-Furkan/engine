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
# Build optimized release binary (-O3 -flto -march=native -DNDEBUG)
make release

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
