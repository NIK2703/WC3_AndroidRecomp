/*
 * host.h - WC3 static recompilation host bootstrap.
 *
 * The lifted modules use the CPU-struct ABI from
 * E:/projects/refs/pcrecomp/runtime/recomp32_cpu. This host maps the original
 * PE images at their preferred 32-bit VAs, binds imports to HLE thunks, and
 * dispatches lifted function addresses to L_<va>.
 *
 * Guest memory is allocated below 4 GiB at fixed addresses for the Windows
 * and first Android bootstrap. A based-memory allocator remains the portable
 * long-term option; no lifted code depends on host pointers being 64-bit.
 */
#ifndef WC3_HOST_H
#define WC3_HOST_H
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"
#include "pe_format.h"

#include "guest_layout.h"

typedef struct {
    uint32_t base;
    uint32_t image_base;
    uint32_t span;
    uint32_t entry;
    uint32_t import_rva;
    uint32_t export_rva, export_size;
    uint32_t reloc_rva, reloc_size;
    uint32_t resource_rva, resource_size;
    uint32_t stamp;
    const char *path;
} pe_image;

/* guest_mem.c: fixed low mappings. Returns host pointer or NULL. */
void *guest_map(uint32_t base, uint32_t size);
void  guest_unmap(uint32_t base, uint32_t size);
int   guest_commit(uint32_t base, uint32_t size);
/* Ranges committed *after* startup, i.e. not part of the fixed layout: the
 * D3D9 staging arena is VirtualAlloc'd in the gap immediately above game.dll,
 * so a validator built from the layout table alone rejects every pointer into
 * it.  guest_commit() records each committed range here. */
void  guest_range_add(uint32_t base, uint32_t size);
int   guest_range_ok(uint32_t base, uint32_t size);
uint32_t guest_range_clamp(uint32_t base, uint32_t n);

/* pe32.c */
int  pe_load(const char *path, uint32_t preferred_base, pe_image *out, int allow_relocate);
uint32_t pe_export(const pe_image *img, const char *name);
uint32_t pe_export_ordinal(const pe_image *img, uint16_t ordinal);
/* RT_STRING lookup: writes the id-th string of the module's string table into
 * out as ASCII (non-ASCII code units become '?') and returns its length, or 0
 * when the resource is missing. */
int  pe_string_resource(const pe_image *img, unsigned id, char *out, size_t cap);
int  pe_bind_imports(const pe_image *img,
                     uint32_t (*resolve)(const char *dll, const char *name,
                                         uint16_t ordinal, void *user),
                     void *user);

/* dispatch.c: generated table and HLE thunk range. */
void wc3_dispatch_init(void);
void wc3_dispatch_shutdown(void);

/* hle.c */
uint32_t hle_thunk(const char *dll, const char *name, uint16_t ordinal);
void     hle_call(uint32_t thunk, CPU *c);
/* Diagnostic: identifies the HLE entry that is currently executing, for
 * "unknown guest target" reports (returns "" when the address is not one). */
const char *hle_describe(uint32_t thunk);
int      hle_thunk_info(uint32_t thunk, char *buf, size_t n);
void     hle_init(void);
void     hle_start(const char *war3_path);
void     hle_set_war3_base(uint32_t base);
void     hle_set_storm_base(uint32_t base);
void     hle_set_ijl_base(uint32_t base);
void     hle_set_mss_base(uint32_t base);

/* Guest image registry.  The profiler resolves a sampled EIP through this:
 * the recompiled images live in guest memory, not in the host module list,
 * so toolhelp would report every guest address as unknown. */
void     hle_guest_module_add(uint32_t base, uint32_t span, const char *name);
const char *hle_guest_module_find(uint32_t addr, uint32_t *off);

/* TEB helpers */
void     guest_teb_init(CPU *c);
void     guest_teb_init_at(CPU *c, uint32_t teb, uint32_t stack_base,
                           uint32_t stack_size, uint32_t tid);

/* The CPU whose lifted code is currently executing on this host thread. */
void     hle_set_current_cpu(CPU *c);
CPU     *hle_current_cpu(void);

/* Diagnostic registry: every live guest CPU, for the WC3_PCTRACE sampler. */
void     hle_sample_register(CPU *c);
void     hle_sample_unregister(CPU *c);
int      hle_sample_snapshot(CPU **out, int max);

/* Diagnostic: guest thread identity, so host thread IDs sampled by the
 * profiler can be attributed to a guest start routine. */
typedef struct {
    uint32_t gid;
    uint32_t start;
    unsigned long host_tid;
    int running;
    /* Published live from the guest CPU struct, so the profiler can see where
     * a guest thread is without suspending it (suspending a thread that holds
     * a guest lock turns a profile into a deadlock). */
    uint32_t eip;
    uint32_t esp;
} HleThreadInfo;
int      hle_thread_info(HleThreadInfo *out, int max);
void     hle_publish_eip(uint32_t eip, uint32_t esp);
/* Diagnostic: WC3_FILE_IO=1 keeps a per-name open/read/byte count; the crash
 * handler calls this because atexit() never runs on an access violation. */
void     hle_fio_dump(void);

/* Diagnostic: per-HLE-entry call counts between dumps (WC3_SAMPLE only). */
void     hle_stats_enable(int on);
void     hle_stats_dump(void);

/* Guest call-chain ring buffer (main.c), fed by the generated dispatcher. */
void     wc3_trace_dispatch(uint32_t target, uint32_t esp, uint32_t ebp);
/* Diagnostic: WC3_PROFILE=1 ranks guest targets by dispatch count. */
void     wc3_profile_dump(void);
/* Diagnostic: print the guest call chain ring (main.c). */
void     wc3_dump_trace(const char *tag);
/* Host C stack depth guard (main.c): aborts with the dispatch ring before an
 * unbounded tail-jump chain exhausts the host stack. */
void     wc3_host_stack_guard(uint32_t target, uint32_t esp);
/* Diagnostic: WC3_RING_OUT=<path> rewrites the ring tail to that file every
 * few hundred HLE calls.  A fail-fast terminates the process without running
 * any handler, so this is the only way to keep the call chain that led to it. */
void     wc3_ring_spill(void);
/* Diagnostic: WC3_FRAME_OUT=<path> publishes the guest EIP/ESP and the host
 * C frame chain of the guest thread.  wc3_set_spill_cpu() names that thread. */
void     wc3_frame_spill(void);
void     wc3_set_spill_cpu(CPU *c);
/* Diagnostic: per-target probe selected by WC3_WATCH=<hex VA> (main.c).
 * g_dispatch_diag is zero unless one of the per-dispatch diagnostic envs is
 * set, so the generated dispatcher can skip the hook with one branch. */
extern int g_dispatch_diag;
void     wc3_dispatch_watch(CPU *c, uint32_t target);
/* Diagnostic: WC3_STACK_GUARD=1 verifies that every dispatch target returns
 * with a constant guest ESP delta.  Every non-varargs function releases the
 * return address plus its `ret imm16` operands, so the delta is fixed; the
 * first target that reports two different deltas is the one whose stack
 * contract is wrong.  Non-zero means the guard is armed. */
extern int g_stack_guard;
void     wc3_stack_balance(uint32_t target, uint32_t esp_in, uint32_t esp_out);
/* Diagnostic: resolve dwords on the guest stack into module+offset. */
void     wc3_dump_stack_candidates(uint32_t esp, unsigned dwords, const char *tag);
/* Diagnostic: WC3_HEAPDUMP=<prefix> + WC3_HEAPDUMP_FRAME=<f,f,...> writes every
 * committed, writable guest region to <prefix>_f<frame>.bin at those frames.
 * Called once per presented frame (hle_vk_end), because "what does the engine
 * hold after it reads a file" is a question about a moment, not about a log. */
void     wc3_heap_dump_tick(uint32_t frame);
/* Same snapshot, triggered by the n-th read that overlaps WC3_IO_RANGE.  The
 * guest stops presenting during a load, so reads are the only clock that
 * keeps running while the engine is decompressing. */
void     wc3_heap_dump_read(unsigned hit);

/* hle_user32.c: pump the real presentation window's messages. */
void     vu_host_pump(void);
void     vu_host_client_size(int *w, int *h);
/* Press at (x,y); the matching release comes from vu_host_release() on a
 * later presented frame, because the UI latches the press and activates on
 * the release of the next frame. */
void     vu_host_click(int x, int y);
void     vu_host_release(void);
/* The two halves of a click, so the caller can spread them over separate
 * frames: move parks the cursor, press sets the button, release lets go. */
void     vu_host_move(int x, int y);
void     vu_host_press(void);
/* Synthetic keyboard: down/up are separate calls so the caller can hold the
 * key across consecutive presents (the credits prompt latches an edge). */
void     vu_host_key_down(unsigned vk);
void     vu_host_key_up(unsigned vk);
/* Deliver the activation/focus a real click implies (WM_ACTIVATEAPP,
 * WM_ACTIVATE, WM_SETFOCUS) and point the virtual focus queries at the guest
 * window.  Without it a guest that gates input on "am I active" ignores every
 * synthetic click. */
void     vu_host_activate(void);
/* Mouse messages that reached the guest queue / were dropped for lack of a
 * mirrored guest window.  Printed next to the synthetic click so a run says
 * whether the click was lost or ignored. */
void     vu_host_input_stats(unsigned *moves, unsigned *downs, unsigned *ups,
                             unsigned *dropped);

extern uint32_t g_image_delta;

#endif
