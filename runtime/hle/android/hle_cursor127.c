/*
 * hle_cursor127.c - optional suppression of the guest's own cursor sprite.
 *
 * The cursor is one named image
 * -----------------------------
 * The game draws its mouse pointer with a sprite whose draw target is an
 * image that it resolves by name in the sprite registry (L_6F342220) and
 * hands to the sprite submit function (L_6F1A47B0):
 *
 *   GVA(0x6FAA9C1C) = "Cursor".  One image that carries the cursor
 *   animation, submitted by L_6F2E0340 (menu), by the four cursor setups
 *   L_6F2DD126/32/48/5D and by L_6F367B43/45 (game UI).
 *
 * Do not confuse it with the adjacent names in the same read-only block:
 *
 *   0x6FAC03D0 = "MinimapIndicator"
 *   0x6FAC03E4 = "MinimapWaypoint"
 *   0x6FAC03F4 = "MinimapCreepCamp"
 *   0x6FAC0408 = "MinimapCreepCampTough"
 *   0x6FABB838 = "TargetPointConfirm"
 *
 * The first four are the player's point markers and the creep-camp icons on
 * the minimap; L_6F39F8C0 builds their cells with the same
 * resolve-name-then-submit sequence as the cursor.  Suppressing those names
 * removes the minimap icons as well, which is not what this option is for.
 * Only "Cursor" is in the suppression table.
 *
 * How a cursor draw is recognised
 * -------------------------------
 * Every cursor render of the corpus resolves the image by name and submits
 * that very image as the next thing it does:
 *
 *     ecx = name;      L_6F342220()      // image = the guest's own lookup
 *     edx = image;     L_6F1A47B0()      // submit it as the draw target
 *
 * so the draw is recognised by that sequence alone.  A resolution of the
 * cursor name marks the next submission, which is dropped; a resolution of
 * any other name clears the mark, so a sprite submitted without being the
 * cursor is never touched.  On the device this catches every cursor draw:
 * the menu at 0x6F2E0375, the game UI at 0x6F367E7B and the four setups at
 * 0x6F2DD282.  The minimap names clear the mark instead of setting it.
 *
 * Two other signals were tried and are deliberately not used.
 *
 *   - The value the lookup returns cannot identify the cursor.  L_6F342220
 *     has two exits: the direct one returns the registry value in EAX, but the
 *     other leaves through dispatch_jmp(c, 0x6F342258), a deferred tail that
 *     produces the image only after the lifted body has returned.  A wrapper
 *     therefore still sees the old EAX - on the device the lookup of
 *     0x6FAA9C1C reported 0 while the next submit drew 0x61E63F08.
 *
 *   - The objects of a cursor draw cannot identify later draws either.  The
 *     guest reuses both image objects and sprite objects: the menu cursor
 *     submits 0x61E63F08 through sprite 0x628E008C, and the loading screen
 *     later submits its own elements (0x61E63B50, 0x61E67798, 0x61E677E0)
 *     through those same sprite objects.  Filtering on them turned the
 *     loading screen black, and it never matched a single cursor draw that
 *     the sequence above had not matched already.
 *
 * How the drawing is cut
 * ----------------------
 * L_6F1A47B0 takes the draw target in EDX and starts with
 *
 *     L_6F1A0FA0(sprite);         // reset the sprite
 *     if (target == 0) return;    // nothing to draw into
 *
 * and otherwise builds the render element, uploads the geometry and issues
 * the draw call.  A NULL target is therefore the guest's own "draw nothing"
 * case: the sprite is reset, no render element is built, no geometry is
 * uploaded and no draw call reaches the renderer.  The cursor is not drawn
 * at all.
 *
 * Nothing about the guest's data structures is changed.  The image stays
 * registered, the sprite is still created, still advanced, still stored in
 * the frame cell and still released - only the submission is dropped.
 *
 * What must keep running
 * ----------------------
 * The UI reads the frame's sprite back with GetSprite (L_6F0F7060) and
 * dereferences it without a NULL check, so the render must not be skipped: an
 * empty sprite cell faults the next UI frame - that was the SIGSEGV at 0x30
 * with ECX=0 in L_6F1A42B0 on the device.  With the submit suppressed the
 * sprite still exists and is still registered, so the frame contract holds.
 *
 * The link uses --wrap so neither the generated dispatcher nor the generated
 * corpus is edited.
 */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "wc3_debug.h"

void __real_L_6F342220(CPU *c);
void __real_L_6F1A47B0(CPU *c);

/* The real cursor image name in game.dll's read-only data.  The value is the
 * absolute VA of the "Cursor" string; the live address is GVA() of it, so the
 * table stays usable in a relocatable build.  The adjacent Minimap* names are
 * deliberately absent: they are minimap sprites, not cursor states. */
#define WC3_CURSOR_NAME_COUNT 1
static const uint32_t g_cursor_names[WC3_CURSOR_NAME_COUNT] = {
    0x6FAA9C1Cu, /* Cursor: menu and game UI pointer */
};

/* Build invariant: the product link forces this member in and checks the
 * marker, so a configuration that loses the cursor wrapper fails visibly
 * instead of silently drawing the cursor again. */
#if defined(__GNUC__)
__attribute__((used))
#endif
const char wc3_cursor127_build_marker[] = "WC3_HIDE_CURSOR";

/* Non-zero while the next submission is the draw of the cursor image that was
 * just resolved.  Only the render thread touches the mark, but the wrappers
 * may run on different guest threads, so it is updated atomically. */
static uint32_t g_cursor_pending;

/* The launch setting is latched into the process environment before
 * android_main starts.  The render thread reads it in a hot path, so cache the
 * answer and never call getenv more than once. */
static int g_hide_cursor = -1;

static int cursor_hidden(void)
{
    const char *setting;
    int value = __atomic_load_n(&g_hide_cursor, __ATOMIC_ACQUIRE);

    if (value >= 0)
        return value;

    setting = getenv("WC3_HIDE_CURSOR");
    value = setting && setting[0] == '1' && setting[1] == '\0' ? 1 : 0;
    __atomic_store_n(&g_hide_cursor, value, __ATOMIC_RELEASE);
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    fprintf(stderr, "[graphics] hide cursor setting=%d env=%s\n", value,
            setting ? setting : "(unset)");
#endif
    return value;
}

static int cursor_name_matches(uint32_t name)
{
    int i;

    for (i = 0; i < WC3_CURSOR_NAME_COUNT; i++) {
        if (name == GVA(g_cursor_names[i]))
            return 1;
    }
    return 0;
}

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
/* The diagnostics below are bounded and de-duplicated: a run costs a few
 * dozen lines, and a cursor that is still visible can be traced to the call
 * site that draws it. */
#define WC3_CURSOR_NAME_LOG 16
#define WC3_CURSOR_SUBMIT_LOG 64

static uint32_t g_name_log[WC3_CURSOR_NAME_LOG][2]; /* name, resolved value */
static int g_name_log_count;
static uint32_t g_submit_log[WC3_CURSOR_SUBMIT_LOG][2]; /* site, target */
static int g_submit_log_count;

static void cursor_name_report(uint32_t name, uint32_t value, uint32_t site)
{
    int i;

    for (i = 0; i < g_name_log_count; i++) {
        if (g_name_log[i][0] == name && g_name_log[i][1] == value)
            return;
    }
    if (g_name_log_count >= WC3_CURSOR_NAME_LOG)
        return;
    g_name_log[g_name_log_count][0] = name;
    g_name_log[g_name_log_count][1] = value;
    g_name_log_count++;
    fprintf(stderr, "[graphics] cursor name=%08x lookup=%08x at=%08x\n", name,
            value, site);
}

static void cursor_submit_report(uint32_t site, uint32_t target,
                                 int suppressed)
{
    int i;

    for (i = 0; i < g_submit_log_count; i++) {
        if (g_submit_log[i][0] == site && g_submit_log[i][1] == target)
            return;
    }
    if (g_submit_log_count >= WC3_CURSOR_SUBMIT_LOG)
        return;
    g_submit_log[g_submit_log_count][0] = site;
    g_submit_log[g_submit_log_count][1] = target;
    g_submit_log_count++;
    fprintf(stderr, "[graphics] submit site=%08x target=%08x%s\n", site,
            target, suppressed ? " suppressed by sequence" : "");
}
#else
#define cursor_name_report(name, value, site) ((void)0)
#define cursor_submit_report(site, target, suppressed) ((void)0)
#endif

/* Mark the submission that follows a cursor name resolution.  The value of
 * the lookup is deliberately not used: L_6F342220 produces it on a deferred
 * tail that runs after this wrapper has returned. */
void __wrap_L_6F342220(CPU *c)
{
    uint32_t name = c->ecx;

    __real_L_6F342220(c);

    if (!cursor_hidden() || !cursor_name_matches(name)) {
        /* Another name was resolved, so the previous cursor lookup was not
         * followed by the draw of its image. */
        __atomic_store_n(&g_cursor_pending, 0u, __ATOMIC_RELEASE);
        return;
    }

    __atomic_store_n(&g_cursor_pending, 1u, __ATOMIC_RELEASE);
    cursor_name_report(name, c->eax, rd32(c->esp));
}

/* Drop the submission that draws the image the guest just resolved. */
void __wrap_L_6F1A47B0(CPU *c)
{
    if (cursor_hidden()) {
        uint32_t pending =
            __atomic_exchange_n(&g_cursor_pending, 0u, __ATOMIC_ACQ_REL);

        cursor_submit_report(rd32(c->esp), c->edx, pending != 0);
        if (pending) {
            /* EDX is the draw target.  A NULL target is the guest's own "draw
             * nothing" case: the sprite is reset and the function returns
             * before it builds a render element or issues a draw call. */
            c->edx = 0;
        }
    }

    __real_L_6F1A47B0(c);
}
