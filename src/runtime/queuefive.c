/* Queue Five, part of Modern controls (docs/modern-controls.md): copy extra production events into the
 * active game's event ring. */
#include <stdint.h>
#include <string.h>

#include "recomp_target.h"

/* Guest addresses as host pointers: the same on Windows; through win32hle
 * off it, where on a 64-bit host (macOS) the guest's memory is one block. */
#ifdef _WIN32
#define GPTR(va) ((void *)(uintptr_t)(uint32_t)(va))
#else
#include "win32hle.h"
#define GPTR(va) hle_ptr((uint32_t)(va))
#endif

#define QUEUE_FIVE_EVENT_COUNT 0x80u
#define QUEUE_FIVE_EVENT_MASK  0x7Fu
#define QUEUE_FIVE_EVENT_SIZE  111u

int ra2_queue_five_append(uint32_t event_va, uint32_t timestamp) {
    volatile uint32_t *count = (volatile uint32_t *)GPTR(RA2_QUEUE_FIVE_COUNT_VA);
    volatile uint32_t *index = (volatile uint32_t *)GPTR(RA2_QUEUE_FIVE_INDEX_VA);
    volatile uint32_t *stamps = (volatile uint32_t *)GPTR(RA2_QUEUE_FIVE_STAMPS_VA);
    uint32_t slot;

    if (!event_va || *count >= QUEUE_FIVE_EVENT_COUNT) return 0;

    slot = *index & QUEUE_FIVE_EVENT_MASK;
    memcpy(GPTR(RA2_QUEUE_FIVE_RING_VA + slot * QUEUE_FIVE_EVENT_SIZE),
           GPTR(event_va), QUEUE_FIVE_EVENT_SIZE);
    stamps[slot] = timestamp;
    *index = (slot + 1u) & QUEUE_FIVE_EVENT_MASK;
    ++*count;
    return 1;
}
