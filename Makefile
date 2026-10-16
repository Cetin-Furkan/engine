# Modular C23 Engine Makefile with Isolated Build & Mode Support
.DEFAULT_GOAL := all

DEPS_CACHE_DIR := .dependencies
CONFIG_MK := $(DEPS_CACHE_DIR)/config.mk

# Automatically trigger environment check if config.mk is missing
-include $(CONFIG_MK)

$(CONFIG_MK): scripts/check_env.sh
	@./scripts/check_env.sh

# Fallbacks if config.mk has not been created yet
CC ?= gcc
C23_FLAGS ?= -std=c23

# Versioning & Git Metadata
BASE_VERSION := $(shell cat VERSION 2>/dev/null || echo "0.1.0")
GIT_HASH     := $(shell git rev-parse --short HEAD 2>/dev/null || echo "initial")
GIT_DIRTY    := $(shell git diff --quiet 2>/dev/null || echo "-dirty")

# Build Mode: debug (default) or release
MODE ?= debug

# Baseline x86-64 unless ARCH= or NATIVE=1 is set. This machine's glibc crt
# was built with an x86-64-v4 "needed" note (AVX-512). The linker copies that
# note into every executable, and glibc then refuses to start it on CPUs
# without AVX-512: "CPU ISA level is lower than required". That happens
# before main. The note is removed after link except for NATIVE=1.
ARCH_FLAG := -march=x86-64
ifneq ($(ARCH),)
  ARCH_FLAG := -march=$(ARCH)
else ifeq ($(NATIVE),1)
  ARCH_FLAG := -march=native
endif

ifeq ($(MODE),release)
  OPT_FLAGS := -O3 -DNDEBUG $(ARCH_FLAG) -flto -DENGINE_RELEASE=1
  FULL_VERSION := $(BASE_VERSION)
else
  OPT_FLAGS := -O0 -g3 $(ARCH_FLAG) -DENGINE_DEBUG=1
  FULL_VERSION := $(BASE_VERSION)-dev+$(GIT_HASH)$(GIT_DIRTY)
endif

# Build Output Directories (segregated by mode)
BUILD_DIR := build/$(MODE)
OBJ_DIR   := $(BUILD_DIR)/obj
DEP_DIR   := $(BUILD_DIR)/deps
BIN_DIR   := $(BUILD_DIR)/bin
TARGET    := $(BIN_DIR)/engine

# Clean include paths and preprocessor flags
INCLUDES := -Iinclude -Isrc
ifneq ($(VULKAN_INCLUDE_DIR),)
INCLUDES += -I$(VULKAN_INCLUDE_DIR)
endif

# Compiler & Preprocessor Flags
CFLAGS ?= $(C23_FLAGS) -D_GNU_SOURCE -Wall -Wextra $(OPT_FLAGS) $(INCLUDES) \
          -ffunction-sections -fdata-sections \
          -DENGINE_VERSION_STR=\"$(FULL_VERSION)\" \
          -DENGINE_BUILD_MODE=\"$(MODE)\"

# Linker Flags
LDFLAGS ?= -Wl,--gc-sections
ifeq ($(MODE),release)
  LDFLAGS += -flto
endif
ifneq ($(VULKAN_LIB),)
LDFLAGS += -lvulkan
endif

SRCS := main.c $(shell find src -name '*.c')
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

# Include all generated header dependency rules (.d files)
-include $(DEPS)

# Mode shortcuts
debug:
	@$(MAKE) MODE=debug all

release:
	@$(MAKE) MODE=release all

# Execution helpers
run: debug
	@echo -e "\033[0;32m==> Running Debug Engine...\033[0m"
	@./build/debug/bin/engine

run-release: release
	@echo -e "\033[0;32m==> Running Release Engine...\033[0m"
	@./build/release/bin/engine

# GitHub Release Bump Targets
release-patch:
	@./scripts/bump_version.sh patch
	@$(MAKE) MODE=release all

release-minor:
	@./scripts/bump_version.sh minor
	@$(MAKE) MODE=release all

release-major:
	@./scripts/bump_version.sh major
	@$(MAKE) MODE=release all

# Diagnostics & Info
info: $(CONFIG_MK)
	@echo -e "\033[0;34m=== Engine Build Configuration ===\033[0m"
	@echo "  Target Binary:    $(TARGET)"
	@echo "  Active Mode:      $(MODE)"
	@echo "  Base SemVer:      $(BASE_VERSION)"
	@echo "  Full Version:     $(FULL_VERSION)"
	@echo "  Compiler:         $(CC) ($(C23_FLAGS))"
	@echo "  Kernel Version:   $(KERNEL_VERSION)"
	@echo "  Vulkan Header:    $(VULKAN_HEADER) (v$(VULKAN_VERSION))"
	@echo "  Vulkan Library:   $(VULKAN_LIB)"
	@echo "  Wayland Protocols:$(WAYLAND_PROTOCOLS_VERSION) ($(WAYLAND_PROTOCOLS_DIR))"
	@echo "  Slang Compiler:   $(SLANG_BIN)"
	@echo -e "\033[0;34m==================================\033[0m"

# Help manual
help:
	@echo -e "\033[0;36mModular C23 Engine - Build Commands:\033[0m"
	@echo ""
	@echo -e "  \033[1mBuild & Run:\033[0m"
	@echo "    make               Build debug binary (-O0 -g3, default)"
	@echo "    make debug         Explicit alias for make"
	@echo "    make release       Portable release binary (-O3 -flto -DNDEBUG, baseline x86-64)"
	@echo "    make release NATIVE=1    Same, with -march=native (do not copy this binary to other PCs)"
	@echo "    make release ARCH=x86-64-v3    Same, with an explicit -march"
	@echo "    make run           Build and execute debug binary"
	@echo "    make run-release   Build and execute release binary"
	@echo ""
	@echo -e "  \033[1mCleanup & Cache:\033[0m"
	@echo "    make clean         Remove build/ directory (preserves prerequisite cache)"
	@echo "    make clear         Alias for make clean"
	@echo "    make recheck       Re-run environment probing & refresh .dependencies/"
	@echo "    make distclean     Remove build/ AND .dependencies/ cache"
	@echo ""
	@echo -e "  \033[1mRelease Bumping (GitHub):\033[0m"
	@echo "    make release-patch Increment patch (0.0.1), tag git, and compile release"
	@echo "    make release-minor Increment minor (0.1.0), tag git, and compile release"
	@echo "    make release-major Increment major (1.0.0), tag git, and compile release"
	@echo ""
	@echo -e "  \033[1mDiagnostics:\033[0m"
	@echo "    make info          Print current build settings, paths, and toolchains"
	@echo "    make help          Print this guide"

# Cleanup
clean:
	@echo "Cleaning build outputs..."
	@rm -rf build
	@echo "Build directory cleared."

# clear is an explicit alias for clean
clear: clean

recheck:
	rm -rf $(DEPS_CACHE_DIR)
	@$(MAKE) $(CONFIG_MK)

distclean: clean
	rm -rf $(DEPS_CACHE_DIR)

.PHONY: all debug release run run-release release-patch release-minor release-major info help clean clear recheck distclean
