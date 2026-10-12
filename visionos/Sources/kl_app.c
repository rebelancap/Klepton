// See kl_app.h.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <mach/mach.h>          // task_info(TASK_VM_INFO) — the heartbeat's footprint
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_ndk.h"       // kl_ndk_prepare_looper — the UI thread's ALooper, before initJni
#include "kl_egl.h"
#include "kl_driver.h"    // the guest's own sequences, shared with build/m_boot
#include "kl_audio.h"     // kl_audio_set_latency_ms — streamed audio needs a deeper buffer
#include "kl_opensl.h"    // kl_opensl_heartbeat — the live audio census in the [hb] loop
#include "kl_ovrp.h"      // kl_ovrp_set_frame_pacer — the OVRPlugin frame clock
#include "kl_openxr.h"    // kl_openxr_set_frame_pacer — the OpenXR one
#include "kl_slink.h"
#include "kl_sdl.h"       // ...and SDLActivity's, which an srceng guest comes through
#include "kl_mono.h"      // the flat guest's window seam — frame out, pointer in
#include "kl_glfb.h"
#include "kl_vulkan.h"    // kl_vulkan_memory_report — the GPU half of the tripwire
#include "kl_x18.h"       // claim the veneers' TSD slot before anything else can
#include "kl_env.h"

// --- Which guest ------------------------------------------------------------
//
// The table is runtime/kl_target.h — one row per guest, generated from
// visionos/targets.py and shared with `build/m_boot`, so the host driver and the
// app cannot describe one guest differently.
//
// The default is baked in at BUILD time (KL_TARGET_DEFAULT, set by
// gen_xcodeproj.py) rather than read from the environment, because the app has
// to know which guest it is when it is launched by hand from the Home View with
// no environment at all. KL_TARGET still overrides it, which is what makes an
// A/B possible from `run.sh` without regenerating the project.
#include "kl_target.h"

#ifndef KL_TARGET_DEFAULT
#define KL_TARGET_DEFAULT KL_TARGET_DEFAULT_NAME
#endif

static const kl_target *g_target;

// Which of Steam Link's front doors this run opened, and whether the VR one's
// chain has been mapped yet.
//
// Both doors can be opened by one process here, which is the difference between
// this driver and `build/m_boot`. On the host the shell pairs and then re-EXECs
// into the VR door carrying the session, which makes every question about
// tearing the shell down go away. An app bundle cannot
// re-exec, so the two chains coexist: the shell's fourteen libraries stay
// mapped and its main thread parks inside startVRLink, exactly as an Android
// activity that called finishAndRemoveTask() would stop running.
static kl_slink_door g_door;
static int           g_vr_loaded;

// "Stop pumping." Set by kl_app_guest_stop (the immersive space going away) and
// by the 2D->VR handoff, which is the shell's own end. Plain and volatile
// because it is written once by somebody else and read by a loop inside
// kl_slink, which is handed a flag rather than a lock — the honest shape for
// exactly that.
static volatile int  g_guest_quit;

const char *kl_app_target_name(void) { return g_target ? g_target->name : "(unconfigured)"; }
// See kl_app.h: the target name even before configure sets g_target, so the
// launcher UI knows which game it is on the first render. Same source configure
// uses (KL_TARGET override, else the compile-time default).
const char *kl_app_target_name_or_default(void) {
    return g_target ? g_target->name : kl_env_str("KL_TARGET", KL_TARGET_DEFAULT);
}
int kl_app_target_is_steamlink(void) {
    return g_target && g_target->kind == KL_GUEST_STEAMLINK;
}
static int target_is_jkxr(void) {
    return g_target && g_target->kind == KL_GUEST_JKXR;
}
static int target_is_sdl(void) {
    return g_target && g_target->kind == KL_GUEST_SDL;
}
int kl_app_target_wants_full(void) {
    // In-world VR titles render their MENU as a full stereo scene too, so .mixed
    // leaves it a flat window floating in passthrough (a jagged quad) until gameplay
    // starts. Default those to .full immersion from boot: the native OpenXR kind
    // (vicecity) and the folder-fed launcher games — hl1 (GLES3JNI) and
    // cs1/hl2/portal (SDL2). KL_FULL=0 forces .mixed back for development.
    return g_target && (g_target->kind == KL_GUEST_NATIVE ||
                        g_target->kind == KL_GUEST_GLES3JNI ||
                        g_target->kind == KL_GUEST_SDL2);
}
// By NAME, and deliberately so: `kind` is `ue4` here (it names the
// NativeActivity door, which UE5 kept), and deadpool and re4 share it. The two
// bakes below are answers to things measured in THIS title's device log and
// must not reach a UE4 guest that has never shown either symptom.
__attribute__((unused)) static int target_is_pinballfx(void) {
    return g_target && g_target->name && !strcmp(g_target->name, "pinballfx");
}
// Whether the guest brought its own render thread, and therefore whether there
// is any such thing as an inline frame here. Three of the four kinds do: Steam
// Link spawns one inside onCreate, an Unreal guest inside android_main, and an
// OpenJK guest inside its own onCreate. Only a Unity guest hands us
// nativeRender to call.
int kl_app_target_owns_frame_loop(void) {
    return g_target ? kl_driver_owns_frame_loop() : 0;
}

// The two scene-policy flags, and why they do not read g_target directly.
//
// SwiftUI evaluates the App's `body` — where the ImmersiveSpace and its
// modifiers are declared — on its own schedule, and nothing guarantees that
// kl_app_configure has run by then. `Immersive.wanted` carries a note about
// exactly this hazard and dodges it by being called only from boot(); a scene
// modifier has no such option, because the modifier IS the declaration.
//
// So these resolve the target themselves, from the same two sources configure
// uses and in the same order — KL_TARGET, then the build-time default — and
// touch nothing else. Configure's answer, once it exists, is identical; before
// it exists, this is still right, where reading g_target would silently answer
// "beatsaber's policy" for every target.
static const kl_target *target_now(void) {
    if (g_target) return g_target;
    return kl_target_lookup(kl_env_str("KL_TARGET", KL_TARGET_DEFAULT));
}

int kl_app_target_own_hands(void) {
    const kl_target *t = target_now();
    return t && t->own_hands;
}

int kl_app_target_opaque_world(void) {
    const kl_target *t = target_now();
    return t && t->opaque_world;
}

static char g_libdir[1024];
static char g_assets[1024];
static char g_files[1024];
static char g_apk[1024];
static char g_dylibs[1024];
static char g_log[1024];
static char g_status[512] = "not configured";
const char *kl_app_assets_dir(void) { return g_assets[0] ? g_assets : NULL; }

// Where the run has got to, for the heartbeat below; the driver reports its own
// phases through it.
static void app_phase(const char *p);

const char *kl_app_log_path(void) { return g_log; }
const char *kl_app_status(void)   { return g_status; }

static int have(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0; }

// Frameworks/ always exists (the Swift runtime lives there), so its presence
// says nothing. What matters is whether *this target's* translations were
// embedded — a bundle carrying the other app's guest would pass a bare
// directory test and then fail to find a single library.
static int have_translations(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s.framework/%s", g_dylibs,
             g_target->entry_lib, g_target->entry_lib);
    return have(p);
}

static int missing(const char *what, const char *path) {
    snprintf(g_status, sizeof g_status, "missing %s: %s", what, path);
    return 1;
}

// --- the shader cache outlives the driver, and must not -------------------
//
// r22, and it cost a whole round to find. A GLES guest that ships a program
// binary cache (UE4's `ProgramBinaryCache`, Unity's shader cache) writes
// `glGetProgramBinary` blobs into its external files directory and reads them
// back with `glProgramBinary` on the next launch. Those blobs are only ever
// valid for the driver that produced them — on Android that is enforced by the
// vendor blob cache, which is keyed on the driver build and thrown away when it
// changes. **Here there is no such key.** ANGLE hands the Metal shader compiler
// through, the binaries carry the OS build they were compiled under, and
// nothing on this platform invalidates them.
//
// So Tetris Effect wrote a 54.8 MB cache on 2026-08-23 under one xrOS beta,
// The tester updated the headset on 2026-08-24, and the next launch reloaded that
// cache into programs `glProgramBinary` silently refused to link. UE4 used them
// anyway, every `glUseProgram` answered GL_INVALID_OPERATION, the RHI raised
// Fatal, and `AndroidThunkJava_ForceQuit` was called 611 times before the game
// thread segfaulted. The visible symptom was "it crashes right after the
// Loading screen" — i.e. exactly at the first level whose shaders came from the
// cache rather than from source.
//
// The fix is the key Android has and we did not: fingerprint the driver, store
// it beside the cache, and delete the cache when it changes. This is deliberate
// at the PLATFORM level rather than per-port — every guest that persists
// compiled GL programs is one OS update away from the same crash, so the guard
// lives here where every target passes through it.
//
// KL_SHADER_CACHE_GUARD=0 leaves the cache alone (to reproduce the failure).
#define KL_CACHE_STAMP ".klepton-gl-driver"

static void rm_rf(const char *path, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p, depth + 1);
        else unlink(p);
    }
    closedir(d);
    rmdir(path);
}

// The directory names guests use for compiled-program caches. Matched by name
// at any depth under android-files, because each engine buries it differently
// (UE4 puts ProgramBinaryCache at the root, Unity puts its cache under a
// per-company subtree).
static int is_cache_dir(const char *name) {
    static const char *const names[] = {
        "ProgramBinaryCache",   // UE4 FOpenGLProgramBinaryCache
        "ShaderCache",          // UE4 FShaderCache / misc
        "il2cpp_cache",         // Unity, when it caches compiled output
        "UnityShaderCache",
        "GPUCache",
        NULL,
    };
    for (int i = 0; names[i]; i++) if (!strcmp(name, names[i])) return 1;
    return 0;
}

static int purge_caches(const char *root, int depth) {
    if (depth > 6) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (is_cache_dir(e->d_name)) {
            fprintf(stderr, "  [klepton] shader cache: dropping %s "
                            "(compiled under a different GL driver)\n", p);
            rm_rf(p, 0);
            n++;
        } else {
            n += purge_caches(p, depth + 1);
        }
    }
    closedir(d);
    return n;
}

// The fingerprint. `kern.osversion` is the OS BUILD ("24M5359a"), which is what
// actually moves when a beta lands and is what the Metal compiler versions its
// output by; hw.model pins the GPU family so a restore onto different hardware
// counts as a change too.
static void shader_cache_guard(const char *files) {
    if (!kl_env_on("KL_SHADER_CACHE_GUARD", 1)) return;

    // KL_SHADER_CACHE_DROP=1 — the same sweep, unconditionally, for a run that
    // needs the guest to COMPILE rather than reload. r21's worn log linked
    // exactly SIX programs in eight thousand frames: everything else arrived
    // through glProgramBinary out of UE4's ProgramBinaryCache, and a program
    // that arrives as a binary never calls glShaderSource, so every instrument
    // that reads GLSL is blind on a warm cache. Dropping the cache costs one
    // long "compiling shaders" load and buys the whole shader library as text.
    // ONCE per install, not once per launch: a marker beside the stamp says the
    // forced drop has already been paid for. The tester relaunches a build several
    // times a round and a minutes-long recompile on every one of them is a
    // worse instrument than no instrument.
    const char *drop_tok = kl_env_str("KL_SHADER_CACHE_DROP", NULL);
    if (drop_tok && strcmp(drop_tok, "0")) {
        // The marker records the TOKEN, so a later round forces a fresh drop by
        // handing a different one ("r24", "r25", ...) rather than by finding and
        // deleting a file inside a container it cannot see.
        char mark[1200], had[64] = "";
        snprintf(mark, sizeof mark, "%s/%s", files, ".kl_forced_drop");
        FILE *r = fopen(mark, "r");
        if (r) {
            if (!fgets(had, sizeof had, r)) had[0] = 0;
            fclose(r);
            for (char *p = had; *p; p++) if (*p == '\n') { *p = 0; break; }
        }
        if (strcmp(had, drop_tok)) {
            int dropped = purge_caches(files, 0);
            FILE *m = fopen(mark, "w");
            if (m) { fprintf(m, "%s\n", drop_tok); fclose(m); }
            fprintf(stderr, "  [klepton] KL_SHADER_CACHE_DROP=%s: %d compiled-program "
                            "cache(s) dropped on purpose — this run recompiles from "
                            "GLSL so the source-reading instruments can see it. "
                            "Expect a long first load; later launches of this "
                            "install are warm again.\n", drop_tok, dropped);
        }
    }

    char want[256] = "", tmp[128];
    size_t n = sizeof tmp;
    if (sysctlbyname("kern.osversion", tmp, &n, NULL, 0) != 0) return;
    snprintf(want, sizeof want, "os=%s", tmp);
    n = sizeof tmp;
    if (sysctlbyname("hw.model", tmp, &n, NULL, 0) == 0) {
        size_t at = strlen(want);
        snprintf(want + at, sizeof want - at, " hw=%s", tmp);
    }
    // ...and OUR half of what the cache holds. The blobs are compiled from the
    // text klfb_rewrite_glsl produced, not from the guest's own source, so a
    // change to a rewrite rule invalidates them exactly as an OS update does —
    // and used to do so with nothing anywhere saying it had. See
    // KL_GLFB_REWRITE_GEN. This is what replaces r24's hand-baked
    // KL_SHADER_CACHE_DROP token, which the round it was written for has ended.
    {
        size_t at = strlen(want);
        snprintf(want + at, sizeof want - at, " glsl=%d", KL_GLFB_REWRITE_GEN);
    }

    char stamp[1200];
    snprintf(stamp, sizeof stamp, "%s/%s", files, KL_CACHE_STAMP);

    char have_s[256] = "";
    FILE *f = fopen(stamp, "r");
    if (f) {
        if (!fgets(have_s, sizeof have_s, f)) have_s[0] = 0;
        fclose(f);
        for (char *p = have_s; *p; p++) if (*p == '\n') { *p = 0; break; }
    }
    if (!strcmp(have_s, want)) return;   // same driver — the cache is valid

    // First run after a fresh install has no stamp and no cache either, so the
    // sweep is free; it is only ever expensive when it is also necessary.
    int dropped = purge_caches(files, 0);
    fprintf(stderr, "  [klepton] GL driver fingerprint %s -> %s: %d compiled-program "
                    "cache(s) dropped. A guest's glProgramBinary blobs do not "
                    "survive an OS update, and using them is a silent unlinked "
                    "program (KL_SHADER_CACHE_GUARD=0 keeps them).\n",
            have_s[0] ? have_s : "(none)", want, dropped);

    f = fopen(stamp, "w");
    if (f) { fprintf(f, "%s\n", want); fclose(f); }
}

// <container>/klepton-env.txt: NAME=VALUE per line, `#` comments. A headset
// launch has no command line and the row's baked env needs a rebuild; this file
// is pushed with `devicectl device copy to` between launches, so a knob (render
// scale, MSAA, an A/B) is one file copy and a relaunch. Applied FIRST and with
// overwrite, so it wins over the row and every setenv(...,0) default; each
// line is said out loud. Absent file = nothing happens.
// What kl_env_file_apply set, kept so kl_app_boot can repeat it into the boot
// log: configure runs before open_log, so the lines above reach only the
// console, and a Home View launch has none.
static char g_env_file_applied[1024];

static void kl_env_file_apply(const char *container) {
    char path[1200], line[512];
    snprintf(path, sizeof path, "%s/klepton-env.txt", container);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq || eq == s) {
            fprintf(stderr, "  [env-file] \"%s\" is not NAME=VALUE — skipped\n", s);
            continue;
        }
        *eq = 0;
        setenv(s, eq + 1, 1);
        fprintf(stderr, "  [env-file] %s=%s (klepton-env.txt)\n", s, eq + 1);
        size_t at = strlen(g_env_file_applied);
        snprintf(g_env_file_applied + at, sizeof g_env_file_applied - at,
                 "%s%s=%s", at ? " " : "", s, eq + 1);
    }
    fclose(f);
}

int kl_app_configure(const char *resources, const char *container) {
    if (!resources || !container) return missing("path", "(null)");
    kl_env_file_apply(container);

    const char *want = kl_env_str("KL_TARGET", KL_TARGET_DEFAULT);
    g_target = kl_target_lookup(want);
    if (!g_target) return missing("guest target (KL_TARGET)", want);
    // ...and record it as THE row for this run. kl_target_apply_host does this
    // for a host run; the app builds its own paths and never calls that, so
    // without this line runtime code asking kl_target_current() gets the table
    // DEFAULT — which for a UE4 guest means re4's spelling of
    // nativeSetAndroidVersionInformation and a fault inside the guest.
    kl_target_use(g_target);

    // The veneers' TSD slot is claimed HERE rather than wherever the first guest
    // library happens to load. The slot number is a constant baked into every
    // veneer, and Darwin hands external pthread keys out upward and never
    // reissues a held one — so whether it is still free is a race against
    // everything else in the process that creates keys. Beat Saber wins it by
    // accident: kl_app_boot loads libmain before ANGLE exists. The Steam Link
    // chain does not — something on its load path brings ANGLE up first, ANGLE
    // takes the process past the slot, and the guest fails to load with "TSD
    // slot N is unavailable", which reads as a platform limit and is a
    // scheduling accident.
    //
    // This is the earliest point both targets share. It is not fatal on its own:
    // an ELF-tree run with no veneered library is unaffected, and the load path
    // still refuses by name for one that needs them.
    if (kl_x18_init() != 0)
        fprintf(stderr, "  [klepton] x18: TSD slot %d was already taken at "
                        "configure time — a veneered guest will refuse to load\n",
                KLX_TSD_SLOT);

    // The guest libraries ride in the bundle: AMFI accepts a dylib inside a
    // bundle we signed, and nothing has established that it accepts one pushed
    // into Documents afterwards. The 2.3 GB of assets go the other way — into
    // the container — because they carry no code and re-uploading them on every
    // install would make the edit/run loop unusable. See visionos/README.md.
    snprintf(g_libdir, sizeof g_libdir, "%s/guest/lib/arm64-v8a", resources);
    // Frameworks/, not a directory of our own: that is where Xcode code-signs
    // what it embeds, and a loose Mach-O elsewhere in the bundle is only
    // sealed. kl_load_auto knows both layouts.
    snprintf(g_dylibs, sizeof g_dylibs, "%s/Frameworks", resources);
    snprintf(g_assets, sizeof g_assets, "%s/%s/assets", container, g_target->tree);
    snprintf(g_apk,    sizeof g_apk,    "%s/%s", container, g_target->apk);
    snprintf(g_files,  sizeof g_files,  "%s/android-files", container);
    snprintf(g_log,    sizeof g_log,    "%s/klepton-boot.log", container);

    // A crash report of its own, beside the log. The boot log is stdout with
    // stderr duped onto it, and neither survives a fault reliably — a guest may
    // take both fds, and stdio can hold the last writes behind a lock the dying
    // thread never releases. This path is opened at fault time and fsync'd, so
    // "it crashed and the log stops just before" stops being the whole story.
    static char crash[1024];
    snprintf(crash, sizeof crash, "%s/klepton-crash.log", container);
    kl_fault_set_crash_path(crash);

    // Check before running, not after failing. A missing asset tree otherwise
    // surfaces three layers up inside Unity as something that reads like a shim
    // bug.
    // With translations embedded, g_libdir never has to exist: it is only the
    // string NativeLoader.load() is handed and the prefix the DT_NEEDED walk
    // builds paths from, and kl_load_auto turns "<libdir>/libunity.so" into
    // "Frameworks/libunity.framework/libunity" before touching the disk. The ELF
    // tree is therefore not in the bundle — 80 MB that would buy only an A/B
    // against the mmap loader the translations replace.
    if (!have_translations() && !have(g_libdir))
        return missing("guest libraries (neither translations nor an ELF tree)", g_libdir);
    // Assets and the APK are required only for an engine that reads its data out
    // of the APK's assets/ tree (Unity, UE4, JKXR, …). The FOLDER-FED guests carry
    // no assets/ and no .apk: a plain NativeActivity port (vicecity), and the
    // Xash/Source launcher titles (hl1 = GLES3JNI, cs1/hl2/portal = SDL2) whose
    // whole data set is the game folder the PLAYER supplies through the launcher —
    // there is nothing to stage, so the guard would wrongly refuse to boot. The
    // path strings above are still set (harmless — those guests never resolve an
    // APK/asset path); only the existence check is skipped.
    int folder_fed = g_target->kind == KL_GUEST_NATIVE   ||
                     g_target->kind == KL_GUEST_GLES3JNI ||
                     g_target->kind == KL_GUEST_SDL2;
    if (!folder_fed && !have(g_assets))
        return missing("staged assets (run stage_assets.sh)", g_assets);
    if (!folder_fed && !have(g_apk))
        return missing("staged APK (run stage_assets.sh)", g_apk);
    mkdir(g_files, 0755);
    // (r24's baked KL_SHADER_CACHE_DROP="r24" is GONE. It did its job — the
    // worn run it bought linked 610 programs from source and dumped the five
    // shaders that named the defect — and the reason it had to exist at all is
    // now handled properly, by KL_GLFB_REWRITE_GEN inside the fingerprint. The
    // env knob itself stays available in shader_cache_guard for a run that
    // wants a forced recompile without a rewrite change.)
    //
    // (r26's baked KL_SHADER_CACHE_DROP="r26" is GONE too, and it also did its
    // job: the warm-cache run it bought dumped program 334's vertex and
    // fragment text, which is what named the volumetric fog. r27 changes a
    // rewrite RULE, so the cache drop it needs is the proper one — the bumped
    // KL_GLFB_REWRITE_GEN below is inside the fingerprint and drops the cache
    // once, by itself.)

    // Before any guest code runs, and before the guest's own file layer can
    // hand it a stale blob. See shader_cache_guard.
    shader_cache_guard(g_files);

    // r24's scene-texture shader dump was BAKED here, for EVERY target, because
    // the host cannot produce it: a host run of this guest reaches the loading
    // screen and stops there — four thousand frames of it compile NINE shaders,
    // all of them UE4's own blit/clear pair — so every material shader in this
    // title exists only on device. It did its job (r25 named the missing
    // GL_ARM_shader_framebuffer_fetch_depth_stencil off the text it wrote), and
    // r31 UNBAKES it: it is a per-launch write of up to 64 GLSL files on the
    // link path, in every port, for a question that is closed.
    //
    // The knob is unchanged and still the way to ask it again. Give it a
    // directory that exists — on device, one inside the data container, which is
    // the only place `devicectl device copy from` can read back:
    //
    //   KL_GLFB_DUMP_GLSL_DEPTH=<container>/glsl-depth  (and mkdir it first)
    //   xcrun devicectl device copy from --device <udid> \
    //     --domain-type appDataContainer --domain-identifier <bundle> \
    //     --source Documents/glsl-depth --destination .
    //
    // The relative-path resolver below (cap_knobs) still turns a bare name into
    // a container path and makes the directory, so `KL_GLFB_DUMP_GLSL_DEPTH=
    // glsl-depth` on a devicectl launch is all it takes.

    // r26's pixel history (FILLHIST) was BAKED for tetris through r30 and is
    // UNBAKED here, in r31, because the defect it hunted is fixed and it is the
    // most expensive diagnostic in the tree: klfb_fillhist reads a strip of the
    // eye texture back after every draw of a sampled pass, which serialises the
    // frame — FOUR VISIBLE HITCHES per session, on purpose, plus up to 64 GLSL
    // files written when it names something. A shipping build pays neither.
    //
    // Nothing about it is deleted. To ask the question again on a device build:
    //
    //   KL_GLFB_FILLHIST=1                 the four-pass pixel history
    //   KL_GLFB_FH_SHAPE=6                 nominate by r29's shape, not colour
    //   KL_GLFB_DUMP_GLSL_FILL=glsl-fill   the named program's text
    //   xcrun devicectl device copy from --device <udid> \
    //     --domain-type appDataContainer --domain-identifier <bundle> \
    //     --source Documents/glsl-fill --destination .
    //
    // (r30's `[mipchain]` census and the `[lod]` census stay on: both are free —
    // they read tables the bind tracker already maintains, print at the 15 s
    // cadence with the rest of `[census]`, and cost the player nothing. They are
    // also the two instruments that the still-open question about WHY a level
    // above 0 was ever selected will need, so they should be in a worn log.)
    if (g_target && g_target->name && !strcmp(g_target->name, "tetris")) {
        // r27: the fix the fill dump bought, BAKED for tetris on the neutral arm.
        //
        // Program 334's fragment shader ends in UE4's fog composite —
        // `surface * TEXCOORD7.w + TEXCOORD7.xyz` — and its vertex shader
        // multiplies that transmittance by the ALPHA of a sampler3D lookup, the
        // IntegratedLightScattering volume. A volume that reads zeros therefore
        // does not dim anything, it DELETES every surface past the fog's start
        // distance and leaves the inscattering colour behind: one flat constant
        // that no zoom can change, cut by the fog evaluation's own vertex seams
        // (straight lines in screen space), receding as the camera comes closer.
        // That is the blue fill, in full, with the pitch/zoom/roll behaviour and
        // the "past 80%" threshold falling out of the distance ramp.
        //
        // KL_GLFB_VOLFOG=neutral rewrites the lookup to UE4's own
        // no-volumetric-fog value, vec4(0,0,0,1) — no inscattering, full
        // transmittance — which leaves the ordinary height fog exactly as it
        // was. KL_GLFB_VOLFOG=pass is the A/B control and restores r26.
        //
        // The measurement half rides in the same build: the 3D-texture census
        // (kl_glfb.c, klfb_tex3d_report) prints every volume's size, writes,
        // framebuffer attaches and sampler binds, so the log says whether the
        // volume really was never filled rather than leaving it assumed.
        setenv("KL_GLFB_VOLFOG", "neutral", 0);

        // r28: the volumetric arm ENGAGED — 246 vertex shaders rewritten, and
        // both shaders FILLHIST named that run carry vec4(0.,0.,0.,1.) in their
        // dumped post-rewrite text — and the tester's answer was "no change at
        // all". So the volumetric lookup was not the zero, and the honest next
        // move is not a fourth guess about WHICH term is.
        //
        // KL_GLFB_FOG=off removes the fog composite from the fragment stage
        // entirely: in_TEXCOORD7 is shadowed with UE4's own no-fog value, so
        // `surface * .www + .xyz` becomes `surface`, whatever the vertex shader
        // computed and whatever uniform fed it. Fill GONE -> the composite is
        // the mechanism and the vertex fog's own uniforms are the next round.
        // Fill STILL THERE -> the composite never painted it, `surface` is
        // already the constant, and the whole fog family closes in one run.
        //
        // The measurement half rides along: when FILLHIST names the draw it now
        // also dumps that program's active-uniform VALUES (kl_glfb.c,
        // klfb_uniform_dump). UE4's ExponentialFogParameters are plain
        // glUniform4fv arrays in this title, not a UBO, so glGetUniformfv reads
        // the density, the falloff, the cutoff distance and the inscattering
        // colour back directly instead of a fourth round inferring them.
        // ...and it is BACK TO `pass` as of the end of r28, because the A/B
        // ANSWERED. The tester wore the `off` build (launched 16:04:56, the log
        // carries `fog composite (off): 566 fragment shader(s)`) and reported
        // "still no change at all". The composite was removed from 566 shaders
        // and the fill did not move: **the fog composite does not paint it**.
        // Leaving `off` baked would ship a title with no fog for no benefit.
        // The knob stays, on the control arm, as the disproof's own re-run.
        setenv("KL_GLFB_FOG", "pass", 0);

        // r29: the uniform dump ANSWERED, and the answer is arithmetic.
        //
        // FILLHIST named program 276 — six vertices, the full eye viewport, two
        // 1024x1024 ASTC textures on units 0 and 1 — and its uniform values
        // reduce its own fragment shader (the same material r28 dumped as
        // program 334; identical uniform signature, pc1_m[1]/pc2_m[5]) to
        //
        //     out = 0.66667 * 0.99 * pc2_m[1].xyz * B
        //
        // because pc2_m[2].w is 0 (so the vertex-varying factor collapses to 1)
        // and pc1_m[0].y is 0 (so the invert-mix is a no-op). pc2_m[1] measured
        // (0, 0.01, 0.03), and 0.01/0.03 encoded to sRGB is (0, 25, 48) — which
        // is the tester's screenshot fill of sRGB(0, 24, 46) to within two LSBs,
        // with the red channel EXACTLY zero because pc2_m[1].x is exactly zero.
        // B is the bracket, and it is built out of nothing but texture reads.
        //
        // So the colour is a uniform the game meant to set, and the defect is
        // that B is CONSTANT ACROSS THE PRIMITIVE: every fragment of the quad
        // reads the same texel. (A texture reading black is refuted by the same
        // arithmetic — it makes B negative and the output pure black, not navy;
        // so is a texture reading white, which clips.) Two ways to arrive at one
        // texel, and this build asks both at once:
        //
        //   * KL_GLFB_NOMIP=1 (baked here; KL_GLFB_NOMIP=0 is the control)
        //     demotes every mipmap min filter to level 0. The [lod] census in
        //     this same log says 628 of the run's allocations carry a mip chain
        //     and the deepest is 12 levels, so a 1024x1024 read near its top mip
        //     IS available as a mechanism — and r20's KL_VRR_FLOOR tested one
        //     CAUSE of a high LOD, never the LOD itself. Fill gone -> it is a
        //     mip level, and which one is the round after.
        //   * The other end, measured rather than switched: FILLHIST now reads
        //     the named draw's VERTEX ATTRIBUTE VALUES out of the guest's own
        //     buffers (resolving the draw's first vertex through the index
        //     buffer first) and prints them beside the descriptors, plus each
        //     sampler unit's min filter, base/max level and bound sampler
        //     object. The UVs of this material are `in_ATTRIBUTE4 + time*0.01`,
        //     so an attribute that is IDENTICAL ON EVERY VERTEX paints the same
        //     flat picture from the coordinate side, and the log says so in
        //     those words.
        //
        // Fill still there with NOMIP on, and the attributes varying sanely, and
        // the LOD family closes too — leaving the textures' own contents.
        setenv("KL_GLFB_NOMIP", "1", 0);

        // r30: THE NOMIP ARM ANSWERED, AND THE ANSWER IS YES.
        //
        // The tester wore `f6413ba` and reported "blue fill gone!" — three minutes
        // in the well, zoomed out. `build/logs/r30-worn.log` carries the
        // engagement line (`NOMIP: min filter 0x2700 -> 0x2600`), and its
        // steady-state cadence is 8.33 ms mean with zero past-deadline frames,
        // so forcing level 0 across the whole title costs nothing measurable on
        // this device. NOMIP therefore STAYS BAKED: it is the only thing
        // standing between the tester and the fill, and until the round after names
        // a level, removing it would put the defect back to buy nothing.
        //
        // What that answer does NOT settle is WHY a level above 0 was being
        // read at all. A six-vertex quad covering a 6144x4928 viewport with a
        // 1024x1024 texture is MAGNIFYING — its natural LOD is about -2.6, and
        // the minification filter should never have been consulted. Two things
        // can put it there, and this build measures both instead of switching
        // either:
        //
        //   * THE CHAIN. `[mipchain]` walks every allocation with more than one
        //     level and prints which levels ever received an upload and how
        //     many bytes each got, against the bytes its own dimensions imply.
        //     An allocated-but-never-written level reads whatever the driver
        //     left in it — a constant, which is the symptom exactly. Free: the
        //     destination texture name comes from the bind tracker, not a GL
        //     query. UE4 streams mips, so a MISSING TAIL is normal and named as
        //     such; a MISSING HEAD or an INTERIOR HOLE is not.
        //   * THE COORDINATES. FILLHIST has always been keyed on the FILL'S
        //     COLOUR, and the NOMIP build has no fill — all four of r30's passes
        //     named nothing, which is not evidence of anything. KL_GLFB_FH_SHAPE
        //     nominates by r29's measured SHAPE instead (six vertices into a
        //     half-eye-or-wider viewport), so the units, the uniforms and above
        //     all the vertex-ATTRIBUTE values print on a build where the picture
        //     is already fixed. Huge or degenerate UV derivatives are what drive
        //     an LOD this wrong, and attribute 4 is this material's UV set.
        //
        // r31 UNBAKES FH_SHAPE with FILLHIST — it is the same readback, keyed
        // differently, and it costs the same four hitches. The question it asks
        // is genuinely still open and is written up in STATUS as such; asking it
        // costs one env var on a devicectl launch, and the answer is worth a
        // round of somebody's time, not a hitch in everybody's session.
        //
        // The chain census is FREE and stays on: `[mipchain]` prints inside
        // `[census]` on every target, so the level-write picture is in any worn
        // log without anyone arming anything.

        // r32: THE HUD SUPERSAMPLE.
        //
        // The remaining defect is not the fill and not the LOD family. The tester
        // has reported HUD and menu text as "a little blurry, always has been"
        // across every round, while the intro screens and the rest of the game
        // read sharp — and NOMIP, the foveation A/Bs (r21, r23) and the fog
        // family all left it exactly where it was. Font atlases are
        // single-level, so the whole LOD story was never available to it.
        //
        // What IS available is resolution. UE4 draws each of these panels into
        // a world-space widget render target and then textures it onto a quad,
        // and the r26 framebuffer census measured the two targets it uses:
        //
        //     fb 32  510x688  fmt 0x8c43  399,768 draws (~44 a frame)
        //     fb 28  510x306  fmt 0x8c43  275,509 draws (~31 a frame)
        //
        // Quest-sized. Composited into a 6144-wide eye across a 105 degree
        // field, one of their texels covers several device pixels and the text
        // is soft no matter what the sampler does.
        //
        // x3 in each dimension: 510x688 becomes 1530x2064, which is 12.6 MiB of
        // RGBA8 against a 3780 MiB footprint with ~4400 MiB free — the cost is
        // noise. The fragment work grows 9x on ~75 draws a frame of flat UI,
        // which is the one thing to watch in the log (the steady-state mean
        // should still read 8.33 ms with 0 past deadline).
        //
        // The runtime half is `KL_GLFB_RT_SCALE` in kl_glfb.c, and it carries
        // the new glViewport/glScissor thunks with it: without those the guest
        // would draw its HUD into the bottom-left ninth of the larger texture.
        // Both knobs are OFF by default and no other target sets them.
        //
        // THE CONTROL is KL_GLFB_RT_SCALE= (empty) on a devicectl launch, which
        // puts the HUD back exactly where r31 left it.
        //
        // r33: THE MENU IS ON DIFFERENT TARGETS, AND THE LOG NAMED THEM.
        //
        // The tester wore the r32 build and reported "HUD text is sharp, but not the
        // MENU text, like when you pick which game to play" — so the two sizes
        // above were the HUD's and only the HUD's. The r32 build shipped exactly
        // the instrument that answers "extend the list to what?", and the r33
        // worn log prints thirteen distinct unmatched offscreen colour targets.
        // Every one of them is UNDER 1024 — there is no 3840-wide menu target on
        // this device at all, so the scale guard blocked nothing.
        //
        // Two of the unmatched ones are visible in the framebuffer census with
        // their draw counts, and their signature is exactly a menu panel: they
        // are painted during the menu and then FROZEN. Between the census at
        // 12:28:47 and the one at 12:28:58 the two scaled HUD targets climbed
        // (16,672 -> 29,524 and 16,087 -> 35,349 draws) while
        //
        //     fb 29  680x204   2,727 draws     unchanged across both censuses
        //     fb 35  469x221   5,821 draws     unchanged
        //     fb 27  272x48    3,187 draws     unchanged
        //     fb 33  272x48    3,930 draws     unchanged
        //
        // did not move a single draw. Six more sizes were named by the same
        // instrument with NO draw count, because the per-framebuffer census was
        // capped at 16 entries and the menu filled it (raised to 48 this round).
        //
        // So the list grows by every unmatched size shaped like a UI panel:
        // 680x204, 469x221, 272x48, 272x91, 170x204, 170x122, 326x102, 234x49
        // and 500x500. The four sizes deliberately LEFT OFF are 1x1 and 2x2
        // (degenerate), and 256x256 and 512x512 — power-of-two RGBA8/RGBA16F
        // intermediates, which is what a scene capture or an adaptation buffer
        // looks like, not what text looks like.
        //
        // THE MEMORY, at the same x3 (RGBA8, area x9): 680x204 grows 0.53 -> 4.8
        // MiB, 500x500 0.95 -> 8.6, 469x221 0.40 -> 3.6, and the six small ones
        // total under 4.5 MiB between them. About +20 MiB over r32 against the
        // 4,555 MiB the r33 log measured free at steady state (`[hb] foot=3636M
        // avail=4555M`), so x3 stands rather than dropping to x2 — and r32's
        // cadence never moved off 8.33 ms mean / 0 past deadline with the HUD
        // supersample live, which is what says the fragment cost is affordable.
        //
        // THE RISK TO WATCH, named rather than assumed: if one of these panels is
        // post-processed with a texel-size uniform the CPU computes from the size
        // it ASKED for (a blur radius, an outline width), the effect will be
        // three times finer than intended on that panel. That reads as a slightly
        // thin outline, never as misplacement. MISPLACED or CLIPPED menu text
        // would mean a viewport/scissor path that these targets take and the HUD
        // ones do not — take the size back off the list if so.
        //
        // r34: THE MENU BODY IS 1305 WIDE, AND THE GUARD WAS REFUSING IT.
        //
        // The tester wore the r33 build and reported the menu text still soft, with
        // one exception: "there was one 'Oculus User' panel in the bottom right
        // on the first main menu that was sharp." That single exception is the
        // whole diagnosis. 326x102 is on the list above, and the r34 worn log
        // shows it grown to 978x306 — so the mechanism works, on exactly the one
        // panel the tester could name, and every OTHER menu surface is on a target
        // the list never reached.
        //
        // The r34 log names them. The per-framebuffer census, uncapped since r33,
        // shows four live UI framebuffers that climb through the whole run:
        //
        //     fb 27  tex 2478  1305x183     129 ->  2,924 ->  2,924 draws
        //     fb 28  tex 2485  1305x183   1,183 -> 15,799 -> 20,619 draws
        //     fb 29  tex 2502  1305x734     435 ->  5,178 ->  7,106 draws
        //     fb 32  tex 2500  1305x734           6,427 -> 11,729 draws
        //
        // 0x8c43 (SRGB8_ALPHA8), the same format as the HUD's targets, redrawn
        // every frame rather than painted-and-frozen: a 1305x734 panel is the
        // menu BODY and a 1305x183 one is its title strip. And the r33 round
        // could not have seen them, because it recorded "no target on this device
        // is 1024 or wider" from a 55-second run that never left the first page.
        //
        // WHY THEY WERE MISSED IS THE GUARD, NOT THE LIST. 1305 >= 1024, so even
        // adding the size by hand would have changed nothing — r33's own parse
        // warning would have printed and the target would have stayed native.
        // The guard is fixed in kl_glfb.c this round: it now bounds the size that
        // comes OUT (8192), which is what an "absurd allocation" actually means,
        // and still refuses every eye and scene target in this log.
        //
        // THE MEMORY, at the same x3: 1305x734 becomes 3915x2202, 34.5 MiB of
        // RGBA8 each and the log allocates three of them; 1305x183 becomes
        // 3915x549, 8.6 MiB each and there are two. About +115 MiB over r33,
        // against the 4,383 MiB the r34 log measured free at steady state
        // (`[hb] foot=3808M avail=4383M`). The fragment cost is the thing to
        // watch in the cadence line: fb 28 alone did 20,619 draws into a target
        // that is now 9x the pixels. r33's log held 8.33 ms mean / 0 past
        // deadline with the smaller panels live; if r34's does not, x2 on these
        // two sizes is the trade, not reverting them.
        setenv("KL_GLFB_RT_SCALE",
               "510x688,510x306,680x204,469x221,272x48,272x91,"
               "170x204,170x122,326x102,234x49,500x500,"
               "1305x734,1305x183", 0);
        setenv("KL_GLFB_RT_SCALE_FACTOR", "3", 0);
    }

    // ---- Simpsons Hit & Run: the black world needs a PICTURE, not a counter --
    //
    // Launches #4 and #5 both render Springfield as flat black with a log that
    // has nothing wrong in it: 0 GL errors, 0 link errors, 828 draws and 210k
    // triangles a frame into the eye texture, three shadow cascades reporting
    // FRAMEBUFFER_COMPLETE, and the compositor counting 5,517 frames "with a
    // picture" -- a count of frames PRESENTED, not of frames that were lit.
    // Five rounds of counters have said the world is being drawn and none of
    // them can say what it looks like.
    //
    // So this build takes the picture. KL_GLFB_OUT writes the eye texture and
    // KL_GLFB_DUMP_FBOS writes one PNG per framebuffer beside it -- the shadow
    // cascades, the 516x552 pair, the eye target itself -- and the FIRST one
    // that is black names the pass that lost the world. Both are BAKED, not
    // env-gated, for the reason every instrument on this program is baked: the
    // Home View carries no environment and the tester launches from the Home View,
    // so a knob that only exists on a devicectl launch is not an instrument for
    // the build he actually wears.
    //
    // The budget is the whole design. The interesting frame is minutes into a
    // run nobody can drive from here -- a language screen, two videos, a level
    // load -- so the capture ARMS on the guest's own first shadow pass (a
    // framebuffer with depth and no colour, which only a loaded level builds),
    // waits 300 swaps for the picture to settle, and then takes FOUR captures
    // 600 swaps apart. That is roughly 3 s, 10 s, 17 s and 24 s of gameplay,
    // about 30 PNGs, and then it stops for good. It has to stop: each capture
    // is a full-frame glReadPixels plus one per FBO on the guest's own render
    // thread, and visionOS kills an immersive app that goes 2.0 s without
    // submitting a drawable.
    //
    // KL_GLFB_OUT is relative on purpose -- the loop below resolves it against
    // the data container and makes the directory, which is what makes the files
    // retrievable with `devicectl device copy from`.
    if (g_target && g_target->name && !strcmp(g_target->name, "simpsons")) {
        // ---- ROUND 36: where the radar-capture PNGs go ----
        //
        // Relative on purpose, like KL_GLFB_OUT: the resolver below turns it
        // into a container path, makes it and empties it, so the files come
        // back with `devicectl device copy from` and belong to THIS launch.
        // kl_sdl.c's SHAR door turns the trace itself on; this row only names
        // the directory. KL_SHAR_RADARTRACE=0 in the environment takes both out.
        setenv("KL_SHAR_RADARTRACE_DIR", "radartrace", 0);

        // ---- THE BLACK WORLD, and it is a size answer ----
        //
        // SDL learns how big its window is from SDLSurface.surfaceChanged ->
        // nativeSetScreenResolution, and the generic SDL door has never called
        // it. So SDL had NO window size, SDL_GL_GetDrawableSize -- the only
        // size function this guest imports -- answered zero, and Pure3D turned
        // that into glScissor(0, 0, 0, 0). GL_SCISSOR_TEST with a zero box
        // discards every fragment, glClear included, with no error and a
        // complete framebuffer: a world drawn at 210,000 triangles a frame that
        // never receives one pixel of itself, which is what rounds 4-8 measured.
        //
        // kl_sdl.c now makes that call, and this row states the size. It is
        // 2064x2208 because that is what this guest's eye buffers are -- it
        // pins Quest 3's native per-eye resolution regardless of what the
        // runtime recommends -- and on a Quest the Android surface and the eye
        // are the same numbers, so the box covers the eye exactly. The
        // placeholder window (1832x1920) would clip the top and right of every
        // frame; measured on the host, told 1832x1920 this guest answers
        // glScissor(0,0,1832,1920), and told 2064x2208 it answers
        // glScissor(0,0,2064,2208). It scissors to precisely what it is told.
        //
        // KL_SDL_SCREEN_RES=0 turns the whole handshake off, which is the A/B.
        //
        // ...and that size is also THE FIDELITY KNOB, because everything this
        // guest renders follows from it. Launches #4-#8 ran at 2064x2208 per
        // eye against a device recommending 5120x4107, and the reason was a
        // second never-called Java hook: the port author added
        // SDLSurface.nativeSetSHARRenderResolution(int,int), nothing called it,
        // and the engine took a fallback it names in its own log --
        // "SHAR Android PDDI display resolution [init-display-sdl-fallback]:
        // 2064x2208". kl_sdl.c now calls it, and the log reads
        // [init-display-java] instead.
        //
        // The eye SWAPCHAIN is a second number from a third place: the guest
        // multiplies a fixed 2064x2208 base by a render scale it keeps in
        // vrsettings.cfg. Told the display size alone it still asked the XR
        // runtime for 2064x2208; given the scale as well, its request, its
        // swapchain, its subImage rect, its viewport and its scissor all move
        // together -- which is why the scale is set through the guest's own
        // state rather than by substituting a bigger swapchain behind its back.
        // A swapchain we silently enlarged would leave every one of those
        // describing the old size: a picture drawn into one corner.
        //
        // Round 11 changed HOW: kl_sdl.c writes vrsettings.cfg rather than
        // calling SharOpenXR::SetRenderScale(). That setter -- like every
        // SharOpenXR setter -- saves the WHOLE settings block, and called from
        // the door it saved zeroed .bss over the file the guest was about to
        // read. It cost launch #9 its right-stick turn (smooth=0, snap=0), its
        // shadows (csm=0), its GTAO and its spatial HUD.
        //
        // KL_SIMPSONS_EYE=WxH drives all three from one number. The default is
        // 4128x4416 -- exactly 2x the guest's base in each axis, four times the
        // pixels, and the same per-eye size hl2vr runs on this device. Host at
        // 4128x4416: the front end draws 952,588 lit pixels where 2064x2208
        // drew 239,010, which is 4.0x for 4.0x the pixels with no clipping and
        // no GL errors.
        //
        // IF LAUNCH #9 JUDDERS, this is the one knob to move: the renderer is
        // DUAL-PASS (no multiview on ANGLE-on-Metal), so every pixel is paid
        // for twice. KL_SIMPSONS_EYE=3096x3312 is 1.5x (2.25x pixels) and
        // KL_SIMPSONS_EYE=2064x2208 is exactly the launch-#8 build.
        {
            const char *eye = kl_env_str("KL_SIMPSONS_EYE", "4128x4416");
            int ew = 0, eh = 0;
            if (sscanf(eye, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
                char res[32], scale[32];
                snprintf(res, sizeof res, "%dx%d", ew, eh);
                // The base is the guest's own, and it is not ours to guess:
                // 2064x2208 is what every launch before this one requested with
                // the scale at 1.0.
                snprintf(scale, sizeof scale, "%.4f", (double)ew / 2064.0);
                setenv("KL_SDL_SCREEN_RES", res, 0);
                setenv("KL_SHAR_RENDER_SCALE", scale, 0);
                printf("[app] simpsons eye %s (render scale %s of the guest's "
                       "2064x2208 base)\n", res, scale);
            }
        }

        // THE CAPTURE IS OFF from round 11. It did its job -- rounds 6-9 could
        // not have been read without it -- and it has three reasons to go now:
        // at 4128x4416 each readback is four times the pixels it was designed
        // for, it deflates on the guest's own render thread, and its arming
        // trigger (the first depth-only framebuffer) depends on CSM, which the
        // damaged settings block had switched off. Set KL_GLFB_OUT=glfbcap by
        // hand on a devicectl launch to bring it back; the censuses are cheap
        // and stay on.
        //
        // KL_TRACE_IO=1 is the round-11 instrument, and it is the one that
        // answers the mission-card question: once a second it prints cumulative
        // guest fread/read bytes with the fread attributed per open file. If
        // the ~30 s loading card is disk, the level's .rcf and .p3d reads are
        // in that census with their names on them; if it is not, the census is
        // flat and the time is CPU inside Pure3D. Shader compilation is already
        // ruled OUT by measurement -- launch #9 linked 34 programs, slowest
        // 0.000 s -- and so is the capture, which never fired in that run.
        setenv("KL_TRACE_IO", "1", 0);

        // ---- ROUND 23: the loading is a THREAD, and it loses to the frame ----
        //
        // Launch #22's ledger measured the streaming pipeline end to end and it
        // is not broken, it is slow: boot loads run 17-800 ms and the SAME files
        // through the SAME path in-level take 2.5-18.2 s, one at a time, with
        // the deferred-zone queue climbing to 7. When the tester backgrounded the
        // app at +277.789 the entire backlog -- an outstanding art\l1r1.p3d and
        // several zones' worth of Dump/Load -- completed inside one instant.
        // Loading runs at full speed the moment rendering stops.
        //
        // Round 23 read the guest to find out what "loading" is here, because
        // the fix for a per-frame pump and the fix for a starved thread are
        // different fixes. It is a thread:
        //
        //   radLoadManager's constructor (0x6eff18) ends in
        //   radThreadCreateThread(..., LoadThreadEntry, ..., prio 2, ...)
        //   -> radThread's ctor (0x6ebd8c) -> SDL_CreateThreadWithStackSize
        //   -> libSDL2's pthread_create -> our klb_pthread_create.
        //
        // The pacing the engine THINKS it has is gone. tLoadManager::SwitchTask
        // (0x6aa1d0) resolves through ILoadManager slot 0x70 to
        // radLoadManager::Service (0x6f0c2c), which calls SwitchTasks
        // (0x6f0cc0): Suspend, radThreadSleep(0), Resume. On this Android port
        // radThread::Suspend and ::Resume (0x6ec098, 0x6ec09c) are both a bare
        // `ret` -- the fiber-style hand-off radcore was written around is a
        // no-op, and all that is left of it is a zero-length sleep. So the load
        // thread's forward progress is whatever the scheduler gives it, and
        // nothing in the engine can ask for more.
        //
        // Which makes the deadpool question the right question -- and the
        // deadpool answer is already here. kl_pthread.c has given every guest
        // thread QOS_CLASS_USER_INITIATED since 981336e, and launch #22's own
        // log says so on line 281: "[thr] guest threads are created at QoS
        // user-initiated". So the prime suspect for this round arrived already
        // fixed, and the load thread is losing to the render path a tier ABOVE
        // it: kl_app.c's own frame thread is USER_INTERACTIVE, and so is the
        // compositor it feeds.
        //
        // 981336e chose INITIATED for the whole runtime deliberately and its
        // argument still holds for UE4's task graph -- a worker that shares the
        // frame producer's tier can preempt the thread whose deadline actually
        // matters. It does not hold for THIS guest, which has one loader and
        // one drive thread and no pool at all, and whose loads are measurably
        // getting about a hundredth of the CPU they get with the renderer idle.
        // So the tier is raised HERE, for this target, through 981336e's own
        // knob rather than by changing what the runtime defaults to.
        //
        // Read-out for launch #23, in the ledger that is already on:
        // in-level "<< END >> Async Loading" durations should fall from
        // seconds to the boot band (tens to a few hundred ms), the STALL
        // watchdog should go quiet, and `q` should stop climbing. The
        // "P3D Loading Starved" lines are the direct measure -- they carried
        // 50-95% of every in-level load's wall time on #22.
        //
        // The A/B is one word: KL_GUEST_QOS=initiated restores exactly this
        // build's previous behaviour, KL_GUEST_QOS=off restores pre-981336e.
        // If the frame pacing regresses instead, that is the trade 981336e
        // warned about and this line is the thing to take back out.
        setenv("KL_GUEST_QOS", "interactive", 0);

        // Read-only, and it exists so launch #23 can be read either way: one
        // line per guest thread with its entry pointer, its stack and the QoS
        // it actually got. The banner above it states the default; this states
        // that the loader in particular received it.
        setenv("KL_TRACE_THREADS", "1", 0);

        // ---- ROUND 16 RESULT: the cadence gate is NOT the answer ----
        //
        // KL_GUEST_FPS_DIV=2 held a perfect grid on launch #16 (holds 2x:133,
        // 0 overran, even 60 fps, wait 14.9 ms, fresh pose every release) and
        // The tester reports it WORSE: fuzz on head motion everywhere, menus and
        // splash included, and the intro video could not be skipped (the gate
        // starves a guest that completes no XR frames during movies — valve
        // runs of 24). An even cadence with MORE repeats being worse says the
        // defect is in how a REPEATED frame is presented, not in raggedness.
        // The gate is out again; see simpsons/STATUS.md round 16.

        // ---- ROUND 17: hand the warp to the SYSTEM (KL_WARP=system) ----
        //
        // The compositor's own pass is a rotation-only correction over a flat
        // proxy: a held slot gets NO translation correction, and the
        // correction magnitude alternates fresh/held by construction (see the
        // long note at renderFrame). At ~72-95 delivered fps on a 120 Hz
        // panel a third to a half of slots are held, so the picture
        // alternates between two positions on a sideways head move -- the tester's
        // "bigger, then smaller, as I move left and right", and round 16
        // proved the artefact scales with the SHARE of held slots (an even
        // 60 fps grid made it worse, launch #16). `system` stamps the
        // drawable with the pose the pixels actually embody and leaves our
        // model-view neutral, so visionOS does the one exact warp,
        // translation included, held slots included. Validated on Pistol
        // Whip; per-port arm on a round of its own, which this is. The
        // projection-layer path composes through the same originFromHead, so
        // the arm covers this guest. `KL_WARP=ours` is the backout.
        setenv("KL_WARP", "system", 0);

        // ---- ROUND 18: the Kwik-E-Mart interior, measured and half-fixed ----
        //
        // Indoors the world layer renders TWICE per view and hands the PASS
        // number to WorldScene::Render: pass 0 (the interior) is culled
        // against SuperCam 0's camera, which SharOpenXR::GetActiveCullingCamera
        // overwrites with the headset matrix, while pass 1 (the exterior
        // backdrop) is culled against SuperCam 1's camera, which the headset
        // never touches -- even though the layer has already multiplied that
        // pass's VIEW camera by the interior->exterior transform. Culling and
        // drawing therefore disagree by the whole head pose, and one slab of
        // Springfield is what survives it. kl_sdl.c interposes
        // WorldScene::Render, prints the five matrices per sampled frame, and
        // composes the headset matrix with layer[0x1dc] for pass 1 so the
        // backdrop culls where it draws. Baked, like every instrument here,
        // because the tester launches from the Home View and a devicectl-only knob
        // is not an instrument.
        //
        // KL_SHAR_INTERIOR=0 takes the whole thing back out;
        // KL_SHAR_INTERIOR_FIX=0 keeps the log and leaves the guest alone.
        setenv("KL_SHAR_INTERIOR", "1", 0);

        // ---- ROUND 26: the interior void is DRAWN AND LOST — split it ----
        //
        // Launch #25's framebuffer census settled what the Kwik-E-Mart void is
        // NOT. Standing inside the store, ~287,000 triangles over ~1,330 draws
        // a frame land in fb1 — the eye framebuffer, colour0 tex 1/3 layer
        // 0/1, 4128x4416, colourmask 1111, correct viewport — and the room
        // still reads as flat pale sky-blue. Culling is exonerated (the draws
        // are submitted), and round 19 already closed the two-pass theory
        // (0x1d8=0; this is an ordinary single-pass room). So the world is
        // drawn and lost, and there are exactly two places left to lose it:
        // the pixels never survive in fb1 (a late clear, a fullscreen
        // overpaint, a depth state that rejects everything), or fb1 is right
        // at end of frame and the loss is downstream of the guest entirely.
        //
        // One picture of fb1 taken INDOORS splits those. Both instruments
        // therefore arm on the doorway rather than on the clock or on the
        // first shadow pass: kl_sdl.c's InteriorManager::Enter interposer
        // calls kl_glfb_arm_capture(), and everything below counts from there.
        //
        //   DELAY 180  ~3 s at the rates this build runs, so the entrance
        //              sequence is over and the tester is standing in the store.
        //   EVERY 300  ~5 s apart, so four captures cover ~20 s of walking
        //              around rather than four copies of one instant.
        //   MAX 4      the readback stalls the guest's render thread and
        //              visionOS kills an immersive app that misses 2.0 s of
        //              drawables. Four full-size PNGs is the budget.
        //
        // And KL_GLFB_FRAMETRACE=3 traces three whole frames from the same
        // moment, in order — every clear, every draw-framebuffer bind, and
        // every run of same-state draws, coalesced. The draw-state census
        // samples; a sample cannot say what happened LAST, which is the whole
        // question when something paints over a finished world.
        //
        // Retrieve with `devicectl device copy from` against the data
        // container; the resolver below turns "glfbcap" into a container path
        // and empties it, so what is in there belongs to this launch.
        //
        // Backouts: KL_GLFB_OUT_ARM_INTERIOR=0, KL_GLFB_FRAMETRACE=0, or
        // KL_GLFB_OUT= (empty) to take the capture out entirely.
        // Round 26 ANSWERED that question, so round 27 does not pay for it
        // again: the capture and the trace are left UNSET here and default
        // off. Both instruments are still in the build — a devicectl launch
        // carrying KL_GLFB_OUT=glfbcap KL_GLFB_OUT_ARM_INTERIOR=1
        // KL_GLFB_OUT_DELAY=180 KL_GLFB_OUT_EVERY=300 KL_GLFB_OUT_MAX=4
        // KL_GLFB_FRAMETRACE=3 reproduces launch #26 exactly. They are off
        // because the readback stalls the render thread and round 27 spends
        // that budget on the DSG walk instead, and because one launch answers
        // one question.

        // ---- ROUND 27: the shell is not in the draw list — say which stage ----
        //
        // Launch #26's photographs of fb1 show the void as the wearer sees it:
        // the interior's floor, ceiling, magazine rack and signs all render,
        // the SHELL does not, and through the hole you see the exterior. The
        // ordered three-frame trace shows no wiper — clear-to-sky, ~250
        // same-state runs into fb1, nothing after. So the shell's drawable is
        // never SUBMITTED, and "drawn and lost" is dead.
        //
        // The walk that submits is `WorldScene::RenderScene` (0x5939c0), and
        // reading it end to end says there is NOTHING between a marked section
        // and `IsSphereInCone` — no activation bit, no LOD, no type filter. It
        // iterates two section lists on the scene and five drawable arrays per
        // section and asks the cone about every entry. So a drawable that
        // never reaches the cone (which is what `inf 0` on launches #24 and
        // #26 says about the shell) is not in those arrays at all.
        //
        // And `WorldScene::Place(StaticEntityDSG*)` (0x5921fc) has a silent way
        // to leave it out: at 0x5922c0, when the leaf node rSeekNode picked AND
        // the scene's fallback node are both at capacity, it returns having
        // placed nothing — no log, no error, no retry. Whether that fires
        // depends on how full the node was at the moment the shell was placed,
        // which is per-visit and order-dependent: exactly the shape of the
        // #23-vs-#24/#26 discrepancy.
        //
        // Two read-only probes, both in kl_sdl.c, neither touching a draw:
        //
        //   DSGDUMP  three armed walks of the very arrays RenderScene walks,
        //            from InteriorManager::Enter (or the indoor transition):
        //            +2 s, then every 5 s. Each prints every drawable within
        //            DSGR units of the launch-#23 wall point, every drawable
        //            with a non-finite radius wherever it sits, and per-array
        //            totals. The shell's inf-radius line is either in there or
        //            it is not, and that is the whole question.
        //   ACTLOG   Place(StaticEntityDSG*) and Remove(IEntityDSG*)
        //            interposed. Place is checked AFTER it runs by the only
        //            test that cannot lie — is the entity actually inside
        //            node->array[0..n)? — and a miss or a null node prints with
        //            the node's count and capacity. Counters ride the 5 s
        //            census line as `dsgact ... DROPPED n`.
        //
        // Backouts: KL_SHAR_DSGDUMP=0, KL_SHAR_ACTLOG=0. Dials:
        // KL_SHAR_DSGDELAY, KL_SHAR_DSGEVERY, KL_SHAR_DSGR, KL_SHAR_DSGMAX,
        // KL_SHAR_ACTMAX.
        setenv("KL_SHAR_DSGDUMP", "3", 0);
        setenv("KL_SHAR_DSGDELAY", "2", 0);
        setenv("KL_SHAR_DSGEVERY", "5", 0);
        setenv("KL_SHAR_DSGR", "25", 0);
        setenv("KL_SHAR_DSGMAX", "400", 0);
        setenv("KL_SHAR_ACTLOG", "1", 0);
        setenv("KL_SHAR_ACTMAX", "200", 0);
    }

    if (kl_app_target_is_steamlink()) {
        // Which front door this launch opens. The SHELL is the default because
        // it is the only one that can produce a session: the VR half reads its
        // own out of the launching Intent and leaves before its first frame
        // without one. A run handed a session by hand — KL_SLINK_SARGS from a
        // host pairing — wants the VR door directly, and says so by carrying
        // one.
        g_door = KL_SLINK_SHELL;
        if (kl_env_on("KL_SLINK_VR", 0) || kl_env_str("KL_SLINK_SARGS", NULL))
            g_door = KL_SLINK_VR;
        if (kl_env_on("KL_SLINK_SHELL", 0)) g_door = KL_SLINK_SHELL;

        // Everything this guest is told about itself — the activity class, the
        // four paths, the <meta-data>, the panel size — is kl_slink's, and
        // shared verbatim with `build/m_boot`. Two drivers describing one guest
        // differently is a class of bug with no error surface at all: the run
        // works and answers the guest's questions wrongly.
        if (kl_slink_configure(g_door, g_libdir, g_assets, g_apk, g_files,
                               NULL) != 0)
            return missing("Steam Link front door", kl_slink_error());

        // ...and then Qt's plugin path, overriding what kl_slink_configure
        // derived, because on this platform the two are not the same directory.
        //
        // **Qt reads a plugin as a FILE before it will load it.** libQt6Core's
        // search is a glob (`libplugins_%1_*.so`), so it lists the path and
        // then parses each candidate's ELF metadata for the IID and the Qt
        // version. Everywhere else in this project a guest library is a NAME
        // the loader resolves — on device the ELF tree is deliberately not in
        // the bundle at all — and that is not enough here: with no real files
        // nothing is ever a candidate, and libshell aborts with `Could not find
        // the Qt platform plugin "virtual"`.
        //
        // So the six plugin .so files are staged into the container as data
        // (visionos/targets.py, `qtplugins`), and the dlopen Qt then does is
        // unaffected: kl_load_auto matches a translation on the BASENAME
        // whatever the path says, so it still gets the signed framework out of
        // Frameworks/. A missing directory is left alone rather than pointed at
        // — an empty QT_PLUGIN_PATH and a wrong one fail the same way, and the
        // staging error belongs in stage_assets.sh's report.
        char qtdir[1200];
        snprintf(qtdir, sizeof qtdir, "%s/%s/qtplugins", container, g_target->tree);
        if (have(qtdir)) setenv("QT_PLUGIN_PATH", qtdir, 1);
        else if (g_door == KL_SLINK_SHELL)
            fprintf(stderr, "  [klepton] %s is not staged — Qt will not find its "
                            "platform plugin (run visionos/stage_assets.sh)\n", qtdir);
    } else {
        kl_set_library_path(g_libdir);
        // Explicitly, because the default is a *relative* path that gets
        // absolutised against the working directory — which is the repo root
        // under t_boot and `/` inside an app bundle. Left unset on device it
        // became "//beatsaber/lib/arm64-v8a", and that is the string Unity reads
        // back as ApplicationInfo.nativeLibraryDir and hands to
        // ClassLoader.findLibrary. It survived only because kl_can_load matches
        // a translation on the basename; anything that actually used the
        // directory would have been quietly wrong.
        kl_jni_set_native_lib_dir(g_libdir);
        kl_jni_set_assets_dir(g_assets);
        kl_jni_set_apk_path(g_apk);
        kl_jni_set_files_dir(g_files);
        // The OBB layout is per-target and kl_jni must be TOLD it: on device
        // KL_TARGET is unset, so kl_jni's own fallback resolves the default
        // guest and hands a UE4 title the wrong OBB directory. g_target is the
        // real one this app was built for.
        kl_jni_set_obb_rel(g_target->obb);
        // ...and HOME, for a guest that is a UNIX PORT wearing an Android
        // manifest rather than an Android app. OpenJK derives fs_homepath from
        // $HOME and then walks it with mkdir -p (FS_CreatePath), which is fatal
        // on the first component it cannot make. The OS sets HOME to the app's
        // data container ROOT, and that directory is not ours to extend: the
        // system provisions Documents/, Library/ and tmp/ inside it and refuses
        // anything else. So the engine walked seven directories that already
        // existed and died on mkdir("<container>/.local") with EPERM.
        //
        // The guest's external storage is the writable tree it keeps everything
        // else in, so point HOME there and fs_homepath becomes
        // <ext>/.local/share/openjk, beside <ext>/JKXR.
        //
        // Only for this target. HOME is the OS's own answer to a sandboxed
        // process, Foundation resolves the container through it, and the other
        // guests here reach it through Qt — none of which needs redirecting.
        // Safe at this point for the same reason: the caller resolves the
        // container once, before kl_app_configure runs.
        if (target_is_jkxr()) setenv("HOME", g_files, 1);
        // Raw "<apk>/assets/..." opens: Unity mounts the APK into its VFS and
        // then resolves entries by concatenating onto the mount point.
        kl_guest_path_map(g_apk, g_assets);
    }

    // Prefer klepton-ld translations when the bundle carries them. On device
    // this is not a preference but the whole point — the mmap loader maps guest
    // text RWX from a file the bundle does not own, which is the shape AMFI
    // exists to refuse. A translation that is present and fails is an error,
    // not a fallback (kl_load_auto).
    if (have_translations()) setenv("KL_DYLIB_DIR", g_dylibs, 1);

    // ANGLE rides in the same Frameworks/ directory, so the app can answer
    // "where is ANGLE" itself rather than have the launcher guess at a path
    // that only exists on the device. Not forced: an explicit KL_ANGLE_DIR
    // still wins, which is how a run points at a different build.
    setenv("KL_ANGLE_DIR", g_dylibs, 0);

    // ...and so does MoltenVK, for a guest whose graphics API is Vulkan
    // (BONELAB). Same rule, same reason: the app knows where its own Frameworks
    // are and the launcher does not.
    //
    // kl_vulkan.c would find it anyway — its last resort is
    // `@rpath/MoltenVK.framework/MoltenVK`, and @rpath here IS this directory.
    // Naming it explicitly costs nothing and buys the failure message: without
    // this, the two dlopens that precede the @rpath attempt are against the
    // HOST's vendored path, so a run where MoltenVK is genuinely missing from
    // the bundle reports `vendor-moltenvk/out/macos/...: no such file`, which
    // names a directory that has never existed on a headset.
    setenv("KL_MVK_DIR", g_dylibs, 0);

    // The capture knobs, resolved against the CONTAINER when they are relative.
    //
    // Every one of these names an output directory, and on device there is
    // exactly one writable place and no shell to expand a path against: the
    // process's cwd is `/`, so `KL_VK_OUT=vkcap` writes to `/vkcap` and fails,
    // silently, on a run that otherwise looks healthy. An absolute path still
    // wins outright, and the container's own UUID changes on reinstall — so
    // spelling one out at the launcher is not a workaround either, it is a value
    // that goes stale between runs.
    //
    // Relative-means-container is also what makes the file RETRIEVABLE: only the
    // app data container can be read back with `devicectl device copy from`.
    static const char *const cap_knobs[] = {
        "KL_VK_OUT", "KL_GLFB_OUT", "KL_DUMP_SHADERS", "KL_DUMP_TEXTURES",
        "KL_VTDEC_DUMP",
        // r31: the two GLSL dumps used to be BAKED to an absolute container
        // path (glsl-depth for every target, glsl-fill for tetris). Neither
        // ships any more, so they join the resolver instead — a devicectl
        // launch carrying KL_GLFB_DUMP_GLSL_DEPTH=glsl-depth gets the same
        // container directory, made for it, that the bake used to provide.
        "KL_GLFB_DUMP_GLSL_DEPTH", "KL_GLFB_DUMP_GLSL_FILL",
        // r36 (simpsons): the SHAR radar capture written out as a PNG, from
        // the buffer the guest's own glReadPixels already asked for. Same
        // deal — relative means the container, so it comes back with
        // `devicectl device copy from`.
        "KL_SHAR_RADARTRACE_DIR",
    };
    for (size_t i = 0; i < sizeof cap_knobs / sizeof *cap_knobs; i++) {
        const char *v = getenv(cap_knobs[i]);
        if (!v || !*v || *v == '/') continue;
        char abs[1200];
        snprintf(abs, sizeof abs, "%s/%s", container, v);
        setenv(cap_knobs[i], abs, 1);
        // ...and created, because none of the capture paths make their own
        // directory: on the host you `mkdir /tmp/vk` first, and on device there
        // is nowhere to type that. A capture whose directory does not exist
        // fails per FILE, quietly, and the run looks like one where the guest
        // never drew.
        mkdir(abs, 0755);
        // ...and EMPTIED. The container survives a reinstall, the file names
        // are frame numbers rather than launch-unique, and launch #7's capture
        // directory came back holding launch #6's files beside it -- which is a
        // capture that lies by a different route than reading the wrong
        // surface. Whatever is in here belongs to THIS run.
        DIR *d = opendir(abs);
        if (d) {
            struct dirent *e;
            int gone = 0;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char victim[1400];
                snprintf(victim, sizeof victim, "%s/%s", abs, e->d_name);
                if (unlink(victim) == 0) gone++;
            }
            closedir(d);
            if (gone) printf("[app] %s: removed %d file(s) from a previous run\n",
                             abs, gone);
        }
        printf("[app] %s is relative — resolved to %s\n", cap_knobs[i], abs);
    }

    // The guest's own sequences — boot, lifecycle, frame, reports — are
    // kl_driver's, shared verbatim with `build/m_boot`. What is left in this file
    // is the bundle: the log, the heartbeat, the guest thread, the compositor's
    // pacing and the handoff.
    kl_driver_init(g_target, g_libdir, g_door);
    kl_driver_set_phase_hook(app_phase);
    // 120 s, not the driver's 20: there is no shell here to notice a hang and no
    // way to attach, so the watchdog is the only thing that says WHERE a run
    // stopped — and a device is slower to reach its first frame than the host.
    kl_driver_set_alarm(kl_env_int("KL_ALARM", 120));

    // ---- the audio capture, UNBAKED in r31 ----
    //
    // Four rounds (r15-r18) tried to establish what Tetris Effect's audio
    // sounds like from counters alone. By r18 every counter was clean — the
    // owned output paced in real time, no underruns, 0.0% of it silent, block
    // seams spliced — and the report was still "terrible". A counter cannot
    // hear. So both ends of the path teed to a WAV in the data container, which
    // is the one directory `devicectl device copy from` can read back, and it
    // worked: r19 read defect 1 (a 240-frame burst against a 1024-frame Wwise
    // block) straight off the capture's FILL LENGTHS, and c238411 fixed it.
    //
    // It was BAKED rather than env-gated for the reason the eye cap and the
    // seam knobs are: the Home View carries no environment, and the tester launches
    // from the Home View, so an instrument that only exists on a devicectl
    // launch is not an instrument for the build he actually wears.
    //
    // Both defects it was built for are now closed — defect 1 by the fix,
    // defect 2 (the spectral tilt) by the tester, who says the audio sounds good —
    // so the shipping build stops paying for it: it is a per-launch write of a
    // few MB, on the audio thread's own path, growing for as long as the session
    // runs. To capture again on a devicectl launch, unchanged:
    //
    //   KL_AUDIO_DUMP=klepton-audio-out.wav KL_AAUDIO_DUMP=klepton-audio-guest.wav
    //   ./visionos/pull_audio.sh
    //
    // Both still resolve a bare filename against the container, below.
    //
    // These two name a FILE, not a directory, so they get the container prefix
    // without the mkdir the loop above does — an mkdir here would create a
    // directory exactly where the file needs to be written.
    static const char *const cap_files[] = { "KL_AUDIO_DUMP", "KL_AAUDIO_DUMP" };
    for (size_t i = 0; i < sizeof cap_files / sizeof *cap_files; i++) {
        const char *v = getenv(cap_files[i]);
        if (!v || !*v || *v == '/') continue;
        char abs[1200];
        snprintf(abs, sizeof abs, "%s/%s", container, v);
        setenv(cap_files[i], abs, 1);
        printf("[app] %s is relative — resolved to %s\n", cap_files[i], abs);
    }

    // ...and the app uses it by default. KL_GLFB stays opt-IN for the host,
    // where the null driver is a legitimate answer — `make check` and every
    // lifecycle loop run on it, and the reference renderer is a deliberate
    // extra. In a shipped app it is not an answer at all: the null driver
    // records GL calls and draws nothing, so a hand-launched Klepton would
    // show a black immersive space and look broken.
    //
    // overwrite=0, so an explicit KL_GLFB=0 from run.sh or the launcher still
    // selects the null driver — which is why kl_glfb_enabled() had to start
    // reading the value rather than the presence.
    setenv("KL_GLFB", "1", 0);

    // The eye cap is baked because a Home View launch has no environment, and
    // without it the app silently renders at the 3072 runtime default there.
    // overwrite=0: an explicit KL_OVRP_EYE_MAX still wins, which is how every
    // rung of this ladder was probed.
    //
    // 4032 is the measured ceiling for Beat Saber (2026-08-10, the beatsaber target,
    // in-headset A/B): 4032 is stable through the menu
    // transition and play; 4096 is killed abruptly right after it.
    //
    // It is one number for every target again — but it spent phases 13 and 14 as
    // two, and the reason is the whole story below. Read it in order: SUPERHOT
    // forced it down to 3072, and phase 15 removed the thing that forced it.
    //
    // **It was not a global number, and SUPERHOT was why.** What we advertise is
    // not what the guest allocates: Unity multiplies the recommended eye size
    // by XRSettings.eyeTextureResolutionScale, and SUPERHOT sets that to 2 when
    // the menu opens — doubling BOTH axes, so four times the pixels, in one
    // reconfigure. Measured on device (2026-08-12, phase13-klepton-boot.1.log):
    // the left menu button at T+6 s destroyed the six eye textures and set them
    // up again at 8064x6468, exactly 2x the 4032x3234 we had advertised. The
    // swapchain went 597 -> 2409 MiB, Unity's own render targets and MSAA
    // scaled with it, and the footprint went 3936 MiB -> 8196 MiB in under a
    // second. jetsam killed it: reason `highwater`, 5.6 s of CPU
    // (JetsamEvent-2026-08-12-143635.ips). That is the "boot-time balloon" the
    // three earlier highwater kills recorded and nothing had ever caught in the
    // act — it was never boot, it was the menu.
    //
    // So the cap has to be chosen for the PEAK, not the steady state: whatever
    // we advertise, this guest may ask for four times its pixels, and
    // everything downstream scales with that square.
    //
    // **3072 was that number, and it was walked to, not derived.** The phase 13
    // arithmetic here predicted a ~6150 MiB peak at 3072 with ~2 GiB to spare;
    // it was wrong by more than a gigabyte, because it fitted a scaling
    // constant to a single kill that had already been clamped by the limit it
    // died at. Phase 14 (2026-08-12) walked the ladder in the headset instead,
    // The tester pressing the menu within seconds of each launch — the worst case,
    // since the level load has not settled yet — and measured the footprint at
    // the reconfigure:
    //
    //   advert  menu stage (x3)   swapchain   sampled peak / headroom  verdict
    //   4032    8064x6468 803M      2409M     8196 (jetsam highwater)  KILLED
    //   3584    7168x5750 634M      1902M     6780 sampled, then gone  KILLED
    //   3456    6912x5544 591M      1773M     died inside the reconfig KILLED
    //   3200    6400x5134 501M      1504M     7633 / 558 MiB left      KILLED
    //   3072    6144x4928 466M      1398M     7315 / 876 MiB left      SURVIVED
    //
    // 3072 held the menu open with the footprint settling back to 7011 MiB
    // (1180 MiB free) and stayed there. 3200 — four percent more across, eight
    // percent more pixels — went past 8191 MiB in the same second. So the
    // ceiling was inside a 128 px band, and 3072 was the highest rung that lived
    // *while the menu still doubled*. That qualifier is the one phase 14 could
    // not see, because every rung it tested had the doubling in it.
    //
    // What the ladder also says, and matters more than the number: the peak at
    // 3072 is 7315 MiB, not 6150. The real margin is 876 MiB at the worst
    // instant we sampled, not two gigabytes. A level that loads heavier than
    // the first one can still spend that. The cap is a tourniquet, not a fix —
    // the fix is to stop honouring the doubling (clamp the menu-sized stages),
    // and only ~25% of the spike is the eye swapchain we allocate: at 3200 the
    // menu grew the footprint by 4473 MiB of which 1123 MiB was swapchain, and
    // the rest is Unity's own MSAA and render targets scaling by the same
    // square.
    //
    // **Phase 15 removed the doubling, so the tourniquet comes off and SUPERHOT
    // goes back to 4032 with everything else.** Read the ladder above again with
    // the menu press taken out of it: every rung above 3072 died *in the
    // reconfigure*, and the steady state that each one died from was never the
    // problem — 4032 without a menu press settled at 3936 MiB and stayed there
    // for the whole run. The ladder was measuring the doubling, not the
    // resolution. `superhot-menu-scale` (runtime/kl_gpatch.c, enabled in
    // targets.py) nops the single `fadd s0, s0, s0` that doubles
    // XRSettings.eyeTextureResolutionScale, so the opening arm now sets the
    // resolution the game already plays at, Unity sees the scale it already has,
    // and there is no reconfigure to survive. Host A/B at this headset's own
    // advert (3234x4032): twelve reconfigures to 8064x6468 became zero.
    //
    // So there is no longer a per-target number here, and the two halves of that
    // are worth keeping straight: the patch is what makes 4032 safe, and 4032 is
    // what makes the patch worth having. If the patch is ever turned off in
    // targets.py, this has to come back down to 3072 in the same commit —
    // 4032 unpatched is the configuration jetsam killed in phase 13.
    //
    // Beat Saber is where its own A/B put it, and arrives at the same number
    // independently: 4032 stable through its menu transition and play, 4096
    // killed right after it (2026-08-10, the beatsaber target).
    //
    // **Tetris Effect is the first target where 4032 is the wrong number, and
    // the reason is that it was never chosen for this title at all.** Both
    // ladders above were run on Unity guests whose peak was a menu-time
    // reconfigure. This guest has no such moment, and the r14 worn log
    // (2026-08-23, four minutes of menu and play) measures a footprint that is
    // flat: 2597 MiB steady with 5594 MiB still free, never once spiking.
    //
    // What 4032 costs here is the picture. The display asks for 6888x5525 per
    // eye; the cap is aspect-preserving, so it scales BOTH axes by 4032/6888 =
    // 0.585 and the guest is advertised 4032x3234 — **34% of the pixels the
    // display wants**. That is the "grainy", and it is not the foveation: after
    // the rate map the guest's physical footprint is 2640x1624 per eye against
    // a display that samples 2624x2560, so the horizontal is already at native
    // and the VERTICAL is running at 63% of it. Nothing recovers that but
    // rendering more pixels.
    //
    // 6144 is 1.52x across and 2.3x the pixels. Measured cost, from the r14
    // log's own allocation lines (202 MiB a stage for the 8064x3234 DoubleWide
    // texture, three stages = 606 MiB): the swapchain goes to roughly 1407 MiB,
    // +801 MiB, and the guest's depth scales with it. Call it +1.1 to +1.6 GiB
    // all in, landing near 4 GiB with about 4 GiB still free — which absorbs
    // the phase-14 lesson (that this arithmetic under-predicted a real kill by
    // more than a gigabyte) twice over.
    //
    // The next rung is native, i.e. no cap: 6888x5525, 2.9x the pixels of 4032,
    // an estimated 1770 MiB of swapchain. It is probably also inside budget and
    // it is deliberately NOT taken blind — take it once a worn log has said
    // what 6144 actually cost. Everything else stays at 4032: the two ladders
    // that produced that number were measuring Unity guests that quadruple
    // their eye textures on a menu press, and nothing here changes that.
    //
    // ---- r23: THE r21/r22 A/B IS OVER, AND BOTH HALVES ARE REVERTED ----
    //
    // r21 baked `KL_VRR=0` for this target and dropped the cap back to 4032, to
    // ask the foveation family one decisive question about the blue fill. It
    // has its answer and it is the negative one: The tester wore that build with NO
    // rate map built at all — no `[cp] guest foveation:` line anywhere in
    // build/logs/r23-worn-regression.log, which is the proof it engaged — and
    // reported the fill "is still the same". The mip-LOD reading of the blue
    // fill, and the whole foveation family with it, is DEAD. See STATUS.
    //
    // He reported a large REGRESSION in the same session — a smaller picture,
    // the room showing through around it, black shapes tracking the head — and
    // that is exactly what the A/B put at risk, because it moved three things
    // at once: the eye cap (6144 -> 4032), the composite (onto kl_reproject's
    // 1x1 sub-rect grid, which no worn build had ever taken), and, under both,
    // the OS build (xrOS 24M5355a -> 24M5359a). Nothing in the compositor half
    // of the worn log differs from r21's: same immersive open, same 105.0 x
    // 95.0 deg tangents, same layered drawable, same 6888x5525 screen size,
    // `[rp] warp eye0/eye1 assumes ... MATCH` on both eyes. So the picture we
    // present still covers the whole field of view, and nothing measured here
    // shrank it.
    //
    // This restores r20's configuration exactly — foveated, capped at 6144 —
    // and that IS the next experiment: if the next worn launch looks like r20
    // again, the regression was ours and lives on the unfoveated path or at
    // 4032; if it does not, the regression is 24M5359a and every port needs to
    // know. 6144 is confirmed on hardware (r21's worn log: 3780 MiB footprint,
    // 4411 MiB free, 8.33 ms cadence, 0 frames past deadline).
    int tetris_res = g_target && g_target->name && !strcmp(g_target->name, "tetris");
    if (tetris_res) setenv("KL_OVRP_EYE_MAX", "6144", 0);
    //
    // **Batman: Arkham Shadow measured its own rung and landed on the same
    // 4032** (round 25/27, 2026-08-26). This guest is the hard case for fill:
    // it is a Vulkan guest, we answer `ovrp_GetTiledMultiResSupported=false` by
    // design (the compositor-side rate map does the foveation), and no
    // `[cp] guest foveation:` line is ever printed — so every advertised texel
    // is rendered at full rate. The display asks for 6888x5525; 4032 is
    // aspect-preserving and advertises 4032x3234, 34% of those pixels.
    //
    // The A/B, one variable, worn, same save:
    //
    //   4032:  rat canal 50-60 fps, peak footprint 5039M, 0 panel warnings
    //   5120:  rat canal 36-41 fps, peak footprint 6269M, 4 panel warnings
    //
    // 5120 is 1.6x the pixels of 4032 and it cost a third of the frame rate;
    // 6144 (2.3x, tried in round 13) had already been rejected on memory —
    // `[hb] foot=7067M avail=1124M` at a MENU, before a level was resident.
    // Both are rejected for fill, not for sharpness: The tester confirmed 6144
    // "looks sharp". 4032 is this port's shipping rung, which is also the
    // default every other target takes, so there is no per-target guard here.

    // THE COMPOSITOR WATCHDOG CLASS, kept here because it is generic and no
    // port can read it off its own device afterwards.
    //
    // PTTR's launch #19 died thirteen seconds in with NO crash report, NO
    // JetsamEvent, NO fault report and no scene-phase line — the log simply
    // stopped. The visionOS system log named it:
    //
    //   13:53:58.532  AVHapticClient: Initial XPC call to server timed out
    //   13:53:59.472  wakeboardd  killing [pid] because it hasn't been sending
    //                             frames for 2.0s
    //   13:53:59.511  launchd     killing application because one of its
    //                             cp_layer_renderer_t didn't submit a frame for
    //                             over 2.0 seconds
    //
    // Nothing in the process faulted; it was killed from outside for going
    // quiet, which is why it leaves nothing behind. The stall was OURS: the
    // CompositorServices frame thread was inside CoreHaptics, whose calls are
    // synchronous XPC to a gamecontrollerd that had just gone away. The next
    // frame rebuilt the engine and paid the timeout again.
    //
    // ANY port whose present path can starve for two seconds dies exactly this
    // way. Two things guard against it here: KleptonControllers.swift keeps
    // every CoreHaptics call on its own serial queue with one update in flight
    // and a cooldown after a failed bring-up (the frame thread pulls the level
    // and nothing else), and KleptonCompositor.swift carries the instruments
    // whose absence cost the diagnosis — `[cp] FRAME LOOP STALLED` the first
    // time a single slot exceeds 100 ms, and `slot worst N ms` plus
    // `bridged N (worst run M)` on the present-order census.
    //
    // Reading it after the fact needs USB and root; the tester runs it, and the
    // UDID is the LOCKDOWN one, not the CoreDevice id:
    //
    //   sudo /usr/bin/log collect --device-udid <lockdown-udid> \
    //        --start '2026-08-26 13:53:00' --output x.logarchive
    //   log show x.logarchive --predicate 'processID == <pid>' --info --debug


    // THE CADENCE GATE, KL_GUEST_FPS_DIV — off by default on every target.
    //
    // PTTR's launch #20 ran with `KL_WARP=system` genuinely armed and the
    // head-turn jitter was unchanged, which is evidence about what the artefact
    // IS rather than a failure of the arm. Same build, same eye, same arm:
    //
    //   menu      118-120 guest fps, every frame held ONE slot   -> smooth
    //   gameplay   45-60  guest fps, frames held TWO or THREE
    //              slots in a ragged mix                          -> jitters
    //
    // and the pose path's instruments were clean throughout (`pose filing:
    // by-index 100%`, `filed-at-lag 0x 100%`, `mispairs 0`, `sign flips 0%`).
    // What changed was the frozen series' STEP SIZE, because the guest samples
    // a 120 Hz pose stream on a 2/3-slot beat.
    //
    // Neither warp arm can fix that. Both correct rotation exactly; what
    // survives both is the residue that scales with a frame's AGE, because the
    // eye quad is a flat proxy at KL_REPROJECT_DEPTH and reprojection has no
    // parallax to give. A constant age reads as latency. An age that alternates
    // 2, 3, 2, 3 slots reads as an image alternating between two positions.
    //
    // So the gate paces the guest onto a grid so the age is CONSTANT: `auto`
    // starts at one slot per frame (the behaviour that has always shipped, and
    // a 120 fps guest keeps it) and steps down to 2, 3 or at most 4 slots when
    // the guest overruns, so the bar settles on an even 40 fps rather than a
    // ragged 55. That is a real trade — an even cadence bought with frame rate
    // — so it is NOT a default: it is unset here, and a port that measures the
    // alternating-age shape turns it on for itself with `KL_GUEST_FPS_DIV=auto`
    // (see KleptonCompositor.swift for the pinned `=N` form).
    //
    // PTTR's launch #21 then RAN that gate and killed the hypothesis behind it:
    // `pace grid 1 slots`, the hold histogram collapsed to one bucket in
    // gameplay (`holds 1x:284 2x:0 3x:0` at 57 fps) where launch #20 had a
    // ragged mix of 2s and 3s — and the jitter was reported unchanged. A
    // constant frame age with the jitter still there is what sent that port
    // looking at the pose BOUNDARY instead (see kl_ovrp.c, KL_POSE_BOUND).


    // Where MoltenVK's compiled pipelines are kept between launches.
    //
    // MoltenVK compiles MSL synchronously inside vkCreateGraphicsPipelines, and
    // Unity builds a pipeline the first time a material is actually drawn — so
    // the first time a pointer lands on a menu item, its hover material
    // compiles on the frame thread. The tester reports exactly that, consistently,
    // on first hover. Unity creates a VkPipelineCache and never persists it,
    // because on Android the driver keeps its own; there is no such thing here,
    // so without this every launch recompiles everything it compiled last time.
    //
    // The container, beside klepton-boot.log: it is the one writable place that
    // survives a launch and is small enough to matter (a few MiB). The blob's
    // header carries a device/driver UUID the implementation is required to
    // check, so a MoltenVK update simply starts from empty rather than loading
    // something wrong.
    {
        char pc[1024];
        snprintf(pc, sizeof pc, "%s/klepton-pipeline-cache.bin", container);
        setenv("KL_VK_PIPELINE_CACHE", pc, 0);
    }

    // hl2vr (the SDL guest) is the exception, and for the opposite reason the
    // Unity guests are capped: it does NOT scale the size we hand it, and with
    // the HD pack uploading as DXT its worn peak is 2,255 MB of 8,191 (launch
    // #11, 2026-08-26, mat_picmip 0) — 5.9 GB of headroom against a swapchain
    // that grows by ~340 MiB going from 4032x3235 to the panel's native
    // 5087x4081. 5120 is "no cap" for this panel. An explicit KL_OVRP_EYE_MAX
    // still wins (overwrite=0), which is the A/B back to 4032.
    if (g_target && g_target->kind == KL_GUEST_SDL)
        setenv("KL_OVRP_EYE_MAX", "5120", 0);
    // **Pinball FX is the one exception, and it is arithmetic rather than a
    // preference.** the tester reported the lobby as grainy, and round 11 measured
    // why. This guest submits the two eyes as a CANTED-CONE UNION: each eye's
    // horizontal tan span is 1.732 + 1.000 = 2.732 against a union of 3.464, so
    // one eye occupies 2.732/3.464 = 78.87% of the union texture's width — the
    // device log prints "78.8%", and the right eye's origin lands at 852/4036 =
    // 0.2113 where the geometry says 0.2113. At a 4032 cap the union came out
    // 4036 wide and each eye rendered 3179 across. The panel's own physical
    // width is ~3660 per eye, so the eyes were being handed 87% of the pixels
    // the display can actually show, and the missing 13% is the graininess.
    //
    // 3660 / 0.7887 = 4640, and that is the number: a union wide enough that
    // each eye lands ON the panel's own resolution rather than under it. It is
    // NOT a step up the ladder past 4032 — the union is shared by two eyes, so
    // 4640 union is *less* per eye than 4032 would be for a guest that submits
    // one eye per texture. Beat Saber's 4096 ceiling is not the same measurement
    // and is not being crossed.
    //
    // Cost: the swapchain goes 4036x2552 -> 4640x2934 (aspect preserved), which
    // at RGBA8 x 2 array slices x 3 stages is 247 MB -> 327 MB, about +80 MB.
    // The stall run peaked at 4861 MB of 8191 with one system memory-pressure
    // warning late in the hang, so the headroom is real but not enormous;
    // ~4940 MB is the expected new peak. By NAME, not by kind: deadpool and re4
    // are `ue4` too and neither submits a canted union, so the arithmetic above
    // does not describe them and the number would be wrong for them.
    //
    // Set BEFORE the shared line, with overwrite=0, so the 4032 below does not
    // clobber it and an explicit KL_OVRP_EYE_MAX still beats both.
    // (pinballfx: KL_OVRP_EYE_MAX=4640 is baked in its target row since the
    // 2026-10-09 driver crossing — row env is applied before this line.)
    setenv("KL_OVRP_EYE_MAX", "4032", 0);
    if (tetris_res)
        printf("[app] tetris: eye cap 6144, guest foveation at its default — "
               "the r21/r22 A/B is reverted and this is r20's configuration\n");

    // A UE4 guest survives backgrounding by default, for the same Home-View
    // reason as the eye cap: no environment, and a first load measured in
    // MINUTES of black eye layer (the loading indicator is a quad layer this
    // compositor declines). Exit-on-background turned every headset lift or
    // crown press during that load into a silent restart from zero — three
    // launches in a row on 2026-08-19 died this way at ~90 s each, none of
    // them past the load (klepton-boot.log: draws=0 throughout, then
    // "backgrounded — exiting"). The 2026-08-17 rounds that finished the port
    // all ran with KL_EXIT_ON_BACKGROUND=0 by hand, so the stay-alive path is
    // the proven one for this guest. Unity guests keep the exit default —
    // Lifecycle's comment on unresumable state is about them, and none of
    // them loads long enough to be bitten. overwrite=0: an explicit
    // KL_EXIT_ON_BACKGROUND=1 from run.sh still selects the exit.
    //
    // r32: this read `target_is_ue4()`, a function that exists nowhere in this
    // tree — it arrived with the deadpool commit `665a005` and its definition
    // did not. Nothing caught it because `make check` and `m_boot` never
    // compile this file: it is the visionOS app's, and the last device build
    // anyone made was r30's, BEFORE the r31 promotion replayed that commit
    // here. So the shipping build has not compiled on `driver` or `tetris`
    // since the promotion, for every target, and this round is the first thing
    // to ask it to. The question it meant to ask is answered by the target
    // table, which is where every other kind test in this file asks it.
    if (g_target && g_target->kind == KL_GUEST_UE4)
        setenv("KL_EXIT_ON_BACKGROUND", "0", 0);

    // KL_TRACE_FS and KL_XR_FOVEA_DUMP were BAKED ON for this guest through
    // rounds 5-8, and round 9 takes both back out. They were baked because a
    // Home-View launch has no environment, so a knob only the tethered wrapper
    // can set is off in exactly the runs the tester reports from; both answered
    // their question (the trace found the lowercased mod directory of launch
    // #4, the dump proved the world rendered MONO in launch #7), and an
    // instrument that has answered is an instrument that is now only a cost.
    //
    // **The eye dump was the round-9 stutter, measured.** the tester: "every 8-10
    // seconds it repeats the last half-second of video AND audio for about two
    // seconds." The dump is a synchronous 2064x2208 readback plus a PNG encode,
    // TWICE, on the frame path, at KL_XR_FOVEA_DUMP_EVERY=10. Launch #9's log
    // times it and the compositor counts it:
    //
    //   dump #011..#027   0.40 - 0.65 s of wall clock each, every 10.0 s
    //   [cp] present order, window CONTAINING a dump   repeats 49..83 of 300
    //   [cp] present order, window between two dumps   repeats  0..18 of 300
    //
    // A "repeat" is the compositor presenting the picture it already had
    // because the guest produced no new one — so 50-70 of them is exactly half
    // a second of frozen video, and the guest thread blocked in the readback is
    // the same thread that feeds the mixer, which is the audio half of the same
    // report. Every window with a dump in it has them and every window without
    // one does not; there is no other candidate in the log (0 `multiview: begin
    // failed`, no jetsam warning, peak 3,275 MB of 8,191).
    //
    // KL_TRACE_FS=fail cost less but is not free either: 14,206 lines of
    // launch #9's log, each a formatted write from the guest's own thread.
    //
    // Both stay reachable from a tethered round, where somebody IS watching, by
    // exporting KL_TRACE_FS / KL_XR_FOVEA_DUMP / KL_XR_FOVEA_DUMP_EVERY into
    // the environment the wrapper passes through.

    // Pinball FX's Vulkan pair, baked for that target alone. Both are argued at
    // length where they are implemented (runtime/gfx/kl_vulkan.c); the short of
    // it is that they are ONE fix in two halves and neither is honest alone:
    //
    //  - KL_VK_MAX_SAMPLES=1 clamps the advertised framebuffer sample counts, so
    //    UE builds single-sampled attachments. Round 7 measured that this works:
    //    the device log carries the clamp line and carries NO "the guest created
    //    a MULTISAMPLED image" line at all.
    //
    //  - KL_VK_SUBPASS_MS_DEMOTE=1 clears the MS bit on `Dim == SubpassData`
    //    images in the guest's SPIR-V. Round 7 also measured why this is needed:
    //    with the attachments already single-sampled, eleven pipelines STILL
    //    failed on `GENERATED_SubpassDepthFetchAttachment.read(uint2(...),
    //    gl_ViewIndex)` against a `texture2d_ms_array`. That MS-ness is COOKED
    //    into the pak's SPIR-V — UE's offline Vulkan compiler wrote it when the
    //    title was built for a Quest with mobile MSAA — so no runtime CVar and
    //    no device limit can reach it. Demoting it makes the shader agree with
    //    the framebuffer the clamp already gave it, and SPIRV-Cross's
    //    two-argument read becomes the right call for the type it emits.
    //
    // **KL_OVRP_ARRAY_EYE_VIEWPORT is deliberately NOT set, and round 7 is why.**
    // It was baked on to unify the eyes' rects, and the tester's launch falsified it
    // in the plainest way available: the intro splashes, which had been SINGLE
    // images on every prior build, came back DOUBLED. The submitted pair is not
    // nonsense after all — across the whole DRS ramp eye1's origin holds a fixed
    // 0.268 of eye0's width (1292/4823, 1302/4863, 1399/5224) and eye1's right
    // edge lands on the texture's own (1399+5226 = 6625 of 6628). That is a
    // consistent per-eye placement, the compositor's per-eye grid already
    // expresses it, and overwriting it with eye0's rect is what broke the
    // splashes. The knob stays in the tree as the A/B; nothing selects it.
    //
    // overwrite=0 throughout, so run.sh can still take the other side of any of
    // these without editing anything.
    // And Pinball FX's eye-layer pair, round 9, baked for that target alone for
    // the same reason: they are argued at length in runtime/xr/kl_ovrp.c
    // (klovrp_union_widen, klovrp_eye_desc_size,
    // klovrp_GetLayerRecommendedResolution) and measured on the HOST loop, with
    // KL_OVRP_EYE_FORCE=4032x3234 KL_OVRP_EYE_TAN=vision reproducing the
    // device's canted-display regime exactly — including the 0.268 per-eye
    // offset the device log carries.
    //
    //  - KL_OVRP_EYE_CAP_ALLOC=1 makes KL_OVRP_EYE_MAX above bound the eye
    //    texture the guest ALLOCATES rather than the size we recommend. UE5
    //    multiplies our recommendation by its own 1.300 pixel density, and an
    //    OculusXR guest on a canted display multiplies again by the union
    //    widening (~1.26), so the 4032 cap bought a 6628x4208 layer: ~669 MB of
    //    live swapchain, on a title already measured at 4704 MB footprint.
    //    Host A/B, same run, knob the only difference: eye layers
    //    3840x2464 then 4996x3204 became 3072x1944 then 3076x1944 — 62.6%
    //    fewer pixels, and the second setup stops being an inflation (+0.13%
    //    where it was +30%). The FOV is untouched, which is the whole design:
    //    cropping the tangents to match a capped width would tell a canted
    //    display's guest to render a narrower cone than the display has, and
    //    that is round 7's falsified mistake in new clothes.
    //
    //  - KL_OVRP_LAYER_REC_DISPLAY=1 makes ovrp_GetLayerRecommendedResolution
    //    answer from the display and the cap rather than from the layer's own
    //    current size. Round 9 measured that UE5.3 does NOT currently feed that
    //    answer back into the next desc, so this changes no number on its own —
    //    it removes the only place on the eye path where our answer was a
    //    function of our previous answer.
    //
    // overwrite=0 throughout, so run.sh can still take the other side of any of
    // these without editing anything.
    // pinballfx: every bake that lived here (KL_VK_MAX_SAMPLES,
    // KL_VK_SUBPASS_MS_DEMOTE, KL_OVRP_EYE_CAP_ALLOC, KL_OVRP_LAYER_REC_DISPLAY)
    // moved to its target row's `env` at the 2026-10-09 driver crossing;
    // KL_OVRP_QUAD_SIZE_OFF and KL_OVRP_DROP_NO_EYE were dropped (driver probes
    // the quad offset and drops no-eye frames by default).


    // The eye-stage pixel probe is NOT baked on, and the reason is worth the
    // paragraph, because baking it on is the obvious thing to want here.
    //
    // It was baked to 120 for the black-picture round, on the reasoning that a
    // Home-View launch has no environment, so a knob only run.sh can set is off
    // in exactly the runs the tester reports from. That reasoning still holds. What
    // it missed is that 120 is a PERIOD, not a delay: the sweep counter starts
    // at zero, `seen++ % 120 == 0` on the first call, and the probe therefore
    // fires on the very first ovrp_EndFrame the guest ever issues. On device
    // that first sweep read a compositor-owned private MTLTexture and faulted
    // inside Metal (see klfb_patch_stats in kl_glfb.c), so the app died before
    // one frame reached the display — the worn round produced no picture at
    // all, and the diagnostic was the only reason.
    //
    // The read is safe now. The bake is not coming back on: an instrument that
    // has once ended the run it was measuring does not get to be the default in
    // the runs nobody is watching. `run.sh device` passes the environment
    // through, so a tethered round can still ask for it:
    //
    //     KLEPTON_TARGET=superhot KL_GLFB_STAGE_PROBE=120 ./run.sh device
    //
    // and that is the cheap round to spend it on, not the one the tester is
    // standing up in.

    snprintf(g_status, sizeof g_status, "configured");
    return 0;
}

// Everything the run prints goes to a file, line-buffered.
//
// Line-buffered specifically: an unimplemented JNI slot aborts by design, and
// a fully-buffered stream loses the whole report when the process dies on a
// signal — which on the host reads as a much earlier failure than actually
// happened, and on device would be the only evidence there is.
//
// KL_LOG_FILE=0 leaves both streams alone, which is the ONLY way anything
// outside this process can read them live: `devicectl device process launch
// --console` bridges the app's stdout and stderr, and a freopen'd stdout is
// bridged to a file nobody is watching. Polling the file instead costs a
// `devicectl device copy from` per sample, and that call is ~27 seconds on this
// hardware whatever the file's SIZE — 150 KB and 15 MB take the same time — so a
// run watched by copying is minutes of transfer per minute of run. Nothing here
// is smaller in that mode; the same bytes go somewhere the host can already see.
//
// The file stays the default because it is what survives the process: an
// unimplemented JNI call aborts by design, a launch from the Home View has no
// console at all, and the in-window boot report is read back out of this path.
//
// ...and the previous two runs are kept, because opening "w" on a fixed path
// means the evidence for a crash survives exactly until the next launch — and
// the next launch is the FIRST thing anyone does after a crash. Every jetsam
// kill we have had fell in that blind spot: the app dies, the tester taps the icon
// to see what happened, and the tap destroys the answer. A SIGKILL leaves
// nothing but this file, so this file has to outlive one reflex.
//
// Two generations is enough. The one that died is .1 after the relaunch that
// went looking for it; .2 covers the case where the relaunch ALSO died before
// anyone got the device off their head.
static void rotate_log(void) {
    char base[1024], one[1064], two[1064];
    snprintf(base, sizeof base, "%s", g_log);
    size_t n = strlen(base);
    if (n > 4 && strcmp(base + n - 4, ".log") == 0) base[n - 4] = 0;
    snprintf(one, sizeof one, "%s.1.log", base);
    snprintf(two, sizeof two, "%s.2.log", base);
    // Oldest first, so a failure part-way through loses the oldest generation
    // rather than doubling one up.
    unlink(two);
    rename(one, two);
    rename(g_log, one);
}

static int open_log(void) {
    if (!kl_env_on("KL_LOG_FILE", 1)) {
        // Same buffering discipline either way. The reason is unchanged — a
        // fully-buffered stream loses the report when the process dies on a
        // signal — and it applies harder here, because the far end of the bridge
        // is another process that only ever sees what was actually written.
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IOLBF, 0);
        return 0;
    }
    // Once per process. Two callers reach open_log (the boot path and the
    // template floor test) and only one runs per launch, but rotating twice
    // would quietly spend a generation on nothing.
    static int rotated;
    if (!rotated) { rotated = 1; rotate_log(); }
    if (!freopen(g_log, "w", stdout)) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), STDERR_FILENO);   // the guest's own writes(2) land here too
    setvbuf(stderr, NULL, _IOLBF, 0);
    return 0;
}

// A heartbeat on its own thread, because three different failures look identical
// in a log that simply stops growing — and on device there is no shell, no
// sampler and no way to attach:
//
//   * the process is SUSPENDED  -> the timestamps show a gap and then resume
//   * the process was KILLED    -> the log just ends (jetsam/watchdog send SIGKILL,
//                                  which no handler can report)
//   * the GUEST THREAD is stuck -> the heartbeat KEEPS TICKING while the guest's
//                                  output stops, which is the case nothing else
//                                  distinguishes
//
// Wall clock rather than monotonic on purpose: a suspension is exactly the thing
// a monotonic clock is designed to hide.
//
// The line also carries memory, because "killed" above has two causes that the
// log cannot otherwise tell apart — jetsam and the watchdog both send SIGKILL,
// and neither leaves anything behind. `foot` is our physical footprint, which is
// the number jetsam actually meters; `avail` is what the OS says is left before
// it acts. A run that ends with avail falling toward zero was killed for memory;
// one that ends with avail still comfortable was killed for time.
static const char *volatile g_phase = "start";
static void app_phase(const char *p) { g_phase = p; }

// Bytes of headroom before this app is jetsammed, or -1 where the OS will not
// say. os_proc_available_memory() is the app-process-only API and returns 0 in
// contexts it does not apply to, which is not distinguishable from "none left" —
// hence the explicit -1 rather than passing a bare 0 through to the log.
// TARGET_OS_IPHONE, not __has_include: the header exists on macOS too, where the
// function is marked unavailable — so an include test compiles and then fails at
// the call.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
static long long kl_avail_memory(void) {
#if TARGET_OS_IPHONE
    size_t a = os_proc_available_memory();
    return a ? (long long)a : -1;
#else
    return -1;
#endif
}

static long long kl_phys_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return -1;
    return (long long)info.phys_footprint;
}

// --- the jetsam tripwire ----------------------------------------------------
//
// Three of the kills in the 2026-08-12 session were nothing like the rest:
// ~8.2 GB against the device's 8191 MB ceiling with under ten seconds of CPU
// each. Gameplay sits FLAT at ~3.9 GB, so those three are a boot-time balloon —
// something that inflates and dies before anything gets a chance to describe
// it. Nothing has ever captured one. It cannot be captured after the fact
// either: jetsam sends SIGKILL, which no handler sees, and the .ips it leaves
// names the process and not the allocation.
//
// So take the evidence on the way UP. At the moment headroom drops under 2 GB
// this dumps, once, where the address space actually is — grouped by VM tag,
// because that is the entire question. Eight gigabytes of IOAccelerator is a
// surface or texture leak in the GL seam; eight gigabytes of malloc is ours or
// IL2CPP's; eight gigabytes of nothing-in-particular means the balloon is
// virtual and jetsam is metering something else. Those are three different
// investigations and today we cannot tell them apart at all.
//
// This is evidence, not a fix. It allocates nothing and takes single-digit
// milliseconds — which matters, because it runs on a process that is by
// definition about to be killed.
#include <mach/vm_statistics.h>
#include <malloc/malloc.h>

// mach_vm_region_recurse is unavailable on embedded SDKs (mach_vm.h #errors
// out); vm_region_recurse_64 is the same call with vm_address_t, which is
// 64-bit here anyway. Depth 255 on every iteration so submaps resolve to the
// leaf regions that actually carry the pages — at depth 0 a submap reports as
// one entry and its dirty pages vanish.
#define KL_TAG_SLOTS 32
typedef struct { unsigned tag, n; uint64_t vsize, resident, dirty; } kl_tagsum;

static const char *kl_tag_name(unsigned t) {
    switch (t) {
    case 0:                        return "anon/none";
    case VM_MEMORY_MALLOC:         return "malloc";
    case VM_MEMORY_MALLOC_SMALL:   return "malloc small";
    case VM_MEMORY_MALLOC_LARGE:   return "malloc large";
    case VM_MEMORY_MALLOC_HUGE:    return "malloc huge";
    case VM_MEMORY_MALLOC_TINY:    return "malloc tiny";
    case VM_MEMORY_MALLOC_NANO:    return "malloc nano";
    case VM_MEMORY_IOKIT:          return "IOKit";
    case VM_MEMORY_STACK:          return "stack";
    case VM_MEMORY_DYLIB:          return "dylib";
    case VM_MEMORY_COREGRAPHICS:   return "CoreGraphics";
    case VM_MEMORY_IMAGEIO:        return "ImageIO";
    case VM_MEMORY_LIBDISPATCH:    return "libdispatch";
    case VM_MEMORY_SKYWALK:        return "Skywalk";
    case VM_MEMORY_IOACCELERATOR:  return "IOAccelerator (GPU)";
    default:                       return NULL;   // `vmmap` is the decoder ring
    }
}

// The biggest INDIVIDUAL regions, kept alongside the per-tag totals.
//
// The totals named the balloon's tag and stopped there. Launch #10 died with
// "anon/none 3266 MiB dirty over 887 regions" at the top of the dump and no way
// to tell a guest heap that grew in eight-megabyte steps from four
// eight-hundred-megabyte reservations someone touched once — which are
// different bugs with different fixes. One region is one allocator's decision,
// so the top of this list is the shortest path from "three gigabytes are
// somewhere" to "three gigabytes are HERE".
#define KL_REGION_TOP 12
typedef struct { uint64_t addr, vsize, dirty, resident; unsigned tag; } kl_region;

static void kl_region_note(kl_region *top, unsigned *n, const kl_region *r) {
    // Ranked by dirty, falling back to resident so a shared-or-COW region that
    // reports a flat zero dirty can still make the list on its pages.
    uint64_t key = r->dirty ? r->dirty : r->resident;
    if (key < (1u << 20)) return;                 // under a megabyte says nothing
    unsigned i = *n;
    if (i < KL_REGION_TOP) { top[(*n)++] = *r; }
    else {
        unsigned worst = 0;
        for (unsigned k = 1; k < KL_REGION_TOP; k++) {
            uint64_t kk = top[k].dirty ? top[k].dirty : top[k].resident;
            uint64_t wk = top[worst].dirty ? top[worst].dirty : top[worst].resident;
            if (kk < wk) worst = k;
        }
        uint64_t wk = top[worst].dirty ? top[worst].dirty : top[worst].resident;
        if (key > wk) top[worst] = *r;
    }
}

static void kl_region_summary(FILE *f) {
    kl_tagsum sums[KL_TAG_SLOTS];
    kl_region top[KL_REGION_TOP];
    unsigned ntop = 0;
    memset(sums, 0, sizeof sums);
    memset(top, 0, sizeof top);
    unsigned nsums = 0, regions = 0, dropped = 0;
    uint64_t page = (uint64_t)getpagesize();
    vm_address_t addr = 0;
    for (;;) {
        vm_size_t size = 0;
        natural_t depth = 255;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &addr, &size, &depth,
                                 (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        if (info.is_submap) { addr += size; continue; }
        regions++;
        unsigned i = 0;
        while (i < nsums && sums[i].tag != info.user_tag) i++;
        if (i == nsums) {
            if (nsums == KL_TAG_SLOTS) { dropped++; addr += size; continue; }
            sums[nsums++].tag = info.user_tag;
        }
        sums[i].n++;
        sums[i].vsize    += (uint64_t)size;
        sums[i].resident += (uint64_t)info.pages_resident * page;
        sums[i].dirty    += ((uint64_t)info.pages_dirtied +
                             (uint64_t)info.pages_swapped_out) * page;
        kl_region_note(top, &ntop, &(kl_region){
            .addr = (uint64_t)addr, .vsize = (uint64_t)size,
            .resident = (uint64_t)info.pages_resident * page,
            .dirty = ((uint64_t)info.pages_dirtied +
                      (uint64_t)info.pages_swapped_out) * page,
            .tag = info.user_tag });
        addr += size;
    }
    // Ranked by RESIDENT, not dirty. Dirty is the number jetsam meters and it
    // is the one you would reach for, but the kernel only fills pages_dirtied
    // for privately-mapped regions — measured here, the shared and COW ones
    // report a flat zero, which on this process is most of the address space.
    // Resident is populated everywhere, so it is the column that can actually
    // name the balloon; dirty stays as a column because when it IS non-zero it
    // is decisive. Selection sort over at most 32 entries, so no scratch space.
    fprintf(f, "  [mem] %u regions in %u tags%s   (MiB; dirty is 0 for shared "
               "and COW mappings, which is the kernel's answer, not ours)\n",
            regions, nsums, dropped ? " — SOME TAGS DROPPED" : "");
    fprintf(f, "  [mem]   %-22s %9s %8s %9s %6s\n",
            "tag", "resident", "dirty", "vsize", "n");
    for (unsigned pass = 0; pass < nsums && pass < 14; pass++) {
        unsigned best = pass;
        for (unsigned i = pass + 1; i < nsums; i++)
            if (sums[i].resident > sums[best].resident) best = i;
        kl_tagsum t = sums[pass]; sums[pass] = sums[best]; sums[best] = t;
        // Once sorted, a tag holding neither a megabyte of pages nor a large
        // reservation has nothing to say about an eight-gigabyte balloon.
        if (sums[pass].resident < (1u << 20) && sums[pass].vsize < (64u << 20)) break;
        const char *nm = kl_tag_name(sums[pass].tag);
        char label[32];
        if (nm) snprintf(label, sizeof label, "%s", nm);
        else    snprintf(label, sizeof label, "tag %u", sums[pass].tag);
        fprintf(f, "  [mem]   %-22s %9llu %8llu %9llu %6u\n", label,
                (unsigned long long)(sums[pass].resident >> 20),
                (unsigned long long)(sums[pass].dirty    >> 20),
                (unsigned long long)(sums[pass].vsize    >> 20),
                sums[pass].n);
    }
    // The malloc side, which the tags above split across six buckets and which
    // has its own idea of how much of that is actually in use.
    malloc_statistics_t st;
    memset(&st, 0, sizeof st);
    malloc_zone_statistics(malloc_default_zone(), &st);
    fprintf(f, "  [mem]   default malloc zone: %llu MiB in use of %llu MiB "
               "allocated, %u blocks\n",
            (unsigned long long)((uint64_t)st.size_in_use >> 20),
            (unsigned long long)((uint64_t)st.size_allocated >> 20),
            st.blocks_in_use);

    // ...and the individual regions behind those totals. Selection sort over at
    // most twelve entries, so no scratch space, same as the tag table above.
    if (ntop) {
        fprintf(f, "  [mem]   largest regions (MiB):\n");
        fprintf(f, "  [mem]   %-18s %8s %9s %8s  %s\n",
                "address", "dirty", "resident", "vsize", "tag");
        for (unsigned pass = 0; pass < ntop; pass++) {
            unsigned best = pass;
            for (unsigned i = pass + 1; i < ntop; i++) {
                uint64_t ki = top[i].dirty ? top[i].dirty : top[i].resident;
                uint64_t kb = top[best].dirty ? top[best].dirty : top[best].resident;
                if (ki > kb) best = i;
            }
            kl_region t = top[pass]; top[pass] = top[best]; top[best] = t;
            const char *nm = kl_tag_name(top[pass].tag);
            char label[32];
            if (nm) snprintf(label, sizeof label, "%s", nm);
            else    snprintf(label, sizeof label, "tag %u", top[pass].tag);
            fprintf(f, "  [mem]   0x%016llx %8llu %9llu %8llu  %s\n",
                    (unsigned long long)top[pass].addr,
                    (unsigned long long)(top[pass].dirty    >> 20),
                    (unsigned long long)(top[pass].resident >> 20),
                    (unsigned long long)(top[pass].vsize    >> 20),
                    label);
        }
    }
}

// What the last heartbeat saw, so a crash report can say whether memory was
// going anywhere. Written by the heartbeat thread, read by a fatal signal —
// plain stores of aligned scalars, which is all this needs to be.
static volatile long long g_hb_foot = -1, g_hb_avail = -1;
static volatile int       g_hb_hour, g_hb_min, g_hb_sec, g_hb_seen;

// Registered with kl_fault: the fault report already dumps every surface, and
// "how much memory was left" belongs beside them. A SIGSEGV thirty seconds into
// a balloon and a SIGSEGV at a flat 3.9 GB are different bugs, and until now
// the report could not tell them apart.
static void kl_mem_fault_report(FILE *f) {
    if (!g_hb_seen) { fprintf(f, "  [mem] no heartbeat had run\n"); return; }
    fprintf(f, "  [mem] last heartbeat %02d:%02d:%02d  foot=%lldM avail=%lldM%s\n",
            g_hb_hour, g_hb_min, g_hb_sec, g_hb_foot >> 20, g_hb_avail >> 20,
            (g_hb_avail >= 0 && g_hb_avail < (2LL << 30)) ? "  <- under the tripwire" : "");
}

// Fires once, on the way up, at 2 GB of headroom.
static void kl_mem_tripwire(long long avail, long long foot) {
    static int fired;
    if (fired || avail < 0 || avail >= (2LL << 30)) return;
    fired = 1;
    fprintf(stderr, "\n  [mem] TRIPWIRE: %lld MiB of headroom left (footprint "
                    "%lld MiB). Where the address space is, once:\n",
            avail >> 20, foot >> 20);
    kl_region_summary(stderr);
    kl_glfb_gl_census(stderr);      // our own texture/renderbuffer ledger
    // ...and the GPU, which the region walk above is structurally blind to.
    // On the run that made this necessary the walk accounted for 1.5 GiB of a
    // 6153 MiB process and every one of the missing four and a half was in
    // Metal, where a private MTLHeap is charged to phys_footprint and mapped
    // nowhere.
    kl_vulkan_memory_report(stderr);
    fprintf(stderr, "  [mem] end of tripwire dump\n\n");
    fflush(stderr);
}

static void *heartbeat(void *arg) {
    (void)arg;
    // Polled faster than it prints, so a phase change is reported when it
    // happens rather than up to two seconds later. That matters here: the first
    // device run to reach graphics died inside one 2 s interval, so the whole
    // lifecycle was described by a single "phase=start" line.
    const char *last_phase = NULL;
    int ticks = 0;
    // The 250 ms sample is the honest one; the 2 s line is the one that survives
    // in the log. Launch 19 died in the gap between them — SIGKILL 1.0 s after a
    // line reading foot=4737M avail=3454M — and nothing in the file could say
    // whether the footprint had spiked in that second or stayed flat. Two cheap
    // additions close it, and between them a memory kill can no longer hide in a
    // print interval:
    //
    //   * `peak` is the high-water of every 250 ms sample, so a spike that rises
    //     and falls between two printed lines still leaves its mark on the next
    //     one. Absent this, "it was flat at every print" and "it doubled and came
    //     back" are the same log.
    //   * a printed line is forced whenever the footprint has MOVED more than
    //     256 MiB since the last one, so the interesting seconds print at 4 Hz
    //     and the flat ones stay at 0.5 Hz. Scene loads are exactly the moments
    //     worth the extra lines, and they are the only moments that get them.
    //
    // (The tripwire below stays the memory verdict of record: it samples at
    // 250 ms and fires under 2 GB of headroom. Launch 18 fired it at 1923 MiB and
    // LIVED; launch 19 never fired it and DIED — which is what ruled memory out
    // of launch 19 in the first place.)
    long long last_print_foot = -1;
    for (;;) {
        // Sampled every tick rather than every printed line, because the thing
        // this is watching for takes under ten seconds end to end — a balloon
        // that only had to survive one 2 s print interval would be missed.
        long long foot = kl_phys_footprint(), avail = kl_avail_memory();
        // The PEAK, carried across ticks. The line prints every 2 s and the
        // sample runs every 250 ms, so a spike that lands between two prints
        // is invisible in the log — and the spike is the whole question here:
        // hl2vr's map load moves the footprint by ~2.6 GB in under five
        // seconds and it is the closest this port gets to the ceiling.
        static long long peak_foot, min_avail = -1;
        if (foot > peak_foot) peak_foot = foot;
        if (min_avail < 0 || avail < min_avail) min_avail = avail;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        g_hb_foot = foot; g_hb_avail = avail;
        g_hb_hour = tm.tm_hour; g_hb_min = tm.tm_min; g_hb_sec = tm.tm_sec;
        g_hb_seen = 1;
        const char *phase = g_phase;
        // A printed line is forced whenever the footprint MOVED more than
        // 256 MiB since the last one, so the interesting seconds print at 4 Hz
        // and the flat ones stay at 0.5 Hz.
        int moved = last_print_foot >= 0 && foot >= 0 &&
                    llabs(foot - last_print_foot) >= (256LL << 20);
        if (phase != last_phase || moved || ticks % 8 == 0) {
            // The x18 recovery count rides the heartbeat because the fault it
            // belongs to CANNOT report itself. The other half of the `br x18`
            // veneer's race branches to address 0, and on visionOS that is a
            // CODESIGNING SIGKILL — no signal, no handler, no kl_x18_report, and
            // a device log that just stops mid-line. This count is the only
            // running measure of how hard a guest is racing that veneer, so it
            // has to be in a line that is already on the record when the process
            // dies. Printed only once it is non-zero, so a guest that never
            // races the veneer costs no noise.
            //
            // The REFUSAL count rides it for the same reason and a sharper one:
            // a refused site that writes x18 leaves the shadow stale and every
            // later read of it is wrong, so a non-zero count is a live fault
            // waiting to happen — and until round 20 it could only be asked of
            // `make check` on the host, never of the process that actually
            // died. Launch #19 was a silent SIGKILL and this was the first
            // question that could not be answered from its log.
            unsigned x18rec = kl_x18_recoveries();
            unsigned x18ref = kl_x18_refusals();
            fprintf(stderr, "[hb] %02d:%02d:%02d  phase=%s  foot=%lldM avail=%lldM"
                            "  (peak %lldM, avail low %lldM)",
                    tm.tm_hour, tm.tm_min, tm.tm_sec, phase, foot >> 20, avail >> 20,
                    peak_foot >> 20, min_avail >> 20);
            if (x18rec) fprintf(stderr, "  x18-recoveries=%u", x18rec);
            if (x18ref) fprintf(stderr, "  x18-REFUSED=%u", x18ref);
            fprintf(stderr, "\n");
            fflush(stderr);
            last_phase = phase;
            last_print_foot = foot;
        }
        // Every 10 s, a quarter as often as the memory line: whether the guest
        // is feeding the audio stream and whether what it feeds is silence.
        // "No audio" has cost four device rounds so far and none of them could
        // tell a dead sink from a dropped buffer, because nothing counted.
        if (ticks % 40 == 0) { kl_opensl_heartbeat(stderr); fflush(stderr); }

        // Beside it, every second, once there is a GPU to report on: the
        // footprint alone says a process is growing and never says where, and
        // on a Vulkan guest "where" is somewhere no region walk goes. Four
        // ticks, not eight — a balloon that took two seconds end to end
        // deserves to be sampled more than twice.
        if (ticks % 4 == 0) { kl_vulkan_memory_report(stderr); fflush(stderr); }
        kl_mem_tripwire(avail, foot);
        ticks++;
        struct timespec iv = { 0, 250000000 };  // nanosleep: usleep is EINVAL at >= 1e6
        nanosleep(&iv, NULL);
    }
    return NULL;
}
static void heartbeat_start(void) {
    static pthread_t th;
    static int started;
    if (started) return;
    started = 1;
    kl_fault_add_reporter(kl_mem_fault_report);
    pthread_create(&th, NULL, heartbeat, NULL);
    pthread_detach(th);
}

static int fail(const char *msg) {
    snprintf(g_status, sizeof g_status, "%s", msg);
    fprintf(stderr, "FAIL: %s\n", msg);
    fflush(NULL);
    return 1;
}

// --- Steam Link, both front doors in one process ----------------------------
//
// Map, report and initialise the VR chain. Idempotent, because two paths reach
// it: a launch that was handed a session (the door is VR from the start) and a
// handoff out of the shell (the door was SHELL and the VR chain is loaded on top
// of it). One function so those two cannot diverge.
static int steamlink_load_vr(void) {
    if (g_vr_loaded) return 0;
    // Re-describing the app, because the VR half is a DIFFERENT ACTIVITY in the
    // same package — android/app/NativeActivity with `android.app.lib_name`, not
    // SteamLink with the SDL <meta-data>. The host driver does this by re-exec'ing
    // into a fresh process; here it is a second call.
    g_door = KL_SLINK_VR;
    if (kl_slink_configure(KL_SLINK_VR, g_libdir, g_assets, g_apk, g_files,
                           stdout) != 0)
        return fail(kl_slink_error());
    kl_driver_init(g_target, g_libdir, KL_SLINK_VR);
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    g_vr_loaded = 1;
    return 0;
}

// What this target needs decided before its chain runs, in one place because
// both front doors reach it and neither can be told apart afterwards. Policy
// lives here rather than in kl_driver: the driver describes the guest, the
// frontend chooses how this platform runs it.
static void steamlink_policy(void) {
    // Steam Link's VR door composites several OpenXR projection layers: layer 0
    // is the environment/room (black in an empty space) and the streamed screen
    // is stacked ON TOP. The compositor shows one projection layer, so tell it
    // to take the topmost — the picture — rather than the base layer, which is
    // why the stream came up as a black void before. Harmless for the 2D shell
    // (it submits no projection layers); KL_XR_CAPTURE_LAYER still overrides.
    kl_openxr_set_capture_topmost_layer(1);
    // Steam Link streams FOVEATED: the wide base eye is low resolution and a
    // separate narrow inset carries the sharp centre (kl_openxr lays it over the
    // base). For that inset to keep its detail the compositor's eye texture must
    // be bigger than the base, so the inset is not scaled down to fit — allocate
    // it larger, but cap the larger side near the display's own per-eye
    // resolution so an already-large menu layer is not doubled and the allocator
    // does not churn. KL_XR_EYE_SCALE / KL_XR_EYE_MAX tune it.
    {
        int es = kl_env_int("KL_XR_EYE_SCALE", 2);
        if (es < 1) es = 1;
        if (es > 4) es = 4;
        kl_glfb_set_eye_mirror_scale(es, 1);
        kl_glfb_set_eye_mirror_cap(kl_env_int("KL_XR_EYE_MAX", 3456));
    }
    // Two things this target has always needed passed on the command line, made
    // defaults so a bare launch just works — the env still overrides either:
    //   - the 90 Hz pin (KL_DISPLAY_HZ): Steam Link's VR client dereferences a
    //     null frametime container on any rate the compositor MEASURES, so the
    //     rate it is told has to be fixed up front.
    //   - a deeper audio buffer (KL_AUDIO_LATENCY_MS): the audio is a network
    //     stream, and the 80 ms local-mixer default underruns on jitter — heard
    //     as the micro-stutters. 180 ms buffers the jitter at a latency a
    //     streamed title tolerates.
    kl_ovrp_set_forced_hz_hint((float)kl_env_int("KL_DISPLAY_HZ_DEFAULT", 90));
    kl_audio_set_latency_ms((unsigned)kl_env_int("KL_AUDIO_LATENCY_DEFAULT_MS", 240));
}

// The 2D -> VR handoff — and the one thing about it that is
// this driver's rather than the host driver's is that **there is no re-exec**.
//
// `build/m_boot` replaces its process image, which makes every question about
// tearing the shell down (Qt, SDL, ANGLE's contexts, the audio unit, the
// shell's threads) simply not arise. An app bundle cannot do that. So the two
// front doors coexist: this records the session, hands the ImmersiveSpace its
// cue, and then **parks this thread forever**.
//
// Parking is the honest shape and not a workaround. This is the guest's own
// main thread, inside startVRLink, and on Android the very next thing that
// method does is finishAndRemoveTask() on itself — an activity that stops
// running. kl_jni.h's contract says the same thing from the other end: a
// handler that RETURNS still aborts, because returning tells the guest its
// activity started when nothing did, and the shell then finishes itself with no
// picture and no reason given.
//
// The GL context is released first, and that one is load-bearing rather than
// tidy: kl_glfb hands the root context to one thread at a time, and a parked
// thread still holding it is a VR guest that can never make one current.
static char           g_sargs[4096];
static volatile int   g_handoff;

static void app_vrlink_handoff(const char *sargs) {
    snprintf(g_sargs, sizeof g_sargs, "%s", sargs ? sargs : "");
    // Through the environment as well as the variable, because that is the path
    // a hand-carried session already takes (KL_SLINK_SARGS) and the guest must
    // not be able to tell the two apart.
    setenv("KL_SLINK_SARGS", g_sargs, 1);

    printf("\n=== 2D -> VR handoff: the shell paired and the host authorized ===\n");
    printf("    the OpenXR front door opens in the ImmersiveSpace; this thread "
           "parks (no re-exec inside an app bundle)\n");
    // A stopgap, announced rather than assumed. On the window path nothing ever
    // holds a drawable, so no eye offsets are measured and the IPD is 0 — which
    // this client reads as "unchanged" and never publishes its projection, so
    // the host sends no video.
    if (!getenv("KL_OVRP_IPD")) {
        setenv("KL_OVRP_IPD", "0.063", 1);
        printf("    KL_OVRP_IPD=0.063   (STOPGAP: an IPD of 0 stops the host "
               "sending video)\n");
    }
    fflush(NULL);

    kl_glfb_release_current();
    // From here on the answer to SteamLink.isVRLinkRunning() is yes — the
    // session is recorded and the ImmersiveSpace is about to open the VR door.
    // Set before the park, because the asker is the SHELL's own background
    // threads, which outlive this thread and poll exactly that question.
    kl_jni_set_vrlink_running(1);
    // The shell's pump has nothing left to drain — its main thread is about to
    // stop existing for all practical purposes — so let whoever is pumping it
    // finish and write its report.
    g_guest_quit = 1;
    __atomic_store_n(&g_handoff, 1, __ATOMIC_RELEASE);

    for (;;) {
        struct timespec iv = { 3600, 0 };
        nanosleep(&iv, NULL);
    }
}

int kl_app_vrlink_pending(void) { return __atomic_load_n(&g_handoff, __ATOMIC_ACQUIRE); }
const char *kl_app_vrlink_sargs(void) { return g_sargs; }

// The VR front door opened on top of a process that is already running the
// shell. The other way in — a launch that was handed a session — takes
// kl_app_boot and kl_app_lifecycle_begin like any other target; this exists
// because the immersive space opens mid-run and has to do both halves itself.
//
// It MUST run on the thread that will go on to pump: onCreate takes
// ALooper_forThread() and the guest hangs its UIThreadCallbackHandler off
// exactly that looper.
static int steamlink_vr_begin(void) {
    // The handoff set this to end the SHELL's pump; a new front door is a new
    // run and the VR pump must not inherit it. Cleared here rather than in the
    // handoff because the handoff's own meaning is "stop pumping the shell" and
    // clearing it there would be a race with the loop it is trying to stop.
    g_guest_quit = 0;
    // Here as well as at the handoff, because this door is also reached
    // DIRECTLY — a launch handed KL_SLINK_SARGS never runs the shell, and
    // isVRLinkRunning() must still answer for the door that is opening.
    kl_jni_set_vrlink_running(1);
    if (steamlink_load_vr()) return 1;
    if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());
    return 0;
}

// The SDL door — a FOURTH entry shape, and the bill for its absence was launch
// #1 of this port: without this arm the kind fell through to the libmain.so
// path below and died at `open .../guest/lib/arm64-v8a/libmain.so: No such
// file or directory`, while the host — whose driver had the door inline —
// booted the same guest to its menu. There is no libmain and no NativeLoader
// here: the chain is every library in the APK, mapped dependencies-first, and
// the guest is entered through SDL's own registered nativeRunMain from the
// guest thread.
//
// Shared verbatim with `build/m_boot`, for kl_slink's reason: a guest described
// differently by two drivers is a bug with no error surface.
static int boot_sdl(void) {
    g_phase = "sdl configure";
    // The libdir is ABSOLUTE here already (the container's), which this door
    // needs for the reason the OpenJK one does: APP_LIB_PATH is what the
    // launcher concatenates its module names onto, and the engine chdirs into
    // its game root on the way up.
    if (kl_sdl_configure(g_libdir, stdout) != 0) return fail(kl_sdl_error());
    g_phase = "sdl chain";
    if (kl_sdl_load(stdout) != 0) return fail(kl_sdl_error());

    printf("\n=== the shim gap ===\n");
    kl_sdl_gap(stdout);

    printf("\n=== EXIT CRITERION MET: the srceng chain is bound and "
           "initialised on visionOS ===\n");
    g_phase = "boot report";
    kl_jni_report(stdout);
    fflush(NULL);
    g_phase = "boot done";
    snprintf(g_status, sizeof g_status, "the srceng chain initialised");
    return 0;
}

int kl_app_boot(void) {
    // Once, and never concurrently. The Boot button invites a second press, and
    // the second entry is not merely redundant: the runtime's JNI tables are
    // process-global, so another JNI_OnLoad re-registers every native onto the
    // same table, while open_log() truncates the file the first run is still
    // writing. Five presses on device produced "natives registered: 61" instead
    // of 45 with a chunk of the log missing, which reads as a platform
    // difference and is not one.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_boot was already run in this process");

    if (!*g_libdir) return fail("kl_app_configure was not called");
    if (open_log()) return fail("could not open the log file");
    // The env-file and target-row lines were printed by kl_app_configure,
    // before the log existed; repeat them here so the log says what it ran with.
    fprintf(stderr, "  [env-file] applied: %s\n",
            g_env_file_applied[0] ? g_env_file_applied : "(none)");
    fprintf(stderr, "  [target] %s env: %s (row; an env-file value wins over a row entry)\n",
            g_target ? g_target->name : "?",
            g_target && g_target->env && *g_target->env ? g_target->env : "(none)");
    // The two app-side row flags, which no other line names: a row that lost
    // own_hands in a crossing (gorn2, 2026-10-09) showed the wearer's real
    // hands over the guest's, and nothing in the log said so.
    fprintf(stderr, "  [target] %s own_hands %d (system upper limbs %s unless KL_HANDS), "
            "opaque_world %d\n",
            g_target ? g_target->name : "?",
            g_target ? g_target->own_hands : 0,
            g_target && g_target->own_hands ? "HIDDEN" : "shown",
            g_target ? g_target->opaque_world : 0);

    // Every thread that runs guest code must seed bionic's stack-guard canary
    // into TSD slot 5 first, and this is the thread the whole boot runs on.
    // Beat Saber's chain does not expose the gap: nothing on the path to initJni
    // has a stack protector on it. libSDL3's JNI_OnLoad does, and without this
    // dies with `stack smashing detected in libSDL3.so+0xa6f9c` — a canary
    // mismatch with no buffer overflow anywhere near it. Idempotent, so calling
    // it on a thread that already has one is free.
    kl_thread_init();

    heartbeat_start();
    kl_fault_install();
    // This thread runs the guest lifecycle (initJni, then nativeRender), so on
    // Android it IS the UI thread, which ActivityThread gives a native looper
    // before any activity's onCreate. A Unity guest's own init takes
    // ALooper_forThread() for the UI thread and does NOT check the answer: with
    // no looper it leaves its UI-thread callback-handler singleton NULL, and a
    // later worker thread dereferences the null owner (WMG-34's signal-11 at
    // libunity+0xaa6ff8, x0=0; old-gorn2 GORN2-18 hit the identical crash on
    // device). Idempotent. Carried from gorn2-synced f0305ffa, where every
    // device round of GORN 2 ran with it. Unity-only on this branch, so no
    // other door's Looper.myLooper() answer changes.
    if (g_target && g_target->kind == KL_GUEST_UNITY) kl_ndk_prepare_looper();
    // Subscribe to the OS's memory-pressure signal before the guest allocates
    // anything. On this platform it is the only warning that arrives ahead of
    // jetsam, and jetsam is a kill with no log line of its own.
    kl_mem_pressure_init();
    // Strict: an unimplemented *call* is fatal, so the run stops exactly where
    // the surface genuinely ends. Lookups are not — the guest resolves plenty
    // it never calls.
    kl_jni_set_permissive(kl_env_on("KL_PERMISSIVE", 0));
    kl_egl_dump_textures(kl_env_str("KL_DUMP_TEXTURES", NULL));

    printf("=== Klepton on visionOS ===\n");
    printf("  target    : %s\n", g_target->name);
    // The front door, with the reason. It is chosen from the ENVIRONMENT in
    // kl_app_configure — which runs before the log file is open, so it cannot
    // announce itself there — and an app launched from the Home View has no
    // environment at all. A run that was meant to carry a session and silently
    // took the shell instead is indistinguishable from a broken handoff, and
    // the first thing it does is open a screen that looks like it is working.
    if (kl_app_target_is_steamlink())
        printf("  front door: %s   (KL_SLINK_SARGS %s, KL_SLINK_VR %s, "
               "KL_SLINK_SHELL %s)\n",
               g_door == KL_SLINK_VR ? "VR (libvrlink_scene)" : "SHELL (libshell + Qt)",
               kl_env_str("KL_SLINK_SARGS", NULL) ? "carried" : "UNSET",
               kl_env_on("KL_SLINK_VR", 0) ? "on" : "off",
               kl_env_on("KL_SLINK_SHELL", 0) ? "ON, which overrides the others" : "off");
    printf("  libraries : %s\n", g_libdir);
    printf("  dylibs    : %s%s\n", g_dylibs,
           have_translations() ? "" : "  (none embedded — falling back to the mmap ELF loader)");
    printf("  assets    : %s\n", g_assets);
    printf("  apk       : %s\n", g_apk);
    printf("  files     : %s\n\n", g_files);
    fflush(NULL);

    // Before the chain runs, so nothing it starts reads a default we meant to
    // replace.
    if (kl_app_target_is_steamlink()) steamlink_policy();

    // The door's own sequence, in kl_driver: the chain and its entry point for
    // Steam Link, Unreal and OpenJK; libmain -> NativeLoader.load -> initJni for
    // a Unity guest. It stops before the guest is STARTED, which is what makes
    // "the guest loaded" and "the guest ran" two reports rather than one.
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());

    // The veneer census, here because this is the first moment every guest
    // library is mapped and patched — whichever door the chain took. It prints
    // nothing when there is nothing to say, which is why it was never missed —
    // and why PTTR's launch #19 silent SIGKILL could not be answered from its
    // own log. `br x18` branching to a zeroed shadow is a codesigning kill: no
    // signal, no handler, no crash report, the log just stops. The only way to
    // rule that class in or out after the fact is to know what the pass did to
    // THIS target on THIS device, and that is what this line is.
    kl_x18_report(stdout);
    fflush(NULL);

    if (kl_app_target_is_steamlink()) {
        if (g_door == KL_SLINK_VR) {
            g_vr_loaded = 1;
            // Not "no sArgs, therefore stop": the guest decides that for itself
            // and prints "No sArgs and release build panic" on its own way out.
            // But it exits BEFORE its first frame, and that reads exactly like a
            // compositor failure from the outside.
            if (!getenv("KL_SLINK_SARGS"))
                printf("\n  NOTE: KL_SLINK_SARGS is unset. This guest reads its session out of\n"
                       "  the launching Intent and exits before its first frame without one —\n"
                       "  pair in the 2D shell first; the handoff carries it.\n");
        }
        snprintf(g_status, sizeof g_status, "Steam Link %s chain initialised",
                 kl_slink_door_name());
    } else {
        snprintf(g_status, sizeof g_status, "%s",
                 g_target->kind == KL_GUEST_UE4  ? "the Unreal chain initialised"
                 : target_is_jkxr()              ? "the OpenJK chain initialised"
                                                 : "initJni completed");
    }
    app_phase("boot report");
    kl_jni_report(stdout);
    fflush(NULL);
    app_phase("boot done");
    return 0;
}

// The synthetic /proc, read back on device. Read it BEFORE chasing anything
// else in a device lifecycle run: proc_build() gets the free-page count from
// host_statistics64(), which links on visionOS but may be restricted inside the
// sandbox, and a silent zero there is not read by Unity as "unknown" but as a
// machine with no memory — `Cores = 0, Memory = 0mb`, after which Unity refuses
// to start with nothing anywhere mentioning memory.
static void report_proc(void) {
    static const char *files[] = { "/proc/cpuinfo", "/proc/meminfo",
                                   "/sys/devices/system/cpu/possible" };
    printf("\n=== the synthetic /proc, on device ===\n");
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        // Through the same rewrite the guest's own open() takes, so this reads
        // what the guest reads and not something adjacent to it.
        char buf[1024];
        const char *real = kl_guest_path(files[i], buf, sizeof buf);
        FILE *f = real ? fopen(real, "r") : NULL;
        if (!f) { printf("  %-34s NOT SERVED (%s)\n", files[i], real ? real : "no path");
                  continue; }
        char line[256];
        int shown = 0;
        while (shown < 4 && fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            // MemTotal/MemFree are the two that matter; for cpuinfo the first
            // lines are enough to see it is not empty.
            if (strstr(files[i], "meminfo") && !strstr(line, "Mem")) continue;
            printf("  %-34s %s\n", shown ? "" : files[i], line);
            shown++;
        }
        if (!shown) printf("  %-34s (served but EMPTY)\n", files[i]);
        fclose(f);
    }
    fflush(NULL);
}

int kl_app_lifecycle_begin(void) {
    // Separate from kl_app_boot rather than folded into it, because boot is a
    // gate and refuses a second entry: keeping them apart lets the UI run the
    // gate, read its numbers, and only then go further.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_lifecycle was already run in this process");

    // First, before anything can be misdiagnosed on top of it: proc_build() gets
    // the free-page count from host_statistics64(), which links here but may be
    // restricted inside the sandbox, and Unity reads a silent zero not as
    // "unknown" but as a machine with no memory.
    app_phase("proc");
    report_proc();

    // The 2D shell is the one door with anything to set up first: the frame
    // capture must be live before its first swap (kl_glfb captures only when a
    // sink or a dump directory is set), and the handoff handler is the shell's
    // alone — installing it on another door would let a run take a path it has no
    // business on. app_vrlink_handoff never returns.
    if (kl_app_target_is_steamlink() && g_door == KL_SLINK_SHELL) {
        kl_mono_capture_start();
        kl_jni_set_vrlink_handoff(app_vrlink_handoff);
    }

    // The guest's own start, in kl_driver. It MUST run on the thread that will go
    // on to pump: the three doors that own their frame loop take
    // ALooper_forThread() inside onCreate and hang their callbacks off exactly
    // that looper.
    if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());
    return 0;
}

// ---- DIAGNOSTIC (phase 1, uncommitted): the stall dump -------------------
//
// The device has no sampler and nothing to attach to, so a run whose guest
// simply stops says nothing about WHERE it stopped. `kl_fault.c` already
// answers SIGALRM with a thread dump, but only for the thread the signal
// landed on, and the alarm is armed only *around* one nativeRender — a guest
// whose nativeRender returns promptly while doing nothing never trips it.
//
// So: watch the one number that separates "stalled" from "alive but drawing
// nothing" — the total ovrp_* call count, which a running Unity frame loop
// drives up by ~10 every frame through the node-pose poll. When it has not
// moved for KL_STALL_S seconds, dump EVERY thread's pc and frame chain, once.
// Addresses print as `<guest image>+0x<offset>`, which is directly objdump-able
// against the APK's own libraries.
#include <dlfcn.h>
#include <mach/mach.h>

static void stall_dump_thread(thread_act_t act, mach_port_t self) {
    arm_thread_state64_t ts;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(act, ARM_THREAD_STATE64,
                         (thread_state_t)&ts, &cnt) != KERN_SUCCESS) return;
    char nm[64] = {0};
    pthread_t pt = pthread_from_mach_thread_np(act);
    if (pt) pthread_getname_np(pt, nm, sizeof nm);
    printf("  -- thread %#x%s%s%s\n", act, nm[0] ? " \"" : "", nm[0] ? nm : "",
           nm[0] ? "\"" : (act == self ? " (the watchdog itself)" : ""));

    void *frames[16];
    unsigned nf = 0;
    frames[nf++] = (void *)(uintptr_t)ts.__pc;
    uint64_t fp = ts.__fp;
    while (fp && nf < 16) {
        if (fp & 7) break;
        uint64_t pair[2];
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof pair,
                              (vm_address_t)pair, &got) != KERN_SUCCESS ||
            got != sizeof pair) break;
        if (!pair[1] || pair[0] <= fp) break;
        frames[nf++] = (void *)(uintptr_t)pair[1];
        fp = pair[0];
    }
    for (unsigned f = 0; f < nf; f++) {
        size_t off = 0;
        const char *img = kl_addr_image(frames[f], &off);
        Dl_info di;
        if (img)
            printf("     #%-2u %s+0x%zx\n", f, img, off);
        else if (dladdr(frames[f], &di) && di.dli_sname)
            printf("     #%-2u %s+0x%tx\n", f, di.dli_sname,
                   (const char *)frames[f] - (const char *)di.dli_saddr);
        else
            printf("     #%-2u %p\n", f, frames[f]);
    }
}

static void stall_dump(uint64_t ovrp_total, unsigned quiet_s) {
    printf("\n=== STALL DUMP: ovrp call total flat at %llu for %us "
           "(%llu frames pumped) ===\n",
           (unsigned long long)ovrp_total, quiet_s,
           (unsigned long long)kl_driver_frames());
    fflush(NULL);
    thread_act_array_t list;
    mach_msg_type_number_t nlist = 0;
    if (task_threads(mach_task_self(), &list, &nlist) != KERN_SUCCESS) {
        printf("  task_threads failed\n"); fflush(NULL); return;
    }
    mach_port_t self = mach_thread_self();
    printf("  %u threads\n", (unsigned)nlist);
    for (mach_msg_type_number_t i = 0; i < nlist; i++) {
        stall_dump_thread(list[i], self);
        fflush(NULL);
    }
    mach_port_deallocate(mach_task_self(), self);
    for (mach_msg_type_number_t i = 0; i < nlist; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, nlist * sizeof *list);
    // The surface reports say how far the guest got; kl_pthread_report names
    // any mutex whose holder is parked, which the raw pcs above cannot.
    kl_pthread_report(stdout);
    kl_ovrp_report(stdout);
    printf("=== STALL DUMP end ===\n");
    fflush(NULL);
}

static void stall_watchdog(void) {
    static uint64_t last_total;
    static double   last_move_s;
    static int      dumped;
    if (dumped) return;
    unsigned quiet = kl_env_int("KL_STALL_S", 10);
    if (!quiet) return;
    uint64_t total = kl_ovrp_call_total();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    if (total != last_total || last_move_s == 0) {
        last_total = total; last_move_s = now; return;
    }
    if (now - last_move_s < (double)quiet) return;
    dumped = 1;
    stall_dump(total, quiet);
}

#define QUIT_NOTE "  <- IsQuitting(): the guest has asked to quit"

// One guest frame. Split out of the pump loop so that Compositor Services can be
// the clock: on device the frame deadline belongs to cp_frame_predict_timing, and
// a pump that owns its own loop cannot be paced by something else.
//
// Returns what nativeRender returned, or -1 for a guest that owns its own frame
// loop (the three doors where what our thread owes it is a turning looper) or
// before kl_app_lifecycle_begin has run.
int kl_app_frame(void) {
    int r = kl_driver_frame();
    // What nativeRender returns is a signal, not a status. Unity's body
    // (libunity+0x332dc4 in this title) returns 0 from exactly ONE exit — the
    // IsQuitting() gate at +0x332f5c, i.e. something called
    // UnityEngine.Application.Quit() — and 1 from every other exit, a healthy
    // rendering frame included. So the first value plus every flip says which
    // half of a stall we are in, and costs one line per transition.
    static int r_seen, r_last;
    if (!r_seen || r != r_last) {
        if (!r_seen) printf("[guest] nativeRender -> %d (first, frame %u)%s\n",
                            r, kl_driver_frames(), r ? "" : QUIT_NOTE);
        else         printf("[guest] nativeRender %d -> %d (frame %u)%s\n",
                            r_last, r, kl_driver_frames(), r ? "" : QUIT_NOTE);
        fflush(NULL);
        r_seen = 1; r_last = r;
    }
    stall_watchdog();   // DIAGNOSTIC (phase 1, uncommitted)
    return r;
}

void kl_app_lifecycle_report(void) {
    kl_driver_report(stdout);
    if (kl_app_target_is_steamlink())
        snprintf(g_status, sizeof g_status, "Steam Link %s run ended",
                 kl_slink_door_name());
    else if (g_target->kind == KL_GUEST_UE4)
        snprintf(g_status, sizeof g_status, "the Unreal run ended");
    else if (target_is_jkxr())
        snprintf(g_status, sizeof g_status, "the OpenJK run ended");
    else
        snprintf(g_status, sizeof g_status, "lifecycle ran, %u frames",
                 kl_driver_frames());
}

int kl_app_lifecycle(unsigned frames) {
    int rc = kl_app_lifecycle_begin();
    if (rc) return rc;

    // A guest that owns its own frame loop takes no frames from us, so what a
    // bounded run means for it is a bounded PUMP, in seconds — KL_SLINK_WAIT /
    // KL_UE4_WAIT / KL_JKXR_WAIT, the same budgets the command line reads.
    // `frames` would be a number with nothing behind it.
    //
    // The 2D shell is the one door with no deadline by default: a bounded pump
    // measures how far a guest got by itself, and the shell is not being
    // measured but USED — a person reads it, types a PIN into it and waits on a
    // host, and a first pairing takes ~35 s. An explicit KL_SLINK_WAIT still
    // bounds it, for a scripted run that wants that.
    if (kl_app_target_owns_frame_loop()) {
        int open_ended = kl_app_target_is_steamlink() && g_door == KL_SLINK_SHELL
                      && !kl_env_str("KL_SLINK_WAIT", NULL);
        double secs = open_ended ? -1.0 : kl_driver_pump_default();
        if (open_ended)
            printf("\n=== the shell is running; pumping until the window closes ===\n");
        else
            printf("\n=== pumping the looper for %.1f s ===\n", secs);
        fflush(NULL);
        printf("  pumped %.2f s\n", kl_driver_pump(secs, &g_guest_quit));
        kl_app_lifecycle_report();
        return 0;
    }

    // ...and the SDL guest, which cannot be bounded from here at all: its
    // engine runs inside nativeRunMain and that call does not come back. So the
    // window path enters it and the run ends when the guest ends, exactly as on
    // the immersive path — what differs between the two is which thread pays,
    // not what is measured. (`build/m_boot` bounds the same call with KL_ALARM;
    // an app has no such exit to take.)
    if (target_is_sdl()) {
        printf("\n=== entering the game; nativeRunMain does not return ===\n");
        fflush(NULL);
        int rc = kl_sdl_run_main(stdout);
        printf("  the guest left nativeRunMain (%d)\n", rc);
        kl_app_lifecycle_report();
        return 0;
    }

    printf("\n=== pumping %u frames ===\n", frames);
    fflush(NULL);
    while (kl_driver_frames() < frames && kl_app_frame() >= 0) { }
    kl_app_lifecycle_report();
    return 0;
}

// --- The guest on its own thread ---------------------------------------
// See kl_app.h for what this is for. What is here is the handoff itself, and
// it is deliberately in C rather than Swift for one reason above the language
// boundary: every thread that runs guest code must call
// kl_thread_init() before it does — the TPIDR slot the veneered guest reads is
// per-thread and this is where it is established. A Swift `Thread {}` that
// called kl_app_frame() without it would fault in a way that looks exactly
// like an x18 bug and is not, so the thread is created at the same place the
// init happens and there is no way to get one without the other.
static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    uint64_t        published;    // poses the compositor has offered
    uint64_t        consumed;     // poses the guest has started a frame for
    unsigned        limit;        // KL_FRAMES, or 0 for "until stopped"
    // volatile because the Steam Link pump reads it WITHOUT the lock: that loop
    // is inside kl_slink and is handed a plain flag, which is the honest shape
    // for "somebody else will set this once". Every other reader takes the mutex.
    volatile int    stop;
    int             state;        // kl_app_guest_state()
    int             running;      // has the thread been spawned and not joined?
    int             finished;     // has it left its loop and written the report?
    pthread_t       thread;
} g_guest = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

int kl_app_guest_state(void) {
    pthread_mutex_lock(&g_guest.mu);
    int s = g_guest.state;
    pthread_mutex_unlock(&g_guest.mu);
    return s;
}

static void guest_set_state(int s) {
    pthread_mutex_lock(&g_guest.mu);
    g_guest.state = s;
    pthread_mutex_unlock(&g_guest.mu);
}

// Both exit paths run through here, so kl_app_guest_stop() can never be left
// waiting on a thread that has already gone.
static void guest_finished(void) {
    pthread_mutex_lock(&g_guest.mu);
    g_guest.state = -1;
    g_guest.finished = 1;
    pthread_cond_broadcast(&g_guest.cv);
    pthread_mutex_unlock(&g_guest.mu);
}

// The OpenXR guest's xrWaitFrame, waiting on the same publish the Beat Saber
// loop above consumes. Called from a thread libvrlink_scene created, not from
// the one guest_thread runs on, which is why it takes the lock and touches
// nothing else.
//
// Timed, not indefinite: a compositor that stops publishing must make the guest
// render against the last pose it had, not wedge it. A wedged guest is
// indistinguishable from a crashed one in a device log, and this is exactly the
// path a backgrounded app takes.
static void guest_pace_wait(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_mutex_lock(&g_guest.mu);
    while (!g_guest.stop && g_guest.published == g_guest.consumed)
        if (pthread_cond_timedwait(&g_guest.cv, &g_guest.mu, &ts) == ETIMEDOUT) break;
    // Coalesce, do not queue: everything published since the last frame is one
    // turn. A guest that is behind then skips frames — which reprojection covers
    // — rather than being owed a backlog it can never work off.
    g_guest.consumed = g_guest.published;
    // Counted here because on this path the guest CALLS the clock rather than
    // being called by it: kl_driver_frame never runs for a guest that owns its
    // own loop, so nothing else would count its frames.
    kl_driver_note_frame();
    pthread_mutex_unlock(&g_guest.mu);
}

static void *guest_thread(void *unused) {
    (void)unused;
    // The guest's TPIDR slot, before a single guest instruction runs here.
    kl_thread_init();
    pthread_setname_np("Klepton Guest");

    // Two ways in, and which one this is depends on what the WINDOW already
    // did. On a plain launch this thread is the first thing to run a guest, and
    // kl_app_lifecycle_begin is that. After a 2D->VR handoff the window path has
    // already spent that entry on the shell — so the immersive space is opening
    // on top of a process that is mid-run, and what it starts is the OTHER front
    // door. Both are once-per-process; they are just not the same once.
    int rc = kl_app_vrlink_pending() ? steamlink_vr_begin() : kl_app_lifecycle_begin();
    if (rc != 0) {
        printf("[guest] %s failed: %s\n",
               kl_app_vrlink_pending() ? "the VR front door" : "lifecycle_begin", g_status);
        fflush(NULL);
        guest_finished();
        return NULL;
    }
    guest_set_state(1);

    // Two shapes, because the doors differ in who owns the frame loop.
    //
    // A Unity guest has none: nativeRender is a call, so this thread makes one
    // per published pose. The other three brought their own — a thread spawned
    // inside onCreate or android_main — so what this thread owes them is a
    // turning looper and nothing else. Pacing is not lost by that: it moves to
    // where the XR API puts it (xrWaitFrame, ovrp_WaitToBeginFrame), which blocks
    // on the same published pose through guest_pace_wait. One clock either way.
    //
    // Unbounded here, unlike the window path: the immersive space's own dismissal
    // is what ends the run, through g_guest_quit.

    // The SDL guest owns its loop too, but there is no looper to turn for it:
    // the engine IS nativeRunMain, and this thread is what runs it. It does not
    // return on a healthy run, so everything after the call is the guest's own
    // exit path.
    if (target_is_sdl()) {
        printf("\n=== guest thread entering the game (nativeRunMain) ===\n");
        fflush(NULL);
        int rc = kl_sdl_run_main(stdout);
        printf("[guest] nativeRunMain returned %d\n", rc);
        fflush(NULL);
        kl_app_lifecycle_report();
        guest_finished();
        return NULL;
    }
    if (kl_app_target_owns_frame_loop()) {
        printf("\n=== guest thread pumping the activity's looper ===\n");
        fflush(NULL);
        double secs = kl_driver_pump(-1.0, &g_guest_quit);
        printf("[guest] pumped for %.1fs\n", secs);
        fflush(NULL);
        kl_app_lifecycle_report();
        guest_finished();
        return NULL;
    }

    printf("\n=== guest thread running, one frame per published pose ===\n");
    fflush(NULL);

    for (;;) {
        pthread_mutex_lock(&g_guest.mu);
        while (!g_guest.stop && g_guest.published == g_guest.consumed)
            pthread_cond_wait(&g_guest.cv, &g_guest.mu);
        int stop = g_guest.stop;
        // Coalesce, do not queue: take everything published since the last
        // frame as a single turn. This is what makes "the guest is late" mean
        // "frames were skipped" — which reprojection covers — rather than
        // "frames are owed", which is a backlog that only grows.
        g_guest.consumed = g_guest.published;
        unsigned limit = g_guest.limit;
        pthread_mutex_unlock(&g_guest.mu);
        if (stop) break;

        if (kl_app_frame() < 0) {
            printf("[guest] nativeRender is gone — the frame loop ends here\n");
            fflush(NULL);
            break;
        }
        if (limit && kl_driver_frames() >= limit) {
            printf("[guest] reached KL_FRAMES=%u\n", limit);
            fflush(NULL);
            break;
        }
    }

    // The report belongs to the guest's loop end, not the render loop's: with
    // the two decoupled, "the run ended" has two meanings and these are the
    // guest's numbers.
    kl_app_lifecycle_report();
    guest_finished();
    return NULL;
}

int kl_app_guest_start(void) {
    pthread_mutex_lock(&g_guest.mu);
    if (g_guest.running) {
        pthread_mutex_unlock(&g_guest.mu);
        return fail("kl_app_guest_start was already run in this process");
    }
    g_guest.limit = kl_env_uint("KL_FRAMES", 0);
    g_guest.running = 1;
    g_guest.state = 0;
    pthread_mutex_unlock(&g_guest.mu);

    // Only on this path, and before the thread exists. kl_app_lifecycle() has no
    // publisher at all — it is the command line's shape, a plain loop — so a
    // pacer installed there would block the guest on a pose nobody will ever
    // publish. Installing it here also closes the window in which the guest could
    // reach its first xrWaitFrame unpaced, because the guest does not exist yet.
    // ...and the OpenJK guest takes the same one, because it is the same API:
    // its frame clock is xrWaitFrame, wherever the engine calls it from. Which
    // pacer a guest wants is a property of the XR RUNTIME it drives, not of the
    // door it came through — Steam Link and JKXR are both OpenXR, RE4 is
    // OVRPlugin, and pairing a door with the wrong one is a guest that blocks
    // forever on a pose nobody publishes to it.
    // ...and the SDL guest joins them by the same test: which pacer a guest
    // wants is a property of the XR RUNTIME it drives, not of the door it came
    // through. srceng's VR layer is OpenXR — its libopenxr_loader is the one
    // this project replaces — so its frame clock is xrWaitFrame.
    if (kl_app_target_is_steamlink() || target_is_jkxr() || target_is_sdl())
        kl_openxr_set_frame_pacer(guest_pace_wait);
    // ...and the same clock on the OVRPlugin side, for the other guest that
    // owns its loop. ovrp_WaitToBeginFrame is this API's xrWaitFrame. It is
    // installed ONLY here: every Unity guest reaches that call from inside a
    // frame this driver already initiated, so a pacer there would be the
    // compositor waiting on itself.
    if (g_target->kind == KL_GUEST_UE4) kl_ovrp_set_frame_pacer(guest_pace_wait);

    // User-interactive, because this thread is now the one producing frames.
    // A plain pthread gets an unspecified QoS, and a guest demoted below the
    // compositor is a guest that misses deadlines for scheduling reasons that
    // would then be blamed on the shim.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    int rc = pthread_create(&g_guest.thread, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_guest.mu);
        g_guest.running = 0;
        g_guest.state = -1;
        pthread_mutex_unlock(&g_guest.mu);
        return fail("could not create the guest frame thread");
    }
    return 0;
}

void kl_app_guest_publish(void) {
    pthread_mutex_lock(&g_guest.mu);
    g_guest.published++;
    pthread_cond_signal(&g_guest.cv);
    pthread_mutex_unlock(&g_guest.mu);
}

void kl_app_guest_stop(void) {
    pthread_mutex_lock(&g_guest.mu);
    if (!g_guest.running) { pthread_mutex_unlock(&g_guest.mu); return; }
    g_guest.stop = 1;
    g_guest_quit = 1;      // the kl_slink pumps watch this one
    g_guest.running = 0;
    pthread_t t = g_guest.thread;
    pthread_cond_broadcast(&g_guest.cv);

    // Bounded, not a plain join. The guest can wedge — a Baselib futex nobody
    // will post, the GC failing to stop the world — and that is a documented
    // failure mode here, not a hypothetical. A render loop being torn down
    // must not be the thing that hangs waiting for it: past the deadline the
    // thread is left to whatever it is doing and the caller carries on without
    // the report, which is an honest missing report rather than a silent hang
    // that would read as a compositor bug.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    while (!g_guest.finished)
        if (pthread_cond_timedwait(&g_guest.cv, &g_guest.mu, &ts) == ETIMEDOUT) break;
    int finished = g_guest.finished;
    pthread_mutex_unlock(&g_guest.mu);

    if (finished) {
        // Joined, not detached, in the normal case: the report is written on
        // the way out and the caller's next act may be to hand the log to the UI.
        pthread_join(t, NULL);
    } else {
        pthread_detach(t);
        printf("[guest] did not stop within 10 s — carrying on without its report\n");
        fflush(NULL);
    }
}
