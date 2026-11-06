/* SPDX-License-Identifier: MIT */
#ifndef MALI_KMOD_DARWIN_H
#define MALI_KMOD_DARWIN_H
#include "../MaliMetalKernelABI.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The IOKit connection remains owned by the caller. These functions return the
 * exact IOReturn from transport failures; completion state is separate. */
typedef struct {
    uint32_t connection;
    MMKBOInfo info;
    void *mapping;
    uint64_t mapped_size;
} MMKDarwinBO;
int32_t mmk_darwin_query(uint32_t connection, MMKQuery *query);
int32_t mmk_darwin_clock(uint32_t connection, MMKClockResult *clock);
/* Open a separate user client on the same accelerator. The returned connection
 * is owned by the caller and must be closed with mmk_darwin_close(). */
int32_t mmk_darwin_open_peer(uint32_t connection, uint32_t *peer);
int32_t mmk_darwin_close(uint32_t connection);
int32_t mmk_darwin_alloc(uint32_t connection,uint64_t size,uint32_t flags,MMKDarwinBO *out);
int32_t mmk_darwin_import(uint32_t connection,void *address,uint64_t size,uint32_t flags,MMKDarwinBO *out);
int32_t mmk_darwin_release(MMKDarwinBO *bo);
int32_t mmk_darwin_sync(const MMKDarwinBO *,uint64_t offset,uint64_t length,uint32_t direction);
int32_t mmk_darwin_submit(uint32_t connection,uint64_t chain,uint32_t slot,
                         const uint32_t *handles,uint32_t count,uint64_t dependency,
                         uint64_t *sequence);
int32_t mmk_darwin_wait(uint32_t connection,uint64_t sequence,uint64_t timeout_ns,MMKWaitResult *result);
#ifdef __cplusplus
}
#endif
#endif
