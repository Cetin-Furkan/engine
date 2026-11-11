#include "backend/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// SSD .gmpack Archive Reader & Self-Relative 2D Pointer Block Verifier
// ============================================================================

uint32_t backend_quadtree_level_base(uint8_t lod) {
    switch (lod) {
    case 0u:
        return 0u;
    case 1u:
        return 1u;
    case 2u:
        return 5u;
    case 3u:
        return 21u;
    case 4u:
        return 85u;
    default:
        return 0u;
    }
}

uint32_t backend_quadtree_node_index(uint8_t lod, uint8_t gx, uint8_t gy) {
    if (lod >= k_gmpack_lod_count) {
        return 0u;
    }
    uint32_t side = 1u << lod;
    if (gx >= side || gy >= side) {
        return 0u;
    }
    return backend_quadtree_level_base(lod) + (uint32_t)gy * side + (uint32_t)gx;
}

static int pread_exact(int fd, void *dst, size_t bytes, off_t offset) {
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0u;
    while (done < bytes) {
        ssize_t n = pread(fd, out + done, bytes - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int backend_terrain_ensure_pack(const char *pack_path) {
    if (pack_path == nullptr || pack_path[0] == '\0') {
        return -1;
    }
    struct stat st = {0};
    if (stat(pack_path, &st) == 0 && (size_t)st.st_size == k_gmpack_total_bytes) {
        int fd = open(pack_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            uint32_t magic = 0u;
            uint32_t ver   = 0u;
            if (pread_exact(fd, &magic, sizeof magic, 0) == 0 &&
                pread_exact(fd, &ver, sizeof ver, 4) == 0 &&
                magic == k_gmpack_magic && ver == k_gmpack_version) {
                close(fd);
                return 0;
            }
            close(fd);
        }
    }
    return backend_synth_besparmak_archive(pack_path);
}

int backend_terrain_read_header(const char *pack_path, GpuMathArchiveHeader *out_hdr) {
    if (pack_path == nullptr || out_hdr == nullptr) {
        return -1;
    }
    if (backend_terrain_ensure_pack(pack_path) != 0) {
        return -1;
    }
    int fd = open(pack_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    int rc = pread_exact(fd, out_hdr, sizeof(GpuMathArchiveHeader), 0);
    close(fd);
    if (rc != 0) {
        return -1;
    }
    if (out_hdr->dir.magic != k_gmpack_magic ||
        out_hdr->dir.version != k_gmpack_version ||
        out_hdr->dir.node_count != k_gmpack_node_count) {
        return -1;
    }
    return 0;
}

int backend_terrain_read_tile(const char *pack_path, uint32_t node_index, GpuMathTileBlock *out_tile) {
    if (pack_path == nullptr || out_tile == nullptr || node_index >= k_gmpack_node_count) {
        return -1;
    }
    int fd = open(pack_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    // Read the node's RelPtr2D from the header or compute directly via RelPtr2D block algebra
    GpuMathQuadNode node = {0};
    off_t node_off = (off_t)(sizeof(GpuMathPageDirectory) + node_index * sizeof(GpuMathQuadNode));
    if (pread_exact(fd, &node, sizeof node, node_off) != 0) {
        close(fd);
        return -1;
    }
    uint64_t ssd_byte_offset = relptr2d_resolve_addr(0ull, node.rel_ssd_payload);
    if (ssd_byte_offset < k_gmpack_header_bytes ||
        ssd_byte_offset + k_gmpack_tile_bytes > k_gmpack_total_bytes) {
        close(fd);
        return -1;
    }
    int rc = pread_exact(fd, out_tile, sizeof(GpuMathTileBlock), (off_t)ssd_byte_offset);
    close(fd);
    return rc;
}

int backend_terrain_write_tile(const char *pack_path, uint32_t node_index, const GpuMathTileBlock *in_tile) {
    if (pack_path == nullptr || in_tile == nullptr || node_index >= k_gmpack_node_count) {
        return -1;
    }
    int fd = open(pack_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    GpuMathQuadNode node = {0};
    off_t node_off = (off_t)(sizeof(GpuMathPageDirectory) + node_index * sizeof(GpuMathQuadNode));
    if (pread_exact(fd, &node, sizeof node, node_off) != 0) {
        close(fd);
        return -1;
    }
    uint64_t ssd_byte_offset = relptr2d_resolve_addr(0ull, node.rel_ssd_payload);
    if (ssd_byte_offset < k_gmpack_header_bytes ||
        ssd_byte_offset + k_gmpack_tile_bytes > k_gmpack_total_bytes) {
        close(fd);
        return -1;
    }
    const uint8_t *src = (const uint8_t *)in_tile;
    size_t done = 0u;
    while (done < sizeof(GpuMathTileBlock)) {
        ssize_t n = pwrite(fd, src + done, sizeof(GpuMathTileBlock) - done, (off_t)(ssd_byte_offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}
