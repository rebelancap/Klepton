// QuakeQuest's front door — the second target, and the first OpenXR one.
//
// `m_boot.c` is Beat Saber's front door and is Unity-shaped all the way down:
// it enters through libmain.so, drives com.unity3d.player.NativeLoader to
// dlopen libunity.so, then UnityPlayer.initJni(Context). None of that exists
// here. QuakeQuest is a hand-written C engine (DrBeef's GPL port) whose Java
// half is the Oculus mobile-sample `GLES3JNILib` shape: one library, a
// JNI_OnLoad, and nine lifecycle statics the Activity calls in order.
//
// So this is a separate main rather than a flag on m_boot — the two share the
// runtime, not the entry sequence.
//
// ---------------------------------------------------------------------------
// The lifecycle, read off the APK rather than assumed
//
// apktool'd GLES3JNILib.smali declares (JNI static => JNIEnv*, jclass, args):
//
//   onCreate(Landroid/app/Activity;Ljava/lang/String;)J   -> the engine handle
//   onStart(JLjava/lang/Object;)V
//   onResume(J)V / onPause(J)V / onStop(J)V / onDestroy(J)V
//   onSurfaceCreated(JLandroid/view/Surface;)V
//   onSurfaceChanged(JLandroid/view/Surface;)V
//   onSurfaceDestroyed(J)V
//
// The String is GLES3JNIActivity.commandLineParams, read from
// assets/commandline.txt — "quake" in this build, naming the game directory.
//
// **It must not be empty, and that is a guest bug rather than a preference.**
// onCreate does, at +0x168:
//
//     chars = GetStringUTFChars(env, commandLineParams, &isCopy);
//     cmdline = (chars && strlen(chars)) ? strdup(chars) : NULL;
//     ...
//     strdup(cmdline);            // unconditional — +0x1c0
//
// so an empty command line becomes NULL and is then strdup'd anyway, which is
// a SIGSEGV inside strlen. Passing "" was the first thing this main did, and it
// crashed exactly there. The file is read rather than hardcoded so the value
// stays whatever the APK ships; KL_QQ_CMDLINE overrides it.
//
// These are ordinary exported symbols (`Java_com_drbeef_quakequest_GLES3JNILib_*`)
// rather than RegisterNatives entries, so they resolve with kl_sym off the
// image — no JNI registration table involved.
//
// The Surface argument is opaque here: kl_ndk's ANativeWindow_fromSurface
// ignores it and hands back the singleton window, so any non-NULL jobject
// carries the same meaning. It is passed as a real object anyway, because the
// guest is free to call back into it and a NULL would be a different story.
//
// ---------------------------------------------------------------------------
// What this is expected to do TODAY
//
// Reach OpenXR and stop. Every xr* entry point except xrGetInstanceProcAddr is
// a named abort (kl_openxr.h), so a healthy run of this main ends with
//
//   [klepton] fatal: guest called unimplemented OpenXR entry point 'xrCreateInstance'
//
// and the surface report under it. That is the milestone, not a failure: it
// says the loader, the libc surface, the JNI seam and the lifecycle all work,
// and that the remaining work is the OpenXR runtime itself. KL_PERMISSIVE=1
// turns each abort into an XR_SUCCESS so one run collects the whole surface —
// scouting only, since the guest then proceeds on answers we invented.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_jni.h"
#include "../runtime/kl_openxr.h"
#include "../runtime/kl_fault.h"

static const char *LIBDIR = "quakequest/lib/arm64-v8a";

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// Quake's data lives in a gamedir called id1/, and the engine looks for it under
// its files directory — NOT in the APK. The APK ships assets/pak0.pak (id's
// freely redistributable shareware episode) and on a headset the app copies it
// out on first run; without that step DarkPlaces boots all the way, finds
// nothing, and exits:
//
//   WARNING: base gamedir id1/ not found!
//   couldn't exec quake.rc
//
// This is the same shape as visionos/stage_assets.sh for Beat Saber, and it is
// idempotent for the same reason: 18.7 MB is cheap once and pointless every run.
static int stage_gamedata(const char *root) {
    char files[1200], id1[1300], dst[1400], src[1300];
    snprintf(files, sizeof files, "%s/files", root);
    snprintf(id1,   sizeof id1,   "%s/id1", files);
    snprintf(dst,   sizeof dst,   "%s/pak0.pak", id1);
    snprintf(src,   sizeof src,   "%s/assets/pak0.pak", root);
    mkdir(files, 0755);
    mkdir(id1, 0755);

    struct stat sd, ss;
    if (stat(src, &ss) != 0) {
        fprintf(stderr, "  [qq] %s missing — the guest will find no gamedir\n", src);
        return -1;
    }
    if (stat(dst, &sd) == 0 && sd.st_size == ss.st_size) return 0;   // already staged

    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return -1; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    fprintf(stderr, "  [qq] staged %s -> %s (%lld bytes)\n", src, dst,
            (long long)ss.st_size);
    return 0;
}

// The guest tree root, i.e. LIBDIR with its "/lib/<abi>" tail removed. assets/
// hangs off it, and so does everything else the APK unpacked to.
static void guest_root(char *out, size_t n) {
    snprintf(out, n, "%s", LIBDIR);
    char *lib = strstr(out, "/lib/");
    if (lib) *lib = '\0';
}

// assets/commandline.txt, trailing whitespace stripped. Never returns empty —
// see the header: an empty command line is a guest-side NULL deref, so falling
// back to the shipped default is safer than passing nothing through.
static const char *read_cmdline(void) {
    const char *env = getenv("KL_QQ_CMDLINE");
    if (env && *env) return env;
    static char buf[512];
    char root[1024], path[1200];
    guest_root(root, sizeof root);
    snprintf(path, sizeof path, "%s/assets/commandline.txt", root);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[got] = '\0';
        while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r' ||
                       buf[got - 1] == ' '  || buf[got - 1] == '\t'))
            buf[--got] = '\0';
        if (got) return buf;
    }
    fprintf(stderr, "  [qq] %s unreadable or empty — falling back to \"quake\"\n", path);
    return "quake";
}

typedef int  (*jni_onload_fn)(void *vm, void *reserved);
typedef long (*qq_onCreate_fn)(void *env, void *cls, void *activity, void *cmdline);
typedef void (*qq_onStart_fn)(void *env, void *cls, long h, void *obj);
typedef void (*qq_handle_fn)(void *env, void *cls, long h);
typedef void (*qq_surface_fn)(void *env, void *cls, long h, void *surface);

// Every lifecycle call is wrapped the way m_boot wraps its natives: the JVM
// would pop a native's local frame on return, and the host has to play that
// half itself (kl_jni.h).
#define QQ_CALL(expr) do { kl_jni_local_frame_push(); expr; kl_jni_local_frame_pop(); } while (0)

int main(int argc, char **argv) {
    if (argc > 1) LIBDIR = argv[1];

    // S0.1, and the first thing that must happen: guest code runs on THIS
    // thread, and bionic's -fstack-protector keeps its canary in TLS slot 5,
    // which we squat. Without this the prologue stores whatever Darwin left in
    // slot 5 and the epilogue compares against whatever is there by then —
    // which is how the first run of this main died, "stack smashing detected"
    // inside onCreate with no guest bug anywhere near it. m_slink and m_boot's
    // viewer path both call it for the same reason.
    kl_thread_init();

    // A bare SIGSEGV inside a 30 MB guest image names nothing. The reporter
    // turns it into an image+offset, which is the difference between "it
    // crashed" and a symbol to look up.
    kl_fault_install();

    // TBXR_InitialiseOpenXR reads OPENXR_HMD and strstr()s it for "meta" (and
    // "pico") to decide which of the two loaders the APK ships it is talking to.
    // On a headset the launcher sets it; here nothing does, so getenv returns
    // NULL and the strstr is a SIGSEGV inside the engine's own init — which is
    // exactly where this main died before this line existed. "meta" because
    // libopenxr_loader_meta.so is the soname we claim (kl_openxr.c).
    //
    // overwrite=0: an explicit OPENXR_HMD from the shell still wins, which is
    // the A/B for the Pico path.
    setenv("OPENXR_HMD", "meta", 0);

    kl_set_library_path(LIBDIR);
    kl_jni_set_native_lib_dir(LIBDIR);
    // The engine reads its game data (assets/pak0.pak, the shareware episode
    // the APK ships) through AAssetManager, so the asset root has to be known
    // before onCreate rather than at first use.
    {
        char root[1024], adir[1200];
        guest_root(root, sizeof root);
        snprintf(adir, sizeof adir, "%s/assets", root);
        kl_jni_set_assets_dir(adir);
        // The writable side, where the engine expects id1/ to already be.
        stage_gamedata(root);
        char fdir[1200];
        snprintf(fdir, sizeof fdir, "%s/files", root);
        kl_jni_set_files_dir(fdir);
    }
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);

    // The Activity the guest is handed, and the class it is checked against.
    // Beat Saber's is Unity's; this one is the app's own, named in the manifest.
    kl_jni_set_activity_class("com/drbeef/quakequest/GLES3JNIActivity");

    char path[1024];
    snprintf(path, sizeof path, "%s/libquakequest.so", LIBDIR);

    printf("=== libquakequest.so entry ===\n");
    // Recursive: the engine DT_NEEDEDs libvorbis.so / libogg.so / libvorbis-jni.so,
    // and those are the eight imports the shim surface deliberately does NOT
    // answer — they belong to the guest's own libraries, not to us.
    kl_image *img = kl_load_auto(path);
    if (!img) return fail(kl_error());
    kl_register_image("libquakequest.so", img);
    kl_run_init(img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
    if (!onload) return fail("libquakequest.so exports no JNI_OnLoad");

    int version;
    QQ_CALL(version = onload(kl_jni_vm(), NULL));
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    // Beat Saber asks for 1.6; this engine asks for **1.4**, and that is not a
    // lesser answer — it is the version of the JNIEnv the guest wants to be
    // handed. Ours is 1.6-shaped, a superset, so 1.4 is satisfiable and
    // demanding 1.6 (which m_boot does, correctly, for its own target) would
    // reject a perfectly healthy guest. Only a value we do not recognise means
    // JNI_OnLoad actually failed — bionic returns JNI_ERR (-1) for that.
    switch (version) {
    case 0x00010001: case 0x00010002: case 0x00010004: case 0x00010006: break;
    default: return fail("JNI_OnLoad did not return a JNI version we recognise");
    }

    // ---- the lifecycle exports ----
#define QQ_SYM(n) kl_sym(img, "Java_com_drbeef_quakequest_GLES3JNILib_" #n)
    qq_onCreate_fn onCreate       = (qq_onCreate_fn)QQ_SYM(onCreate);
    qq_onStart_fn  onStart        = (qq_onStart_fn) QQ_SYM(onStart);
    qq_handle_fn   onResume       = (qq_handle_fn)  QQ_SYM(onResume);
    qq_surface_fn  onSurfCreated  = (qq_surface_fn) QQ_SYM(onSurfaceCreated);
    qq_surface_fn  onSurfChanged  = (qq_surface_fn) QQ_SYM(onSurfaceChanged);
#undef QQ_SYM
    if (!onCreate || !onStart || !onResume || !onSurfCreated || !onSurfChanged)
        return fail("GLES3JNILib lifecycle exports missing");
    printf("  lifecycle resolved: onCreate=%p onStart=%p onResume=%p\n",
           (void *)onCreate, (void *)onStart, (void *)onResume);

    printf("\n=== Q1 EXIT CRITERION MET: guest loaded, JNI_OnLoad ran ===\n");
    fflush(NULL);

    // ---- onCreate: the engine handle everything else is keyed on ----
    printf("\n=== lifecycle: onCreate(Activity, commandLine) ===\n");
    fflush(NULL);
    void *activity = kl_jni_activity();
    const char *cmdline_utf8 = read_cmdline();
    printf("  commandLine = \"%s\"\n", cmdline_utf8);
    void *cmdline = kl_jni_new_string(cmdline_utf8);
    long handle = 0;
    QQ_CALL(handle = onCreate(kl_jni_env(), NULL, activity, cmdline));
    printf("  onCreate returned handle 0x%lx\n", handle);
    if (!handle) return fail("onCreate returned a NULL engine handle");

    // Order matters, and not the order the callbacks are NAMED in.
    //
    // On a device the surface arrives asynchronously through SurfaceHolder and
    // in practice lands BEFORE onResume. Driving them in declaration order
    // (start, resume, then surface) deadlocks: the engine's render thread waits
    // for a window that this thread has not delivered yet, and onResume waits on
    // the render thread. The symptom is a main that never returns from onResume
    // and exactly one xrWaitFrame call, with no surface callback ever made.
    printf("\n=== lifecycle: onStart ===\n");
    fflush(NULL);
    QQ_CALL(onStart(kl_jni_env(), NULL, handle, activity));

    printf("\n=== lifecycle: onSurfaceCreated / onSurfaceChanged ===\n");
    fflush(NULL);
    void *surface = kl_jni_new_object("android/view/Surface");
    QQ_CALL(onSurfCreated(kl_jni_env(), NULL, handle, surface));
    QQ_CALL(onSurfChanged(kl_jni_env(), NULL, handle, surface));

    printf("\n=== lifecycle: onResume ===\n");
    fflush(NULL);
    QQ_CALL(onResume(kl_jni_env(), NULL, handle));
    printf("  lifecycle delivered\n");

    // ---- the surface: where the engine starts rendering, and so where it
    // reaches OpenXR ----
    // ---- let the engine actually run ----
    //
    // onCreate spawns the render thread and the lifecycle calls only *start* it;
    // returning from main here kills that thread mid-init, which is why the first
    // runs reached FOCUSED and then stopped with no frame-loop calls at all. The
    // XR work happens on that thread, so this main's last job is to stay out of
    // its way for a while.
    //
    // A sleep rather than a pump: nothing here drives the guest: it has its own
    // loop. KL_QQ_SECONDS bounds it so a hang is a bounded hang.
    int secs = 5;
    if (getenv("KL_QQ_SECONDS")) secs = atoi(getenv("KL_QQ_SECONDS"));
    printf("\n=== running the guest for %d s (KL_QQ_SECONDS) ===\n", secs);
    fflush(NULL);
    for (int i = 0; i < secs * 10 && secs > 0; i++) usleep(100000);

    printf("\n=== lifecycle completed without an OpenXR abort ===\n");
    kl_openxr_report(stdout);
    kl_jni_report(stdout);
    fflush(NULL);
    return 0;
}
