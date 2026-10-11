/* Queue Five: copy extra production events into Yuri's Revenge's event ring. */
#include <stdint.h>
#include <string.h>

#include "recomp_target.h"

#define QUEUE_FIVE_EVENT_COUNT 0x80u
#define QUEUE_FIVE_EVENT_MASK  0x7Fu
#define QUEUE_FIVE_EVENT_SIZE  111u

#if defined(RA2_TARGET_GAMEMD)
#define QUEUE_FIVE_COUNT_ADDR  0x00A802C8u
#define QUEUE_FIVE_INDEX_ADDR  0x00A802D0u
#define QUEUE_FIVE_RING_ADDR   0x00A802D4u
#define QUEUE_FIVE_STAMPS_ADDR 0x00A83A54u

int ra2_queue_five_append(uint32_t event_va, uint32_t timestamp) {
    volatile uint32_t *count = (volatile uint32_t *)(uintptr_t)QUEUE_FIVE_COUNT_ADDR;
    volatile uint32_t *index = (volatile uint32_t *)(uintptr_t)QUEUE_FIVE_INDEX_ADDR;
    volatile uint32_t *stamps = (volatile uint32_t *)(uintptr_t)QUEUE_FIVE_STAMPS_ADDR;
    uint32_t slot;

    if (!event_va || *count >= QUEUE_FIVE_EVENT_COUNT) return 0;

    slot = *index & QUEUE_FIVE_EVENT_MASK;
    memcpy((void *)(uintptr_t)(QUEUE_FIVE_RING_ADDR + slot * QUEUE_FIVE_EVENT_SIZE),
           (const void *)(uintptr_t)event_va, QUEUE_FIVE_EVENT_SIZE);
    stamps[slot] = timestamp;
    *index = (slot + 1u) & QUEUE_FIVE_EVENT_MASK;
    ++*count;
    return 1;
}
#else
/* The Queue Five hook is currently installed only for the gamemd target. */
int ra2_queue_five_append(uint32_t event_va, uint32_t timestamp) {
    (void)event_va;
    (void)timestamp;
    return 0;
}
#endif
