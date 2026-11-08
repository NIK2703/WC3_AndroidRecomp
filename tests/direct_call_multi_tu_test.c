/*
 * direct_call_multi_tu_test.c - G-14 (a)-(e) source test.
 *
 * Compiled with the generated dispatcher, three translation units, and the
 * real CPU header.  It exercises:
 *   (a) tail between more than one TU
 *   (b) direct call -> dispatch_jmp in the callee
 *   (c) generated dispatcher -> direct tail
 *   (d) 100000 deferred tail hops
 *   (e) two guest threads with separate CPU transition state
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "direct_call_multi_tu_common.h"
#include "hle_abi.h"

void wc3_dispatch_init(HleRegistry *registry);
void wc3_dispatch_shutdown(void);
void L_00401000(CPU *c);
void L_00401030(CPU *c);
void L_00401050(CPU *c);

static int fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

static int check_log(const TestCPU *t, const int *want, int n)
{
    if (t->n != n) return 0;
    return memcmp(t->log, want, (size_t)n * sizeof want[0]) == 0;
}

static void *chain_worker(void *arg)
{
    TestCPU *t = (TestCPU *)arg;
    dispatch(&t->cpu, 0x00401050u);
    return NULL;
}

int main(void)
{
    static const int want_direct[] = { 1, 2, 3, 4 };
    static const int want_tail[] = { 5, 6 };
    TestCPU t;
    pthread_t threads[2];
    TestCPU workers[2];

    wc3_dispatch_init(NULL);

    memset(&t, 0, sizeof t);
    dispatch(&t.cpu, 0x00401000u);
    if (!check_log(&t, want_direct, 4)) return fail("cross-TU direct/tail order");
    if (t.cpu.disp_depth != 0u) return fail("direct depth");
    if (t.cpu.disp_pending_valid != 0u) return fail("direct pending");

    memset(&t, 0, sizeof t);
    dispatch(&t.cpu, 0x00401030u);
    if (!check_log(&t, want_tail, 2)) return fail("cross-TU tail order");
    if (t.cpu.disp_depth != 0u) return fail("tail depth");

    memset(&t, 0, sizeof t);
    dispatch(&t.cpu, 0x00401050u);
    if (t.hops != 100000L) return fail("100000 deferred hops");
    if (t.cpu.disp_depth != 0u) return fail("chain depth");

    memset(workers, 0, sizeof workers);
    for (int i = 0; i < 2; i++)
        if (pthread_create(&threads[i], NULL, chain_worker, &workers[i]) != 0)
            return fail("pthread_create");
    for (int i = 0; i < 2; i++)
        if (pthread_join(threads[i], NULL) != 0)
            return fail("pthread_join");
    for (int i = 0; i < 2; i++) {
        if (workers[i].hops != 100000L) return fail("thread hops");
        if (workers[i].cpu.disp_depth != 0u) return fail("thread depth");
        if (workers[i].cpu.disp_pending_valid != 0u) return fail("thread pending");
    }

    wc3_dispatch_shutdown();
    printf("direct_call_multi_tu_test: OK (cross-TU, mixed path, 2 threads)\n");
    return 0;
}
