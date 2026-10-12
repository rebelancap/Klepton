// t_shc — kl_shc's record file under kills (round F3 review). On the headset
// the app is never shut down, it is killed, possibly mid-write: the record
// must load after a kill at ANY byte, be repaired in place, and keep taking
// appends that the next load can read. Also: a header that lies about its
// lengths is rejected before anything is allocated for it.
// No GPU (the loader and writer are plain file I/O).
#include "kl_shc.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char g_path[256];
static long fsize(void) { struct stat st; return stat(g_path, &st) ? -1 : (long)st.st_size; }
static void put(const void *b, size_t n) {
    FILE *f = fopen(g_path, "wb"); fwrite(b, 1, n, f); fclose(f);
}
static unsigned char *get(size_t *n) {
    FILE *f = fopen(g_path, "rb"); fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1); fread(b, 1, *n, f); fclose(f); return b;
}

static const char *src(int i) {
    static char s[4][200];
    snprintf(s[i], sizeof s[i], "#include <metal_stdlib>\nkernel void k%d(){ /* entry %d */ }\n", i, i);
    return s[i];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== kl_shc record file: torn tails, bad headers, lying lengths ===\n");
    snprintf(g_path, sizeof g_path, "%s/t_shc-%d.bin", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    unlink(g_path);

    // Three whole entries.
    for (int i = 0; i < 3; i++) CHECK(kl_shc_test_append(g_path, 100 + i, src(i)), "append %d", i);
    CHECK(kl_shc_test_load(g_path, 1) == 3, "3 entries");
    size_t full_n;
    unsigned char *full = get(&full_n);
    unlink(g_path);
    CHECK(kl_shc_test_append(g_path, 100, src(0)) && kl_shc_test_append(g_path, 101, src(1)), "two");
    long two = fsize();
    CHECK(two > 8 && (size_t)two < full_n, "sizes %ld %zu", two, full_n);

    // Killed at every byte of the third entry: loads 2, cut back to them,
    // and an append afterwards is read back.
    int bad = 0;
    for (size_t cut = (size_t)two; cut < full_n; cut++) {
        put(full, cut);
        uint32_t n = kl_shc_test_load(g_path, 1);
        long after = fsize();
        if (n != 2 || after != two) { bad++; if (bad < 4) printf("  cut %zu: %u entries, file %ld\n", cut, n, after); continue; }
        if (!kl_shc_test_append(g_path, 103, src(3)) || kl_shc_test_load(g_path, 1) != 3) {
            bad++; if (bad < 4) printf("  cut %zu: append after repair unreadable\n", cut);
        }
    }
    CHECK(!bad, "%d of %zu cut points mishandled", bad, full_n - (size_t)two);
    printf("  torn tail: %zu cut points in the last entry -> repaired, appendable\n", full_n - (size_t)two);

    // Killed inside the magic (or a file of junk): started over, then appendable.
    bad = 0;
    for (size_t cut = 1; cut <= 8; cut++) {
        if (cut == 8) put("JUNKJUNK", 8); else put("KLSHW001", cut);
        if (kl_shc_test_load(g_path, 1) != 0 || fsize() != 0) { bad++; printf("  magic cut %zu: file %ld\n", cut, fsize()); continue; }
        if (!kl_shc_test_append(g_path, 100, src(0)) || kl_shc_test_load(g_path, 1) != 1) bad++;
    }
    CHECK(!bad, "%d short/bad headers mishandled", bad);

    // An empty file (killed before the magic): stays empty, appendable.
    put("", 0);
    CHECK(kl_shc_test_load(g_path, 1) == 0 && fsize() == 0, "empty");
    CHECK(kl_shc_test_append(g_path, 100, src(0)) && kl_shc_test_load(g_path, 1) == 1, "empty then append");

    // Lying lengths: entry 2's rawlen = 0xfffffff0 (would malloc 4 GB), and a
    // pipeline state whose rawlen != zlen (would read past its blob): each is
    // the torn point.
    size_t e2 = (size_t)two;                    // entry 2 starts at the old "two" size
    for (int t = 0; t < 2; t++) {
        unsigned char *m = malloc(full_n);
        memcpy(m, full, full_n);
        if (t == 0) { unsigned v = 0xfffffff0u; memcpy(m + e2 + 12, &v, 4); }
        else { memcpy(m + e2, "ENP1", 4); }    // kind 2, but rawlen (MSL) != zlen (zlib)
        put(m, full_n);
        free(m);
        CHECK(kl_shc_test_load(g_path, 1) == 2 && fsize() == two, "lying header %d: not cut at entry 2 (file %ld)", t, fsize());
    }

    // The shipped list is read-only: a torn tail is ignored, the file untouched.
    put(full, full_n - 5);
    CHECK(kl_shc_test_load(g_path, 2) == 2 && fsize() == (long)full_n - 5, "shipped list touched");

    free(full);
    unlink(g_path);
    printf("%s (%d failures)\n", fails ? "FAILED" : "all record checks passed", fails);
    return fails != 0;
}
