#!/usr/bin/env bash
set -euo pipefail

DEPS_DIR=".dependencies"
CONFIG_MK="$DEPS_DIR/config.mk"
SYSTEM_INFO="$DEPS_DIR/system_requirements.info"
STAMP="$DEPS_DIR/.system_ok"

mkdir -p "$DEPS_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== [Engine] System Requirements & Dependency Check ===${NC}"

FAILURES=0

# 1. Check Linux Kernel >= 7.2
KERNEL_RAW=$(uname -r)
KERNEL_MAJOR=$(echo "$KERNEL_RAW" | cut -d. -f1)
KERNEL_MINOR=$(echo "$KERNEL_RAW" | cut -d. -f2)

if [[ "$KERNEL_MAJOR" -gt 7 ]] || { [[ "$KERNEL_MAJOR" -eq 7 ]] && [[ "$KERNEL_MINOR" -ge 2 ]]; }; then
    echo -e "  [${GREEN}OK${NC}] Linux Kernel: $KERNEL_RAW (>= 7.2)"
else
    echo -e "  [${RED}FAIL${NC}] Linux Kernel: $KERNEL_RAW is older than required 7.2"
    FAILURES=$((FAILURES + 1))
fi

# 2. Check C23 Compiler
CC="${CC:-gcc}"
C23_TEST_SRC=$(mktemp /tmp/c23_test_XXXXXX.c)
C23_TEST_BIN=$(mktemp /tmp/c23_test_XXXXXX.out)

cat << 'C23_EOF' > "$C23_TEST_SRC"
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "Not C23"
#endif
int main(void) {
    bool b = true;
    void *ptr = nullptr;
    return (b && ptr == nullptr) ? 0 : 1;
}
C23_EOF

if $CC -std=c23 "$C23_TEST_SRC" -o "$C23_TEST_BIN" 2>/dev/null; then
    CC_VER=$($CC --version | head -n1)
    echo -e "  [${GREEN}OK${NC}] C23 Compiler: $CC ($CC_VER)"
else
    echo -e "  [${RED}FAIL${NC}] C23 Compiler: $CC cannot compile native C23 (-std=c23 failed)"
    FAILURES=$((FAILURES + 1))
fi
rm -f "$C23_TEST_SRC" "$C23_TEST_BIN"

# 3. Check Vulkan 1.4 & Location
VULKAN_HEADER=""
VULKAN_LIB=""
VULKAN_VER=""

# Check header location
SEARCH_PATHS=(
    "/usr/include/vulkan/vulkan.h"
    "/usr/local/include/vulkan/vulkan.h"
    "${VULKAN_SDK:-/nonexistent}/include/vulkan/vulkan.h"
)

for p in "${SEARCH_PATHS[@]}"; do
    if [[ -f "$p" ]]; then
        VULKAN_HEADER="$p"
        break
    fi
done

if [[ -z "$VULKAN_HEADER" ]] && command -v pkg-config >/dev/null 2>&1; then
    PC_INC=$(pkg-config --variable=includedir vulkan 2>/dev/null || true)
    if [[ -n "$PC_INC" && -f "$PC_INC/vulkan/vulkan.h" ]]; then
        VULKAN_HEADER="$PC_INC/vulkan/vulkan.h"
    fi
fi

if [[ -n "$VULKAN_HEADER" ]]; then
    # Detect version from pkg-config or header
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists vulkan; then
        VULKAN_VER=$(pkg-config --modversion vulkan)
    else
        VULKAN_VER=$(grep -E "#define VK_HEADER_VERSION " "$(dirname "$VULKAN_HEADER")/vulkan_core.h" 2>/dev/null | awk '{print $3}' || echo "unknown")
    fi

    # Check for Vulkan 1.4 support (VK_API_VERSION_1_4)
    if grep -q "VK_API_VERSION_1_4" "$VULKAN_HEADER" 2>/dev/null || grep -q "VK_API_VERSION_1_4" "$(dirname "$VULKAN_HEADER")/vulkan_core.h" 2>/dev/null; then
        echo -e "  [${GREEN}OK${NC}] Vulkan 1.4 Header: $VULKAN_HEADER (Version: $VULKAN_VER)"
    else
        echo -e "  [${RED}FAIL${NC}] Vulkan header at $VULKAN_HEADER does not define Vulkan 1.4"
        FAILURES=$((FAILURES + 1))
    fi
else
    echo -e "  [${RED}FAIL${NC}] Vulkan 1.4 headers (vulkan/vulkan.h) not found"
    FAILURES=$((FAILURES + 1))
fi

# Locate Vulkan Library
if ldconfig -p 2>/dev/null | grep -q "libvulkan.so"; then
    VULKAN_LIB=$(ldconfig -p | grep "libvulkan.so" | head -n1 | awk '{print $NF}')
    echo -e "  [${GREEN}OK${NC}] Vulkan Library: $VULKAN_LIB"
elif [[ -f "/usr/lib/libvulkan.so" ]]; then
    VULKAN_LIB="/usr/lib/libvulkan.so"
    echo -e "  [${GREEN}OK${NC}] Vulkan Library: $VULKAN_LIB"
else
    echo -e "  [${YELLOW}WARN${NC}] libvulkan.so not found in standard cache, linker will attempt -lvulkan"
fi

# 4. Check Wayland Protocols >= 1.45
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists wayland-protocols; then
    WP_VER=$(pkg-config --modversion wayland-protocols)
    WP_MAJOR=$(echo "$WP_VER" | cut -d. -f1)
    WP_MINOR=$(echo "$WP_VER" | cut -d. -f2)

    if [[ "$WP_MAJOR" -gt 1 ]] || { [[ "$WP_MAJOR" -eq 1 ]] && [[ "$WP_MINOR" -ge 45 ]]; }; then
        echo -e "  [${GREEN}OK${NC}] Wayland Protocols: $WP_VER (>= 1.45)"
        WAYLAND_PROTOCOLS_DIR=$(pkg-config --variable=pkgdatadir wayland-protocols)
    else
        echo -e "  [${RED}FAIL${NC}] Wayland Protocols: $WP_VER is older than required 1.45"
        FAILURES=$((FAILURES + 1))
    fi
else
    echo -e "  [${RED}FAIL${NC}] wayland-protocols not found via pkg-config"
    FAILURES=$((FAILURES + 1))
fi

# 5. Check Slang (slangc)
SLANG_BIN=""
if command -v slangc >/dev/null 2>&1; then
    SLANG_BIN=$(command -v slangc)
    SLANG_VER=$($SLANG_BIN -v 2>&1 | head -n1 || echo "installed")
    echo -e "  [${GREEN}OK${NC}] Slang Compiler: $SLANG_BIN ($SLANG_VER)"
elif command -v slang >/dev/null 2>&1; then
    SLANG_BIN=$(command -v slang)
    echo -e "  [${GREEN}OK${NC}] Slang Compiler: $SLANG_BIN"
else
    echo -e "  [${RED}FAIL${NC}] Slang shader compiler (slangc) not found in PATH"
    FAILURES=$((FAILURES + 1))
fi

echo -e "${BLUE}=======================================================${NC}"

if [[ $FAILURES -gt 0 ]]; then
    echo -e "${RED}Error: System verification failed with $FAILURES unsatisfied requirements.${NC}"
    exit 1
fi

echo -e "${GREEN}All requirements satisfied! Caching configuration to $CONFIG_MK...${NC}"

# Generate .dependencies/config.mk
cat << CONF_EOF > "$CONFIG_MK"
# Automatically generated by scripts/check_env.sh - DO NOT EDIT MANUALLY
# Generated on: $(date -Iseconds)

KERNEL_VERSION := $KERNEL_RAW
CC := $CC
C23_FLAGS := -std=c23

VULKAN_HEADER := $VULKAN_HEADER
VULKAN_VERSION := $VULKAN_VER
VULKAN_LIB := $VULKAN_LIB
VULKAN_INCLUDE_DIR := $(dirname "$(dirname "$VULKAN_HEADER")")

WAYLAND_PROTOCOLS_VERSION := $WP_VER
WAYLAND_PROTOCOLS_DIR := $WAYLAND_PROTOCOLS_DIR

SLANG_BIN := $SLANG_BIN

CONFIG_ENV_OK := 1
CONF_EOF

# Generate human-readable system_requirements.info
cat << INFO_EOF > "$SYSTEM_INFO"
Engine System Requirements & Environment Snapshot
=================================================
Checked at: $(date)
Status: PASSED

- Linux Kernel: $KERNEL_RAW (Required: >= 7.2)
- C23 Compiler: $CC ($CC_VER)
- Vulkan Version: $VULKAN_VER (Required: 1.4)
  * Header Location: $VULKAN_HEADER
  * Library Location: $VULKAN_LIB
- Wayland Protocols: $WP_VER (Required: >= 1.45)
  * Protocol Directory: $WAYLAND_PROTOCOLS_DIR
- Slang Shader Compiler: $SLANG_BIN ($SLANG_VER)
INFO_EOF

touch "$STAMP"
echo -e "${GREEN}Environment cache ready in $DEPS_DIR/${NC}"
