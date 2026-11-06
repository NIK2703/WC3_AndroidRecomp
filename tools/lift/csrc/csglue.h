/* csglue.h - load capstone.dll at runtime.
 *
 * The lift tools decode x86 with the same engine the Python lifter used, so an
 * instruction boundary, an operand size or a `skipdata` decision cannot differ
 * between the two implementations.  The DLL is loaded from the tool's own
 * directory, which keeps the executables free of an import library and lets the
 * skill ship one copy next to the binaries.
 *
 * Nothing here calls the exported capstone symbols directly; the header is
 * included for the struct layouts only.  The loaded function table lives in
 * csglue.c so that a multi-file tool shares one instance.
 */
#ifndef WC3_CSGLUE_H
#define WC3_CSGLUE_H

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "capstone/capstone.h"

typedef cs_err (__cdecl *cs_open_fn)(cs_arch, cs_mode, csh *);
typedef cs_err (__cdecl *cs_option_fn)(csh, cs_opt_type, size_t);
typedef cs_err (__cdecl *cs_close_fn)(csh *);
typedef cs_insn *(__cdecl *cs_malloc_fn)(csh);
typedef bool (__cdecl *cs_disasm_iter_fn)(csh, const uint8_t **, size_t *,
                                          uint64_t *, cs_insn *);
typedef void (__cdecl *cs_free_fn)(cs_insn *, size_t);
typedef const char *(__cdecl *cs_reg_name_fn)(csh, unsigned int);
typedef unsigned int (__cdecl *cs_version_fn)(int *, int *);
typedef const char *(__cdecl *cs_strerror_fn)(cs_err);

typedef struct {
    HMODULE lib;
    cs_open_fn open;
    cs_option_fn option;
    cs_close_fn close;
    cs_malloc_fn malloc_insn;
    cs_disasm_iter_fn iter;
    cs_free_fn free_insn;
    cs_reg_name_fn reg_name;
    cs_version_fn version;
    cs_strerror_fn strerror;
    char dll_path[MAX_PATH];
} CsGlue;

extern CsGlue csg;

int csg_load(void);
int csg_open_x86_ex(csh *handle, int skipdata);
int csg_open_x86(csh *handle);

#endif /* WC3_CSGLUE_H */
