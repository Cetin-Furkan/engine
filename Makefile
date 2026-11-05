# Modular C23 Engine Makefile with Isolated Build & Mode Support

.DEFAULT_GOAL := all

DEPS_CACHE_DIR := .dependencies
CONFIG_MK      := $(DEPS_CACHE_DIR)/config.mk

# Automatically trigger environment check if config.mk is missing
-include $(CONFIG_MK)

$(CONFIG_MK): scripts/check_env.sh
	@./scripts/check_env.sh

# Fallbacks if config.mk has not been created yet
CC        ?= gcc
C23_FLAGS ?= -std=c23

# Versioning & Git Metadata
BASE_VERSION := $(shell cat VERSION 2>/dev/null || echo "0.1.0")
GIT_HASH     := $(shell git rev-parse --short HEAD 2>/dev/null || echo "initial")
GIT_DIRTY    := $(shell git diff --quiet 2>/dev/null || echo "-dirty")

# Build Mode: debug (default) or release
MODE ?= debug

# Baseline x86-64 unless ARCH= or NATIVE=1 is set.
ARCH_FLAG := -march=x86-64
ifneq ($(ARCH),)
    ARCH_FLAG := -march=$(ARCH)
else ifeq ($(NATIVE),1)
    ARCH_FLAG := -march=native
endif

ifeq ($(MODE),release)
    OPT_FLAGS    := -O3 -DNDEBUG $(ARCH_FLAG) -flto=auto -DENGINE_RELEASE=1
    FULL_VERSION := $(BASE_VERSION)
else
    OPT_FLAGS    := -O0 -g3 $(ARCH_FLAG) -DENGINE_DEBUG=1
    FULL_VERSION := $(BASE_VERSION)-dev+$(GIT_HASH)$(GIT_DIRTY)
endif

# Build Output Directories (segregated by mode)
BUILD_DIR := build/$(MODE)
OBJ_DIR   := $(BUILD_DIR)/obj
DEP_DIR   := $(BUILD_DIR)/deps
BIN_DIR   := $(BUILD_DIR)/bin
TARGET    := $(BIN_DIR)/engine

# Clean include paths and preprocessor flags
GEN_DIR  := $(BUILD_DIR)/gen
INCLUDES := -Iinclude -Isrc -Iexamples/demo/include -Iexamples/demo -I$(GEN_DIR)
ifneq ($(VULKAN_INCLUDE_DIR),)
    INCLUDES += -I$(VULKAN_INCLUDE_DIR)
endif

SLANGC ?= $(or $(SLANG_BIN),slangc)

$(GEN_DIR)/%.spv: shaders/%.slang
	@mkdir -p $(dir $@)
	@set -e; \
	case "$<" in \
		*.vert.slang) stage=vertex ;; \
		*.frag.slang) stage=fragment ;; \
		*.comp.slang) stage=compute ;; \
		*) echo "unknown shader stage: $<" >&2; exit 1 ;; \
	esac; \
	$(SLANGC) -target spirv -profile spirv_1_6 -stage "$$stage" -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/$*.d" -Ishaders -o "$@" "$<"

# plane.frag calls WaveRotate. The rotate profile keeps slangc from
# auto-upgrading spirv_1_6 and warning on that one module.
$(GEN_DIR)/plane.frag.spv: shaders/plane.frag.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6+spvGroupNonUniformRotateKHR -stage fragment -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/plane.frag.d" -Ishaders -o "$@" "$<"

$(GEN_DIR)/bevel.comp.spv: shaders/face_grid.slang
$(GEN_DIR)/heat.comp.spv: shaders/heat_nbr.slang shaders/face_grid.slang
$(GEN_DIR)/cube.vert.spv: shaders/camera.slang shaders/material.slang shaders/face_grid.slang
$(GEN_DIR)/cube.frag.spv: shaders/lighting.slang
$(GEN_DIR)/solid.vert.spv: shaders/camera.slang shaders/material.slang
$(GEN_DIR)/solid.frag.spv: shaders/lighting.slang
$(GEN_DIR)/shape.vert.spv: shaders/camera.slang shaders/material.slang
$(GEN_DIR)/plot.vert.spv: shaders/camera.slang
$(GEN_DIR)/gimbal.vert.spv: shaders/camera.slang
$(GEN_DIR)/mesh.vert.spv: shaders/camera.slang shaders/material.slang
$(GEN_DIR)/mesh.frag.spv: shaders/lighting.slang
$(GEN_DIR)/terrain_filter.comp.spv: shaders/terrain_math.slang
$(GEN_DIR)/terrain.vert.spv: shaders/camera.slang shaders/terrain_math.slang
$(GEN_DIR)/terrain.frag.spv: shaders/camera.slang shaders/terrain_math.slang shaders/lighting.slang

-include $(wildcard $(GEN_DIR)/*.d)

$(OBJ_DIR)/examples/demo/render/cube.o: $(GEN_DIR)/cube.vert.spv $(GEN_DIR)/cube.frag.spv $(GEN_DIR)/bevel.comp.spv $(GEN_DIR)/heat.comp.spv
$(OBJ_DIR)/examples/demo/render/solid.o: $(GEN_DIR)/solid.vert.spv $(GEN_DIR)/solid.frag.spv $(GEN_DIR)/solid.comp.spv
$(OBJ_DIR)/examples/demo/render/shape.o: $(GEN_DIR)/shape.vert.spv $(GEN_DIR)/shape.frag.spv
$(OBJ_DIR)/examples/demo/render/plane.o: $(GEN_DIR)/plane.vert.spv $(GEN_DIR)/plane.frag.spv
$(OBJ_DIR)/examples/demo/render/plot.o: $(GEN_DIR)/plot.vert.spv $(GEN_DIR)/plot.frag.spv $(GEN_DIR)/plot.comp.spv
$(OBJ_DIR)/examples/demo/render/gimbal.o: $(GEN_DIR)/gimbal.vert.spv $(GEN_DIR)/gimbal.frag.spv
$(OBJ_DIR)/examples/demo/render/mesh.o: $(GEN_DIR)/mesh.vert.spv $(GEN_DIR)/mesh.frag.spv
$(OBJ_DIR)/examples/demo/render/terrain.o: $(GEN_DIR)/terrain.vert.spv $(GEN_DIR)/terrain.frag.spv
$(OBJ_DIR)/examples/demo/render/hud.o: $(GEN_DIR)/hud.vert.spv $(GEN_DIR)/hud.frag.spv
$(OBJ_DIR)/examples/demo/render/grid.o: $(GEN_DIR)/grid.comp.spv

$(GEN_DIR)/text.vert.spv: examples/demo/text/draw/text.vert.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6 -stage vertex -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/text.vert.d" -Ishaders -o "$@" "$<"

$(GEN_DIR)/text.frag.spv: examples/demo/text/draw/text.frag.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6 -stage fragment -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/text.frag.d" -Ishaders -o "$@" "$<"

$(OBJ_DIR)/examples/demo/text/draw/draw.o: $(GEN_DIR)/text.vert.spv $(GEN_DIR)/text.frag.spv
$(OBJ_DIR)/examples/demo/text/face/face.o: examples/demo/text/face/bundled/InterVariable.ttf
$(OBJ_DIR)/src/chrome/gpu.o: $(GEN_DIR)/chrome.vert.spv $(GEN_DIR)/chrome.frag.spv
$(OBJ_DIR)/src/input/page/page.o: $(GEN_DIR)/input.comp.spv
$(OBJ_DIR)/src/backend/math/gpu/directory.o: $(GEN_DIR)/terrain_filter.comp.spv

# Compiler & Preprocessor Flags
CFLAGS ?= $(C23_FLAGS) -D_GNU_SOURCE -Wall -Wextra -Wunused-function -Werror=unused-function -Wmissing-declarations \
          $(OPT_FLAGS) $(INCLUDES) --embed-dir=$(GEN_DIR) \
          -ffunction-sections -fdata-sections \
          -DENGINE_VERSION_STR=\"$(FULL_VERSION)\" \
          -DENGINE_BUILD_MODE=\"$(MODE)\"

# Linker Flags
LDFLAGS ?= -Wl,--gc-sections
ifeq ($(MODE),release)
    LDFLAGS += -flto=auto
endif
ifneq ($(VULKAN_LIB),)
    LDFLAGS += -lvulkan
endif
LDFLAGS += -lm

# Text uses FreeType SDF, HarfBuzz, fontconfig, and libxkbcommon. The test
# target links the libraries itself; these flags are for the engine binary.
TEXT_PKGS   := freetype2 harfbuzz fontconfig xkbcommon
TEXT_CFLAGS := $(shell pkg-config --cflags $(TEXT_PKGS))
TEXT_LIBS   := $(shell pkg-config --libs $(TEXT_PKGS))
CFLAGS      += $(TEXT_CFLAGS)
LDFLAGS     += $(TEXT_LIBS)

SRCS := main.c $(shell find src examples/demo -name '*.c')
OBJS := $(SRCS:%.c=$(OBJ_DIR)/%.o)
DEPS := $(SRCS:%.c=$(DEP_DIR)/%.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	@if ! $(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) -Wl,--print-gc-sections > $(BUILD_DIR)/link.log 2>&1; then \
		cat $(BUILD_DIR)/link.log >&2; \
		exit 1; \
	fi
	@dead=$$(grep -E "removing unused section '\.text\.[^']*' in file '.*(src/vulkan|src/frame|src/ring|src/wayland|src/input|src/chrome|src/system|src/backend)" $(BUILD_DIR)/link.log || true); \
	if [ -n "$$dead" ]; then \
		echo -e "\033[0;31m[Link Error] Dead/hallucinated functions detected:\033[0m" >&2; \
		echo "$$dead" >&2; rm -f $@; exit 1; \
	fi
ifneq ($(NATIVE),1)
	objcopy --remove-section .note.gnu.property $@
endif

$(OBJ_DIR)/%.o: %.c $(CONFIG_MK) VERSION
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

-include $(DEPS)

debug:
	@$(MAKE) MODE=debug all

release:
	@$(MAKE) MODE=release all

run: debug
	@echo -e "\033[0;32m==> Running Debug Engine...\033[0m"
	@./build/debug/bin/engine

run-release: release
	@echo -e "\033[0;32m==> Running Release Engine...\033[0m"
	@./build/release/bin/engine

release-patch:
	@./scripts/bump_version.sh patch
	@$(MAKE) MODE=release all

release-minor:
	@./scripts/bump_version.sh minor
	@$(MAKE) MODE=release all

release-major:
	@./scripts/bump_version.sh major
	@$(MAKE) MODE=release all

info: $(CONFIG_MK)
	@echo -e "\033[0;34m=== Engine Build Configuration ===\033[0m"
	@echo "  Target Binary:     $(TARGET)"
	@echo "  Active Mode:       $(MODE)"
	@echo "  Base SemVer:       $(BASE_VERSION)"
	@echo "  Full Version:      $(FULL_VERSION)"
	@echo "  Compiler:          $(CC) ($(C23_FLAGS))"
	@echo "  Kernel Version:    $(KERNEL_VERSION)"
	@echo "  Vulkan Header:     $(VULKAN_HEADER) (v$(VULKAN_VERSION))"
	@echo "  Vulkan Library:    $(VULKAN_LIB)"
	@echo "  Wayland Protocols: $(WAYLAND_PROTOCOLS_VERSION) ($(WAYLAND_PROTOCOLS_DIR))"
	@echo "  Shader Compiler:   $(SLANGC) (Native Slang 2026 / SPIR-V 1.6)"
	@echo -e "\033[0;34m==================================\033[0m"

help:
	@echo -e "\033[0;36mModular C23 Engine - Build Commands:\033[0m"
	@echo ""
	@echo -e "  \033[1mBuild & Run:\033[0m"
	@echo "    make                         Build debug binary (-O0 -g3, default)"
	@echo "    make debug                   Explicit alias for make"
	@echo "    make release                 Portable release binary (-O3 -flto=auto -DNDEBUG, baseline x86-64)"
	@echo "    make release NATIVE=1        Same, with -march=native (do not copy this binary to other PCs)"
	@echo "    make release ARCH=x86-64-v3  Same, with an explicit -march"
	@echo "    make run                     Build and execute debug binary"
	@echo "    make run-release             Build and execute release binary"
	@echo ""
	@echo -e "  \033[1mCleanup & Cache:\033[0m"
	@echo "    make clean                   Remove build/ directory (preserves prerequisite cache)"
	@echo "    make clear                   Alias for make clean"
	@echo "    make recheck                 Re-run environment probing & refresh .dependencies/"
	@echo "    make distclean               Remove build/ AND .dependencies/ cache"
	@echo ""
	@echo -e "  \033[1mRelease Bumping (GitHub):\033[0m"
	@echo "    make release-patch           Increment patch (0.0.1), tag git, and compile release"
	@echo "    make release-minor           Increment minor (0.1.0), tag git, and compile release"
	@echo "    make release-major           Increment major (1.0.0), tag git, and compile release"
	@echo ""
	@echo -e "  \033[1mDiagnostics:\033[0m"
	@echo "    make info                    Print current build settings, paths, and toolchains"
	@echo "    make help                    Print this guide"

clean:
	@echo "Cleaning build outputs..."
	@rm -rf build
	@echo "Build directory cleared."

clear: clean

recheck:
	rm -rf $(DEPS_CACHE_DIR)
	@$(MAKE) $(CONFIG_MK)

distclean: clean
	rm -rf $(DEPS_CACHE_DIR)

test: $(CONFIG_MK)
	$(CC) $(CFLAGS) -o build/test_handles tests/test_handles.c
	./build/test_handles
	$(CC) $(CFLAGS) -o build/test_clock tests/test_clock.c src/frame/clock/clock.c
	./build/test_clock
	$(CC) $(CFLAGS) -o build/test_world tests/test_world.c examples/demo/world/table.c examples/demo/world/spin.c -lm
	./build/test_world
	$(CC) $(CFLAGS) -o build/test_motion tests/test_motion.c examples/demo/world/motion.c examples/demo/world/fillet.c -lm
	./build/test_motion
	$(CC) $(CFLAGS) -o build/test_policy tests/test_policy.c examples/demo/world/policy.c
	./build/test_policy
	$(CC) $(CFLAGS) -o build/test_sheet tests/test_sheet.c examples/demo/world/sheet.c -lm
	./build/test_sheet
	$(CC) $(CFLAGS) -o build/test_text tests/test_text.c \
		examples/demo/text/face/face.c examples/demo/text/shape/shape.c $(TEXT_LIBS) -lm
	./build/test_text
	$(CC) $(CFLAGS) -o build/test_bda tests/test_bda.c -lvulkan
	./build/test_bda
	$(CC) $(CFLAGS) -o build/test_backend tests/test_backend.c \
		src/backend/math/pointer/relptr2d.c \
		src/backend/core/cell/dispatch.c \
		src/backend/terrain/storage/synth_besparmak.c \
		src/backend/terrain/storage/pack_io.c \
		src/backend/terrain/stream/lod_select.c -lm
	./build/test_backend

.PHONY: all debug release run run-release release-patch release-minor release-major info help clean clear recheck distclean test