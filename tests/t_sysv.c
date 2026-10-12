// SysV IPC emulation in the glibc personality — `make sysv`, in `make check`.
//
// visionOS has no SysV IPC (the kernel answers SIGSYS), and the first headset
// launch died in libtier0's semget inside libsteamaudio's constructors. The
// personality now emulates semaphores and shared memory in-process; these are
// the paths Valve's libraries actually take (tier0's CThreadSyncObject: keyed
// IPC_CREAT|IPC_EXCL create, EEXIST reopen, IPC_STAT sem_otime poll, SETVAL,
// IPC_NOWAIT acquire spin, blocking acquire/release, GETNCNT/GETZCNT/GETPID
// orphan check, IPC_RMID; OpenSSL's key-114 "random seeded" shm segment),
// called through the rows the guest binds, with Linux constants.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/libc/kl_glibc.h"
#include "../runtime/klepton.h"   // kl_shim_lookup

typedef struct { unsigned short num; short op; short flg; } sembuf_l;
static int  (*p_semget)(int, int, int);
static int  (*p_semop)(int, sembuf_l *, size_t);
static int  (*p_semtimedop)(int, sembuf_l *, size_t, const struct timespec *);
static int  (*p_semctl)(int, int, int, uint64_t);
static int  (*p_shmget)(int, size_t, int);
static void *(*p_shmat)(int, const void *, int);
static int  (*p_shmdt)(const void *);
static int  (*p_shmctl)(int, int, void *);

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
    else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int g_id;
static void *waiter(void *arg) {
    sembuf_l b = { 0, (short)(intptr_t)arg, 0 };
    int r = p_semop(g_id, &b, 1);
    return (void *)(intptr_t)(r == 0 ? 0 : errno);
}

int main(void) {
    printf("=== SysV IPC emulation (glibc personality) ===\n");
    unsetenv("KL_GLIBC_SYSV");
    p_semget = kl_glibc_lookup_native("semget");
    p_semop = kl_glibc_lookup_native("semop");
    p_semtimedop = kl_glibc_lookup_native("semtimedop");
    p_semctl = kl_glibc_lookup_native("semctl");
    p_shmget = kl_glibc_lookup_native("shmget");
    p_shmat = kl_glibc_lookup_native("shmat");
    p_shmdt = kl_glibc_lookup_native("shmdt");
    p_shmctl = kl_glibc_lookup_native("shmctl");
    if (!p_semget || !p_semop || !p_semtimedop || !p_semctl || !p_shmget || !p_shmat ||
        !p_shmdt || !p_shmctl) { printf("FAIL: a row is missing\n"); return 1; }

    // tier0's create: keyed, IPC_CREAT|IPC_EXCL|0600, then SETVAL.
    int key = 0x5eed1234;
    int id = p_semget(key, 1, 0x780);
    CHECK(id > 0, "semget(key, IPC_CREAT|IPC_EXCL) = %d", id);
    int r = p_semget(key, 1, 0x780);
    CHECK(r == -1 && errno == EEXIST, "second exclusive create -> EEXIST (r %d errno %d)", r, errno);
    CHECK(p_semget(key, 1, 0600) == id, "reopen by key answers the same id");
    CHECK(p_semget(0x7777, 1, 0600) == -1 && errno == ENOENT, "absent key without IPC_CREAT -> ENOENT");
    CHECK(p_semctl(id, 0, 16, 2) == 0 && p_semctl(id, 0, 12, 0) == 2, "SETVAL 2 / GETVAL 2");

    uint8_t ds[88]; memset(ds, 0, sizeof ds);
    int64_t otime; uint64_t nsems;
    CHECK(p_semctl(id, 0, 2, (uint64_t)(uintptr_t)ds) == 0, "IPC_STAT");
    memcpy(&otime, ds + 48, 8); memcpy(&nsems, ds + 64, 8);
    CHECK(otime == 0 && nsems == 1, "  before any semop: sem_otime 0, sem_nsems 1");

    // The IPC_NOWAIT acquire spin: two succeed, the third is EAGAIN.
    sembuf_l take = { 0, -1, 0x800 };
    CHECK(p_semop(id, &take, 1) == 0 && p_semop(id, &take, 1) == 0, "two IPC_NOWAIT|-1 acquires");
    r = p_semop(id, &take, 1);
    CHECK(r == -1 && errno == EAGAIN, "third -> EAGAIN (errno %d)", errno);
    p_semctl(id, 0, 2, (uint64_t)(uintptr_t)ds); memcpy(&otime, ds + 48, 8);
    CHECK(otime != 0, "  after a semop: sem_otime set (tier0's creator-ready poll)");
    CHECK(p_semctl(id, 0, 11, 0) == getpid(), "GETPID = this process (tier0's orphan check)");

    // Blocking wait in one thread, post from another.
    g_id = id;
    pthread_t t; pthread_create(&t, NULL, waiter, (void *)(intptr_t)-1);
    int n = 0;
    for (int i = 0; i < 200 && (n = p_semctl(id, 0, 14, 0)) != 1; i++) usleep(1000);
    CHECK(n == 1, "GETNCNT = 1 with a thread parked on -1");
    sembuf_l post = { 0, 1, 0x1000 /* SEM_UNDO, ignored */ };
    CHECK(p_semop(id, &post, 1) == 0, "post +1 (SEM_UNDO accepted)");
    void *tr; pthread_join(t, &tr);
    CHECK(tr == NULL && p_semctl(id, 0, 12, 0) == 0 && p_semctl(id, 0, 14, 0) == 0,
          "the waiter woke, took it; value 0, GETNCNT 0");

    // Wait-for-zero and its counter.
    p_semctl(id, 0, 16, 1);
    pthread_create(&t, NULL, waiter, (void *)(intptr_t)0);
    for (int i = 0; i < 200 && (n = p_semctl(id, 0, 15, 0)) != 1; i++) usleep(1000);
    CHECK(n == 1, "GETZCNT = 1 with a thread waiting for zero");
    sembuf_l dec = { 0, -1, 0 };
    p_semop(id, &dec, 1);
    pthread_join(t, &tr);
    CHECK(tr == NULL, "the zero-waiter woke at 0");

    // semtimedop times out with EAGAIN.
    struct timespec ts = { 0, 30 * 1000 * 1000 };
    r = p_semtimedop(id, &dec, 1, &ts);
    CHECK(r == -1 && errno == EAGAIN, "semtimedop 30 ms on 0 -> EAGAIN");

    // Atomic multi-op: nothing applies if one would block.
    int id2 = p_semget(0, 2, 0600);
    p_semctl(id2, 0, 16, 1);
    sembuf_l two[2] = { { 0, -1, 0x800 }, { 1, -1, 0x800 } };
    CHECK(p_semop(id2, two, 2) == -1 && errno == EAGAIN && p_semctl(id2, 0, 12, 0) == 1,
          "two-op semop with one blocked applies neither");
    unsigned short all[2] = { 3, 4 }, got[2] = { 0, 0 };
    CHECK(p_semctl(id2, 0, 17, (uint64_t)(uintptr_t)all) == 0 &&
          p_semctl(id2, 0, 13, (uint64_t)(uintptr_t)got) == 0 && got[0] == 3 && got[1] == 4,
          "SETALL / GETALL");
    CHECK(p_semctl(id2, 0, 0, 0) == 0, "IPC_PRIVATE set removed");

    // IPC_RMID with a waiter parked: it wakes with EIDRM; the id is dead.
    pthread_create(&t, NULL, waiter, (void *)(intptr_t)-1);
    for (int i = 0; i < 200 && p_semctl(id, 0, 14, 0) != 1; i++) usleep(1000);
    CHECK(p_semctl(id, 0, 0, 0) == 0, "IPC_RMID with a waiter parked");
    pthread_join(t, &tr);
    CHECK((intptr_t)tr == EIDRM, "the waiter woke with EIDRM (%ld)", (long)(intptr_t)tr);
    CHECK(p_semctl(id, 0, 12, 0) == -1 && errno == EINVAL, "the removed id -> EINVAL");
    CHECK(p_semget(key, 1, 0600) == -1 && errno == ENOENT, "its key is free again");

    // OpenSSL's key-114 segment: open (absent), create 0444, attach RDONLY.
    CHECK(p_shmget(114, 1, 0) == -1 && errno == ENOENT, "shmget(114) absent -> ENOENT");
    int sid = p_shmget(114, 1, 0x324);
    CHECK(sid > 0, "shmget(114, 1, IPC_CREAT|0444) = %d", sid);
    CHECK(p_shmget(114, 1, 0) == sid, "reopen by key");
    char *a = p_shmat(sid, NULL, 0x1000), *b = p_shmat(sid, NULL, 0);
    CHECK(a != (void *)-1 && a == b, "two attaches share one mapping");
    b[0] = 42;
    CHECK(a[0] == 42, "a write is visible through the other attach");
    uint8_t sd[112]; uint64_t segsz, nattch;
    CHECK(p_shmctl(sid, 2, sd) == 0, "shmctl IPC_STAT");
    memcpy(&segsz, sd + 48, 8); memcpy(&nattch, sd + 88, 8);
    CHECK(segsz == 1 && nattch == 2, "  shm_segsz 1, shm_nattch 2");
    CHECK(p_shmctl(sid, 0, NULL) == 0, "IPC_RMID while attached");
    CHECK(p_shmget(114, 1, 0) == -1 && errno == ENOENT, "a removed segment's key no longer matches");
    CHECK(p_shmdt(a) == 0 && p_shmdt(b) == 0, "two detaches");
    CHECK(p_shmdt(a) == -1 && errno == EINVAL, "a third detach -> EINVAL (unmapped at the last)");
    CHECK(p_shmat(sid, NULL, 0) == (void *)-1, "the dead id cannot attach");

    // A bionic guest's sem*/shm* bind to the same emulation (kl_sysv_lookup,
    // kl_shim_lookup's first stop) — not to Darwin's SysV, SIGSYS on visionOS.
    {
        static const char *const nm[] = { "semget", "semop", "semtimedop", "semctl",
                                          "shmget", "shmat", "shmdt", "shmctl" };
        int same = 1;
        for (unsigned i = 0; i < sizeof nm / sizeof *nm; i++)
            if (kl_shim_lookup(nm[i]) != kl_glibc_lookup_native(nm[i])) same = 0;
        CHECK(same, "the bionic shim answers the 8 sem*/shm* names with the emulation");
        CHECK(kl_shim_lookup("sem_init") != kl_glibc_lookup_native("semget"),
              "  ...and POSIX sem_* are not caught by the prefix");
    }

    printf(fails ? "SYSV: %d FAILED\n" : "SYSV: all passed\n", fails);
    return fails != 0;
}
