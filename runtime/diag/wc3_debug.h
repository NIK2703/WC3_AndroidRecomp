/*
 * wc3_debug.h - master debug switch and the single registry of debug tags.
 *
 * Normative owner: docs/BUILD_VARIANTS.md (BV-5/BV-6/BV-7, ADR-030).
 *
 * Rules:
 *   - WC3_DEBUG is the master switch.  It is derived from WC3_VARIANT by the
 *     build system, never from a local #define in a translation unit.
 *   - Every diagnostic instruction is guarded by
 *         #if WC3_DEBUG && WC3_DEBUG_<TARGET>
 *     where the tag names the subsystem being observed, not the directory the
 *     file lives in (BV-6.1).
 *   - Use #if, never #ifdef: WC3_DEBUG is always *defined* (0 or 1), so an
 *     #ifdef guard would be true in release and ship debug code (BV-5.1).
 *   - Tags default to 0 here.  The build enables a subset with
 *     -DWC3_DEBUG_<TAG>=1 through WC3_DEBUG_TAGS; release enables none.
 *   - This header is host-owned: including it must not change any
 *     guest-visible struct layout or the HLE registry order (BV-9).
 *
 * tools/check_debug_tags.py parses the WC3_DEBUG_TAG_* definitions below and
 * is the T0 owner of "every tag is declared exactly once, no unknown tags".
 */
#ifndef WC3_DIAG_WC3_DEBUG_H
#define WC3_DIAG_WC3_DEBUG_H

/* Standalone/unit builds have no WC3_VARIANT: behave like release. */
#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

/* --- Tag registry: one default per subsystem --------------------------- */
#if !defined(WC3_DEBUG_BOOT)
#define WC3_DEBUG_BOOT 0
#endif
#if !defined(WC3_DEBUG_ASSETS)
#define WC3_DEBUG_ASSETS 0
#endif
#if !defined(WC3_DEBUG_MEMORY)
#define WC3_DEBUG_MEMORY 0
#endif
#if !defined(WC3_DEBUG_CPU)
#define WC3_DEBUG_CPU 0
#endif
#if !defined(WC3_DEBUG_HLE)
#define WC3_DEBUG_HLE 0
#endif
#if !defined(WC3_DEBUG_HLE_SYNC)
#define WC3_DEBUG_HLE_SYNC 0
#endif
#if !defined(WC3_DEBUG_HLE_SLEEP)
#define WC3_DEBUG_HLE_SLEEP 0
#endif
#if !defined(WC3_DEBUG_NET)
#define WC3_DEBUG_NET 0
#endif
#if !defined(WC3_DEBUG_THREADS)
#define WC3_DEBUG_THREADS 0
#endif
#if !defined(WC3_DEBUG_GRAPHICS)
#define WC3_DEBUG_GRAPHICS 0
#endif
#if !defined(WC3_DEBUG_RESOLUTION)
#define WC3_DEBUG_RESOLUTION 0
#endif
#if !defined(WC3_DEBUG_VULKAN)
#define WC3_DEBUG_VULKAN 0
#endif
#if !defined(WC3_DEBUG_AUDIO)
#define WC3_DEBUG_AUDIO 0
#endif
#if !defined(WC3_DEBUG_INPUT)
#define WC3_DEBUG_INPUT 0
#endif
#if !defined(WC3_DEBUG_SAVE)
#define WC3_DEBUG_SAVE 0
#endif
#if !defined(WC3_DEBUG_JASS)
#define WC3_DEBUG_JASS 0
#endif
#if !defined(WC3_DEBUG_JNI)
#define WC3_DEBUG_JNI 0
#endif
#if !defined(WC3_DEBUG_CRASH)
#define WC3_DEBUG_CRASH 0
#endif
#if !defined(WC3_DEBUG_PERF)
#define WC3_DEBUG_PERF 0
#endif

/*
 * BV-5.3: a release build must not activate any tag.  The guard expression
 * `WC3_DEBUG && WC3_DEBUG_X` already makes such tags inert, but failing at
 * compile time keeps a misconfigured build out of the artifact instead of
 * silently producing dead code.
 */
#if !WC3_DEBUG && ( \
    WC3_DEBUG_BOOT || WC3_DEBUG_ASSETS || WC3_DEBUG_MEMORY || \
    WC3_DEBUG_CPU || WC3_DEBUG_HLE || WC3_DEBUG_HLE_SYNC || \
    WC3_DEBUG_HLE_SLEEP || WC3_DEBUG_NET || WC3_DEBUG_THREADS || \
    WC3_DEBUG_GRAPHICS || \
    WC3_DEBUG_RESOLUTION || WC3_DEBUG_VULKAN || WC3_DEBUG_AUDIO || \
    WC3_DEBUG_INPUT || WC3_DEBUG_SAVE || WC3_DEBUG_JASS || \
    WC3_DEBUG_JNI || WC3_DEBUG_CRASH || WC3_DEBUG_PERF)
#error "WC3_DEBUG=0 but a WC3_DEBUG_<TAG> is enabled; see BV-5.3"
#endif

#endif /* WC3_DIAG_WC3_DEBUG_H */
