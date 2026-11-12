#include "ring/ring.h"
#include "system/system.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int s_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                   \
        }                                                                   \
    } while (0)

typedef struct SideDispatchState {
    uint32_t hits;
    uint64_t last_ud;
    int32_t  last_res;
} SideDispatchState;

static void on_default_cqe(const struct io_uring_cqe *cqe, void *user) {
    SideDispatchState *st = (SideDispatchState *)user;
    if (st != nullptr && cqe != nullptr) {
        st->hits++;
        st->last_ud  = cqe->user_data;
        st->last_res = cqe->res;
    }
}

static void test_system_capabilities(void) {
    CHECK(system_init() == 0);
    const SystemCapabilities *caps = system_get_capabilities();
    CHECK(caps != nullptr);
    CHECK(caps->is_initialized);
    CHECK(caps->cpu.logical_cores >= 1u);
    CHECK(caps->cpu.cache_line_bytes >= 32u && caps->cpu.cache_line_bytes <= 256u);
    CHECK(caps->mem.page_size >= 4096ull);
    CHECK(caps->mem.total_ram_bytes > 0ull);
    CHECK(caps->mem.avail_ram_bytes > 0ull);
    CHECK(caps->mem.avail_ram_bytes <= caps->mem.total_ram_bytes);
    CHECK(caps->uring.nr_request_opcodes > 0u);
}

static void test_ud_packing(void) {
    uint64_t packed = ring_ud_pack(0x1234u, 0xabcdu, 0xdeadbeefu);
    CHECK(ring_ud_tag(packed)  == 0x1234u);
    CHECK(ring_ud_slot(packed) == 0xabcdu);
    CHECK(ring_ud_gen(packed)  == 0xdeadbeefu);

    CHECK(ring_ud_pack((uint16_t)ENG_UD_WL_SEND, 0u, 0u) == ENG_UD_WL_SEND);
    CHECK(ring_ud_tag(ENG_UD_WL_RECV) == ENG_UD_WL_RECV);
    CHECK(ring_ud_slot(ENG_UD_WL_RECV) == 0u);
    CHECK(ring_ud_gen(ENG_UD_WL_RECV) == 0u);
}

static void test_ring_router_and_large_file_load(void) {
    RingContext r = {0};
    CHECK(ring_context_init(&r, "test_ring", true) == 0);

    // Verify 4096-byte page alignment of pager fixed buffers and UMA release
    uint8_t *pager0 = ring_fixed_ptr(&r, ENG_FBUF_PAGER_BASE);
    CHECK(pager0 != nullptr);
    CHECK(((uintptr_t)pager0 & 4095u) == 0u);
    CHECK(r.pager_fixed_on);

    ring_release_pager_fixed_buffers(&r);
    CHECK(!r.pager_fixed_on);
    CHECK(ring_fixed_ptr(&r, ENG_FBUF_SIGNAL) != nullptr);
    CHECK(ring_fixed_ptr(&r, ENG_FBUF_RELEASE) != nullptr);

    // Install default CQE router and verify side eventfd CQE is routed during ring_read_file_all
    SideDispatchState side = {0};
    ring_set_dispatcher(&r, on_default_cqe, &side);

    int efd = eventfd(0, EFD_CLOEXEC);
    CHECK(efd >= 0);
    CHECK(ring_update_file(&r, ENG_RELEASE_FD_SLOT, efd) == 0);
    uint64_t custom_ud = ring_ud_pack((uint16_t)ENG_UD_GPU_RELEASE, 3u, 99u);
    CHECK(ring_arm_read_fixed(&r, ENG_RELEASE_FD_SLOT, true, -1, ENG_FBUF_RELEASE, custom_ud) == 0);

    // Create a 160 KiB test payload to exercise multi-read / full-file verification
    const size_t file_sz = 160u * 1024u + 37u;
    uint8_t *expected = (uint8_t *)malloc(file_sz);
    uint8_t *actual   = (uint8_t *)calloc(1, file_sz + 64u);
    CHECK(expected != nullptr && actual != nullptr);
    for (size_t i = 0; i < file_sz; ++i) {
        expected[i] = (uint8_t)((i * 131u + 17u) & 0xffu);
    }

    char path[128];
    snprintf(path, sizeof path, "/tmp/engine_ring_test_%d.bin", (int)getpid());
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    size_t written = 0u;
    while (written < file_sz) {
        ssize_t w = write(fd, expected + written, file_sz - written);
        CHECK(w > 0);
        written += (size_t)w;
    }
    close(fd);

    // Trigger eventfd right before blocking file load (passing on_cqe = nullptr!)
    uint64_t one = 7ull;
    CHECK(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one));

    size_t got_len = 0u;
    CHECK(ring_read_file_all(&r, path, actual, file_sz + 64u, &got_len, nullptr, nullptr) == 0);
    CHECK(got_len == file_sz);
    CHECK(memcmp(expected, actual, file_sz) == 0);

    // If the file chain completed before the eventfd CQE was drained, peek/enter once
    if (side.hits == 0u) {
        struct io_uring_cqe cqe;
        if (ring_peek(&r, &cqe)) {
            on_default_cqe(&cqe, &side);
        }
    }
    CHECK(side.hits == 1u);
    CHECK(side.last_ud == custom_ud);
    CHECK(side.last_res == 8);
    uint64_t latched = 0ull;
    memcpy(&latched, ring_fixed_ptr(&r, ENG_FBUF_RELEASE), sizeof(latched));
    CHECK(latched == 7ull);

    // Missing file open failure must return negative errno cleanly without corrupting ring
    char missing_path[128];
    snprintf(missing_path, sizeof missing_path, "/tmp/engine_missing_%d.bin", (int)getpid());
    size_t bad_len = 999u;
    int miss_rc = ring_read_file_all(&r, missing_path, actual, file_sz, &bad_len, nullptr, nullptr);
    CHECK(miss_rc == -ENOENT);

    unlink(path);
    (void)ring_update_file(&r, ENG_RELEASE_FD_SLOT, -1);
    close(efd);
    free(expected);
    free(actual);
    ring_context_shutdown(&r);
}

static void test_linux7_inc_pbuf_uma_and_multishot_timeout(void) {
    RingContext r = {0};
    CHECK(ring_context_init(&r, "test_l7", true) == 0);
    CHECK(r.file_alloc_range_on);

    // 1. Verify IOU_PBUF_RING_INC incremental buffer-ring consumption and final recycle
    uint16_t bid = 3u;
    uint16_t tail_before = r.pbuf_tail;
    uint8_t *base = r.bufs + (size_t)bid * ENG_PBUF_LEN;
    uint8_t *c0 = ring_pbuf_chunk(&r, bid, 512u);
    CHECK(c0 == base);
    if (r.pbuf_inc_on) {
        ring_pbuf_finish(&r, bid, IORING_CQE_F_BUFFER | IORING_CQE_F_BUF_MORE, 512);
        CHECK(r.pbuf_bid_off[bid] == 512u);
        CHECK(r.pbuf_tail == tail_before);

        uint8_t *c1 = ring_pbuf_chunk(&r, bid, 1024u);
        CHECK(c1 == base + 512u);
        ring_pbuf_finish(&r, bid, IORING_CQE_F_BUFFER, 1024);
        CHECK(r.pbuf_bid_off[bid] == 0u);
        CHECK(r.pbuf_tail == (uint16_t)(tail_before + 1u));
    } else {
        ring_pbuf_finish(&r, bid, IORING_CQE_F_BUFFER, 512);
        CHECK(r.pbuf_tail == (uint16_t)(tail_before + 1u));
    }

    // 2. Verify zero-GUP UMA buffer registration in slot ENG_FBUF_UMA_BDA (after pager release)
    ring_release_pager_fixed_buffers(&r);
    CHECK(!r.pager_fixed_on);
    void *uma_mock = aligned_alloc(4096u, 65536u);
    CHECK(uma_mock != nullptr);
    memset(uma_mock, 0xa5, 65536u);
    CHECK(ring_register_uma_buffer(&r, uma_mock, 65536u) == 0);
    CHECK(r.uma_buf_registered);
    CHECK(r.uma_buf_base == uma_mock && r.uma_buf_len == 65536u);
    ring_unregister_uma_buffer(&r);
    CHECK(!r.uma_buf_registered);
    free(uma_mock);

    // 3. Verify native multishot IORING_OP_TIMEOUT + clean IORING_TIMEOUT_REMOVE
    const SystemCapabilities *caps = system_get_capabilities();
    if (caps != nullptr && caps->uring.nr_request_opcodes > (uint32_t)IORING_OP_TIMEOUT) {
        uint64_t ms_ud = ring_ud_pack((uint16_t)ENG_UD_SIM_TIMER, 0u, 7u);
        uint64_t rm_ud = ENG_UD_SIM_CANCEL;
        CHECK(ring_arm_timeout_multishot(&r, 3000000ull, ms_ud) == 0);

        uint32_t fires = 0u;
        uint64_t deadline = ring_mono_ns() + 500000000ull;
        while (fires < 2u && ring_mono_ns() < deadline) {
            unsigned sub = ring_submit(&r);
            int rc = ring_enter(&r, sub, 1u, 50);
            CHECK(rc >= 0 || rc == -ETIME || rc == -EINTR);
            struct io_uring_cqe cqe;
            while (ring_peek(&r, &cqe)) {
                if (cqe.user_data == ms_ud) {
                    CHECK(cqe.res == -ETIME);
                    CHECK((cqe.flags & IORING_CQE_F_MORE) != 0u);
                    fires++;
                }
            }
        }
        CHECK(fires >= 2u);

        CHECK(ring_remove_timeout(&r, ms_ud, rm_ud, false) == 0);
        bool saw_terminated = false;
        bool saw_remove_cqe = false;
        while ((!saw_terminated || !saw_remove_cqe) && ring_mono_ns() < deadline) {
            unsigned sub = ring_submit(&r);
            int rc = ring_enter(&r, sub, 1u, 50);
            CHECK(rc >= 0 || rc == -ETIME || rc == -EINTR);
            struct io_uring_cqe cqe;
            while (ring_peek(&r, &cqe)) {
                if (cqe.user_data == ms_ud && (cqe.flags & IORING_CQE_F_MORE) == 0u) {
                    saw_terminated = true;
                } else if (cqe.user_data == rm_ud) {
                    saw_remove_cqe = true;
                }
            }
        }
        CHECK(saw_terminated && saw_remove_cqe);
    }

    ring_context_shutdown(&r);
}

int main(void) {
    test_system_capabilities();
    test_ud_packing();
    test_ring_router_and_large_file_load();
    test_linux7_inc_pbuf_uma_and_multishot_timeout();
    system_shutdown();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_ring_dispatch: all checks passed\n");
    return 0;
}
