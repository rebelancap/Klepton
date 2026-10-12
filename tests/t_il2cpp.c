// libil2cpp.so loads with 0 unresolved imports and il2cpp_init
// returns. This drives Unity's IL2CPP runtime far enough to parse global-metadata.dat
// and stand up its type system.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/klepton.h"
#include "../runtime/guest/kl_il2cpp.h"
#include "../runtime/diag/kl_mprobe.h"

struct dump_ctx { FILE *f; const uint8_t *base; };
static void dump_one(uint64_t addr, const char *name, void *vctx) {
    struct dump_ctx *c = vctx;
    fprintf(c->f, "0x%llx %s\n", (unsigned long long)(addr - (uint64_t)c->base), name);
}

// Decode an Il2CppString* slot from the live image (Unity 2019.4: 16-byte
// object header, int32 length, UTF-16 chars) for diagnostics. Walks a few
// derefs, because codegen literal loads go through the runtime string table.
// Lossy on purpose: this is for reading string literals a comparison loads.
static void print_il2cpp_string(const uint8_t *base, size_t span, uint64_t off) {
    fflush(stdout);
    const uint8_t *slot;
    memcpy(&slot, base + off, 8);
    printf("  base+%#llx: slot=%p", (unsigned long long)off, slot);
    if (!slot) { printf(" (null)\n"); return; }
    const uint8_t *p = slot;
    for (int depth = 0; depth < 3; depth++) {
        if (p < base || p >= base + span) { printf("  [depth %d: %p out of image]\n", depth, p); fflush(stdout); return; }
        int32_t len;
        memcpy(&len, p + 0x10, 4);
        if (len > 0 && len <= 4096) {
            printf("  [depth %d] [%d] \"", depth, len);
            for (int32_t i = 0; i < len; i++) {
                uint16_t ch;
                memcpy(&ch, p + 0x14 + (size_t)i * 2, 2);
                putchar(ch >= 0x20 && ch < 0x7f ? (int)ch : '?');
            }
            printf("\"");
        }
        const uint8_t *next;
        memcpy(&next, p, 8);
        if (next == p) break;
        p = next;
    }
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *lib  = argc > 1 ? argv[1] : "beatsaber/lib/arm64-v8a/libil2cpp.so";
    const char *data = argc > 2 ? argv[2] : "beatsaber/assets/bin/Data";

    kl_thread_init();
    printf("=== klepton: %s ===\n", lib);

    kl_image *img = kl_load(lib);
    if (!img) { printf("load failed: %s\n", kl_error()); return 1; }
    const kl_stats *st = kl_get_stats(img);
    printf("  %u relocations, %u imports bound, %u unresolved\n",
           st->relative + st->abs64 + st->glob_dat + st->jump_slot,
           st->imports_bound, st->imports_missing);
    if (st->imports_missing) { printf("  !! unresolved imports remain\n"); return 1; }

    kl_set_library_path("beatsaber/lib/arm64-v8a");
    kl_register_image(lib, img);

    printf("  running DT_INIT_ARRAY…\n"); fflush(stdout);
    kl_run_init(img);
    printf("  init OK\n");

    void (*set_data_dir)(const char *)   = kl_sym(img, "il2cpp_set_data_dir");
    void (*set_config_dir)(const char *) = kl_sym(img, "il2cpp_set_config_dir");
    void *(*il2cpp_init)(const char *)   = kl_sym(img, "il2cpp_init");
    void *(*domain_get)(void)            = kl_sym(img, "il2cpp_domain_get");
    void *(*get_assemblies)(void *, size_t *) = kl_sym(img, "il2cpp_domain_get_assemblies");

    if (!il2cpp_init) { printf("  !! il2cpp_init not exported\n"); return 1; }
    if (set_data_dir)   { printf("  il2cpp_set_data_dir(\"%s\")\n", data); set_data_dir(data); }
    if (set_config_dir) set_config_dir(data);

    printf("\n  calling il2cpp_init(\"klepton\")…\n"); fflush(stdout);
    void *domain = il2cpp_init("klepton");
    printf("  il2cpp_init returned %p\n", domain);
    if (!domain) { printf("  !! il2cpp_init failed\n"); return 1; }

    if (domain_get) printf("  il2cpp_domain_get() = %p\n", domain_get());
    if (get_assemblies) {
        size_t n = 0;
        void *as = get_assemblies(domain, &n);
        printf("  loaded assemblies: %zu (array %p)\n", n, as);
    }

    printf("\n=== EXIT CRITERION MET: IL2CPP runtime initialised ===\n");

    // KL_PROBE_ENUM=<Namespace.Type[/Nested]>[,...] — the values of a managed
    // enum, read out of the guest by reflection. Here rather than only in the
    // frame pump because the values are fixed the moment the runtime is up, and
    // the pump is unreachable on the host for a guest that stops at initJni
    // (Arkham Shadow does). This is where kl_ovrplat's ovrMessageType constants
    // come from: ParseMessageHandle switches on the number, so a guessed one is
    // indistinguishable from a completion that never arrives.
    kl_mprobe_enums();

    // KL_RESOLVE=0xoff,0xoff,... — init the method-name resolver and print
    // the managed method containing each libil2cpp image offset. Built to
    // read static disassembly call targets (bl 0x...) without a live boot.
    const char *rz = getenv("KL_RESOLVE");
    // KL_DUMP_METHODS=path — the inverse: every known method as
    // "0x<image offset> <name>", for name -> address lookups. Pair with
    // tools/annotate.py to read a method's disassembly with resolved calls.
    const char *dump = getenv("KL_DUMP_METHODS");
    if (dump) {
        char meta[1024];
        snprintf(meta, sizeof meta, "%s/Metadata/global-metadata.dat", data);
        if (kl_il2cpp_resolver_init(img, meta)) {
            FILE *f = fopen(dump, "w");
            if (!f) { printf("  !! cannot open %s\n", dump); return 1; }
            kl_il2cpp_methods_each(dump_one, &(struct dump_ctx){ f, kl_base(img) });
            fclose(f);
            printf("  dumped %u methods to %s\n", kl_il2cpp_method_count(), dump);
        } else printf("  !! resolver init failed\n");
    }
    // KL_READ_STR=0xoff,0xoff,... — after init, read each image offset as an
    // Il2CppString* slot and print the string. For resolving the literals a
    // disassembled comparison loads. (Literals initialise lazily: entries
    // read as null until the owning type has run. When that bites, decode
    // from global-metadata.dat instead — see PORTING.md.)
    const char *rs = getenv("KL_READ_STR");
    if (rs) {
        char *copy = strdup(rs), *tok = strtok(copy, ", ");
        while (tok) {
            print_il2cpp_string(kl_base(img), kl_span(img), strtoull(tok, NULL, 0));
            tok = strtok(NULL, ", ");
        }
        free(copy);
    }
    if (rz) {
        char meta[1024];
        snprintf(meta, sizeof meta, "%s/Metadata/global-metadata.dat", data);
        if (kl_il2cpp_resolver_init(img, meta)) {
            printf("  resolver up: %u methods\n", kl_il2cpp_method_count());
            char *copy = strdup(rz), *tok = strtok(copy, ", ");
            while (tok) {
                uint64_t off = strtoull(tok, NULL, 0);
                const char *m = kl_il2cpp_method_at((uint8_t *)kl_base(img) + off);
                printf("  libil2cpp+%#llx: %s\n", (unsigned long long)off,
                       m ? m : "(not a known method)");
                tok = strtok(NULL, ", ");
            }
            free(copy);
        } else printf("  !! resolver init failed\n");
    }
    return 0;
}
