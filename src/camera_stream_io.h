#pragma once
#include <stddef.h>
#include <stdint.h>

// Return -2 for a transient write, -1 for a broken connection.
typedef int (*camera_write_fn)(void *, const char *, size_t);
typedef int64_t (*camera_clock_fn)(void *);
typedef void (*camera_wait_fn)(void *);

// A WS payload must be complete: a short positive send is not success.
// One deadline bounds the whole write, including repeated partial writes.
static inline int camera_send_all(void *ctx, const char *data, size_t length,
                                  camera_write_fn write_fn, camera_clock_fn clock_fn,
                                  camera_wait_fn wait_fn, int64_t timeout_us)
{
    size_t offset = 0;
    int64_t start = clock_fn(ctx);
    while (offset < length) {
        if (clock_fn(ctx) - start >= timeout_us) return -1;
        int n = write_fn(ctx, data + offset, length - offset);
        if (n == -2) { wait_fn(ctx); continue; }
        if (n <= 0 || (size_t)n > length - offset) return -1;
        offset += (size_t)n;
    }
    return (int)length;
}
