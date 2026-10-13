// SPDX-License-Identifier: Apache-2.0
#ifndef OF_HEAP_H
#define OF_HEAP_H

#include <stdint.h>
#include <stddef.h>
#include "of_services.h"

/* Positive only when the OS can prove an immediate fresh mmap allocation
 * fits the untouched gap and has a tracking slot. This does not reserve
 * memory or inspect musl's free lists; add allocator headroom to the payload.
 * Zero (including unsupported firmware) requires the caller's normal malloc
 * probe. Call with app allocation quiescent; never cache a positive result. */
static inline int of_heap_can_reserve(uint32_t bytes)
{
#ifndef OF_PC
    const struct of_services_table *svc = OF_SVC;
    const uint32_t index = (uint32_t)(
        (offsetof(struct of_services_table, heap_can_reserve) - 12u) / sizeof(void *));
    if (!bytes || !svc || svc->magic != OF_SVC_MAGIC ||
        svc->version != OF_SVC_VERSION || svc->count <= index ||
        !svc->heap_can_reserve)
        return 0;
    return svc->heap_can_reserve(bytes) > 0;
#else
    (void)bytes;
    return 0;
#endif
}

#endif /* OF_HEAP_H */
