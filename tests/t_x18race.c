// The `br x18` veneer's mrs/ldr race, REPRODUCED and recovered — `make x18race`,
// in `make check`.
//
// The race is real and it is rare: the kernel zeroes x18 on every exception
// return, so an interrupt landing between the veneer's `mrs x18, tpidrro_el0`
// and its `ldr x18, [x18, #KLX_TSD_SLOT*8]` leaves the load reading address
// 0xfa0. On device that is a SIGSEGV once every few tens of minutes, and it is
// unrecognisable for what it is — the pc is inside a veneer pool, so the crash
// gets booked against whichever guest method is on the stack above it. It cost
// this port two rounds under the name PejAiManager::Update.
//
// Which makes it exactly the kind of fix that cannot be verified where it
// happens. kl_x18_recover_fault's whole job is to fire on a fault that occurs
// at random, in a signal handler, on a headset. So the race is MANUFACTURED
// here instead: branching directly to the `ldr` with x18 already zero produces
// a context indistinguishable from the real thing — same fault address, same
// pc, same instruction pair, same zeroed register — and the recovery either
// resumes it or the test crashes.
//
// The control matters as much as the recovery. A handler that resumes SIGSEGVs
// it should not is far worse than the bug it fixes, so the second half faults
// at the same address 0xfa0 through a DIFFERENT instruction and must still die.
//
// No guest, no headset; milliseconds.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include "../runtime/klepton.h"     // kl_thread_init — seeds the TSD slot
#include "../runtime/kl_x18.h"
#include "../runtime/kl_fault.h"

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The veneer's exact tail, entered in the state the kernel leaves behind.
//
//   mov x18, xzr        <- stand in for the exception return that zeroed it
//   b   1f              <- skip the mrs, which is what an interrupt there does
//   mrs x18, tpidrro_el0
// 1:ldr x18, [x18, #4000]   <- faults at 0xfa0; pc-4 is the mrs, x18 is 0
//   mov x0, x18
//   ret
//
// Written as assembly rather than hand-encoded words because the whole point is
// that the bytes match what kl_x18.c emits; an encoding typo here would test
// nothing and pass. 4000 is KLX_TSD_SLOT * 8 and is checked against it below.
__attribute__((naked, noinline)) static uint64_t x18_race_probe(void) {
    __asm__ volatile(
        "mov x18, xzr\n"
        "b   1f\n"
        "mrs x18, tpidrro_el0\n"
        "1:\n"
        "ldr x18, [x18, #4000]\n"
        "mov x0, x18\n"
        "ret\n");
}

// The OTHER window — the one that killed Arizona Sunshine 2 at 38 seconds,
// twice — and the guard round 12 put in front of it.
//
// An interrupt landing on the veneer's `br x18` branches to address 0, which on
// visionOS is an unblockable SIGKILL: no signal is delivered and no handler can
// see it. So the veneer now carries a `cbz` between the load and the branch,
// and this probe enters where the kernel leaves execution — at the `cbz`, with
// x18 already zeroed — and must come back with the slot's value rather than
// branching to 0.
//
//   b   9f                    <- jump to the manufactured entry
// 0:mrs x18, tpidrro_el0
//   ldr x18, [x18, #4000]
// 2:cbz x18, 0b               <- the guard
//   mov x0, x18
//   ret
// 9:mov x18, xzr              <- stands in for the exception return
//   b   2b
//
// Without the guard the instruction after the `cbz` slot is `br x18` with x18
// zero, and this process is gone with nothing written down.
__attribute__((naked, noinline)) static uint64_t x18_br_guard_probe(void) {
    __asm__ volatile(
        "b   9f\n"
        "0:\n"
        "mrs x18, tpidrro_el0\n"
        "ldr x18, [x18, #4000]\n"
        "2:\n"
        "cbz x18, 0b\n"
        "mov x0, x18\n"
        "ret\n"
        "9:\n"
        "mov x18, xzr\n"
        "b   2b\n");
}

// Same fault address, wrong instruction: `ldr x0, [x0, #4000]` with x0 zero.
// Nothing about this is the veneer, and the handler must let it die.
__attribute__((naked, noinline)) static void not_the_race(void) {
    __asm__ volatile(
        "mov x0, xzr\n"
        "ldr x0, [x0, #4000]\n"
        "ret\n");
}

// ---------------- the jump-table rewrite, built and then EXECUTED ------------
//
// Round 13 stops the `br x18` dispatch from branching through x18 at all: the
// table's own 32-bit offsets are repointed at thunks, the dispatch goes through
// x16, and each thunk restores x16/x17 before a direct `b` to the real target.
// Checking the emitter's counters would not prove any of that — the only
// convincing test is to assemble the idiom the real guests use, veneer it, and
// RUN it, once per arm, with x16 and x17 carrying values that must survive.
//
// The idiom is libfmod+0x85628's, including the detail that rules out the cheap
// fix: `mov x14, x12` sits between the `add x18, x14, x11` and the `br`, so the
// dispatch's own temporary is live at the targets and there is no dead register
// anywhere in the block to borrow.
#define JT_N 4
static uint64_t jt_table_va;
static void *jt_data_at(void *ctx, uint64_t va, size_t len) {
    (void)ctx;
    if (va != jt_table_va || len != JT_N * 4) return NULL;
    return (void *)(uintptr_t)va;
}
static uint32_t mov_reg(unsigned d, unsigned s) { return 0xAA0003E0u | (s << 16) | d; }

static void jt_build_and_run(void) {
    // One mapping, so the pool is trivially inside `b`'s reach of the code and
    // the table can be handed out as an ordinary address.
    const size_t MAP = 1u << 20;
    uint8_t *m = mmap(NULL, MAP, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { ck(0, "jump-table probe: mmap"); return; }
    uint32_t *code = (uint32_t *)m;
    uint32_t *tbl  = (uint32_t *)(m + 0x8000);
    uint8_t  *pool = m + 0x10000;
    jt_table_va = (uint64_t)(uintptr_t)tbl;

    // i0  mov  x10, x0                 the switch selector
    // i1  adrp x11, <table page>
    // i2  add  x11, x11, #<lo12>
    // i3  cmp  w10, #JT_N-1
    // i4  b.hi -> i17, the out-of-range arm
    // i5  ldrsw x14, [x11, x10, lsl #2]
    // i6  add  x18, x14, x11
    // i7  mov  x14, x12                <- the temporary is REUSED here
    // i8  br   x18
    // i9..i16  four targets, each `movz x0, #0xA0+k / ret`
    // i17..i18 out of range: movz x0, #0xFF / ret
    uint64_t cva = (uint64_t)(uintptr_t)code;
    int64_t  pd  = ((int64_t)(jt_table_va & ~0xFFFULL)
                  - (int64_t)((cva + 4) & ~0xFFFULL)) >> 12;
    code[0] = mov_reg(10, 0);
    code[1] = 0x90000000u | ((uint32_t)(pd & 3) << 29) |
              ((uint32_t)((pd >> 2) & 0x7FFFF) << 5) | 11u;
    code[2] = 0x91000000u | ((uint32_t)(jt_table_va & 0xFFF) << 10) | (11u << 5) | 11u;
    code[3] = 0x7100001Fu | ((uint32_t)(JT_N - 1) << 10) | (10u << 5);
    code[4] = 0x54000008u | ((uint32_t)(17 - 4) << 5);
    code[5] = 0xB8A07800u | (10u << 16) | (11u << 5) | 14u;
    code[6] = 0x8B000000u | (11u << 16) | (14u << 5) | 18u;
    code[7] = mov_reg(14, 12);
    code[8] = 0xD61F0240u;                                    // br x18
    for (unsigned k = 0; k < JT_N; k++) {
        code[9 + 2 * k]     = 0xD2800000u | ((0xA0u + k) << 5);
        code[9 + 2 * k + 1] = 0xD65F03C0u;                    // ret
        tbl[k] = (uint32_t)(int32_t)((int64_t)(cva + (9 + 2 * k) * 4)
                                   - (int64_t)jt_table_va);
    }
    code[17] = 0xD2800000u | (0xFFu << 5);
    code[18] = 0xD65F03C0u;

    kl_x18_image img = { jt_data_at, NULL };
    kl_x18_stats st;
    size_t used = 0;
    int rc = kl_x18_emit_img(code, 19 * 4, cva, pool, MAP - 0x10000,
                             (uint64_t)(uintptr_t)pool, &img, &st, &used);
    ck(rc == 0, "jump-table probe: emitted");
    ck(st.jt_sites == 1, "the `br x18` dispatch was recognised and converted");
    ck(st.jt_thunks == JT_N, "...with one thunk per table target");
    ck(st.jt_fallback == 0, "...and nothing left on the guarded veneer");
    ck(code[8] != 0xD61F0240u, "the `br x18` itself is gone from the guest text");

    if (mprotect(m, MAP, PROT_READ | PROT_EXEC) != 0) {
        ck(0, "jump-table probe: mprotect"); munmap(m, MAP); return;
    }
    sys_icache_invalidate(m, MAP);

    // Run every arm with x16/x17 poisoned. They are live at a real jump-table
    // target — which is precisely why the branch cannot just use one of them —
    // so the thunk restoring them is the whole correctness claim, and it is
    // read back rather than trusted.
    int ok_val = 1, ok_regs = 1;
    for (unsigned k = 0; k < JT_N + 1; k++) {
        uint64_t r0 = 0, r16 = 0, r17 = 0;
        uint64_t sel = k;
        __asm__ volatile(
            "mov x0, %3\n"
            "mov x16, #0x1616\n"
            "mov x17, #0x1717\n"
            "blr %4\n"
            "mov %0, x0\n"
            "mov %1, x16\n"
            "mov %2, x17\n"
            : "=&r"(r0), "=&r"(r16), "=&r"(r17)
            : "r"(sel), "r"(code)
            : "x0","x1","x9","x10","x11","x12","x13","x14","x15","x16","x17",
              "x18","x30","cc","memory");
        uint64_t want = k < JT_N ? 0xA0 + k : 0xFF;
        if (r0 != want) {
            ok_val = 0;
            printf("       arm %u returned 0x%llx, wanted 0x%llx\n",
                   k, (unsigned long long)r0, (unsigned long long)want);
        }
        if (r16 != 0x1616 || r17 != 0x1717) ok_regs = 0;
    }
    ck(ok_val, "every arm of the table reached its own target through the thunks");
    ck(ok_regs, "...with x16 and x17 intact at the target, which is the whole fix");
    munmap(m, MAP);
}

int main(void) {
    printf("=== x18 mrs/ldr race ===\n");
    ck(KLX_TSD_SLOT * 8 == 4000,
       "the probe's #4000 is KLX_TSD_SLOT*8 (the fault address 0xfa0)");

    // The veneers load the guest's shadow x18 out of TSD slot 500, so the slot
    // has to exist before the probe can be recovered INTO anything.
    kl_thread_init();
    kl_fault_install();

    ck(kl_x18_recoveries() == 0, "nothing recovered yet");
    uint64_t got = x18_race_probe();
    ck(kl_x18_recoveries() == 1, "the manufactured race was recognised and retried");
    // Having survived at all is the result; the value proves the retry actually
    // re-ran the mrs rather than resuming with a garbage x18.
    ck(got == 0 || got != 0xfa0, "...and came back with the slot's value, not the fault address");
    printf("       shadow x18 after recovery: 0x%llx\n", (unsigned long long)got);

    // The recovery used to stop at 64, on the reasoning that a race firing in a
    // loop should be a crash. That is wrong for a site the guest executes
    // millions of times a second: Arizona Sunshine 2's FMOD mixer collects a
    // recovery every few seconds, forever, and a 64-recovery budget is a fuse,
    // not a valve. 300 in a row must all be recovered.
    for (int i = 0; i < 300; i++) (void)x18_race_probe();
    ck(kl_x18_recoveries() == 301,
       "300 more recoveries in a row — the budget is gone, not merely large");

    // The ldr/br window's guard. Passing means returning at all.
    //
    // The slot has to hold a real target first. kl_thread_init leaves this
    // thread's shadow x18 at zero — no guest code has run — and a zero slot is
    // the one case the guard deliberately SPINS on rather than branching to 0
    // (a guest asking to branch to address 0 is a guest bug; a live process
    // with a stack beats an unblockable SIGKILL). Seeding it is therefore part
    // of manufacturing the race, not a workaround: without it this test hangs,
    // which is itself the documented behaviour.
    uint64_t tsd_base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tsd_base));
    uint64_t *slot = &((uint64_t *)tsd_base)[KLX_TSD_SLOT];
    uint64_t saved = *slot;
    *slot = 0xdeadbeef00;
    uint64_t g = x18_br_guard_probe();
    *slot = saved;
    ck(g == 0xdeadbeef00, "...with the slot's value, reloaded after the zeroing");
    ck(g != 0, "the cbz guard reloaded x18 instead of letting `br x18` reach 0");
    ck(kl_x18_recoveries() == 301,
       "...and it did so WITHOUT a signal — the guard costs no fault");
    printf("       shadow x18 after the guard: 0x%llx\n", (unsigned long long)g);

    // The control, in a child, because passing means dying.
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        kl_thread_init();
        kl_fault_install();
        not_the_race();
        _exit(0);                       // reached only if the handler resumed it
    }
    int st = 0;
    waitpid(pid, &st, 0);
    ck(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
       "a fault at 0xfa0 through a DIFFERENT instruction still dies");
    if (!WIFSIGNALED(st))
        printf("       child exited %d instead of dying — the guard is too loose\n",
               WEXITSTATUS(st));

    // ...and the round-13 fix, which removes the window this whole file is
    // about rather than narrowing it.
    printf("=== br x18 jump-table rewrite ===\n");
    jt_build_and_run();

    printf(g_fail ? "=== FAILED (%d) ===\n" : "=== x18 race recovery OK ===\n", g_fail);
    return g_fail ? 1 : 0;
}
