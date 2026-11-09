/*
 * init_array_probe.c - debug-only constructor counter (O-44).
 *
 * Counted at constructor priority 101, before the default-priority DXVK
 * constructors.  The probe reads DT_INIT_ARRAYSZ from the loaded object's
 * dynamic table, so it does not depend on linker-provided start/end symbols.
 */
#include "init_array_probe.h"

typedef int wc3_init_array_probe_translation_unit;

#if WC3_DEBUG && WC3_DEBUG_BOOT

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct InitArrayCount {
    uintptr_t base;
    uint32_t count;
    int found;
} InitArrayCount;

static uint32_t g_init_array_entries;

static int is_own_library(const struct dl_phdr_info *info)
{
    const char *name = info->dlpi_name;
    if (!name || !*name) return 0;
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    return strcmp(leaf, "libwc3gameactivity.so") == 0;
}

static int count_init_array_cb(struct dl_phdr_info *info, size_t size,
                               void *data)
{
    (void)size;
    InitArrayCount *out = (InitArrayCount *)data;
    if (out->base != 0u) {
        if ((uintptr_t)info->dlpi_addr != out->base) return 0;
    } else if (!is_own_library(info)) {
        return 0;
    }

    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_DYNAMIC) continue;

        const ElfW(Dyn) *dyn =
            (const ElfW(Dyn) *)((uintptr_t)info->dlpi_addr + ph->p_vaddr);
        size_t bytes = 0;
        for (; dyn->d_tag != DT_NULL; dyn++) {
            if (dyn->d_tag == DT_INIT_ARRAYSZ) {
                bytes = (size_t)dyn->d_un.d_val;
            }
        }
        if (bytes > 0u) {
            out->count = (uint32_t)(bytes / sizeof(void *));
        }
        out->found = 1;
        return 1;
    }
    return 0;
}

/* A default-visibility anchor lets dladdr() find this shared object without
 * depending on any product symbol being exported in a diagnostic build. */
__attribute__((used))
void wc3_init_array_probe_anchor(void) {}

static void __attribute__((constructor(101))) wc3_init_array_probe_ctor(void)
{
    InitArrayCount out = {0u, 0u, 0};
    Dl_info dl;
    if (dladdr((void *)wc3_init_array_probe_anchor, &dl) && dl.dli_fbase) {
        out.base = (uintptr_t)dl.dli_fbase;
    }
    (void)dl_iterate_phdr(count_init_array_cb, &out);
    g_init_array_entries = out.found ? out.count : 0u;
}

uint32_t wc3_init_array_count(void)
{
    return g_init_array_entries;
}

#endif /* WC3_DEBUG && WC3_DEBUG_BOOT */
