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

ifeq ($(MODE),release)
  OPT_FLAGS := -O3 -DNDEBUG -march=native -flto -DENGINE_RELEASE=1
  FULL_VERSION := $(BASE_VERSION)
else
  OPT_FLAGS := -O0 -g3 -DENGINE_DEBUG=1
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

# Query headers used by main.c to detect unreferenced modules during compilation
MAIN_INCLUDES := $(shell $(CC) $(C23_FLAGS) -D_GNU_SOURCE $(INCLUDES) -MM main.c 2>/dev/null)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: %.c $(CONFIG_MK) VERSION
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	@if [ "$<" != "main.c" ]; then \
		base=$$(basename "$<" .c); \
		if ! echo "$(MAIN_INCLUDES)" | grep -qE "(^|[ /])$${base}\.h"; then \
			echo -e "  \033[1;33m[Warning]\033[0m $< is not used in main.c, but compiled (linker will dead-strip)"; \
		fi; \
	fi
	$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

# Include all generated header dependency rules (.d files)
-include $(DEPS)

# Mode shortcuts
debug:
	@$(MAKE) MODE=debug all

release:
	@$(MAKE) MODE=release all

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

# Clean removes the entire build directory (both debug & release)
clean:
	rm -rf build

recheck:
	rm -rf $(DEPS_CACHE_DIR)
	@$(MAKE) $(CONFIG_MK)

distclean: clean
	rm -rf $(DEPS_CACHE_DIR)

.PHONY: all debug release release-patch release-minor release-major clean recheck distclean
