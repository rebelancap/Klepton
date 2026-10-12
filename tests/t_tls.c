// S0.1b — the TLSDESC resolver's two contracts, both of which are silent when
// broken. A resolver that clobbers a register corrupts its CALLER, thousands of
// instructions from anything named TLS; a resolver that hands two threads the
// same block turns `thread_local` into a shared global and shows up as a race.
// Neither has a symptom that says "TLS", so both are gated here.
//
// The call sequence in tests/t_tls_call.S is the guest's own (kl_tls.h quotes
// it): x0 = &descriptor, blr desc[0], and the answer is an offset added to the
// thread pointer. This test therefore checks the same instruction sequence
// libovrbody.so contains, without needing libovrbody.so.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "kl_tls.h"

int64_t kl_test_tlsdesc_call(void *desc, uint64_t *out);

#define BASE 0x1111111100000000ULL

static kl_tlsmod *g_mod;
static uint64_t   g_desc[2];        // [0] resolver, [1] &arg — a real descriptor
static kl_tlsdesc_arg g_arg;
static int        g_fail;
// The shape libovrbody.so's PT_TLS has: a few initialised bytes and a much
// larger block that must read zero (filesz 8, memsz 0xe0 there).
static const uint8_t g_init[8] = { 0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4 };

static void fail(const char *what) { printf("  FAIL: %s\n", what); g_fail = 1; }

// One thread's view: call the descriptor, check the registers survived, check
// the block is this thread's own, and hand the address back.
//
// Everything about the BLOCK is checked in here rather than after the join,
// because the key's destructor frees it when the thread exits — reading it from
// main afterwards reads freed memory, and the first draft of this test did
// exactly that and blamed the loader. The pointer is still useful outside: an
// address is a value, and comparing two of them touches nothing.
static void *resolve_here(void *poison) {
    uint64_t out[48];
    memset(out, 0, sizeof out);
    int64_t off = kl_test_tlsdesc_call(g_desc, out);

    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    uint8_t *addr = (uint8_t *)(uintptr_t)(tp + (uint64_t)off);   // the guest's `ldr x0,[x8,x0]` address

    for (int i = 0; i < 15; i++)
        if (out[i] != BASE + (uint64_t)(i + 1)) {
            char m[64]; snprintf(m, sizeof m, "x%d clobbered (%#llx)", i + 1,
                                 (unsigned long long)out[i]);
            fail(m);
        }
    if (out[15] != 0x60000000ULL) fail("NZCV clobbered");
    for (int i = 0; i < 32; i++) {
        uint64_t want = i <= 14 ? BASE + (uint64_t)(i + 1)
                      : i == 15 ? BASE + 1
                      : i <= 29 ? BASE + (uint64_t)(i - 14)
                      : i == 30 ? BASE + 1 : BASE + 2;
        if (out[16 + i] != want) {
            char m[64]; snprintf(m, sizeof m, "d%d clobbered (%#llx)", i,
                                 (unsigned long long)out[16 + i]);
            fail(m);
        }
    }

    // The block: initialisation image at the front, zeroes past filesz, and —
    // for the second thread, which runs after main has written its own — none
    // of the other thread's writes.
    if (memcmp(addr - 8, g_init, sizeof g_init) != 0) fail("initialisation image not copied");
    if (addr[0] || addr[7]) fail("bytes past filesz are not zero");
    if (poison && *addr == *(uint8_t *)poison) fail("a write on one thread was visible on another");

    // ...and the SECOND call, which takes the fast path, must agree with the
    // first, which took the slow one. Two paths computing the same thing is
    // exactly where a per-thread cache goes wrong.
    uint64_t out2[48];
    int64_t off2 = kl_test_tlsdesc_call(g_desc, out2);
    if (off2 != off) fail("fast path and slow path disagree");

    *addr = poison ? 0xa5 : 0x5a;
    return addr;
}

int main(void) {
    printf("=== TLSDESC resolver ===\n");
    if (kl_tls_init() != 0) { printf("  FAIL: kl_tls_init\n"); return 1; }

    g_mod = kl_tls_register("t_tls", g_init, sizeof g_init, 64, 8);
    if (!g_mod) { printf("  FAIL: kl_tls_register\n"); return 1; }

    g_arg.mod = g_mod;
    g_arg.off = 8;                       // past the initialised bytes
    g_desc[0] = (uint64_t)(uintptr_t)&kl_tlsdesc_resolver;
    g_desc[1] = (uint64_t)(uintptr_t)&g_arg;

    uint8_t *a = (uint8_t *)resolve_here(NULL);
    printf("  main thread   -> %p\n", (void *)a);

    pthread_t th;
    void *b = NULL;
    pthread_create(&th, NULL, resolve_here, a);
    pthread_join(th, &b);
    printf("  second thread -> %p\n", b);
    if (b == (void *)a) fail("two threads got the SAME block");
    if (*a != 0x5a) fail("the first thread's write did not survive the other thread");

    printf(g_fail ? "  FAILED\n" : "  ok: registers preserved, per-thread blocks distinct\n");
    return g_fail;
}
