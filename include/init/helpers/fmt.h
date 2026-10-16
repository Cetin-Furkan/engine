#ifndef ENGINE_FMT_H
#define ENGINE_FMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Bounded writer over a caller-owned char buffer.
 *
 * cap includes the trailing NUL. Writes that do not fit are dropped.
 */
typedef struct FmtBuf {
    char  *data;
    size_t cap;
    size_t len;
} FmtBuf;

static inline FmtBuf fmt_init(char *buf, size_t cap) {
    if (buf != nullptr && cap > 0) {
        buf[0] = '\0';
    }
    return (FmtBuf){
        .data = buf,
        .cap  = cap,
        .len  = 0,
    };
}

static inline void fmt_putc(FmtBuf *fb, char c) {
    if (fb == nullptr || fb->data == nullptr || fb->cap < 2 || fb->len + 1 >= fb->cap) {
        return;
    }
    fb->data[fb->len++] = c;
    fb->data[fb->len] = '\0';
}

static inline void fmt_str(FmtBuf *fb, const char *s) {
    if (s == nullptr) {
        s = "(null)";
    }
    while (*s != '\0') {
        fmt_putc(fb, *s++);
    }
}

static inline void fmt_u64(FmtBuf *fb, uint64_t v) {
    char tmp[20];
    uint32_t n = 0;
    if (v == 0) {
        fmt_putc(fb, '0');
        return;
    }
    while (v > 0 && n < sizeof tmp) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        fmt_putc(fb, tmp[--n]);
    }
}

static inline void fmt_flush_stdout(const FmtBuf *fb) {
    if (fb != nullptr && fb->data != nullptr && fb->len > 0) {
        fputs(fb->data, stdout);
    }
}

static inline void fmt_flush_stderr(const FmtBuf *fb) {
    if (fb != nullptr && fb->data != nullptr && fb->len > 0) {
        fputs(fb->data, stderr);
    }
}

static inline void log_info_raw(const char *s) {
    if (s != nullptr) {
        fputs(s, stdout);
    }
}

static inline void log_err_raw(const char *s) {
    if (s != nullptr) {
        fputs(s, stderr);
    }
}

#endif /* ENGINE_FMT_H */
