/* BSurface pixel read, native fast path (run_lift.py bsurface_fast_patches).
 * From lynx8989's patch (issue #67).
 *
 * Get_Pixel(POINT*) (Surface slot 10: 0x007BAE60 in gamemd.exe, 0x007741D0
 * in game.exe) is what the voxel plotter calls twice per sample. Lifted, it
 * is six virtual calls: Lock (which calls Bytes_Per_Pixel and Get_Pitch,
 * which calls Get_Width), Bytes_Per_Pixel again, Unlock. For a plain
 * BSurface those are field reads: width [this+4], the lock count [this+0xC]
 * (up and down again), bpp [this+0x10], buffer [this+0x14]. Only when the
 * vtable still holds BSurface's own methods for slots 23, 24, 28, 29 and 31
 * (XSurface and DSurface override some) does this answer; otherwise, or with
 * a null buffer, it returns 0 and the lifted code runs unchanged. */
#include <stdint.h>

#include "recomp_target.h"

#ifdef _WIN32
#define GUEST_PTR(va) ((void *)(uintptr_t)(uint32_t)(va))
#else
#include "win32hle.h"
#define GUEST_PTR(va) hle_ptr((uint32_t)(va))
#endif
#define G32(va) (*(volatile uint32_t *)GUEST_PTR(va))
#define G16(va) (*(volatile uint16_t *)GUEST_PTR(va))
#define G8(va)  (*(volatile uint8_t *)GUEST_PTR(va))

#ifdef RA2_TARGET_GAME
#define BSURF_LOCK   0x00411370u   /* slot 23 */
#define BSURF_UNLOCK 0x004112F0u   /* slot 24 */
#define BSURF_BPP    0x004113B0u   /* slot 28 */
#define BSURF_PITCH  0x004113C0u   /* slot 29 */
#define BSURF_WIDTH  0x004112C0u   /* slot 31, Surface's */
#else
#define BSURF_LOCK   0x004115F0u
#define BSURF_UNLOCK 0x00411570u
#define BSURF_BPP    0x00411630u
#define BSURF_PITCH  0x00411640u
#define BSURF_WIDTH  0x00411540u
#endif

int ra2_bsurf_getpixel(uint32_t self, uint32_t pt, uint32_t *out) {
    uint32_t vt = G32(self), bpp, base, addr;
    if (G32(vt + 0x5C) != BSURF_LOCK || G32(vt + 0x60) != BSURF_UNLOCK ||
        G32(vt + 0x70) != BSURF_BPP || G32(vt + 0x74) != BSURF_PITCH ||
        G32(vt + 0x7C) != BSURF_WIDTH)
        return 0;
    base = G32(self + 0x14);
    if (!base) return 0;
    bpp = G32(self + 0x10);
    addr = base + G32(pt) * bpp + G32(pt + 4) * (G32(self + 4) * bpp);
    *out = bpp == 2 ? (uint32_t)G16(addr) : (uint32_t)G8(addr);
    return 1;
}
