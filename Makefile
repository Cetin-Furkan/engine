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
ifeq ($(GLSLANG),)
GLSLANG := glslangValidator
endif

# slangc compiles the shaders it accepts. glslangValidator keeps buffer_reference,
# explicit offsets, and compute. The branch is per source, not a parse-time ifeq.
SLANG_SRCS := shaders/chrome.vert shaders/chrome.frag shaders/cube.frag shaders/solid.frag

$(GEN_DIR)/%.spv: shaders/%
	@mkdir -p $(dir $@)
	@set -e; \
	if printf '%s\n' $(SLANG_SRCS) | grep -Fxq "$<"; then \
		case "$<" in \
			*.vert) stage=vertex ;; \
			*.frag) stage=fragment ;; \
			*.comp) stage=compute ;; \
			*) echo "unknown shader stage: $<" >&2; exit 1 ;; \
		esac; \
		$(SLANGC) -lang glsl -target spirv -profile spirv_1_6 -stage "$$stage" -entry main -fvk-use-entrypoint-name -o "$@" "$<"; \
	else \
		$(GLSLANG) -V --target-env vulkan1.4 -Ishaders --depfile "$(GEN_DIR)/$*.d" -o "$@" "$<"; \
	fi

$(GEN_DIR)/bevel.comp.spv: shaders/face_grid.inc
$(GEN_DIR)/heat.comp.spv: shaders/heat_nbr.inc shaders/face_grid.inc
$(GEN_DIR)/cube.vert.spv: shaders/camera.inc shaders/material.inc
$(GEN_DIR)/solid.vert.spv: shaders/camera.inc shaders/material.inc
$(GEN_DIR)/shape.vert.spv: shaders/camera.inc shaders/material.inc
$(GEN_DIR)/plane.vert.spv: shaders/camera.inc
$(GEN_DIR)/plot.vert.spv: shaders/camera.inc
$(GEN_DIR)/gimbal.vert.spv: shaders/camera.inc
$(GEN_DIR)/mesh.vert.spv: shaders/camera.inc shaders/material.inc

-include $(wildcard $(GEN_DIR)/*.d)

$(OBJ_DIR)/examples/demo/render/cube.o: $(GEN_DIR)/cube.vert.spv $(GEN_DIR)/cube.frag.spv $(GEN_DIR)/bevel.comp.spv $(GEN_DIR)/heat.comp.spv
$(OBJ_DIR)/examples/demo/render/solid.o: $(GEN_DIR)/solid.vert.spv $(GEN_DIR)/solid.frag.spv $(GEN_DIR)/solid.comp.spv
$(OBJ_DIR)/examples/demo/render/shape.o: $(GEN_DIR)/shape.vert.spv $(GEN_DIR)/shape.frag.spv
$(OBJ_DIR)/examples/demo/render/plane.o: $(GEN_DIR)/plane.vert.spv $(GEN_DIR)/plane.frag.spv
$(OBJ_DIR)/examples/demo/render/plot.o: $(GEN_DIR)/plot.vert.spv $(GEN_DIR)/plot.frag.spv $(GEN_DIR)/plot.comp.spv
$(OBJ_DIR)/examples/demo/render/gimbal.o: $(GEN_DIR)/gimbal.vert.spv $(GEN_DIR)/gimbal.frag.spv
$(OBJ_DIR)/examples/demo/render/mesh.o: $(GEN_DIR)/mesh.vert.spv $(GEN_DIR)/mesh.frag.spv
$(OBJ_DIR)/examples/demo/render/hud.o: $(GEN_DIR)/hud.vert.spv $(GEN_DIR)/hud.frag.spv
$(OBJ_DIR)/examples/demo/render/grid.o: $(GEN_DIR)/grid.comp.spv
$(OBJ_DIR)/src/chrome/gpu.o: $(GEN_DIR)/chrome.vert.spv $(GEN_DIR)/chrome.frag.spv

# Compiler & Preprocessor Flags
CFLAGS ?= $(C23_FLAGS) -D_GNU_SOURCE -Wall -Wextra $(OPT_FLAGS) $(INCLUDES) \
          --embed-dir=$(GEN_DIR) \
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

SRCS := main.c $(shell find src examples/demo -name '*.c')
OBJS := $(SRCS:%.c=$(OBJ_DIR)/%.o)
DEPS := $(SRCS:%.c=$(DEP_DIR)/%.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)
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
	@echo "  Slang Compiler:    $(SLANGC)"
	@echo "  GLSL Compiler:     $(GLSLANG)"
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
	$(CC) $(CFLAGS) -o build/test_clock tests/test_clock.c src/frame/loop/clock.c
	./build/test_clock
	$(CC) $(CFLAGS) -o build/test_world tests/test_world.c examples/demo/world/table.c examples/demo/world/spin.c -lm
	./build/test_world
	$(CC) $(CFLAGS) -o build/test_policy tests/test_policy.c examples/demo/world/policy.c
	./build/test_policy

.PHONY: all debug release run run-release release-patch release-minor release-major info help clean clear recheck distclean test