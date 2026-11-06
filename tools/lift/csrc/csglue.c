/* csglue.c - one instance of the dynamically loaded capstone entry points. */
#include "csglue.h"

CsGlue csg;

/* Directory of the running executable, without the trailing separator. */
static void csg_exe_dir(char *out, size_t cap)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    for (DWORD i = n; i > 0; i--) {
        if (out[i - 1] == '\\' || out[i - 1] == '/') { out[i - 1] = 0; return; }
    }
    out[0] = 0;
}

int csg_load(void)
{
    char dir[MAX_PATH];
    if (csg.lib) return 0;
    csg_exe_dir(dir, sizeof dir);
    snprintf(csg.dll_path, sizeof csg.dll_path, "%s\\capstone.dll", dir);
    csg.lib = LoadLibraryA(csg.dll_path);
    if (!csg.lib) {
        /* Fall back to the plain name in case the DLL sits in the cwd or on
         * PATH; the error message names the path that was tried. */
        csg.lib = LoadLibraryA("capstone.dll");
        if (!csg.lib) {
            fprintf(stderr, "[!] cannot load %s (error %lu)\n",
                    csg.dll_path, (unsigned long)GetLastError());
            return -1;
        }
    }
    /* GetProcAddress returns a generic FARPROC; the cast is the documented way
     * to turn it into a typed pointer. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    csg.open        = (cs_open_fn)GetProcAddress(csg.lib, "cs_open");
    csg.option      = (cs_option_fn)GetProcAddress(csg.lib, "cs_option");
    csg.close       = (cs_close_fn)GetProcAddress(csg.lib, "cs_close");
    csg.malloc_insn = (cs_malloc_fn)GetProcAddress(csg.lib, "cs_malloc");
    csg.iter        = (cs_disasm_iter_fn)GetProcAddress(csg.lib, "cs_disasm_iter");
    csg.free_insn   = (cs_free_fn)GetProcAddress(csg.lib, "cs_free");
    csg.reg_name    = (cs_reg_name_fn)GetProcAddress(csg.lib, "cs_reg_name");
    csg.version     = (cs_version_fn)GetProcAddress(csg.lib, "cs_version");
    csg.strerror    = (cs_strerror_fn)GetProcAddress(csg.lib, "cs_strerror");
#pragma GCC diagnostic pop
    if (!csg.open || !csg.option || !csg.close || !csg.malloc_insn ||
        !csg.iter || !csg.free_insn || !csg.reg_name) {
        fprintf(stderr, "[!] %s is missing entry points\n", csg.dll_path);
        return -1;
    }
    return 0;
}

/* 32-bit x86 with detail on.  `skipdata` mirrors the Python configuration of
 * the caller: the lifter runs with it OFF (capstone stops at undecodable
 * bytes, which is what the Python Lifter does), extent recovery runs with it
 * ON so a data run yields a one-byte `.byte` pseudo-instruction instead of
 * ending the walk. */
int csg_open_x86_ex(csh *handle, int skipdata)
{
    if (csg_load() != 0) return -1;
    if (csg.open(CS_ARCH_X86, CS_MODE_32, handle) != CS_ERR_OK) {
        fprintf(stderr, "[!] cs_open failed\n");
        return -1;
    }
    csg.option(*handle, CS_OPT_DETAIL, CS_OPT_ON);
    if (skipdata) csg.option(*handle, CS_OPT_SKIPDATA, CS_OPT_ON);
    return 0;
}

int csg_open_x86(csh *handle) { return csg_open_x86_ex(handle, 0); }
