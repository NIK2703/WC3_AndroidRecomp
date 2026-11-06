/*
 * hle_registry.h - the import registry shared by the Android C runtime and the
 * C++ translation units that are linked into the same image.
 *
 * hle_android.c owns the table.  hle_d3d9.cpp (the guest Direct3D 9 COM
 * surface, the same file the Windows host links) needs three things from it:
 * the HleEnt layout, a way to append an entry, and a way to turn a thunk
 * address back into its entry.  Both sides must agree on the *layout* of
 * HleEnt, so it is declared exactly once, here, and included by both.
 *
 * The Windows host has its own copy of this contract in runtime/host/
 * hle_internal.h; the two tables are deliberately not interchangeable (the
 * Android one is smaller and carries a call counter), which is why
 * hle_internal.h selects this header instead of trying to share a struct.
 */
#ifndef WC3_ANDROID_HLE_REGISTRY_H
#define WC3_ANDROID_HLE_REGISTRY_H

#include <stdint.h>

#include "host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 4096 rather than the host's value: the D3D9 layer alone registers one entry
 * per COM method slot (324 of them) on top of the OS surface. */
#define HLE_MAX 4096
#define HLE_STRIDE GUEST_HLE_STRIDE

typedef void (*HleFn)(CPU *);

typedef struct {
    char     dll[24];
    char     name[80];
    uint16_t ord;
    uint16_t argc;
    uint8_t  cdecl_abi;
    uint8_t  used;
    HleFn    fn;
    uint32_t calls;
    /* Per-slot back-pointer.  hle_d3d9.cpp stores its own Slot descriptor
     * here so its entry point can recover (interface, method index) from the
     * thunk address alone; nothing else uses it.  Last field on purpose: every
     * other accessor keeps working when it was added. */
    void    *fwd;
} HleEnt;

/* Defined in hle_android.c.  `reg` is the name the Windows host's D3D9 layer
 * calls; on Android it is the same append-only registration as reg_ent(). */
extern HleEnt   g_hle[HLE_MAX];
extern unsigned g_hle_count;

HleEnt *reg(const char *dll, const char *name, uint16_t ord, HleFn fn,
            int argc, int cdecl_abi);
HleEnt *hle_entry(uint32_t thunk);

const char *hle_describe(uint32_t thunk);

/* Guest memory helpers (hle_android.c).  Non-static because the D3D9 layer
 * decodes guest arguments with the same code the rest of the HLE uses.
 *
 * arg32 is deliberately *not* declared here: hle_d3d9.cpp carries its own
 * static copy, and a file-scope `static` definition after a non-static
 * declaration of the same name is a hard error in C++. */
uint32_t rd32g(uint32_t a);
void     wr32g(uint32_t a, uint32_t v);

/* Guest heap. */
uint32_t g_alloc0(uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
