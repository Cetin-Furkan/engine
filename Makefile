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
AR        ?= $(if $(findstring gcc,$(CC)),gcc-ar,ar)
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
BUILD_DIR      ?= build/$(MODE)
OBJ_DIR        := $(BUILD_DIR)/obj
DEP_DIR        := $(BUILD_DIR)/deps
BIN_DIR        := $(BUILD_DIR)/bin
LIB_ENGINE     := $(BUILD_DIR)/libengine.a
TARGET         := $(BIN_DIR)/engine
MINIMAL_TARGET := $(BIN_DIR)/minimal

# Clean include paths and preprocessor flags (demo headers are scoped to examples/demo)
GEN_DIR         := $(BUILD_DIR)/gen
MINIMAL_GEN_DIR := $(BUILD_DIR)/gen_minimal
INCLUDES        := -Iinclude -Isrc
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
$(GEN_DIR)/plane.frag.spv: shaders/plane.frag.slang shaders/camera.slang shaders/lighting.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6+spvGroupNonUniformRotateKHR -stage fragment -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/plane.frag.d" -Ishaders -o "$@" "$<"

# scene_cull.comp and scene_hiz.comp use Vulkan 1.4 subgroup ballot, arithmetic, and lane shuffles.
$(GEN_DIR)/scene_cull.comp.spv: shaders/scene_cull.comp.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6+spvGroupNonUniform+spvGroupNonUniformBallot+spvGroupNonUniformArithmetic -stage compute -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/scene_cull.comp.d" -Ishaders -o "$@" "$<"

$(GEN_DIR)/scene_hiz.comp.spv: shaders/scene_hiz.comp.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6+spvGroupNonUniform+spvGroupNonUniformShuffle -stage compute -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/scene_hiz.comp.d" -Ishaders -o "$@" "$<"

$(GEN_DIR)/survey_2dgs_cull.comp.spv: shaders/survey_2dgs_cull.comp.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6+spvGroupNonUniform+spvGroupNonUniformBallot+spvGroupNonUniformArithmetic -stage compute -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/survey_2dgs_cull.comp.d" -Ishaders -o "$@" "$<"

$(GEN_DIR)/bevel.comp.spv: shaders/face_grid.slang
$(GEN_DIR)/heat.comp.spv: shaders/heat_nbr.slang shaders/face_grid.slang
$(GEN_DIR)/cube.vert.spv: shaders/camera.slang shaders/material.slang shaders/face_grid.slang
$(GEN_DIR)/cube.frag.spv: shaders/lighting.slang
$(GEN_DIR)/shape.vert.spv: shaders/camera.slang shaders/material.slang
$(GEN_DIR)/plot.vert.spv: shaders/camera.slang
$(GEN_DIR)/gimbal.vert.spv: shaders/camera.slang
$(GEN_DIR)/terrain_filter.comp.spv: shaders/terrain_math.slang
$(GEN_DIR)/terrain_cull.comp.spv: shaders/camera.slang shaders/terrain_math.slang
$(GEN_DIR)/terrain.vert.spv: shaders/camera.slang shaders/terrain_math.slang
$(GEN_DIR)/terrain.frag.spv: shaders/camera.slang shaders/terrain_math.slang shaders/lighting.slang
$(GEN_DIR)/scene_cull.comp.spv: shaders/scene_common.slang shaders/camera.slang shaders/material.slang
$(GEN_DIR)/scene_hiz.comp.spv: shaders/scene_common.slang shaders/camera.slang shaders/material.slang
$(GEN_DIR)/scene_visbuf.vert.spv: shaders/scene_common.slang shaders/camera.slang shaders/material.slang
$(GEN_DIR)/scene_visbuf.frag.spv: shaders/scene_common.slang shaders/camera.slang shaders/material.slang
$(GEN_DIR)/scene_visbuf_resolve.frag.spv: shaders/scene_common.slang shaders/camera.slang shaders/material.slang shaders/lighting.slang
$(GEN_DIR)/scene_meshlet.vert.spv: shaders/scene_common.slang shaders/camera.slang shaders/material.slang
$(GEN_DIR)/scene_meshlet.frag.spv: shaders/lighting.slang
$(GEN_DIR)/lighting_gbuf.comp.spv: shaders/lighting_gi_common.slang shaders/camera.slang shaders/terrain_math.slang shaders/lighting.slang
$(GEN_DIR)/lighting_hrc.comp.spv: shaders/lighting_gi_common.slang shaders/camera.slang shaders/terrain_math.slang shaders/lighting.slang
$(GEN_DIR)/lighting_ssilvb.comp.spv: shaders/lighting_gi_common.slang shaders/camera.slang shaders/terrain_math.slang shaders/lighting.slang
$(GEN_DIR)/survey_2dgs_cull.comp.spv: shaders/camera.slang
$(GEN_DIR)/survey_2dgs.vert.spv: shaders/camera.slang
$(GEN_DIR)/survey_2dgs.frag.spv: shaders/camera.slang shaders/terrain_math.slang shaders/lighting.slang

-include $(wildcard $(GEN_DIR)/*.d)

$(OBJ_DIR)/examples/demo/render/cube.o: $(GEN_DIR)/cube.vert.spv $(GEN_DIR)/cube.frag.spv $(GEN_DIR)/bevel.comp.spv $(GEN_DIR)/heat.comp.spv
$(OBJ_DIR)/examples/demo/render/solid.o: $(GEN_DIR)/solid.comp.spv
$(OBJ_DIR)/examples/demo/render/shape.o: $(GEN_DIR)/shape.vert.spv $(GEN_DIR)/shape.frag.spv
$(OBJ_DIR)/examples/demo/render/plane.o: $(GEN_DIR)/plane.vert.spv $(GEN_DIR)/plane.frag.spv
$(OBJ_DIR)/examples/demo/render/plot.o: $(GEN_DIR)/plot.vert.spv $(GEN_DIR)/plot.frag.spv $(GEN_DIR)/plot.comp.spv
$(OBJ_DIR)/examples/demo/render/gimbal.o: $(GEN_DIR)/gimbal.vert.spv $(GEN_DIR)/gimbal.frag.spv
$(OBJ_DIR)/examples/demo/render/terrain.o: $(GEN_DIR)/terrain.vert.spv $(GEN_DIR)/terrain.frag.spv
$(OBJ_DIR)/examples/demo/render/hud.o: $(GEN_DIR)/hud.vert.spv $(GEN_DIR)/hud.frag.spv
$(OBJ_DIR)/examples/demo/render/grid.o: $(GEN_DIR)/grid.comp.spv

$(GEN_DIR)/text.vert.spv: examples/demo/text/draw/text.vert.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6 -stage vertex -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/text.vert.d" -Ishaders -o "$@" "$<"

$(GEN_DIR)/text.frag.spv: examples/demo/text/draw/text.frag.slang
	@mkdir -p $(dir $@)
	$(SLANGC) -target spirv -profile spirv_1_6 -stage fragment -entry main -fvk-use-entrypoint-name -depfile "$(GEN_DIR)/text.frag.d" -Ishaders -o "$@" "$<"

$(MINIMAL_GEN_DIR)/%.spv: examples/minimal/%.slang
	@mkdir -p $(dir $@)
	@set -e; \
	case "$<" in \
		*.vert.slang) stage=vertex ;; \
		*.frag.slang) stage=fragment ;; \
		*.comp.slang) stage=compute ;; \
		*) echo "unknown shader stage: $<" >&2; exit 1 ;; \
	esac; \
	$(SLANGC) -target spirv -profile spirv_1_6 -stage "$$stage" -entry main -fvk-use-entrypoint-name -depfile "$(MINIMAL_GEN_DIR)/$*.d" -o "$@" "$<"

-include $(wildcard $(MINIMAL_GEN_DIR)/*.d)

$(OBJ_DIR)/examples/demo/text/draw/draw.o: $(GEN_DIR)/text.vert.spv $(GEN_DIR)/text.frag.spv
$(OBJ_DIR)/examples/demo/text/face/face.o: examples/demo/text/face/bundled/InterVariable.ttf
$(OBJ_DIR)/examples/minimal/minimal.o: $(MINIMAL_GEN_DIR)/minimal.vert.spv $(MINIMAL_GEN_DIR)/minimal.frag.spv
$(OBJ_DIR)/src/chrome/gpu.o: $(GEN_DIR)/chrome.vert.spv $(GEN_DIR)/chrome.frag.spv
$(OBJ_DIR)/src/input/page/page.o: $(GEN_DIR)/input.comp.spv
$(OBJ_DIR)/src/backend/math/gpu/directory.o: $(GEN_DIR)/terrain_filter.comp.spv $(GEN_DIR)/terrain_cull.comp.spv
$(OBJ_DIR)/src/backend/terrain/survey/gpu_survey.o: $(GEN_DIR)/survey_mvs.comp.spv $(GEN_DIR)/survey_2dgs_cull.comp.spv $(GEN_DIR)/survey_2dgs.vert.spv $(GEN_DIR)/survey_2dgs.frag.spv
$(OBJ_DIR)/src/scene/gpu/scene_gpu.o: $(GEN_DIR)/scene_cull.comp.spv $(GEN_DIR)/scene_hiz.comp.spv $(GEN_DIR)/scene_visbuf.vert.spv $(GEN_DIR)/scene_visbuf.frag.spv $(GEN_DIR)/scene_visbuf_resolve.vert.spv $(GEN_DIR)/scene_visbuf_resolve.frag.spv $(GEN_DIR)/scene_meshlet.vert.spv $(GEN_DIR)/scene_meshlet.frag.spv
$(OBJ_DIR)/src/lighting/gpu/lighting_gpu.o: $(GEN_DIR)/lighting_gbuf.comp.spv $(GEN_DIR)/lighting_hrc.comp.spv $(GEN_DIR)/lighting_ssilvb.comp.spv
$(OBJ_DIR)/src/physics/gpu/avbd_gpu.o: $(GEN_DIR)/physics_avbd.comp.spv

# Scoped Platform vs Hosted-Program Dependencies
XKB_CFLAGS    := $(shell pkg-config --cflags xkbcommon)
XKB_LIBS      := $(shell pkg-config --libs xkbcommon)
PNG_CFLAGS    := $(shell pkg-config --cflags libpng)
DEMO_PKGS     := freetype2 harfbuzz fontconfig libpng
DEMO_INCLUDES := -Iexamples/demo/include -Iexamples/demo
DEMO_CFLAGS   := $(DEMO_INCLUDES) $(shell pkg-config --cflags $(DEMO_PKGS))
DEMO_LIBS     := $(shell pkg-config --libs $(DEMO_PKGS))
TEXT_LIBS     := $(DEMO_LIBS) $(XKB_LIBS)

EMBED_DIR    := $(GEN_DIR)
EXTRA_CFLAGS :=

# Compiler & Preprocessor Flags
CFLAGS ?= $(C23_FLAGS) -D_GNU_SOURCE -Wall -Wextra -Wunused-function -Werror=unused-function -Wmissing-declarations \
          $(OPT_FLAGS) $(INCLUDES) $(XKB_CFLAGS) \
          -ffunction-sections -fdata-sections \
          -DENGINE_VERSION_STR=\"$(FULL_VERSION)\" \
          -DENGINE_BUILD_MODE=\"$(MODE)\"

$(OBJ_DIR)/src/backend/%.o: EXTRA_CFLAGS := $(PNG_CFLAGS)
$(OBJ_DIR)/examples/demo/%.o: EXTRA_CFLAGS := $(DEMO_CFLAGS)
$(OBJ_DIR)/examples/minimal/%.o: EMBED_DIR := $(MINIMAL_GEN_DIR)

# Linker Flags
LDFLAGS ?= -Wl,--gc-sections
ifeq ($(MODE),release)
    LDFLAGS += -flto=auto
endif

PLATFORM_LIBS :=
ifneq ($(VULKAN_LIB),)
    PLATFORM_LIBS += -lvulkan
else
    PLATFORM_LIBS += -lvulkan
endif
PLATFORM_LIBS += $(XKB_LIBS) -lm

MAIN_SRC     := main.c
ENGINE_SRCS  := $(shell find src -name '*.c')
DEMO_SRCS    := $(shell find examples/demo -name '*.c')
MINIMAL_SRCS := $(shell find examples/minimal -name '*.c')

MAIN_OBJ     := $(MAIN_SRC:%.c=$(OBJ_DIR)/%.o)
ENGINE_OBJS  := $(ENGINE_SRCS:%.c=$(OBJ_DIR)/%.o)
DEMO_OBJS    := $(DEMO_SRCS:%.c=$(OBJ_DIR)/%.o)
MINIMAL_OBJS := $(MINIMAL_SRCS:%.c=$(OBJ_DIR)/%.o)
OBJS         := $(MAIN_OBJ) $(ENGINE_OBJS) $(DEMO_OBJS)
ALL_SRCS     := $(MAIN_SRC) $(ENGINE_SRCS) $(DEMO_SRCS) $(MINIMAL_SRCS)
DEPS         := $(ALL_SRCS:%.c=$(DEP_DIR)/%.d)

all: $(TARGET) $(MINIMAL_TARGET)

minimal: $(MINIMAL_TARGET)

$(LIB_ENGINE): $(ENGINE_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(ENGINE_OBJS)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	@if ! $(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(PLATFORM_LIBS) $(DEMO_LIBS) -Wl,--print-gc-sections > $(BUILD_DIR)/link.log 2>&1; then \
		cat $(BUILD_DIR)/link.log >&2; \
		exit 1; \
	fi
	@dead=$$(grep -E "removing unused section '\.text\.[^']*' in file '.*(src/vulkan|src/frame|src/ring|src/wayland|src/input|src/chrome|src/system|src/backend|src/scene|src/lighting|src/physics)" $(BUILD_DIR)/link.log || true); \
	if [ -n "$$dead" ]; then \
		echo -e "\033[0;31m[Link Error] Dead/hallucinated functions detected:\033[0m" >&2; \
		echo "$$dead" >&2; rm -f $@; exit 1; \
	fi
ifneq ($(NATIVE),1)
	objcopy --remove-section .note.gnu.property $@
endif

$(MINIMAL_TARGET): $(MAIN_OBJ) $(MINIMAL_OBJS) $(LIB_ENGINE)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(MAIN_OBJ) $(MINIMAL_OBJS) $(LIB_ENGINE) $(LDFLAGS) $(PLATFORM_LIBS)
ifneq ($(NATIVE),1)
	objcopy --remove-section .note.gnu.property $@
endif

$(OBJ_DIR)/%.o: %.c $(CONFIG_MK) VERSION
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) --embed-dir=$(EMBED_DIR) -I$(EMBED_DIR) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

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

run-minimal: debug
	@echo -e "\033[0;32m==> Running Debug Minimal App...\033[0m"
	@./build/debug/bin/minimal

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
	@echo "    make minimal                 Build standalone minimal EngineApp binary against libengine.a"
	@echo "    make run                     Build and execute debug binary"
	@echo "    make run-minimal             Build and execute debug minimal binary"
	@echo "    make run-release             Build and execute release binary"
	@echo "    make test                    Build and run all 16 automated unit, failure, and truth tests"
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
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/test_handles tests/test_handles.c
	./build/test_handles
	$(CC) $(CFLAGS) -o build/test_clock tests/test_clock.c src/frame/clock/clock.c
	./build/test_clock
	$(CC) $(CFLAGS) $(DEMO_CFLAGS) -o build/test_world tests/test_world.c examples/demo/world/table.c examples/demo/world/spin.c -lm
	./build/test_world
	$(CC) $(CFLAGS) $(DEMO_CFLAGS) -o build/test_motion tests/test_motion.c examples/demo/world/motion.c examples/demo/world/fillet.c src/physics/math/avbd_math.c -lm
	./build/test_motion
	$(CC) $(CFLAGS) $(DEMO_CFLAGS) -o build/test_policy tests/test_policy.c examples/demo/world/policy.c
	./build/test_policy
	$(CC) $(CFLAGS) $(DEMO_CFLAGS) -o build/test_sheet tests/test_sheet.c examples/demo/world/sheet.c src/physics/math/avbd_math.c -lm
	./build/test_sheet
	$(CC) $(CFLAGS) $(DEMO_CFLAGS) -o build/test_text tests/test_text.c \
		examples/demo/text/face/face.c examples/demo/text/shape/shape.c \
		examples/demo/text/place/place.c $(TEXT_LIBS) -lm
	./build/test_text
	$(CC) $(CFLAGS) -o build/test_bda tests/test_bda.c -lvulkan
	./build/test_bda
	$(CC) $(CFLAGS) $(PNG_CFLAGS) -o build/test_backend tests/test_backend.c \
		$(shell find src/ring src/system -name '*.c') \
		src/backend/math/pointer/relptr2d.c \
		src/backend/core/cell/dispatch.c \
		src/backend/terrain/storage/synth_besparmak.c \
		src/backend/terrain/storage/pack_io.c \
		src/backend/terrain/stream/lod_select.c \
		src/backend/terrain/survey/camera_model.c \
		src/backend/terrain/survey/mvs_fusion.c \
		src/backend/terrain/survey/manifest_io.c \
		src/backend/terrain/survey/surfel_2dgs.c \
		-lpng16 -lm
	./build/test_backend
	$(CC) $(CFLAGS) -o build/test_scene tests/test_scene.c \
		src/scene/meshlet/quantize.c \
		src/scene/meshlet/builder.c \
		-lm
	./build/test_scene
	$(CC) $(CFLAGS) -o build/test_lighting tests/test_lighting.c \
		src/lighting/math/lighting_math.c \
		-lm
	./build/test_lighting
	$(CC) $(CFLAGS) -o build/test_physics tests/test_physics.c \
		src/physics/math/avbd_math.c \
		-lm
	./build/test_physics
	$(CC) $(CFLAGS) -o build/test_ring_dispatch tests/test_ring_dispatch.c \
		$(shell find src/ring src/system -name '*.c')
	./build/test_ring_dispatch
	$(CC) $(CFLAGS) -o build/test_wayland_wire tests/test_wayland_wire.c \
		$(shell find src/wayland src/ring src/system -name '*.c')
	./build/test_wayland_wire
	$(CC) $(CFLAGS) -o build/test_failure_paths tests/test_failure_paths.c \
		src/vulkan/core/types.c \
		src/vulkan/device/device.c \
		$(LDFLAGS) -lvulkan
	./build/test_failure_paths
	$(CC) $(CFLAGS) -o build/test_platform_truth tests/test_platform_truth.c \
		src/lighting/math/lighting_math.c \
		$(LDFLAGS) -lm
	./build/test_platform_truth

bench:
	./scripts/run_benchmark.sh benchmarks/latest

.PHONY: all minimal debug release run run-release run-minimal release-patch release-minor release-major info help clean clear recheck distclean test bench