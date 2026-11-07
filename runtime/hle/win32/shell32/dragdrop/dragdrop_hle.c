/*
 * dragdrop_hle.c - target-side SHELL32 drag/drop absence.
 *
 * Android has no OLE drop source and the target window never receives a
 * WM_DROPFILES message, so the HDROP list is genuinely empty.  DragQueryFileA
 * therefore reports zero files for both the count query and any name query;
 * DragAcceptFiles has no host drop target to register and no return value.
 */
#include "dragdrop_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

#define DRAG_QUERY_FILE_COUNT 0xFFFFFFFFu

static void h_drag_query_file(CPU *c)
{
    const uint32_t file_index = wc3_legacy_arg32(c, 1u);
    (void)wc3_legacy_arg32(c, 0u);  /* HDROP: no drop source can create one */
    (void)wc3_legacy_arg32(c, 2u);  /* name buffer is never written */
    (void)wc3_legacy_arg32(c, 3u);
    (void)file_index;
    c->eax = 0u;
}

static void h_drag_accept_files(CPU *c)
{
    (void)wc3_legacy_arg32(c, 0u);
    (void)wc3_legacy_arg32(c, 1u);
    c->eax = 0u;
}

typedef struct DragDropAddition {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} DragDropAddition;

static const DragDropAddition g_additions[] = {
    { "DragAcceptFiles", 2, h_drag_accept_files },
    { "DragQueryFileA", 4, h_drag_query_file },
};

int wc3_hle_shell32_dragdrop_install(void)
{
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("SHELL32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
