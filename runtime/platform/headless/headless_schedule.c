/*
 * headless_schedule.c - deterministic T1 clock identity and input script.
 *
 * A schedule is opt-in.  Without one the backend keeps host clocks and host
 * scheduling, which is useful for the platform contract test but not for a
 * reproducible difftest.  With a schedule, monotonic zero is virtual, the
 * realtime epoch is fixed, and scripted input is released by virtual time.
 */
#include "headless_internal.h"

#include <stdlib.h>
#include <string.h>

static void hash_bytes(uint64_t *hash, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = *hash;
    for (size_t i = 0; i < size; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    *hash = h;
}

static void hash_u32(uint64_t *hash, uint32_t value)
{
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4u; i++) bytes[i] = (uint8_t)(value >> (8u * i));
    hash_bytes(hash, bytes, sizeof bytes);
}

static void hash_u64(uint64_t *hash, uint64_t value)
{
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8u; i++) bytes[i] = (uint8_t)(value >> (8u * i));
    hash_bytes(hash, bytes, sizeof bytes);
}

static uint64_t schedule_hash(const HeadlessState *st,
                              const Wc3HeadlessScheduleConfig *config)
{
    /* FNV-1a over explicit fields, not struct bytes: padding must not affect
     * the identity.  The Python identity tool can hash the same canonical
     * values without needing a C SHA-256 implementation here. */
    uint64_t h = 14695981039346656037ull;
    hash_u32(&h, st->schedule_policy);
    hash_u32(&h, st->schedule_seed);
    hash_u64(&h, st->epoch_unix_ns);
    hash_u32(&h, (uint32_t)st->timezone_minutes);
    hash_u32(&h, config ? config->mailbox_delay_ns : 0u);
    size_t id_len = strlen(st->schedule_id);
    hash_u64(&h, (uint64_t)id_len);
    hash_bytes(&h, st->schedule_id, id_len);
    hash_u32(&h, st->script_count);
    for (uint32_t i = 0; i < st->script_count; i++) {
        const HeadlessScript *s = &st->script[i];
        hash_u64(&h, s->event.time_ns);
        hash_u64(&h, s->publish_ns);
        hash_u32(&h, s->event.event.kind);
        hash_u32(&h, s->event.event.pointer_id);
        hash_u32(&h, (uint32_t)s->event.event.x);
        hash_u32(&h, (uint32_t)s->event.event.y);
        hash_u32(&h, s->event.event.key);
        hash_u32(&h, s->event.event.codepoint);
        hash_u32(&h, s->event.event.modifiers);
    }
    return h;
}

int headless_schedule_init(HeadlessState *st,
                           const Wc3HeadlessScheduleConfig *config)
{
    if (!st) return -1;
    if (!config) {
        st->virtual_clock = 0;
        return 0;
    }
    if (config->policy < WC3_HEADLESS_SCHED_ROUND_ROBIN ||
        config->policy > WC3_HEADLESS_SCHED_WAKE_REORDER)
        return -1;
    if (!config->id || !*config->id) return -1;
    if (strlen(config->id) >= sizeof st->schedule_id) return -1;
    if (config->timezone_minutes < -1440 ||
        config->timezone_minutes > 1440)
        return -1;
    if (config->input_count > HEADLESS_SCRIPT_MAX) return -1;
    if (config->input_count && !config->input_script) return -1;

    HeadlessScript *script = NULL;
    if (config->input_count) {
        script = (HeadlessScript *)calloc(config->input_count, sizeof *script);
        if (!script) return -1;
    }

    uint64_t previous = 0u;
    for (uint32_t i = 0; i < config->input_count; i++) {
        const Wc3HeadlessScriptEvent *src = &config->input_script[i];
        if (i && src->time_ns < previous) {
            free(script);
            return -1;
        }
        if (!headless_input_kind_valid(src->event.kind)) {
            free(script);
            return -1;
        }
        const uint64_t release =
            src->time_ns + (uint64_t)config->mailbox_delay_ns;
        if (release < src->time_ns) {
            free(script);
            return -1;
        }
        script[i].event = *src;
        script[i].event.event.time_ns = release;
        script[i].publish_ns = release;
        previous = src->time_ns;
    }

    st->virtual_clock = 1;
    st->epoch_unix_ns = config->epoch_unix_ns
                            ? config->epoch_unix_ns
                            : HEADLESS_DEFAULT_EPOCH_NS;
    st->timezone_minutes = config->timezone_minutes;
    st->schedule_policy = config->policy;
    st->schedule_seed = config->seed;
    (void)headless_copy_string(st->schedule_id, sizeof st->schedule_id,
                               config->id);
    st->script = script;
    st->script_count = config->input_count;
    st->script_next = 0u;
    atomic_store_explicit(&st->clock_ns, 0u, memory_order_release);
    st->schedule_hash = schedule_hash(st, config);
    return 0;
}

void headless_schedule_free(HeadlessState *st)
{
    if (!st) return;
    free(st->script);
    st->script = NULL;
    st->script_count = 0u;
    st->script_next = 0u;
    st->virtual_clock = 0;
    st->epoch_unix_ns = 0u;
    st->timezone_minutes = 0;
    st->schedule_policy = 0u;
    st->schedule_seed = 0u;
    st->schedule_id[0] = '\0';
    st->schedule_hash = 0u;
    atomic_store_explicit(&st->clock_ns, 0u, memory_order_release);
}

int headless_clock_advance(HeadlessState *st, uint64_t ns)
{
    if (!st || !st->virtual_clock) return -1;

    uint64_t old = atomic_load_explicit(&st->clock_ns, memory_order_acquire);
    for (;;) {
        if (UINT64_MAX - old < ns) return -1;
        const uint64_t next = old + ns;
        if (atomic_compare_exchange_weak_explicit(
                &st->clock_ns, &old, next, memory_order_release,
                memory_order_acquire))
            break;
    }
    headless_input_publish_due(st);
    return 0;
}

uint64_t wc3_headless_clock_now_ns(void)
{
    HeadlessState *st = g_headless;
    return st ? headless_now_ns(st) : 0u;
}

int wc3_headless_clock_advance_ns(uint64_t ns)
{
    HeadlessState *st = g_headless;
    return st ? headless_clock_advance(st, ns) : -1;
}

int wc3_headless_schedule_policy(void)
{
    HeadlessState *st = g_headless;
    return st ? (int)st->schedule_policy : 0;
}

uint64_t wc3_headless_schedule_seed(void)
{
    HeadlessState *st = g_headless;
    return st ? (uint64_t)st->schedule_seed : 0u;
}

const char *wc3_headless_schedule_id(void)
{
    HeadlessState *st = g_headless;
    return st && st->schedule_id[0] ? st->schedule_id : NULL;
}

uint64_t wc3_headless_schedule_hash(void)
{
    HeadlessState *st = g_headless;
    return st ? st->schedule_hash : 0u;
}
