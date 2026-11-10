/*
 * W3L / PvPGN / rubattle compatibility helpers.
 *
 * This module is a build-profile compat shim, not part of the exact baseline
 * and not HLE: it reproduces effects that wl27.dll applies to Game.dll.
 * The generated corpus is changed only by tools/lift/passes/w3l_compat.py;
 * this header is force-included in compat builds so the pass can emit direct
 * calls to the native helpers below.
 *
 * The guest ABI is unchanged: no CPU fields are added and no dispatch
 * signature changes.  The header is safe to include before generated C.
 */
#ifndef WC3_COMPAT_W3L_COMPAT_H
#define WC3_COMPAT_W3L_COMPAT_H

#include <stdint.h>

#include "cpu.h"

/* wl27 reads latency.txt with a default of 100 and clamps it to 30..255.
 * A compat build makes it a compile-time profile value. */
#ifndef WC3_W3L_LATENCY
#define WC3_W3L_LATENCY 100
#endif
#if WC3_W3L_LATENCY < 30 || WC3_W3L_LATENCY > 255
#error "WC3_W3L_LATENCY must be in the wl27 range 30..255"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hash the account field at src+0x20 with the PvPGN logon-proof variant used
 * by wl27 wrapper 0x100018B0:
 *   - copy 16 bytes, lowercase ASCII A..Z, stop at the first NUL;
 *   - one Blizzard/PvPGN SHA-1-like compression with the non-standard
 *     ROTL32(1, xor) word expansion;
 *   - write 20 little-endian state bytes to src+0xA8 and dst;
 *   - return 1 and do not write dst+0x14.
 *
 * The C implementation is independent of the wl27 binary; the public W3L /
 * PvPGN source is behavioral reference only.
 */
uint32_t wc3_w3l_pvpgn_hash(CPU *c, uint32_t dst, uint32_t src);

/*
 * Handle rubattle packet 0x83 from inside lifted L_6F8DC630.
 *
 * The caller has already loaded and zero-extended the packet id into c->eax
 * and still holds the dispatcher frame in c->ebp.  On every path this helper
 * returns with the dispatcher frame unwound exactly as the original jmp-table
 * handler does: saved ebx/esi/edi restored, ebp popped, esp = old_ebp + 0x18
 * after `ret 0x10`, and eax = 1.  The sole exception is a null incoming ecx
 * (send object), where wl27 worker 0x10001B30 returns eax = 0.  The caller
 * must return immediately.
 */
void wc3_w3l_handle_0x83(CPU *c);

#ifdef __cplusplus
}
#endif

#endif /* WC3_COMPAT_W3L_COMPAT_H */
