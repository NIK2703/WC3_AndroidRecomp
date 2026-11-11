/*
 * dxvk3_gate.h - Turnip package policy for DXVK 3.1.1.
 *
 * The package labelled "Mesa Turnip driver v26.0.0 - R8" is built from Mesa
 * 5ac41be677 (2026-01-08) and reports driver version 25.99.99.  Its
 * ir3_nir_opt_prefetch_descriptors pass can leave a NULL insertion anchor:
 * DXVK 3.1.1 emits a texture access whose sampler and texture descriptors are
 * defined in different blocks, find_insert_block_for_defs() returns NULL, and
 * the release build dereferences it in nir_instr_insert().
 *
 * Mesa fixed this in 44b82c7576d ("ir3/opt_prefetch_descriptors: rematerialize
 * defs at preamble start", 2026-06-19).  The fix is not in the R8 package.
 *
 * Automatic selection keeps a package below Turnip 26.0.0 on DXVK 1.10.3.
 * A forced DXVK 3.1.1 is still honoured: instead of falling back, the entry
 * layer sets IR3_SHADER_DEBUG=nodescprefetch before Turnip compiles shaders.
 * That disables only the buggy descriptor-prefetch optimization; DXVK 3.1.1
 * itself runs unchanged.
 */
#ifndef WC3_DXVK3_GATE_H
#define WC3_DXVK3_GATE_H

#include <stdint.h>

/* VK_MAKE_VERSION(26, 0, 0) */
#define WC3_DXVK3_MIN_PACKAGE_DRIVER 0x06800000u

#define WC3_TURNIP_SHADER_DEBUG_ENV "IR3_SHADER_DEBUG"
#define WC3_TURNIP_NODESCPREFETCH "nodescprefetch"

/*
 * True for an active package below the first known-good boundary.  If the
 * capability probe failed, stay conservative for an active package; a
 * missing active id means the system driver, which this policy does not
 * classify.
 */
static inline int wc3_dxvk3_package_needs_nodescprefetch(
    const char *active_id, int have_caps, uint32_t driver_version)
{
    return active_id && active_id[0] &&
           (!have_caps || driver_version < WC3_DXVK3_MIN_PACKAGE_DRIVER);
}

#endif /* WC3_DXVK3_GATE_H */
