#include "system/internal.h"
#include "ring/ring.h"
#include <errno.h>
#include <linux/io_uring.h>
#include <linux/io_uring/query.h>
#include <sys/syscall.h>
#include <unistd.h>

static long sys_uring_register_raw(int fd, unsigned int opcode, const void *arg, unsigned int nr_args) {
    long rc = syscall(__NR_io_uring_register, fd, opcode, arg, nr_args, 0L, 0L);
    return (rc < 0) ? -errno : rc;
}

int system_probe_uring_kernel(SystemUringInfo *out) {
    if (out == nullptr) {
        return -EINVAL;
    }
    *out = (SystemUringInfo){0};

    struct io_uring_query_opcode opcodes = {0};
    struct io_uring_query_hdr hdr = {
        .next_entry = 0,
        .query_data = (uint64_t)(uintptr_t)&opcodes,
        .query_op   = IO_URING_QUERY_OPCODES,
        .size       = sizeof(opcodes),
    };
    long rc = sys_uring_register_raw(-1, IORING_REGISTER_QUERY, &hdr, 0);
    if (rc < 0) {
        return (int)rc;
    }
    if (hdr.result < 0) {
        return hdr.result;
    }

    if ((opcodes.ring_setup_flags & k_ring_setup_mask) != k_ring_setup_mask) {
        return -ENOTSUP;
    }
    if ((opcodes.feature_flags & k_ring_required_features) != k_ring_required_features) {
        return -ENOTSUP;
    }
    if (opcodes.nr_request_opcodes <= IORING_OP_RECV_ZC ||
        opcodes.nr_register_opcodes <= IORING_REGISTER_MEM_REGION) {
        return -ENOTSUP;
    }

    out->setup_flags          = opcodes.ring_setup_flags;
    out->feature_flags        = opcodes.feature_flags;
    out->nr_request_opcodes   = opcodes.nr_request_opcodes;
    out->nr_register_opcodes  = opcodes.nr_register_opcodes;
    out->has_no_iowait        = (opcodes.feature_flags & IORING_FEAT_NO_IOWAIT) != 0;
    out->has_recvsend_bundle  = (opcodes.feature_flags & IORING_FEAT_RECVSEND_BUNDLE) != 0;
    out->has_min_timeout      = (opcodes.feature_flags & IORING_FEAT_MIN_TIMEOUT) != 0;
    out->has_cqe_skip         = (opcodes.feature_flags & IORING_FEAT_CQE_SKIP) != 0;
    out->has_ext_arg          = (opcodes.feature_flags & IORING_FEAT_EXT_ARG) != 0;
    out->has_reg_reg_ring     = (opcodes.feature_flags & IORING_FEAT_REG_REG_RING) != 0;
    return 0;
}
