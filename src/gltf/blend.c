#include "gltf/gltf.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Increment when export options change so stale cached GLBs regenerate automatically.
static constexpr uint32_t k_blend_cache_schema_ver = 1u;

// Version-adaptive Blender Python exporter (supports Blender 3.6, 4.x, and 5.x LTS).
// Introspects bpy.ops.export_scene.gltf RNA properties so every supported PBR /
// Geometry Nodes / modifier / tangent / material feature exports cleanly.
static const char k_blend_export_py[] =
    "import bpy, sys\n"
    "argv = sys.argv\n"
    "out_path = argv[argv.index('--') + 1]\n"
    "props = set(p.identifier for p in bpy.ops.export_scene.gltf.get_rna_type().properties)\n"
    "wanted = {\n"
    "    'filepath': out_path,\n"
    "    'export_format': 'GLB',\n"
    "    'export_yup': True,\n"
    "    'export_apply': True,\n"
    "    'export_gn_mesh': True,\n"
    "    'export_texcoords': True,\n"
    "    'export_normals': True,\n"
    "    'export_tangents': True,\n"
    "    'export_materials': 'EXPORT',\n"
    "    'export_image_format': 'AUTO',\n"
    "    'export_attributes': True,\n"
    "    'export_all_vertex_colors': False,\n"
    "    'export_vertex_color': 'MATERIAL',\n"
    "    'export_cameras': False,\n"
    "    'export_lights': False,\n"
    "    'export_animations': False,\n"
    "    'export_skins': False,\n"
    "    'export_morph': False,\n"
    "    'export_draco_mesh_compression_enable': False,\n"
    "    'export_meshopt_compression_enable': False,\n"
    "    'use_visible': True,\n"
    "}\n"
    "kwargs = {k: v for k, v in wanted.items() if k in props}\n"
    "res = bpy.ops.export_scene.gltf(**kwargs)\n"
    "if 'FINISHED' not in res:\n"
    "    sys.exit(2)\n";

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t n) {
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0u; i < n; ++i) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void mkdir_p(const char *dir) {
    char tmp[512];
    int n = snprintf(tmp, sizeof tmp, "%s", dir);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return;
    }
    for (size_t i = 1u; i < (size_t)n; ++i) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        (void)mkdir(tmp, 0755);
        tmp[i] = '/';
    }
    (void)mkdir(tmp, 0755);
}

static void cache_base_dir(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        snprintf(out, cap, "%s/engine/blend_glb", xdg);
        return;
    }
    const char *home = getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        snprintf(out, cap, "%s/.cache/engine/blend_glb", home);
        return;
    }
    snprintf(out, cap, "/tmp/engine_blend_glb");
}

int gltf_blend_export_to_glb(const char *blend_path, char *out_glb_path, size_t out_cap) {
    if (blend_path == nullptr || blend_path[0] == '\0' ||
        out_glb_path == nullptr || out_cap < 32u) {
        return -1;
    }

    struct stat st = {0};
    if (stat(blend_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "[Blender] Cannot stat '%s': %s\n", blend_path, strerror(errno));
        return -1;
    }

    char abs_path[PATH_MAX];
    const char *key_path = realpath(blend_path, abs_path) != nullptr ? abs_path : blend_path;

    uint64_t h = 14695981039346656037ull;
    h = fnv1a_update(h, key_path, strlen(key_path));
    h = fnv1a_update(h, &st.st_size, sizeof st.st_size);
    h = fnv1a_update(h, &st.st_mtim.tv_sec, sizeof st.st_mtim.tv_sec);
    h = fnv1a_update(h, &st.st_mtim.tv_nsec, sizeof st.st_mtim.tv_nsec);
    h = fnv1a_update(h, &k_blend_cache_schema_ver, sizeof k_blend_cache_schema_ver);

    char dir[384];
    cache_base_dir(dir, sizeof dir);
    mkdir_p(dir);

    int wrote = snprintf(out_glb_path, out_cap, "%s/%016llx.glb",
                         dir, (unsigned long long)h);
    if (wrote <= 0 || (size_t)wrote >= out_cap) {
        return -1;
    }

    struct stat cst = {0};
    if (stat(out_glb_path, &cst) == 0 && S_ISREG(cst.st_mode) && cst.st_size >= 20) {
        printf("[Blender] Cache hit for '%s' -> '%s' (%.1f KiB)\n",
               blend_path, out_glb_path, (double)cst.st_size / 1024.0);
        fflush(stdout);
        return 0;
    }

    char tmp_glb[576];
    int tw = snprintf(tmp_glb, sizeof tmp_glb, "%s/%016llx.%ld.tmp.glb",
                      dir, (unsigned long long)h, (long)getpid());
    if (tw <= 0 || (size_t)tw >= sizeof tmp_glb) {
        return -1;
    }

    const char *blender_bin = getenv("ENGINE_BLENDER");
    if (blender_bin == nullptr || blender_bin[0] == '\0') {
        blender_bin = "blender";
    }

    printf("[Blender] Exporting '%s' via headless %s -> cached GLB...\n", blend_path, blender_bin);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[Blender] fork() failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // Silence noisy Blender startup banner on stdout while preserving real errors on stderr.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        char *const argv[] = {
            (char *)blender_bin,
            (char *)"--background",
            (char *)"--factory-startup",
            (char *)blend_path,
            (char *)"--python-expr",
            (char *)k_blend_export_py,
            (char *)"--",
            tmp_glb,
            nullptr,
        };
        execvp(blender_bin, argv);
        fprintf(stderr, "[Blender] Failed to execute '%s' (%s). Install Blender or export .glb/.gltf.\n",
                blender_bin, strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        unlink(tmp_glb);
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[Blender] Export failed for '%s' (exit=%d)\n",
                blend_path, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        unlink(tmp_glb);
        return -1;
    }

    struct stat tst = {0};
    if (stat(tmp_glb, &tst) != 0 || tst.st_size < 20) {
        fprintf(stderr, "[Blender] Export produced empty output for '%s'\n", blend_path);
        unlink(tmp_glb);
        return -1;
    }

    if (rename(tmp_glb, out_glb_path) != 0) {
        unlink(tmp_glb);
        fprintf(stderr, "[Blender] Failed to commit cache file '%s': %s\n",
                out_glb_path, strerror(errno));
        return -1;
    }

    printf("[Blender] Exported '%s' -> '%s' (%.1f KiB)\n",
           blend_path, out_glb_path, (double)tst.st_size / 1024.0);
    fflush(stdout);
    return 0;
}
