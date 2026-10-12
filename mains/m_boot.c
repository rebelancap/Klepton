// The host driver: enter the guest through its real entry point, on macOS.
//
// The guest's own sequences are runtime/guest/kl_driver.c, shared verbatim with
// the visionOS app so that one guest cannot be described two ways. What is here
// is everything a COMMAND LINE adds and an app bundle has no use for: the DRM
// policy self-test, the re-exec'd recon child (an unimplemented JNI slot aborts
// by design, so the run that measures the surface must be disposable), the SDL
// viewer, the host-only instruments (sampler, managed probe, metadata dump), the
// Metal stand-in for Compositor Services, and how far to go — KL_LIFECYCLE,
// KL_FRAMES, KL_SLINK_MAIN, KL_GAP_ONLY.
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/gfx/kl_glfb.h"
#include "../runtime/kl_jni.h"
#include "../runtime/gfx/kl_egl.h"
#include "../runtime/media/kl_opensl.h"
#include "../runtime/media/kl_mediandk.h"
#include "../runtime/xr/kl_openxr.h"
#include "../runtime/xr/kl_ovrp.h"
#include "../runtime/xr/kl_ovrplat.h"
#include "../runtime/gfx/kl_view.h"
#include "../runtime/gfx/kl_vulkan.h"
#include "../runtime/diag/kl_sample.h"
#include "../runtime/diag/kl_mprobe.h"
#include "../runtime/diag/kl_metadump.h"
#include "../runtime/kl_fault.h"
#include "../runtime/kl_target.h"
#include "../runtime/guest/kl_driver.h"
#include "../runtime/guest/kl_slink.h"
#include "../runtime/guest/kl_sdl.h"
#include "../runtime/gfx/kl_reproject.h"
#include "../tests/t_mtl_provider.h"

// Which guest, and where its libraries are. Both come from the target table
// (runtime/kl_target.h) rather than from four literals here and in kl_jni.c's
// defaults — the APK, the asset tree and the userdata directory have to agree
// with the libraries, and when they did not it was silent: SUPERHOT's libraries
// opening Beat Saber's APK is a guest reading someone else's game data.
//
//   ./build/m_boot                      the default target (beatsaber)
//   ./build/m_boot superhot             ...by name
//   ./build/m_boot superhot/lib/arm64-v8a   ...the same, by path
//   ./build/m_boot steamlink-vr         ...including Steam Link, whose front
//                                       door is KL_SLINK_VR / KL_SLINK_SHELL
//
// The path form is what every documented run command and every Makefile gate
// passes, and it resolves to the same target as the name.
static const kl_target *TARGET;
static const char *LIBDIR;
static kl_slink_door DOOR;          // read only by a Steam Link target
#define DOOR_IS_VR (TARGET->kind == KL_GUEST_STEAMLINK && DOOR == KL_SLINK_VR)

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// The fatal-signal reporter moved to runtime/kl_fault.c when the visionOS app
// needed the same thing inside a bundle — it is a diagnostic
// that ships, not a harness detail. The sampling profiler stays host-only, so
// t_boot registers it rather than kl_fault.c referencing RUNTIME_DIAG.
static void install_fault_reporter(void) {
    kl_fault_add_reporter(kl_sample_stop_report);
    // A crash report that does not depend on stderr surviving. On the host that
    // is usually the terminal and fine, but a viewer run redirects, a guest can
    // take the fd, and stdio can hold the last writes behind a lock the dying
    // thread never releases — and a signal death is exactly when the output
    // matters most. KL_CRASH_LOG moves it; the default sits beside the run.
    kl_fault_set_crash_path(kl_env_str("KL_CRASH_LOG", "/tmp/klepton-crash.log"));
    kl_fault_install();
    // After kl_fault_install, deliberately: the watch chains to whatever it
    // finds on SIGSEGV/SIGBUS, so a real crash still reaches the reporter.
    kl_fault_add_reporter(kl_metadump_watch_report);
    kl_metadump_watch_install();
}

// The entitlement refusal in kl_ovrplat.c is a policy guard, and Beat Saber does
// not currently reach it — the platform fails to initialise first, so the guard
// has never fired in a real run. A safety mechanism that has never been exercised
// is an assumption, not a mechanism, so it is checked here directly.
//
// Two properties matter: that an ownership query does not resolve to something
// callable-and-harmless, and that calling it dies rather than returning a value.
// The call is made in a child because passing the test means aborting.
// The resolve happens in the child too, not just the call. kl_ovrplat_sym records
// what was looked up, and the surface report separates entitlement lookups out
// specifically so a real one is visible — probing from the parent would leave our
// own test sitting in that list, indistinguishable from the guest asking.
// What does calling `name` actually do? Run it in a child, because for most of
// this surface the PASSING outcome is death. The value matters as well as the
// refusal now — the line below distinguishes the application's own entitlement
// (answered, and answered yes) from DLC (refused), and "it returned something"
// cannot tell those apart.
enum { KLDRM_UNRESOLVED = -1, KLDRM_ABORTED, KLDRM_ZERO, KLDRM_NONZERO };

static int drm_call(const char *name) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        // The refusal prints a paragraph and a surface report; not wanted here.
        freopen("/dev/null", "w", stderr);
        void *fn = kl_ovrplat_sym(name);
        if (!fn) _exit(2);              // resolved to nothing at all
        uint64_t v = ((uint64_t (*)(void))fn)();
        _exit(v ? 4 : 3);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) return KLDRM_ABORTED;
    if (!WIFEXITED(st)) return KLDRM_UNRESOLVED;
    switch (WEXITSTATUS(st)) {
    case 3: return KLDRM_ZERO;
    case 4: return KLDRM_NONZERO;
    default: return KLDRM_UNRESOLVED;
    }
}

// ...and the same thing for the half of the family that takes an ASSET NAME.
// Not a convenience: `ovr_AssetFile_StatusByName` and `_DownloadByName` read
// their first argument as a string, and calling them through the no-argument
// cast above hands them whatever x0 happened to hold — which segfaults the
// child and reads back as UNRESOLVED, i.e. the guard passing or failing on the
// register file rather than on the policy.
static int drm_call_name(const char *name, const char *asset) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        freopen("/dev/null", "w", stderr);
        void *fn = kl_ovrplat_sym(name);
        if (!fn) _exit(2);
        uint64_t v = ((uint64_t (*)(const char *))fn)(asset);
        _exit(v ? 4 : 3);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) return KLDRM_ABORTED;
    if (!WIFEXITED(st)) return KLDRM_UNRESOLVED;
    switch (WEXITSTATUS(st)) {
    case 3: return KLDRM_ZERO;
    case 4: return KLDRM_NONZERO;
    default: return KLDRM_UNRESOLVED;
    }
}

// The line this asserts, in both directions. It moved once, deliberately, and
// the move is the reason the value is now checked rather than just the refusal:
//
//   The APPLICATION's own entitlement is answered, and answered yes. Klepton
//   runs an APK unpacked from a device the user owns, so ownership of the
//   application itself is the one thing in this family we can actually assert
//   — and it is asserted about a title the user already bought. It is not a
//   fabricated answer in the sense the refusals below are about; it is the
//   true state of affairs for the only way this project is used.
//
//   DLC is a different question and stays refused, because it is one we
//   genuinely cannot answer: paid content here is out-of-band asset data plus
//   a licence held by a platform that is absent, and neither half is on this
//   host to check. Claiming ownership of content we cannot see IS the
//   circumvention, so it keeps aborting by name.
//
// The two are separable because the API separates them — the viewer-entitled
// call is about the app, the AssetFile and IAP families are about content.
// The Application SpaceWarp guard.
//
// Launch #8 rendered exactly ONE frame and then reprojected it for forty
// seconds while the guest's simulation, audio and input ran on. The cause was
// one refused entry point: `ovrp_GetLayerTextureSpaceWarp`, which the Unity
// Oculus display provider calls once per eye-swapchain stage when the title was
// built with Application SpaceWarp enabled — and whose failure it treats as a
// failure to create the EYE textures, tearing the whole display provider down
// (OculusDisplayProvider::CreateMotionVectorTexture returns 1 on any non-zero
// result; CreateEyeTextureStage propagates it; CreateEyeTextures then destroys
// the layer). Nothing in the log named the call: what it showed was a render
// thread that could not begin a render pass, four hundred frames later.
//
// So the two halves are asserted here, because neither is visible in a boot
// log until it is far too late:
//
//   a) a layer that ASKS for SpaceWarp storage gets a motion-vector SIZE in its
//      desc — the provider reads it straight back out as the width and height
//      of the render texture it wraps, so a zero there is the same failure one
//      step later; and
//   b) asking that layer for the textures is no longer the blanket refusal.
//      On this host there is neither a Vulkan device nor a GL context, so the
//      call cannot actually hand storage over — it answers NOT_INITIALIZED, and
//      what is asserted is that it is not -1000, the refusal that was the bug.
//
// ...and the other direction, which is what keeps (a) from being a widening:
// a layer set up WITHOUT the flag must still be told there is no such storage.
#define KLBOOT_OVRP_FAILURE          (-1000)
#define KLBOOT_OVRP_UNSUPPORTED      (-1004)
#define KLBOOT_LAYERFLAG_SPACEWARP   0x300      // allocation | dedicated depth

// The eye-layer desc, by offset. kl_ovrp.c owns the struct and keeps it static;
// only four fields are read here and each one is pinned by a _Static_assert
// there, so an offset table is honest about how much this test knows.
#define KLBOOT_DESC_TEXSIZE_W  0x08
#define KLBOOT_DESC_TEXSIZE_H  0x0c
#define KLBOOT_DESC_MVSIZE_W   0x74
#define KLBOOT_DESC_MVSIZE_H   0x78

static int spacewarp_desc_scaled(unsigned char *desc, int flags, float scale) {
    uint64_t (*calc)(int, int, int, int, int, int, int, int, void *, float, float) =
        kl_ovrp_sym("ovrp_CalculateEyeLayerDesc3");
    if (!calc) return fail("SpaceWarp guard: ovrp_CalculateEyeLayerDesc3 resolved "
                           "to nothing");
    memset(desc, 0, 0x80);
    // (layout=Array, mips=1, samples=1, colour=0 sRGB, depth=10 D24_S8,
    //  motion vectors=2 RGBA16F, motion-vector depth=7, flags, desc, scale, scale)
    if (calc(3, 1, 1, 0, 10, 2, 7, flags, desc, scale, 1.0f) != 0)
        return fail("SpaceWarp guard: ovrp_CalculateEyeLayerDesc3 refused a desc");
    return 0;
}

static int spacewarp_desc(unsigned char *desc, int flags) {
    return spacewarp_desc_scaled(desc, flags, 1.0f);
}

// The eye-size ceiling, asserted where the guest ALLOCATES from.
//
// KL_OVRP_EYE_MAX had one caller for the whole project — the advertisement —
// and Unity does not allocate from the advertisement. It multiplies it by
// `XRSettings.eyeTextureResolutionScale` and allocates from THAT, and nothing
// bounded the product. Paint the Town Red VR ships 1.5: a 4032x3234
// advertisement became a 7584x4851 eye layer, 2.25x the pixels in our images
// and in every render target Unity sizes off them, and the process went from
// 620 MiB to 7170 MiB in two seconds and was jetsammed three seconds into the
// first picture the port had ever drawn. SUPERHOT died of the same shape at 2x
// on 2026-08-12.
//
// Both directions, so this cannot regress into either a ceiling that does not
// hold or a ceiling that has eaten the scale-down case a guest legitimately
// asks for.
static int check_eye_cap_guard(void) {
    unsigned char desc[0x80];
    int base_w, base_h, big_w, big_h, small_w, small_h;
    const char *env = getenv("KL_OVRP_EYE_MAX");
    int cap = env ? atoi(env) : 3072;
    if (cap <= 0) { printf("  eye cap: KL_OVRP_EYE_MAX is off, nothing to assert\n");
                    return 0; }

    if (spacewarp_desc_scaled(desc, 0, 1.0f)) return 1;
    memcpy(&base_w, desc + KLBOOT_DESC_TEXSIZE_W, 4);
    memcpy(&base_h, desc + KLBOOT_DESC_TEXSIZE_H, 4);
    if (base_w > cap || base_h > cap)
        return fail("eye cap guard: the unscaled desc is already over "
                    "KL_OVRP_EYE_MAX");

    // (a) a render scale of 4 does not become four times the pixels.
    if (spacewarp_desc_scaled(desc, 0, 4.0f)) return 1;
    memcpy(&big_w, desc + KLBOOT_DESC_TEXSIZE_W, 4);
    memcpy(&big_h, desc + KLBOOT_DESC_TEXSIZE_H, 4);
    if (big_w > cap || big_h > cap)
        return fail("eye cap guard: a guest render scale walked the eye desc "
                    "past KL_OVRP_EYE_MAX — that is the allocation jetsam kills "
                    "the process for, and the cap is supposed to hold HERE");
    // Deliberately NOT "no bigger than the unscaled desc": under the cap, a
    // render scale is a legitimate supersample and 2290x2400 asking for 4x
    // SHOULD come back at the 3072 ceiling. The ceiling is the invariant; the
    // ratio is the guest's business right up to it.

    // (b) ...and a guest that asks for LESS still gets less. A cap that clamped
    //     both directions would be a resolution cut wearing a ceiling's name.
    //     KL_OVRP_EYE_SCALE_MIN is the deliberate, opt-in exception (a target
    //     row may set it — pttr does): case (b) runs with the floor suspended,
    //     and with a floor set the same 0.5 must come back at the floor.
    const char *floor_env = getenv("KL_OVRP_EYE_SCALE_MIN");
    char floor_saved[32] = {0};
    if (floor_env) snprintf(floor_saved, sizeof floor_saved, "%s", floor_env);
    unsetenv("KL_OVRP_EYE_SCALE_MIN");
    int rc_small = spacewarp_desc_scaled(desc, 0, 0.5f);
    if (floor_env) setenv("KL_OVRP_EYE_SCALE_MIN", floor_saved, 1);
    if (rc_small) return 1;
    memcpy(&small_w, desc + KLBOOT_DESC_TEXSIZE_W, 4);
    memcpy(&small_h, desc + KLBOOT_DESC_TEXSIZE_H, 4);
    if (small_w >= base_w || small_h >= base_h)
        return fail("eye cap guard: a render scale below 1 did not shrink the "
                    "eye desc — the cap must be a ceiling, not a fixed size");
    if (floor_env && atof(floor_saved) >= 1.0f) {
        int fw, fh;
        if (spacewarp_desc_scaled(desc, 0, 0.5f)) return 1;
        memcpy(&fw, desc + KLBOOT_DESC_TEXSIZE_W, 4);
        memcpy(&fh, desc + KLBOOT_DESC_TEXSIZE_H, 4);
        if (fw < base_w || fh < base_h)
            return fail("eye cap guard: KL_OVRP_EYE_SCALE_MIN >= 1 is set but a "
                        "render scale of 0.5 still shrank the eye desc");
        printf("  eye scale floor: KL_OVRP_EYE_SCALE_MIN=%s holds scale 0.5 at "
               "%dx%d (unscaled %dx%d)\n", floor_saved, fw, fh, base_w, base_h);
    }

    printf("  eye cap: KL_OVRP_EYE_MAX=%d holds the DESC (scale 1 -> %dx%d, "
           "scale 4 -> %dx%d, scale 0.5 -> %dx%d)\n",
           cap, base_w, base_h, big_w, big_h, small_w, small_h);
    return 0;
}

// The two rungs of the eye ladder, asserted as ARITHMETIC rather than as a
// comment.
//
// kl_app.c bakes 4032 for every target and 3072 for pttr, and STATUS quotes
// what each one costs in pixels (13.0 Mpx and 7.6 Mpx per eye, against the
// ~4.5 Mpx a Quest hands this title). Those numbers are load-bearing — the
// resolution cut of round 14 was taken against them — so they are checked here
// instead of being three places that can drift apart. The input is the size
// launch #13's log recorded the guest asking for: 6048x4851, its own 1.5x
// render scale applied to a 4032x3234 advertisement.
static int check_eye_rung_guard(void) {
    const char *saved = getenv("KL_OVRP_EYE_MAX");
    char keep[32];
    if (saved) snprintf(keep, sizeof keep, "%s", saved);
    // KL_OVRP_EYE_ALIGN is a separate, opt-in rounding AFTER the cap (a row
    // may bake it — re4 and fnaf do, =4), and these rungs assert the cap's own
    // arithmetic, so the guard runs with it suspended exactly as the eye cap
    // guard suspends KL_OVRP_EYE_SCALE_MIN. Without this, `make boot` on an
    // ALIGN=4 row failed here on 3234 -> 3232 (2026-10-11).
    const char *align = getenv("KL_OVRP_EYE_ALIGN");
    char align_keep[32];
    if (align) snprintf(align_keep, sizeof align_keep, "%s", align);
    unsetenv("KL_OVRP_EYE_ALIGN");
    struct { const char *cap; int w, h; const char *who; } rung[] = {
        { "4032", 4032, 3234, "every other target" },
        { "3072", 3072, 2464, "pttr's rounds 14-15 tourniquet" },
        { "3584", 3584, 2875, "the pttr fallback rung, if round 16 misses" },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof rung / sizeof *rung; i++) {
        setenv("KL_OVRP_EYE_MAX", rung[i].cap, 1);
        int w = 6048, h = 4851;                 // launch #13's own request
        kl_ovrp_cap_eye_size(&w, &h);
        if (w != rung[i].w || h != rung[i].h) {
            printf("  eye rungs: KL_OVRP_EYE_MAX=%s turned 6048x4851 into "
                   "%dx%d, not the %dx%d %s and STATUS both claim\n",
                   rung[i].cap, w, h, rung[i].w, rung[i].h, "kl_app.c");
            bad = 1;
        }
        // Under the cap nothing moves: a ceiling that also shrinks a small
        // request is a resolution cut wearing a ceiling's name, and round 14's
        // cut has to stay the one deliberate place that happens.
        int sw = 1024, sh = 822;
        kl_ovrp_cap_eye_size(&sw, &sh);
        if (sw != 1024 || sh != 822) {
            printf("  eye rungs: KL_OVRP_EYE_MAX=%s moved a 1024x822 request "
                   "that was already under it (-> %dx%d)\n", rung[i].cap, sw, sh);
            bad = 1;
        }
        (void)rung[i].who;
    }
    if (saved) setenv("KL_OVRP_EYE_MAX", keep, 1); else unsetenv("KL_OVRP_EYE_MAX");
    if (align) setenv("KL_OVRP_EYE_ALIGN", align_keep, 1);
    if (bad) return fail("eye rung guard: a baked cap does not produce the size "
                         "its own comment and STATUS quote");
    printf("  eye rungs: 6048x4851 caps to 4032x3234 at 4032 (13.0 Mpx), to "
           "3584x2875 at 3584 (10.3 Mpx) and to 3072x2464 at 3072 (7.6 Mpx), "
           "aspect held, and a request already under the cap is untouched\n");
    return 0;
}

static int check_spacewarp_guard(void) {
    void *sw = kl_ovrp_sym("ovrp_GetLayerTextureSpaceWarp");
    if (!sw) return fail("SpaceWarp guard: ovrp_GetLayerTextureSpaceWarp resolved "
                          "to nothing");
    uint64_t (*setup)(void *, const void *, int *) = kl_ovrp_sym("ovrp_SetupLayer");
    uint64_t (*destroy)(int) = kl_ovrp_sym("ovrp_DestroyLayer");
    if (!setup || !destroy)
        return fail("SpaceWarp guard: the layer setup pair resolved to nothing");
    int64_t (*get)(int, int, int, uint64_t *, int *, uint64_t *, int *) = sw;

    unsigned char desc[0x80];
    int w, h, mw, mh, id = 0;

    // (a) with the flag: a motion-vector size, and it is derived from the eye
    //     texture's rather than invented — a quarter per axis.
    if (spacewarp_desc(desc, KLBOOT_LAYERFLAG_SPACEWARP)) return 1;
    memcpy(&w,  desc + KLBOOT_DESC_TEXSIZE_W, 4);
    memcpy(&h,  desc + KLBOOT_DESC_TEXSIZE_H, 4);
    memcpy(&mw, desc + KLBOOT_DESC_MVSIZE_W,  4);
    memcpy(&mh, desc + KLBOOT_DESC_MVSIZE_H,  4);
    if (mw <= 0 || mh <= 0)
        return fail("SpaceWarp guard: a layer that asked for motion-vector "
                    "storage was described with a 0x0 motion-vector texture — "
                    "the provider wraps a render texture around exactly that");
    if (mw != (w / 4 > 0 ? w / 4 : 1) || mh != (h / 4 > 0 ? h / 4 : 1))
        return fail("SpaceWarp guard: the motion-vector size is not the eye "
                    "texture's quarter — the two must be derived together");

    // (b) ...and asking for the textures is not the old blanket refusal.
    if (setup(NULL, desc, &id) != 0 || id == 0)
        return fail("SpaceWarp guard: ovrp_SetupLayer refused the SpaceWarp layer");
    {
        uint64_t mv = 0, dep = 0;
        int msz[2] = { 0, 0 }, dsz[2] = { 0, 0 };
        int64_t r = get(id, 0, 0, &mv, msz, &dep, dsz);
        destroy(id);
        if (r == KLBOOT_OVRP_FAILURE)
            return fail("SpaceWarp guard: ovrp_GetLayerTextureSpaceWarp still "
                        "answers the blanket -1000 refusal — that is the answer "
                        "that tore the display provider down");
        if (r == KLBOOT_OVRP_UNSUPPORTED)
            return fail("SpaceWarp guard: the SpaceWarp layer was told its own "
                        "storage is unsupported");
    }

    // (c) the other direction: no flag, no storage.
    if (spacewarp_desc(desc, 0)) return 1;
    memcpy(&mw, desc + KLBOOT_DESC_MVSIZE_W, 4);
    memcpy(&mh, desc + KLBOOT_DESC_MVSIZE_H, 4);
    if (mw != 0 || mh != 0)
        return fail("SpaceWarp guard: a layer that did NOT ask for SpaceWarp was "
                    "given a motion-vector size — a real plugin leaves it zero");
    id = 0;
    if (setup(NULL, desc, &id) != 0 || id == 0)
        return fail("SpaceWarp guard: ovrp_SetupLayer refused the plain eye layer");
    {
        uint64_t mv = 0, dep = 0;
        int msz[2] = { 0, 0 }, dsz[2] = { 0, 0 };
        int64_t r = get(id, 0, 0, &mv, msz, &dep, dsz);
        destroy(id);
        if (r != KLBOOT_OVRP_UNSUPPORTED)
            return fail("SpaceWarp guard: a layer with no SpaceWarp flag was not "
                        "told its motion-vector storage is unsupported");
    }
    printf("  SpaceWarp: motion-vector storage described (%dx%d for a %dx%d eye) "
           "and served; a layer without the flag is refused\n", mw ? mw : w / 4,
           mh ? mh : h / 4, w, h);
    return 0;
}

// Mixed Reality Capture, and the GPU memory budget. Both landed in round 11 and
// both are the kind of change that regresses silently — one is a table entry,
// the other a number nobody reads until a guest is already dying of it.
//
// The MRC half asserts the SPLIT, not the family. ovrp_Media_* is mixed: the
// setters are recorded state and must be ACCEPTED (a refusal there desyncs the
// guest from the ovrp_Media_Initialize success we already report), while the
// gates must be REFUSED so OVRManager never enters the encode path — which on
// this host would be a second camera render at eye resolution on a process
// already losing a race with jetsam. A blanket prefix rule would get one of
// those two wrong, so both directions are asserted.
static int check_mrc_guard(void) {
    // (a) the name that aborted launch #10b. Must resolve to something real and
    //     must answer ovrpSuccess — the managed wrapper is `cmp w0,#0 / cset
    //     w0,eq`, so only 0 reads as success.
    void *fn = kl_ovrp_sym("ovrp_Media_SetAvailableQueueIndexVulkan");
    if (!fn) return fail("MRC guard: ovrp_Media_SetAvailableQueueIndexVulkan "
                         "resolved to nothing");
    long long r = ((long long (*)(unsigned))fn)(0u);
    if (r != 0)
        return fail("MRC guard: the Vulkan queue-index push was not accepted — "
                    "it is a recorded-state setter and its caller treats "
                    "anything but ovrpSuccess as failure");

    // (b) ...and the gate, which must say NO. There are two honest spellings of
    //     no here and this runtime uses both, so the guard asserts the ANSWER
    //     rather than the spelling:
    //
    //       - ovrpFailure (-1000). The managed IsMrcEnabled wrapper is
    //         `cmp w0,#0 / cset` and then reads the out-param only on success,
    //         so a refusal reads as false. This is what the ovrp_Media_* refusal
    //         list (g_ovrp_mrc_no) produces.
    //       - ovrpSuccess with *out = 0, which the real bodies in the Media
    //         facade produce. Every Get*/Is* in that family is ovrpResult plus
    //         an out-pointer, and a body that writes the pointer is the more
    //         literal answer.
    //
    //     What must never happen is "available": success with a non-zero out.
    //     That is what starts the guest compositing spectator frames — a second
    //     camera render at eye resolution on a process already losing a race
    //     with jetsam.
    void *gate = kl_ovrp_sym("ovrp_Media_IsMrcEnabled");
    if (!gate) return fail("MRC guard: ovrp_Media_IsMrcEnabled resolved to nothing");
    unsigned out = 1;
    long long g = ((long long (*)(unsigned *))gate)(&out);
    if (g >= 0 && out != 0)
        return fail("MRC guard: ovrp_Media_IsMrcEnabled reported MRC AVAILABLE — "
                    "a host with no spectator camera must answer no, either by "
                    "refusing or by succeeding with *out = 0, or the guest "
                    "starts compositing frames for it");

    printf("  MRC: the queue-index push is accepted and the MRC gate answers no "
           "(%s)\n", g < 0 ? "refused" : "ovrpSuccess with *out = 0");
    return 0;
}

// The GPU half of the memory budget. Asserted in both directions for the same
// reason the eye cap is: a budget that does not hold is not a budget, and a
// budget that cannot be raised is a bug of its own.
static int check_vk_budget_guard(void) {
    unsigned long long b = kl_vulkan_heap_budget_bytes();
    if (!b) return fail("vk budget guard: the GPU heap budget is zero, which "
                        "would clamp every heap to nothing");
    unsigned long long cpu = kl_mem_budget_bytes();
    if (cpu && b != cpu && !getenv("KL_VK_HEAP_MB"))
        return fail("vk budget guard: with KL_VK_HEAP_MB unset the GPU budget "
                    "must BE the CPU budget — on a unified-memory device they "
                    "are the same pages, and two numbers is how the guest was "
                    "told it had 6 GB of RAM and 16 GB of VRAM");
    // ...and the override still wins, which is what makes a per-target rung
    // possible without a rebuild.
    const char *had = getenv("KL_VK_HEAP_MB");
    setenv("KL_VK_HEAP_MB", "2048", 1);
    unsigned long long forced = kl_vulkan_heap_budget_bytes();
    if (had) setenv("KL_VK_HEAP_MB", had, 1); else unsetenv("KL_VK_HEAP_MB");
    if (forced != (2048ull << 20))
        return fail("vk budget guard: KL_VK_HEAP_MB did not override the budget");

    printf("  vk budget: the GPU heap is capped at %llu MiB, the same number the "
           "CPU side reports (KL_VK_HEAP_MB overrides)\n", b >> 20);
    return 0;
}

// The memoryless seam, asserted as a table for the same reason the format seam
// is: MoltenVK's rule is one comparison and the whole 1.7 GiB turns on getting
// it right in both directions.
static int check_vk_lazy_guard(void) {
    const unsigned TR = KL_VK_USAGE_TRANSIENT_ATTACHMENT;
    const unsigned IN = KL_VK_USAGE_INPUT_ATTACHMENT;
    const unsigned DS = KL_VK_USAGE_DEPTH_STENCIL_ATTACHMENT;
    const unsigned CO = KL_VK_USAGE_COLOR_ATTACHMENT;

    // 1. The case that costs the gigabyte: Unity's own URP depth attachment,
    //    exactly as launch #16 logged it. The input bit comes off and NOTHING
    //    else does — a dropped depth-attachment bit is a render pass that
    //    cannot be built at all.
    if (kl_vulkan_lazy_transient_usage(DS | TR | IN) != (DS | TR))
        return fail("vk lazy guard: depth-att|transient|input-att must lose "
                    "ONLY the input bit — MoltenVK strips the memoryless memory "
                    "types from any image carrying transient and input together");
    if (kl_vulkan_lazy_transient_usage(CO | TR | IN) != (CO | TR))
        return fail("vk lazy guard: colour-att|transient|input-att must lose "
                    "only the input bit");

    // 2. An image nobody asked to be transient keeps every bit. Dropping the
    //    input bit off a real subpass input would build the MTLTexture without
    //    ShaderRead and break a read that genuinely happens.
    if (kl_vulkan_lazy_transient_usage(CO | IN) != (CO | IN))
        return fail("vk lazy guard: a NON-transient input attachment must be "
                    "left exactly alone");

    // 3. Somebody reads the contents outside the render pass, so they have to
    //    exist. Memoryless storage would hand them a blank texture, and that is
    //    a black picture rather than a log line.
    if (kl_vulkan_lazy_transient_usage(CO | TR | IN | KL_VK_USAGE_SAMPLED)
            != (CO | TR | IN | KL_VK_USAGE_SAMPLED))
        return fail("vk lazy guard: a SAMPLED transient attachment must keep "
                    "its input bit — its contents are read outside the pass");
    if (kl_vulkan_lazy_transient_usage(CO | TR | IN | KL_VK_USAGE_TRANSFER_SRC)
            != (CO | TR | IN | KL_VK_USAGE_TRANSFER_SRC))
        return fail("vk lazy guard: a transient attachment that is also a "
                    "transfer source must be left alone");

    // 4. Idempotent. An image already without the input bit must come back
    //    byte-identical, or the seam is rewriting things it did not decide.
    if (kl_vulkan_lazy_transient_usage(DS | TR) != (DS | TR))
        return fail("vk lazy guard: an image already without the input bit must "
                    "be returned unchanged");

    printf("  vk lazy: a transient attachment loses ONLY its INPUT_ATTACHMENT "
           "bit (which is the one MoltenVK reads as 'never memoryless'), and "
           "keeps it wherever the contents are read outside the render pass\n");
    return 0;
}

// The format seam. Asserted as a TABLE, on the host, with no MoltenVK needed —
// which is the whole reason kl_vulkan_format_substitute lives above the
// __has_include split. Four properties, and each of them is a way launch #11's
// crash could come back wearing a different hat.
static int check_vk_format_guard(void) {
    int d;

    // 1. The 24-bit families widen, and they widen WITHIN their own family.
    //    A substitution that dropped sRGB would compile, run, and wash every
    //    albedo texture out by a gamma.
    d = -1;
    if (kl_vulkan_format_substitute(23, &d) != 37 || d != 1)
        return fail("vk format guard: R8G8B8_UNORM must widen to R8G8B8A8_UNORM "
                    "and be flagged as a texel-size change");
    d = -1;
    if (kl_vulkan_format_substitute(29, &d) != 43)
        return fail("vk format guard: R8G8B8_SRGB must land on R8G8B8A8_SRGB — "
                    "substituting a linear format for an sRGB one is a gamma "
                    "bug that looks like a lighting bug");
    if (kl_vulkan_format_substitute(36, NULL) != 50)
        return fail("vk format guard: B8G8R8_SRGB must land on B8G8R8A8_SRGB");

    // 2. The depth substitutions are byte-SAFE, and must say so — a
    //    depth/stencil attachment is never a buffer copy's destination, so
    //    flagging it as a layout change would replace real uploads with clears
    //    for no reason.
    d = -1;
    if (kl_vulkan_format_substitute(129, &d) != 130 || d != 0)
        return fail("vk format guard: D24_UNORM_S8_UINT must land on "
                    "D32_SFLOAT_S8_UINT with no layout change");
    //    ...and S8_UINT, which is the one launch #12 actually caught being
    //    refused. Every member of the family must have a backing, because the
    //    capability seam now reports the whole family SUPPORTED and a format
    //    reported supported with nothing behind it is a worse crash than the
    //    refusal it replaced.
    for (int f = 124; f <= 130; f++) {
        int sub = kl_vulkan_format_substitute(f, &d);
        if (f == 126 || f == 130) {          // Metal has these outright
            if (sub) return fail("vk format guard: a depth format Metal has "
                                 "must not be substituted");
            continue;
        }
        if (!sub || d != 0)
            return fail("vk format guard: every depth/stencil format must have "
                        "a byte-safe substitute — the capability answers now "
                        "promise one");
    }
    if (kl_vulkan_format_substitute(127, NULL) != 130)
        return fail("vk format guard: S8_UINT must land on D32_SFLOAT_S8_UINT — "
                    "it is the refusal launch #12 recorded, and the format "
                    "Unity's table turned into NONE");

    // 3. A format Metal HAS must never be rewritten. This is the property that
    //    keeps the seam inert on every image that was already fine — the
    //    unmappable test is MoltenVK's, and this table only ever answers what
    //    to reach for after it has fired.
    if (kl_vulkan_format_substitute(37, NULL) != 0 ||
        kl_vulkan_format_substitute(43, NULL) != 0 ||
        kl_vulkan_format_substitute(126, NULL) != 0 ||
        kl_vulkan_format_substitute(130, NULL) != 0)
        return fail("vk format guard: a Metal-expressible format must have no "
                    "substitute at all");

    // 4. No chains. Every substitute must itself be a final answer, or a
    //    device that cannot map the replacement either would loop the guest
    //    through a second rewrite nobody wrote.
    for (int f = 0; f <= 184; f++) {
        int sub = kl_vulkan_format_substitute(f, NULL);
        if (!sub) continue;
        if (sub == f)
            return fail("vk format guard: a format substituted for itself");
        if (kl_vulkan_format_substitute(sub, NULL))
            return fail("vk format guard: a substitute has a substitute of its "
                        "own — the seam would rewrite twice");
    }

    // 5. The last resort, which is round 13's net. VK_FORMAT_UNDEFINED has no
    //    family to widen inside — it is what Unity's GraphicsFormat table
    //    produces when it has decided a format is unsupported — so the answer
    //    comes from what the image is FOR. A depth attachment must get a real
    //    depth format and must be flagged byte-safe; a colour target must get
    //    RGBA8 and must NOT be, since nothing can be uploaded into it; and an
    //    image that is neither must still answer 0, so the refusal survives
    //    where a substitute would be a guess.
    d = -1;
    if (kl_vulkan_format_last_resort(0, KL_VK_USAGE_DEPTH_STENCIL_ATTACHMENT
                                        | KL_VK_USAGE_TRANSIENT_ATTACHMENT
                                        | KL_VK_USAGE_INPUT_ATTACHMENT, &d) != 130
        || d != 0)
        return fail("vk format guard: an UNDEFINED depth attachment must last-"
                    "resort to D32_SFLOAT_S8_UINT, byte-safe — this is exactly "
                    "the image that ended launch #12");
    d = -1;
    if (kl_vulkan_format_last_resort(0, KL_VK_USAGE_COLOR_ATTACHMENT, &d) != 37
        || d != 1)
        return fail("vk format guard: an UNDEFINED colour attachment must last-"
                    "resort to R8G8B8A8_UNORM and be flagged as a texel-size "
                    "change");
    if (kl_vulkan_format_last_resort(0, KL_VK_USAGE_TRANSFER_DST, NULL) != 0)
        return fail("vk format guard: an UNDEFINED image that is neither an "
                    "attachment nor sampled must still be refused");
    //    ...and the last resort must never override a family answer, or a
    //    depth format with a real sibling would land on RGBA8.
    if (kl_vulkan_format_last_resort(129, KL_VK_USAGE_COLOR_ATTACHMENT, NULL) != 0 ||
        kl_vulkan_format_last_resort(23, KL_VK_USAGE_COLOR_ATTACHMENT, NULL) != 0)
        return fail("vk format guard: the last resort must answer 0 for any "
                    "format the family table already handles");

    if (strcmp(kl_vulkan_format_name(23), "R8G8B8_UNORM") ||
        strcmp(kl_vulkan_format_name(129), "D24_UNORM_S8_UINT"))
        return fail("vk format guard: the format names are off by one, so every "
                    "census line names the wrong thing");

    printf("  vk formats: the whole depth/stencil family is byte-safe-backed "
           "(S8_UINT -> D32_SFLOAT_S8_UINT), the unmappable colour families "
           "widen inside their own family, and an UNDEFINED attachment "
           "last-resorts by usage instead of being refused into a NULL deref\n");
    return 0;
}

static int check_drm_guard(void) {
    // 1. The application's own entitlement: answered, and non-zero. Checked for
    //    its VALUE, since a 0 here reads to the guest as "not entitled" and
    //    sends a legitimately-owned title down its licence-failure path.
    int r = drm_call("ovr_Entitlement_GetIsViewerEntitled");
    if (r == KLDRM_UNRESOLVED)
        return fail("entitlement guard: the name resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("entitlement guard ABORTED — the application's own entitlement "
                    "is the one we can assert, and refusing it stops a title the "
                    "user owns");
    if (r != KLDRM_NONZERO)
        return fail("entitlement guard answered 0 — that reads as 'not entitled' "
                    "and is the licence-failure path, not a neutral answer");

    // 1b. ...and the COMPLETION says so, because the value alone stopped being
    //     the answer. It is a request, not a predicate: the SDK wraps the return
    //     in a `Request` and awaits a message, so a non-zero id with nothing
    //     arriving for it is a guest that waits forever — which is exactly what
    //     stopped 1.40 at the epilepsy screen, one request over. Assert the pair.
    {
        void *pop = kl_ovrplat_sym("ovr_PopMessage");
        void *ent = kl_ovrplat_sym("ovr_Entitlement_GetIsViewerEntitled");
        void *rid = kl_ovrplat_sym("ovr_Message_GetRequestID");
        void *err = kl_ovrplat_sym("ovr_Message_IsError");
        if (!pop || !ent || !rid || !err)
            return fail("entitlement guard: the message pump resolved to nothing");
        while (((void *(*)(void))pop)()) { }             // drain anything queued
        uint64_t req = ((uint64_t (*)(void))ent)();
        void    *msg = ((void *(*)(void))pop)();
        if (!msg)
            return fail("the entitlement request queued NO completion — the guest "
                        "awaits one and would wait forever");
        if (((uint64_t (*)(void *))rid)(msg) != req)
            return fail("the entitlement completion carries the wrong request id");
        if (((uint64_t (*)(void *))err)(msg))
            return fail("the entitlement completion is an ERROR — that is the "
                        "licence-failure path, not a neutral answer");
    }

    // 2. Anything that would DELIVER paid content must still die rather than
    //    answer. This is also what proves the ovr_AssetFile_GetList carve-out
    //    below is an exact name and not a widening of the "AssetFile" marker to
    //    the whole family.
    r = drm_call("ovr_AssetFile_DownloadById");
    if (r == KLDRM_UNRESOLVED)
        return fail("DRM guard: ovr_AssetFile_DownloadById resolved to nothing");
    if (r != KLDRM_ABORTED)
        return fail("DRM guard did NOT abort on ovr_AssetFile_DownloadById — a "
                    "content-delivery call must never be answered");

    // 3. The carve-out, in the other direction: enumerating installed DLC is
    //    answered with "none", because that grants nothing (kl_ovrplat.c,
    //    g_plat_enumeration). If this ever starts aborting the game stops at
    //    the language select again.
    r = drm_call("ovr_AssetFile_GetList");
    if (r == KLDRM_UNRESOLVED) return fail("ovr_AssetFile_GetList resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_AssetFile_GetList aborted — enumerating DLC we do not have "
                    "is a fact about this host, not a licence decision");

    // 4. ...and the PER-FILE status spelling: answered, not refused (a title
    //    that asks about its packs by name would otherwise stop there), and
    //    answered with NOTHING — the completion's AssetDetails must say
    //    "available" / "not-entitled" with no path, for any name. Anything
    //    else would be an ownership or installation claim.
    r = drm_call_name("ovr_AssetFile_StatusByName", "klepton-nonexistent-asset");
    if (r == KLDRM_UNRESOLVED)
        return fail("ovr_AssetFile_StatusByName resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_AssetFile_StatusByName aborted — a per-file status query "
                    "must be answered (available / not-entitled), not refused");
    {
        void *pop   = kl_ovrplat_sym("ovr_PopMessage");
        void *st    = kl_ovrplat_sym("ovr_AssetFile_StatusByName");
        void *rid   = kl_ovrplat_sym("ovr_Message_GetRequestID");
        void *det   = kl_ovrplat_sym("ovr_Message_GetAssetDetails");
        void *dl    = kl_ovrplat_sym("ovr_AssetDetails_GetDownloadStatus");
        void *iap   = kl_ovrplat_sym("ovr_AssetDetails_GetIapStatus");
        void *path  = kl_ovrplat_sym("ovr_AssetDetails_GetFilepath");
        void *freem = kl_ovrplat_sym("ovr_FreeMessage");
        if (!pop || !st || !rid || !det || !dl || !iap || !path || !freem)
            return fail("status guard: the per-file status surface resolved to nothing");
        while (((void *(*)(void))pop)()) { }             // drain anything queued
        uint64_t req = ((uint64_t (*)(const char *))st)("klepton-status-probe.obb");
        if (!req)
            return fail("ovr_AssetFile_StatusByName answered 0 — the guest awaits a "
                        "completion that would never come");
        void *msg = ((void *(*)(void))pop)();
        if (!msg) return fail("the status request queued NO completion");
        if (((uint64_t (*)(void *))rid)(msg) != req)
            return fail("the status completion carries the wrong request id");
        void *ad = ((void *(*)(void *))det)(msg);
        const char *d = ad ? ((const char *(*)(void *))dl)(ad) : NULL;
        const char *e = ad ? ((const char *(*)(void *))iap)(ad) : NULL;
        const char *fp = ad ? ((const char *(*)(void *))path)(ad) : "x";
        if (!d || strcmp(d, "available") || !e || strcmp(e, "not-entitled") || fp)
            return fail("per-file status granted something — it must answer "
                        "available / not-entitled with no path");
        ((uint64_t (*)(void *))freem)(msg);
    }

    // 5. Downloading by name is delivery: it must still die.
    r = drm_call_name("ovr_AssetFile_DownloadByName", "klepton-nonexistent-asset");
    if (r == KLDRM_UNRESOLVED)
        return fail("DRM guard: ovr_AssetFile_DownloadByName resolved to nothing");
    if (r != KLDRM_ABORTED)
        return fail("DRM guard did NOT abort on ovr_AssetFile_DownloadByName — a "
                    "content-delivery call must never be answered");

    // 5b. The EMPTY purchase answer, asserted in BOTH directions,
    //     because "the viewer owns nothing" and "the viewer owns something" are
    //     one bit apart and only one of them is allowed to be said here:
    //
    //       - ovr_IAP_GetViewerPurchases must be ANSWERED (a real request id)
    //         and its completion must carry a purchase array of size ZERO. A
    //         non-empty array would be a manufactured ownership record;
    //       - ovr_IAP_LaunchCheckoutFlow — buying — must still abort, which is
    //         what proves the carve-out is an exact name and not a widening of
    //         the "IAP" marker to the whole family.
    {
        void *pop  = kl_ovrplat_sym("ovr_PopMessage");
        void *iap  = kl_ovrplat_sym("ovr_IAP_GetViewerPurchases");
        void *rid  = kl_ovrplat_sym("ovr_Message_GetRequestID");
        void *arr  = kl_ovrplat_sym("ovr_Message_GetPurchaseArray");
        void *sz   = kl_ovrplat_sym("ovr_PurchaseArray_GetSize");
        void *el   = kl_ovrplat_sym("ovr_PurchaseArray_GetElement");
        void *freem = kl_ovrplat_sym("ovr_FreeMessage");
        if (!pop || !iap || !rid || !arr || !sz || !el || !freem)
            return fail("purchase guard: the empty-purchase surface resolved to nothing");
        while (((void *(*)(void))pop)()) { }             // drain anything queued
        uint64_t req = ((uint64_t (*)(void))iap)();
        if (!req)
            return fail("ovr_IAP_GetViewerPurchases answered 0 — the guest awaits a "
                        "completion that would never come");
        void *msg = ((void *(*)(void))pop)();
        if (!msg)
            return fail("the purchase request queued NO completion");
        if (((uint64_t (*)(void *))rid)(msg) != req)
            return fail("the purchase completion carries the wrong request id");
        void *pa = ((void *(*)(void *))arr)(msg);
        if (((uint64_t (*)(void *))sz)(pa) != 0)
            return fail("the purchase array is NOT EMPTY — nothing here may report "
                        "an owned purchase; that is a manufactured ownership record");
        if (((void *(*)(void *, uint64_t))el)(pa, 0) != NULL)
            return fail("the empty purchase array handed back an element");
        ((uint64_t (*)(void *))freem)(msg);
    }

    r = drm_call("ovr_IAP_LaunchCheckoutFlow");
    if (r == KLDRM_UNRESOLVED)
        return fail("DRM guard: ovr_IAP_LaunchCheckoutFlow resolved to nothing");
    if (r != KLDRM_ABORTED)
        return fail("DRM guard did NOT abort on ovr_IAP_LaunchCheckoutFlow — "
                    "buying content must never be answered");

    // 6. The platform surface no longer stops the run on a name nobody has
    //    classified. This is the round-8 change and it is asserted in BOTH
    //    directions, because "answer everything" and "refuse ownership" are the
    //    two halves that have to stay separable:
    //
    //      - a request we refuse (a leaderboard fetch: what the player SCORED)
    //        must resolve, must not abort, and must answer 0;
    //      - a name that is in no tier at all must do the same, rather than
    //        abort — that is what cost a launch per name for four rounds;
    //      - and the ownership families must STILL abort, which #2 above
    //        already asserted and which this cannot be allowed to soften.
    r = drm_call("ovr_Leaderboard_GetEntries");
    if (r == KLDRM_UNRESOLVED)
        return fail("ovr_Leaderboard_GetEntries resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_Leaderboard_GetEntries ABORTED — a leaderboard records "
                    "what a player scored, not what they bought; it is a refused "
                    "REQUEST and must answer 0");
    if (r != KLDRM_ZERO)
        return fail("ovr_Leaderboard_GetEntries answered non-zero — a fabricated "
                    "request id is one nothing ever completes");

    r = drm_call("ovr_ThisNameIsInNoTierAtAll_Probe");
    if (r == KLDRM_UNRESOLVED)
        return fail("an unclassified platform name resolved to nothing — every "
                    "name must resolve, so the guest's dlsym succeeds");
    if (r == KLDRM_ABORTED)
        return fail("an unclassified platform name ABORTED — the terminal tier "
                    "answers 0 and names itself in the log; stopping the run one "
                    "name at a time is what round 8 removed");
    if (r != KLDRM_ZERO)
        return fail("the unclassified tier answered non-zero");

    // 7. The request half of the ownership families, both directions. The
    //    decision of 2026-08-20 (Pinball FX aborted at its store-catalogue
    //    fetch on every launch): a call that RETURNS AN ovrRequest may answer 0,
    //    "the request could not be made", because there is no Oculus service
    //    here to make it of. That reports no product, no purchase and no
    //    ownership. Assert the value, not just the survival — an abort here is
    //    the crash we fixed, and a NON-zero would be a fabricated request id
    //    with a completion that can never arrive.
    r = drm_call("ovr_IAP_GetProductsBySKU");
    if (r == KLDRM_UNRESOLVED)
        return fail("ovr_IAP_GetProductsBySKU resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_IAP_GetProductsBySKU ABORTED — a request-shaped call "
                    "must fail honestly, not stop the guest (this was the "
                    "Pinball FX splash crash)");
    if (r != KLDRM_ZERO)
        return fail("ovr_IAP_GetProductsBySKU answered non-zero — that is a "
                    "fabricated request id, and its completion can never come");

    //    ...and the ACCESSORS over what such a request would have returned are
    //    the other direction, and must still die: this is where a fabricated 0
    //    would read as an ANSWER ("you own this"), which is the circumvention.
    r = drm_call("ovr_Purchase_GetSKU");
    if (r == KLDRM_UNRESOLVED)
        return fail("DRM guard: ovr_Purchase_GetSKU resolved to nothing");
    if (r != KLDRM_ABORTED)
        return fail("DRM guard did NOT abort on ovr_Purchase_GetSKU — a purchase "
                    "record must never be answered, only the request refused");

    // 8. The leaderboard REQUESTS. Pinball FX stopped at
    //    ovr_Leaderboard_GetEntriesAfterRank on the logo screen (round 6): a
    //    leaderboard is not an ownership question — it records what a player
    //    scored — so its request half answers 0 as ovr_Achievements_* does.
    //    Assert the value, for the same reason as the IAP case above.
    r = drm_call("ovr_Leaderboard_GetEntriesAfterRank");
    if (r == KLDRM_UNRESOLVED)
        return fail("ovr_Leaderboard_GetEntriesAfterRank resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_Leaderboard_GetEntriesAfterRank ABORTED — a request-shaped "
                    "call must fail honestly, not stop the guest (this was the "
                    "Pinball FX logo-screen crash)");
    if (r != KLDRM_ZERO)
        return fail("ovr_Leaderboard_GetEntriesAfterRank answered non-zero — that "
                    "is a fabricated request id whose completion can never arrive");

    printf("  the app's own entitlement answers yes AND completes, DLC delivery\n"
           "  refuses and aborts (guard verified); DLC enumeration is answered with\n"
           "  an empty list and per-file status with available / not-entitled\n"
           "  (grants nothing, no path);\n"
           "  a refused request and an unclassified name both answer 0 without\n"
           "  stopping the run, and the ownership families still abort;\n"
           "  the viewer's purchases are answered EMPTY (size 0, no element) and\n"
           "  buying still aborts; the IAP/AssetFile REQUESTS fail honestly with 0\n"
           "  while every purchase accessor still aborts\n");
    return 0;
}

// ---- the guest run, in one function ----
// This is everything from libmain.so entry through the lifecycle recon. It
// runs either in the re-exec'd child (normal), or in-process (KL_NOFORK=1,
// for debugging). Returns only on a test failure; success is _exit(0), as
// before.
//
// view_pump is the KL_VIEW shape: the same sequence, but the frame pump runs
// until the viewer window closes instead of KL_FRAMES times, and the function
// RETURNS instead of _exit(0) — the guest is a spawned thread in that mode and
// the reports belong to the main thread after the join.
static volatile int g_view_quit;
// Where this target's `global-metadata.dat` is on disk. Two instruments want it
// — the sampler, to name managed frames, and the metadata dump, to compare what
// is in memory against what was loaded — and they must not derive it
// differently: a sampler resolving against one file while the dump measures
// another is two answers about one guest.
static const char *metadata_path(char *buf, size_t n) {
    const char *menv = getenv("KL_IL2CPP_METADATA");
    if (menv) { snprintf(buf, n, "%s", menv); return buf; }
    // <apk>/lib/arm64-v8a -> <apk>/assets/bin/Data/Managed/...
    snprintf(buf, n, "%s", LIBDIR);
    char *tail = strstr(buf, "/lib/");
    if (tail) *tail = 0;
    snprintf(buf + strlen(buf), n - strlen(buf),
             "/assets/bin/Data/Managed/Metadata/global-metadata.dat");
    return buf;
}

// The guest's frame, read back through Metal. It belongs to the RUN rather than
// to the Unity lifecycle: every non-Unity door reaches the eye textures the same
// way (kl_glfb's eye table), so a driver that stops before this reports a
// working pipeline as nothing at all.
// ---- the composite audit, run on the host that has no compositor -----------
//
// **The gap ITR's first device launch fell into.** Everything above measures
// the guest's eye TEXTURE — is it lit, is it the right picture, does it match
// the reference readback — and every one of those numbers was healthy while the
// headset showed the picture several times over. Nothing on the host had ever
// executed the step between the texture and the display: the crop, the per-eye
// rects, the frame record a compositor reads them out of. That step is
// arithmetic, it needs no drawable, and kl_composite_audit_eyes is it.
//
// Sampled rather than run per frame (the pump does ~48,000 of them), but with a
// FAULT LATCH rather than a snapshot: the first fault is kept verbatim, faults
// are counted, and a run that ends clean has to have been clean at every sample
// rather than merely at the last one. A transient — one frame's record filed
// without a viewport, a stage whose texture was re-created a generation late —
// is exactly what a single end-of-run reading would miss, and exactly what
// flickers in a headset.
static unsigned long g_audit_n, g_audit_faults;
static unsigned long g_audit_kind[KL_CF_COUNT];
static char g_audit_first[320];
static char g_audit_last[320];
static int  g_audit_shared = -1;

static void composite_audit_tick(int frame) {
    if (frame % 31) return;
    kl_ovrp_render_pose rec;
    int have = kl_ovrp_last_complete_record(&rec);
    int stage = have ? rec.stage : 0;
    if (stage < 0) stage = 0;
    int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    void *t0 = kl_mtl_eye_size(0, stage, &w0, &h0);
    void *t1 = kl_mtl_eye_size(1, stage, &w1, &h1);
    // Nothing allocated yet is not a fault — it is the whole of the run before
    // the guest reaches its first ovrp_SetupLayer, and calling that a broken
    // composite would drown the real ones.
    if (!t0) return;
    // "Shared" has to mean the eyes read the same TEXELS, not merely the same
    // MTLTexture object. Round 25 found this reading 3855 of 3855 samples
    // FAULTY/DOUBLED on a display that was correct: under the ARRAY layout
    // (ovrpLayout 3, what a multiview guest asks for) there is one texture with
    // two array layers and the eye IS the slice — kl_reproject.c:686-693 hands
    // each eye its own `slice`, so a full-texture rect on slice 0 and the same
    // rect on slice 1 are two different pictures, not one shown twice. A
    // pointer comparison alone cannot tell that apart from DoubleWide, where
    // both eyes really do sample slice 0. So the slice is part of the identity.
    int s0 = 0, s1 = 0;
    kl_glfb_eye_mtl_texture(0, stage, &s0);
    kl_glfb_eye_mtl_texture(1, stage, &s1);
    int shared = (t1 != NULL) && (t1 == t0) && (s0 == s1);
    g_audit_shared = shared;
    kl_composite_audit a;
    kl_composite_audit_eyes(have ? &rec : NULL, w0, h0, shared, &a);
    g_audit_n++;
    if (!a.fault) return;
    g_audit_faults++;
    if (a.kind > 0 && a.kind < KL_CF_COUNT) g_audit_kind[a.kind]++;
    snprintf(g_audit_last, sizeof g_audit_last,
             "frame %d, %dx%d eye texture, stage %d, eyes %s storage: %s",
             frame, w0, h0, stage,
             shared ? "SHARE one" :
             (t1 == t0 ? "share one texture but DIFFERENT slices (Array)"
                       : "have two"), a.why);
    if (!g_audit_first[0]) snprintf(g_audit_first, sizeof g_audit_first, "%s",
                                    g_audit_last);
}

// ...sampled from a thread of its own, which is the shape it has to have: a
// compositor is a SEPARATE thread reading these two globals while the guest
// runs, and the doors that matter here (UE4's kl_ue4_pump) own their frame loop
// inside the runtime rather than handing m_boot an iteration to hook. Host-only
// by construction — it lives in m_boot, so nothing about the app build changes.
static volatile int g_audit_stop;
static void *composite_audit_thread(void *unused) {
    (void)unused;
    for (int i = 0; !g_audit_stop; i += 31) {
        composite_audit_tick(i);
        struct timespec ts = { 0, 20 * 1000 * 1000 };   // 50 Hz, a compositor's rate
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void report_composite_audit(void) {
    if (!g_audit_n && !g_audit_faults) return;
    printf("\n=== the composite audit: what a compositor would SAMPLE ===\n");
    // The last sample in full, so a healthy run says its numbers rather than
    // only the word "clean" — a check with no visible output is a check nobody
    // can tell is running.
    kl_ovrp_render_pose rec;
    int have = kl_ovrp_last_complete_record(&rec);
    int stage = have && rec.stage >= 0 ? rec.stage : 0;
    int w = 0, h = 0;
    void *t0 = kl_mtl_eye_size(0, stage, &w, &h);
    if (t0) {
        kl_composite_audit a;
        kl_composite_audit_eyes(have ? &rec : NULL, w, h,
                                g_audit_shared > 0, &a);
        // Three states, not two — see composite_audit_tick on why the Array
        // layout is not a doubling even though both eyes name one texture.
        int s0 = 0, s1 = 0;
        kl_glfb_eye_mtl_texture(0, stage, &s0);
        void *t1 = kl_glfb_eye_mtl_texture(1, stage, &s1);
        const char *how = g_audit_shared > 0
            ? "SHARE one MTLTexture (DoubleWide)"
            : (t1 == t0 ? "share one MTLTexture as ARRAY SLICES "
                          "(eye 0 = slice 0, eye 1 = slice 1)"
                        : "have a texture each");
        printf("  eye texture %dx%d, stage %d, the two eyes %s\n", w, h, stage, how);
        for (int e = 0; e < 2; e++)
            printf("  eye %d samples uv %.4f,%.4f %.4fx%.4f  (%s)\n", e,
                   (double)a.uv[e][0], (double)a.uv[e][1],
                   (double)a.uv[e][2], (double)a.uv[e][3],
                   a.cropped[e] ? "cropped by the render viewport"
                                : "the WHOLE texture — no crop");
        printf("  eye 1's rect covers %.0f%% of eye 0's\n",
               (double)(a.overlap * 100.0f));
    }
    printf("  %lu sample%s, %lu FAULTY\n", g_audit_n, g_audit_n == 1 ? "" : "s",
           g_audit_faults);
    if (!g_audit_faults) {
        printf("  the composite is sound at every sample\n");
        return;
    }
    // By CLASS, because that is what separates "healthy from the first
    // submitted frame" from "showing the picture twice". A run whose only
    // faults are NO RECORD was faulty only before the guest had drawn anything.
    static const char *const names[KL_CF_COUNT] = {
        "", "no eye texture", "no completed frame record",
        "the crop was REFUSED (viewport_of skew)",
        "*** DOUBLED: both eyes read one shared texture whole",
        "an eye sampled outside its own texture",
    };
    for (int k = 1; k < KL_CF_COUNT; k++)
        if (g_audit_kind[k])
            printf("    %6lu  %s\n", g_audit_kind[k], names[k]);
    printf("  *** first: %s\n", g_audit_first);
    if (strcmp(g_audit_first, g_audit_last) != 0)
        printf("  *** last:  %s\n", g_audit_last);
}

static void report_eye_interop(void) {
    if (!kl_glfb_has_mtl_provider() && !kl_glfb_eye_mtl_texture(0, 0, NULL)) return;
    printf("\n=== Metal interop: the guest's frame, read back ===\n");
    // Not "did the interop bind" — that is reported when it happens — but
    // "did the guest's rendering arrive in the MTLTexture".
    // The lit count uses the same luma threshold as kl_glfb's capture, so
    // it is directly comparable with the reference path's number: the two
    // should agree, and a lit reference frame beside a black interop one
    // means the binding took and the rendering went elsewhere.
    // ...and it fires for an eye MTLTexture that arrived any way at
    // all, not just through a registered provider. On the VULKAN path
    // there IS no provider: MoltenVK backs the guest's eye VkImage with
    // a texture of its own and kl_vulkan.c publishes that one
    // (kl_glfb_note_eye_mtl_texture), so a provider test would have
    // skipped the only guest whose compositor wiring is new.
    // Under the viewer's hardware compositor there IS no reference:
    // registering a GPU fence replaces the readback, so kl_glfb has
    // counted nothing and a bare 0 here would read as a black frame.
    if (!kl_glfb_has_mtl_provider())
        printf("  reference: KL_VK_OUT — on the Vulkan path the eye "
               "is read back from the VkImage, and this is the SAME "
               "storage seen as Metal\n");
    else if (kl_glfb_has_gpu_fence())
        printf("  reference: none — the GPU compositor is driving, "
               "so nothing was read back\n");
    else
        printf("  reference (glReadPixels, kl_glfb): %lu lit\n",
               kl_glfb_last_frame_lit());
    // EVERY stage, not stage 0. This used to read and dump stage 0
    // alone while the compositors show
    // kl_ovrp_last_complete_stage() — so on a guest whose swapchain
    // has more than one image the PNG and the screen are DIFFERENT
    // PICTURES, and the readback can report a perfect frame for
    // something nobody is looking at — an instrument answering
    // confidently about the wrong subject. Open Brush has 3 OpenXR
    // swapchain images and shows it.
    int shown = kl_ovrp_last_complete_stage();
    int stages = kl_ovrp_stage_count();
    if (stages < 1) stages = 1;
    printf("  the compositors show stage %d (kl_ovrp_last_complete_stage)"
           "%s\n", shown,
           shown < 0 ? " — nothing filed, so they fall back to stage 0" : "");
    for (int eye = 0; eye < 2; eye++) {
        for (int st = 0; st < stages; st++) {
            int w = 0, h = 0;
            if (!kl_glfb_eye_mtl_texture(eye, st, NULL)) continue;
            unsigned long lit = kl_mtl_count_lit(eye, st, &w, &h);
            unsigned long n = w && h ? ((unsigned long)((w + 7) / 8)
                                      * (unsigned long)((h + 7) / 8)) : 0;
            printf("  eye %d stage %d MTLTexture %dx%d: %lu/%lu lit, "
                   "mean luma %u%s%s\n",
                   eye, st, w, h, lit, n, kl_mtl_mean_luma(),
                   lit ? "" : "  <<< BLACK",
                   st == shown ? "   <- ON SCREEN" : "");
            // A picture, not just a count: KL_GLFB_OUT already holds the
            // reference frames, so writing the interop eyes beside them
            // is what makes "does it render the same" answerable.
            const char *out = getenv("KL_GLFB_OUT");
            if (out) {
                char p[1200];
                snprintf(p, sizeof p, "%s/mtl_eye%d_s%d.png", out, eye, st);
                printf("    -> %s%s\n", p,
                       kl_mtl_dump_png(eye, st, p) ? "" : "  (write FAILED)");
            }
        }
    }
    // ...and the layers that are NOT an eye. A guest can present its whole
    // frame as an OpenXR quad — JKXR submits nothing else — and for that guest
    // every number above is 0 and says nothing: the eye table is empty because
    // there is no eye. This is the same measurement asked of the storage the
    // overlay pass samples, and it is the only evidence on host that the
    // picture reached the compositor at all.
    for (int i = 0; i < kl_ovrp_overlay_count(); i++) {
        kl_ovrp_overlay ov;
        if (!kl_ovrp_overlay_get(i, &ov)) continue;
        // EVERY image of the layer's swapchain, not just the one the last frame
        // named. The guest rotates through them and the compositor follows, so
        // a single image that stopped receiving the guest's rendering is a
        // FROZEN picture alternating with live ones — which is what a stale
        // frame flickering over a correct one looks like, and what no
        // measurement of one image can see. `stages` here is the record ring's
        // count, which is the same 3 an OpenXR swapchain has.
        for (int st = 0; st < stages; st++) {
            int w = 0, h = 0;
            if (!kl_glfb_layer_mtl_texture(ov.layer_id, st, NULL, NULL)) {
                if (st == ov.stage)
                    printf("  layer %d stage %d (%.2fx%.2f m): no MTLTexture — "
                           "NOT composited\n", ov.layer_id, st,
                           (double)ov.size[0], (double)ov.size[1]);
                continue;
            }
            unsigned long lit = kl_mtl_count_lit_layer(ov.layer_id, st, &w, &h);
            unsigned long n = w && h ? ((unsigned long)((w + 7) / 8)
                                      * (unsigned long)((h + 7) / 8)) : 0;
            // Alpha, beside the luma, because a quad layer is BLENDED: a guest
            // that leaves it at 0 (trap 33) submits a perfect picture that
            // composites to nothing, and that is the same display as black with
            // every other number here healthy.
            unsigned alpha = kl_mtl_mean_alpha();
            printf("  layer %d stage %d MTLTexture %dx%d (%.2fx%.2f m at "
                   "%.2f %.2f %.2f): %lu/%lu lit, mean luma %u, mean alpha %u%s%s%s\n",
                   ov.layer_id, st, w, h, (double)ov.size[0], (double)ov.size[1],
                   (double)ov.pose[4], (double)ov.pose[5], (double)ov.pose[6],
                   lit, n, kl_mtl_mean_luma(), alpha,
                   st == ov.stage ? "   <- LAST SUBMITTED" : "",
                   lit ? "" : "  <<< BLACK",
                   lit && alpha < 8 ? "  <<< TRANSPARENT: a blended layer with "
                                      "this alpha composites to nothing" : "");
            const char *out = getenv("KL_GLFB_OUT");
            if (out) {
                char p[1200];
                snprintf(p, sizeof p, "%s/mtl_layer%d_s%d.png", out, ov.layer_id, st);
                printf("    -> %s%s\n", p,
                       kl_mtl_dump_png_layer(ov.layer_id, st, p) ? ""
                                                                : "  (write FAILED)");
            }
        }
    }
}

// ---- the 2D -> VR handoff ----
//
// SteamLink.startVRLink(String) is the end of the shell's job: it has paired, the
// host has authorized, and it now starts the VR activity with the session as
// `sArgs` and calls finishAndRemoveTask() on itself. Both halves exist in this
// binary, but they are two front doors with different chains, different libraries
// and different lifecycles, and the shell's `main` is on the stack of the thread
// making this call.
//
// So: re-EXEC, which is what Android's own model already is — a new activity in a
// fresh task, the old one finishing. It replaces the process image, so every
// question about tearing the shell down (Qt, SDL, ANGLE's contexts, the audio
// unit, the shell's threads) does not arise. The exec'd process reads the session
// out of the environment through the same KL_SLINK_SARGS path a pasted one uses,
// so nothing downstream can tell the difference.
//
// KL_SLINK_HANDOFF=0 leaves the session printed and the run stopped by name,
// which is what a gate measuring the SHELL wants: a run that re-execs into a
// different front door measures something else.
static const char *g_argv0 = "./build/m_boot";

static void slink_setenv_default(const char *k, const char *v, const char *why) {
    if (getenv(k)) return;
    setenv(k, v, 1);
    printf("    %s=%s   (%s)\n", k, v, why);
}

static void slink_vrlink_handoff(const char *sargs) {
    if (!kl_env_on("KL_SLINK_HANDOFF", 1)) {
        printf("  (KL_SLINK_HANDOFF=0 — not entering the VR front door)\n");
        return;                      // the caller aborts by name, as before
    }

    printf("\n=== 2D -> VR handoff: re-exec into the OpenXR front door ===\n");
    printf("    %s %s\n", g_argv0, TARGET->name);
    setenv("KL_SLINK_SARGS", sargs, 1);
    setenv("KL_SLINK_VR", "1", 1);
    unsetenv("KL_SLINK_SHELL");      // the door resolver would otherwise tie
    unsetenv("KL_VIEW_POKE");        // a click script for the SHELL's UI, and
                                     // replaying it into the VR view would be
                                     // synthetic input nobody asked for
    slink_setenv_default("KL_SLINK_MAIN", "1",
                         "drive the lifecycle on into the OpenXR frame loop");
    slink_setenv_default("KL_GLFB", "1",
                         "the null GL driver cannot present a stream");
    slink_setenv_default("KL_OVRP_IPD", "0.063",
                         "HOST STOPGAP: an IPD of 0 stops the host sending video");
    if (!getenv("KL_VIEW"))
        slink_setenv_default("KL_SLINK_WAIT", "45",
                             "the VR deadline; 10 s would waste the pairing");
    fflush(NULL);

    execl(g_argv0, g_argv0, TARGET->name, (char *)NULL);

    // Only reachable if exec failed. Returning is the contract for "could not do
    // the handoff": the caller then aborts by name with the session printed
    // above, so the run is no worse off than before.
    printf("  !! exec(\"%s\") failed: %s\n", g_argv0, strerror(errno));
    fflush(NULL);
}

// The doors whose guest owns its own frame loop and takes a looper rather than a
// render call: Unreal's NativeActivity, OpenJK's static exports, and Steam Link's
// three. What differs between them is entirely inside kl_driver; what this adds
// is the command line's part — the budget in SECONDS (there is no render call
// here to count), the watchdog around it, and the interop readback, which is the
// only thing that says whether the guest's rendering ARRIVED.
//
// Under the viewer the deadline is the window: the pump runs until it closes,
// and the reports belong to the main thread after the join.
static int pump_and_report(double want, int view_pump) {
    const char *aenv = getenv("KL_ALARM");
    if (view_pump) printf("\n=== pumping the looper until the viewer closes ===\n");
    else           printf("\n=== pumping the looper for %.1f s ===\n", want);
    fflush(NULL);
    // The driver's per-call watchdog off, one alarm around the whole pump: this
    // is a single call that runs for the budget.
    kl_driver_set_alarm(0);
    if (!view_pump) alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : (unsigned)(want + 30));
    pthread_t audit;
    int audit_live = pthread_create(&audit, NULL, composite_audit_thread, NULL) == 0;
    double spent = kl_driver_pump(view_pump ? -1.0 : want,
                                  view_pump ? &g_view_quit : NULL);
    alarm(0);
    if (audit_live) { g_audit_stop = 1; pthread_join(audit, NULL); }
    printf("  pumped %.2f s\n", spent);
    if (view_pump) return 0;

    printf("\n=== reports ===\n");
    kl_driver_report(stdout);
    report_eye_interop();
    report_composite_audit();
    fflush(NULL);
    return 0;
}

static int begin_and_pump(int view_pump) {
    // Before the engine builds its OpenXR swapchains, which is where the images
    // that need MTLTexture storage are created. The Unity path registers this
    // inside its lifecycle (ovrp_SetupEyeTexture2 arrives there). Self-guarding
    // on KL_GLFB_MTL / KL_VIEW, so a plain run is unchanged.
    //
    // KL_GUEST_UE4 is on this list because "an Unreal guest needs no provider
    // at all, its eye images come from MoltenVK" is only true of an Unreal
    // guest on VULKAN. A UE4 guest on the OVRPlugin GL path (Tetris Effect)
    // asks for its eye textures through ovrp_SetupEyeTexture2 /
    // ovrp_GetLayerTexture2 exactly as a Unity one does. Without the provider
    // on this door the whole MTLTexture-backed branch of kl_glfb — including
    // the "replaced with no teardown" backstop below — is unreachable from any
    // host run, which is exactly why that bug was device-only.
    if (TARGET->kind == KL_GUEST_JKXR || TARGET->kind == KL_GUEST_UE4 || DOOR_IS_VR)
        kl_mtl_provider_install();

    // The same layer policy the device app applies, and for the same reasons —
    // Steam Link's VR door stacks projection layers and streams FOVEATED, so the
    // eye is the widest layer and the narrow one is laid over its centre, into
    // an eye texture allocated larger so the inset keeps its detail.
    //
    // It lived only in the visionOS frontend, which made the host silently a
    // DIFFERENT pipeline: no inset composite at all. That is why this arc's
    // foveal defects "did not reproduce in the macOS viewer" — not a stable
    // layer shape, as was first written down, but a path that was never on.
    // A host run is the cheap half of every A/B here, and it can only be that
    // if it runs what the device runs.
    if (DOOR_IS_VR) {
        kl_openxr_set_capture_topmost_layer(1);
        int es = kl_env_int("KL_XR_EYE_SCALE", 2);
        if (es < 1) es = 1;
        if (es > 4) es = 4;
        kl_glfb_set_eye_mirror_scale(es, 1);
        kl_glfb_set_eye_mirror_cap(kl_env_int("KL_XR_EYE_MAX", 3456));
    }

    if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());
    return pump_and_report(kl_driver_pump_default(), view_pump);
}

// The headless host's display clock, for an OpenXR guest with no window to be
// paced by. On the headset xrWaitFrame blocks in Compositor Services
// (kl_app.c's guest_pace_wait) and in the viewer on kl_view_pace_wait; a
// headless m_boot run had neither, so the Alyx host's XR/input loop span at
// ~400 k frames/s, starved Valve's render thread (13 stereo frames in 75 s)
// and made its telemetry ~1 GB a run. This stands in for the display: an
// absolute-deadline sleep on CLOCK_MONOTONIC at the rate the guest is told
// (kl_ovrp_display_frequency — 90 Hz for this door, as the headset's panel).
// A caller that falls behind waits for the next tick on the grid rather than
// being owed a burst of catch-up frames, as the compositor coalesces.
//
// It is the headless display's tracker as well: installing a pacer tells
// kl_openxr a frontend is publishing (the boot gate and the honest-head rule
// key on it), so each tick publishes a head at rest at eye height. Without it
// xrLocateViews answered no VALID bits and the Alyx host never calibrated its
// views (xr-view-calibration viewsValid=0, projections all 0 -> a 0-wide eye).
static void headless_pace_wait(void) {
    static float eye_y = -1.0f;
    if (eye_y < 0) eye_y = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    kl_ovrp_set_head_pose(0.0f, eye_y, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    static uint64_t next_ns;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    float hz = kl_ovrp_display_frequency();
    if (!(hz > 0)) hz = 90.0f;
    uint64_t period = (uint64_t)(1e9 / hz);
    pthread_mutex_lock(&mu);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t t = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    // A display's vsync: the next tick on the same grid. A caller that missed
    // ticks waits for the next one, not a whole period from now — the old
    // re-base (next = now + period) charged every late frame a full extra
    // period, which on hla's native door (WaitGetPoses on Valve's render path)
    // turned a ~22 ms frame into ~34 ms (29 fps).
    if (!next_ns) next_ns = t;                                        // first frame
    if (t >= next_ns + period) next_ns += (t - next_ns) / period * period;   // skip missed ticks
    next_ns += period;
    uint64_t due = next_ns;
    pthread_mutex_unlock(&mu);
    if (due > t) {
        uint64_t d = due - t;
        struct timespec ts = { (time_t)(d / 1000000000ull), (long)(d % 1000000000ull) };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
}

static int native_door_run(int view_pump) {
    // An OpenXR guest that owns its XR loop, run with no viewer, is paced here
    // (above) when KL_XR_HEADLESS_PACE=1 — opt-in for any native-door target
    // (the Alyx host needs it; nothing on driver did). 90 Hz unless
    // KL_DISPLAY_HZ pins it.
    if (!view_pump && kl_env_on("KL_XR_HEADLESS_PACE", 0)) {
        if (!getenv("KL_DISPLAY_HZ")) kl_ovrp_set_display_frequency(90.0f);
        // ...and two eyes apart. With no frontend the eyes coincide (IPD 0), and
        // the host's view calibration refuses that (xr-view-calibration
        // calibrated=0 ipd=0) — every eye copy then stops at reason=render-pose.
        setenv("KL_OVRP_IPD", "0.063", 0);
        kl_openxr_set_frame_pacer(headless_pace_wait);
        // Published before the first wait, so the boot gate finds a head.
        kl_ovrp_set_head_pose(0.0f, kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f), 0.0f,
                              0.0f, 0.0f, 0.0f, 1.0f);
        fprintf(stderr, "  [xr] headless pacer: xrWaitFrame held to %.1f Hz "
                        "(KL_XR_HEADLESS_PACE=1)\n",
                (double)kl_ovrp_display_frequency());
    }
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    if (kl_driver_gap_only()) return 0;
    return begin_and_pump(view_pump);
}

// Steam Link, which is the one target whose front door is a choice rather than a
// property of the tree. All three are the same guest and the same profile — the
// pairing credential the shell stores is exactly what the VR door is handed at
// the handoff — so what a run selects is only which chain it maps and which
// entry it calls.
static int steamlink_run(int view_pump) {
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    if (kl_driver_gap_only()) return 0;

    // The VR door without KL_SLINK_MAIN is a measurement of its own: the chain,
    // the init arrays and ANativeActivity_onCreate, which is where a glue that
    // returned early shows up as an empty callback table. Nothing after it runs,
    // so the guest never reaches OpenXR.
    if (DOOR_IS_VR && !getenv("KL_SLINK_MAIN")) {
        printf("\n=== ANativeActivity_onCreate (the VR front door) ===\n");
        fflush(NULL);
        if (kl_slink_vr_create(stdout) != 0) return fail(kl_slink_error());
        printf("  (KL_SLINK_MAIN unset: stopping after onCreate)\n");
        kl_driver_report(stdout);
        return 0;
    }
    // ...and the SDL doors without it stop one step earlier still, at the bound
    // chain: `main` is where everything unimplemented starts failing by name, and
    // the chain gate has to stay green independently of that.
    if (!DOOR_IS_VR && !getenv("KL_SLINK_MAIN") && !view_pump) {
        kl_driver_report(stdout);
        return 0;
    }
    return begin_and_pump(view_pump);
}

// The SDL door's budget expiring — see the alarm note at the call site. This
// runs as a SIGNAL HANDLER inside the game's frame loop, which is the same
// shape kl_fault.c's reporters already run in: stdio from a handler is not
// async-signal-safe in the letter of POSIX and is how this tree reports every
// fatal path, so the reports run here too and the run exits CLEANLY — the
// budget spent with a live frame loop is this door's success case.
static void sdl_budget_expired(int sig) {
    (void)sig;
    printf("\n=== SDL door: the KL_ALARM budget expired inside the frame loop\n"
           "    — the game ran; the eglSwapBuffers count below is the proof ===\n");
    // The narrow set, NOT kl_sdl_report: this runs inside the game's frame
    // loop, and the two reports that door adds for a normal return — the
    // mutex-owner map and AAudio's stream list — take locks the interrupted
    // threads can be holding. Doing that here wedged the run in place of
    // ending it, which is the one failure this handler exists to avoid.
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    kl_opensl_report(stdout);
    kl_mediandk_report(stdout);
    kl_openxr_report(stdout);
    report_eye_interop();
    fflush(NULL);
    _exit(0);
}

// The SDL2 door — Half-Life 2 VR (nillerusr's srceng Quest port). A third
// ENTRY shape: not libmain's JNI_OnLoad-driven Unity chain, not UE4's
// NativeActivity, but SDL's own convention, read off this APK's smali.
//
// What the guest IS — the chain order, the ValveActivity2 environment, the
// SDLActivity boot sequence — lives in runtime/guest/kl_sdl.c and is shared
// verbatim with the visionOS app, for kl_slink's reason: a guest described
// differently by two drivers is a bug with no error surface, and the way THAT
// one presented was the app taking the generic libmain.so path for this kind
// and failing at "No such file or directory". What stays here is the POLICY:
// the recon pass's gap-only mode and the time budget around a call that never
// returns.
//
// Shaped like ue4_run and jkxr_run deliberately, down to KL_GAP_ONLY.
static int sdl_run(void) {
    if (kl_sdl_configure(LIBDIR, stdout) != 0) return fail(kl_sdl_error());
    if (kl_sdl_load(stdout) != 0) return fail(kl_sdl_error());

    // The work list, before anything is INITIALIZED — the same property
    // kl_ue4_gap reports for the UE4 door, over every image this door mapped.
    // For a brand-new target this is the most valuable thing a run produces,
    // and it is printed ahead of the initializers because an initializer that
    // dies (the first run of this door did exactly that) must not take the
    // list down with it. KL_GAP_ONLY stops here.
    printf("\n=== the shim gap ===\n");
    kl_sdl_gap(stdout);
    if (getenv("KL_GAP_ONLY")) { fflush(NULL); return 0; }

    if (kl_sdl_begin(stdout) != 0) return fail(kl_sdl_error());

    // The alarm is a TIME BUDGET on this door, not a deadlock watchdog, and
    // the difference is the exit status: an SDL guest owns its frame loop
    // inside nativeRunMain the way a NativeActivity guest owns its
    // (ue4_run's "seconds, not frames"), so the healthy run never returns and
    // the budget expiring IS the completion — the eglSwapBuffers count in the
    // report is the evidence that the loop was a frame loop and not a hang.
    // KL_ALARM sizes the budget, as everywhere else. The app has no budget at
    // all: there the same call is the guest thread's whole life.
    signal(SIGALRM, sdl_budget_expired);
    const char *aenv = getenv("KL_ALARM");
    alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : 20);
    int rc = kl_sdl_run_main(stdout);
    alarm(0);
    signal(SIGALRM, SIG_DFL);
    (void)rc;

    printf("\n=== reports ===\n");
    kl_sdl_report(stdout);
    report_eye_interop();
    fflush(NULL);
    return 0;
}

static int recon_run(int view_pump) {
    // Every thread that runs guest code seeds bionic's stack-guard canary into
    // TSD slot 5 first, and this is the thread the whole run is on. Without it
    // the slot holds whatever Darwin last left there, libSDL3's JNI_OnLoad copies
    // that at entry, something writes the slot during its 66 RegisterNatives
    // calls, and the epilogue reports "stack smashing detected" — a real canary
    // mismatch with no buffer overflow anywhere near it. libUE4 and the OpenJK
    // engine have stack protectors too; Beat Saber's path to initJni has none,
    // which is the only reason it never needed this.
    kl_thread_init();
    install_fault_reporter();
    kl_mem_pressure_init();
    // Strict: an unimplemented *call* is fatal. Lookups are not, so this
    // stops only where the surface genuinely ends. KL_PERMISSIVE=1 flips it
    // to a zero return, which collects a whole batch in one run when pushing
    // into new territory — scouting only, since the guest then carries on
    // with answers we made up.
    //
    // Set BEFORE the door is chosen, with the texture dump: both are properties
    // of the run rather than of the engine, and they used to sit below the UE4
    // branch, which silently made KL_PERMISSIVE and KL_DUMP_TEXTURES do nothing
    // at all on a NativeActivity target — the batch-scouting mode, absent
    // exactly where a brand-new target most needs it.
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);

    // Armed before anything runs: texture uploads happen all through init and the
    // lifecycle, not just inside the frame pump.
    kl_egl_dump_textures(getenv("KL_DUMP_TEXTURES"));

    // Which door this target takes. The Unity sequence is not a default that
    // happens to fit every guest — it names libmain, NativeLoader and
    // UnityPlayer.initJni — so the others branch before any of it. All four live
    // in kl_driver, shared with the visionOS app.
    kl_driver_set_alarm(getenv("KL_ALARM")
                        ? (unsigned)strtoul(getenv("KL_ALARM"), NULL, 10) : 20);

    // KL_OVRP_EYE_FORCE=WxH — stand in for the frontend's display measurement.
    // On device KleptonCompositor calls kl_ovrp_set_eye_texture_size with the
    // size it measured and KL_OVRP_EYE_MAX/KL_OVRP_EYE_SCALE act on THAT; with
    // no frontend the host keeps the compile-time Quest 2 default (2290x2400)
    // and both knobs are inert, which reads as "the cap does nothing" when it
    // means "nothing measured a display". Set before the guest loads, because
    // a guest reads the recommended size exactly once, at VR device init.
    //
    // **ABOVE the door dispatch, and that is the whole of round 9's change.**
    // It used to sit below it, so the four doors that `return` — SteamLink,
    // UE4, JKXR and SDL — never reached it: on ITR, a UE4 guest, the knob did
    // nothing at all and said nothing about it, which is the "a probe that
    // answers confidently and wrongly" failure this project has now been burned
    // by four times. The cost of the miss is exact and was paid in device
    // launches: the host ran ITR at the Quest 2 default (2290x2400 -> a
    // 5888x2664 eye layer) while the headset runs it at 4032x3234 -> 13568x3588,
    // so every host capture in docs/shots/ was taken at a geometry the device
    // never uses, and the ONE premise nobody could check — what the guest
    // actually draws into its eye texture on the DEVICE — was unreachable from
    // the host by construction. With this hoisted:
    //
    //     KL_OVRP_EYE_FORCE=6888x5525 KL_OVRP_EYE_MAX=4032 KL_VK_OUT=<dir> \
    //       KL_UE4_WAIT=300 ./build/m_boot itr
    //
    // reproduces the headset's exact eye chain on a laptop and reads the eye
    // texture back. Every guest kind now honours the knob; nothing changes for
    // any run that does not set it (the block is a no-op without it), and the
    // Unity doors below are byte-identical because the knob was already applied
    // before `kl_driver_boot` on their path.
    {
        const char *s = kl_env_str("KL_OVRP_EYE_FORCE", NULL);
        int fw, fh;
        if (s && sscanf(s, "%dx%d", &fw, &fh) == 2)
            kl_ovrp_set_eye_texture_size(fw, fh);
    }

    if (TARGET->kind == KL_GUEST_STEAMLINK) return steamlink_run(view_pump);
    if (TARGET->kind == KL_GUEST_UE4 || TARGET->kind == KL_GUEST_JKXR)
        return native_door_run(view_pump);
    if (TARGET->kind == KL_GUEST_SDL) return sdl_run();

    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());

    // The lifecycle, behind a knob: everything above is a gate that must stay
    // green, and everything below stops by name until the shim catches up.
    if (getenv("KL_LIFECYCLE")) {
        // With KL_GLFB_MTL=1, the eye textures Unity is about to ask for
        // become MTLTextures we allocated — the host stand-in for what
        // Compositor Services will hand over. Registered here because
        // ovrp_SetupEyeTexture2 arrives inside nativeRecreateGfxState.
        kl_mtl_provider_install();
        if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());

        // One nativeRender is the engine's first frame and it is almost all
        // setup — no scene is loaded and nothing is drawn yet, which is why
        // the GL surface looked so small. KL_FRAMES pumps the render loop
        // the way UnityPlayer's own thread would, draining the posted-task
        // queue between frames as the UI thread's looper does.
        // In view_pump mode (KL_VIEW) the pump instead runs until the viewer
        // window closes, paced to 72 Hz — the Quest 2 display frequency we
        // report through ovrp_GetSystemDisplayFrequency, so the Choreographer
        // ticks at the rate the engine believes the hardware runs at. The
        // watchdog alarm is not armed there: the window may stay open for
        // minutes, and a human at the keyboard IS the watchdog.
        const char *fenv = getenv("KL_FRAMES");
        unsigned frames = fenv ? (unsigned)strtoul(fenv, NULL, 10) : 0;
        if (frames || view_pump) {
            // Here rather than earlier because it wants il2cpp_init to have
            // decrypted the tables, and it is a scan of live memory — the guest
            // is between frames at this point, which is the quietest the heap
            // gets. It does nothing unless KL_DUMP_METADATA names a file.
            //
            // KL_DUMP_METADATA_AT=end moves it to after the pump instead, for a
            // loader that decrypts LAZILY: at init only whatever
            // MetadataCache::Initialize touched is plaintext, and the tables the
            // game reaches for while it runs are not. `init` stays the default
            // because a guest that dies in the pump still gets a dump.
            const char *mdat = getenv("KL_DUMP_METADATA_AT");
            int md_late = mdat && !strcmp(mdat, "end");
            if (!md_late) {
                char meta[1024];
                kl_metadump_run(metadata_path(meta, sizeof meta));
            }
            if (view_pump)
                printf("\n=== recon: pumping frames until the viewer closes ===\n");
            else
                printf("\n=== recon: pumping %u frames ===\n", frames);
            fflush(NULL);
            const char *aenv2 = getenv("KL_ALARM");
            unsigned budget = aenv2 ? (unsigned)strtoul(aenv2, NULL, 10) : 60;
            // One watchdog around the whole pump rather than the driver's
            // per-frame one, and none at all under the viewer: the window may
            // stay open for minutes, and a human at the keyboard IS the watchdog.
            kl_driver_set_alarm(0);
            if (!view_pump) alarm(budget);
            // KL_SAMPLE_MS: while the pump runs, sample every guest thread's
            // pc/backtrace and resolve it (kl_sample.c) — built to name the
            // loop the loading-pace arc kept measuring. The metadata sits next
            // to the libs in the unpacked APK tree.
            const char *senv = getenv("KL_SAMPLE_MS");
            int sampling = 0;
            if (senv) {
                char meta[1024];
                sampling = kl_sample_start((unsigned)strtoul(senv, NULL, 10),
                                           metadata_path(meta, sizeof meta));
            }
            const long frame_ns = 1000000000L / 72;
            unsigned haptic_pulses = 0;
            unsigned i;
            for (i = 0; view_pump ? !g_view_quit : i < frames; i++) {
                struct timespec t0;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                // The guest's frame: pose latch, nativeRender, the posted-task
                // drain and the memory-pressure poll (kl_driver_frame). It
                // returns -1 when nativeRender was never registered, which is
                // the only way this loop ends early.
                if (kl_driver_frame() < 0) break;
                // The haptics seam's host end. Nothing on macOS can vibrate, so
                // this only drains and counts — but draining is the point: it
                // is what makes the host a working A/B for the whole path down
                // to the actuator, and KL_HAPTICS_TRACE=1 then prints the same
                // pulses a headset would have played. (The queue does not need
                // it to stay healthy; it retires samples on the wall clock
                // whether anyone is listening or not.)
                for (int hand = 0; hand < 2; hand++) {
                    float amp, secs;
                    if (kl_ovrp_haptics_pull(hand, &amp, &secs)) haptic_pulses++;
                }
                // KL_PROBE_INPUT: ask Unity's own managed API what it sees —
                // joystick count, the bound axes, the XR node poses. Between
                // frames on the thread that just ran one, which is where
                // managed calls are safe.
                kl_mprobe_tick(i);
                // KL_PROBE_TIMELINE: and what the guest's own scripted
                // sequences think they are doing. Same seam, same safety —
                // between frames, on the thread that just ran one.
                kl_mprobe_timeline(i);
                if (view_pump) {
                    struct timespec t1;
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long used = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                                (t1.tv_nsec - t0.tv_nsec);
                    if (used < frame_ns) {
                        struct timespec rem = { 0, frame_ns - used };
                        nanosleep(&rem, NULL);
                    }
                }
            }
            alarm(0);
            if (sampling) kl_sample_stop_report(stdout);
            if (md_late) {
                char meta[1024];
                kl_metadump_run(metadata_path(meta, sizeof meta));
            }
            if (haptic_pulses)
                printf("  haptic pulses drained: %u (nothing to play them on "
                       "here — see kl_ovrp.h)\n", haptic_pulses);
            report_eye_interop();
            report_composite_audit();
            const char *sd = getenv("KL_DUMP_SHADERS");
            if (sd) kl_egl_dump_shaders(sd);
            if (getenv("KL_DUMP_TEXTURES"))
                printf("  wrote %u texture upload%s as PNG\n",
                       kl_egl_texture_count(),
                       kl_egl_texture_count() == 1 ? "" : "s");
        }
    }

    if (view_pump)
        return 0;   // the main thread prints the reports after the join
    kl_driver_report(stdout);
    kl_metadump_watch_report(stdout);   // host-only, so not the driver's
    fflush(NULL);   // _exit does not flush stdio, and the report is the point
    _exit(0);
}

// The KL_VIEW guest thread: the same recon sequence the re-exec'd child runs,
// but in-process and pumping until the window closes. kl_thread_init() first —
// this thread runs guest code, and guest code needs its TLS slot.
static void *view_guest_thread(void *arg) {
    (void)arg;
    kl_thread_init();
    recon_run(1);
    return NULL;
}

// KL_VIEW=1: the interactive frontend (kl_view.c). This deliberately skips
// BOTH process games main() normally plays:
//  - the DRM-guard fork test, because the guard itself lives in kl_ovrplat.c
//    and is unaffected by who forks, and
//  - the re-exec, because Metal's XPC shader compiler refuses forked children
//    (the AGX abort story above) and a windowed app never forks in the first
//    place — in-process is the whole point.
// The guest runs on a spawned thread; the main thread runs SDL, because macOS
// requires windowing on the main thread.
static int view_run(void) {
    // kl_env_on, not getenv: this has to agree with kl_glfb_enabled() about what
    // KL_GLFB=0 means, or the viewer starts on the strength of a knob the
    // renderer read as off and then displays nothing.
    //
    // ...unless the guest's API is Vulkan, and that is not knowable here: it is
    // measured by kl_vulkan_guest_active() long after this point, from what the
    // guest actually did. A Vulkan guest never brings ANGLE up at all, and its
    // eye textures reach the compositor from MoltenVK instead
    // (kl_glfb_note_eye_mtl_texture), so KL_GLFB has nothing to do with whether
    // there is a picture. Refusing here would refuse the one target whose
    // compositing is new. So this is a NOTE now, not a refusal — the viewer
    // waits for an eye texture from whichever source produces one, and says so
    // in its HUD if none ever arrives.
    if (!kl_env_on("KL_GLFB", 0))
        fprintf(stderr, "view: KL_GLFB is not set — nothing will be displayed "
                        "unless the guest turns out to render through VULKAN, "
                        "whose eye textures do not come from ANGLE\n");
    if (!kl_view_available()) {
        fprintf(stderr, "KL_VIEW=1 but t_boot was built without SDL3\n");
        return 1;
    }
    // The frame-out seam, and which of its two implementations to use. Decided
    // HERE, on the main thread, before the guest thread exists: kl_glfb_mtl_
    // device() brings ANGLE up to answer, and kl_glfb_init() is not something
    // two threads may race into.
    //
    // Hardware is the default — the guest's eye textures become MTLTextures we
    // allocated and the viewer composites one straight into the window's
    // CAMetalLayer, so no frame is ever read back or copied. KL_VIEW_CPU=1
    // keeps the old glReadPixels path, which is the A/B when the compositor
    // shows the wrong picture.
    int hw = 0;
    if (TARGET->kind == KL_GUEST_STEAMLINK && DOOR != KL_SLINK_VR) {
        // A FLAT guest (the shell and client doors): its picture is the default
        // framebuffer of an EGL window surface, not an eye texture keyed by
        // (eye, stage). Requested HERE, before ANGLE is up, that framebuffer's
        // storage is an IOSurface kl_glfb allocates — the compositor samples
        // the guest's own pixels and nothing reads back. The panel size is
        // already published (kl_slink_configure ran in main), so bringing
        // ANGLE up now creates the surface at the size the guest will draw.
        // KL_VIEW_CPU=1 keeps the glReadPixels path as the A/B.
        //
        // The VR door is NOT flat: its eyes are mirror-blitted into provider
        // textures keyed (eye, stage) — the same seam the device compositor
        // samples — so it takes the hardware chain below like every XR door.
        // Keying this on the TARGET rather than the DOOR once made a VR viewer
        // run pay the mirror blits AND a full-frame glReadPixels per frame.
        if (!getenv("KL_VIEW_CPU") && kl_env_on("KL_GLFB", 0)) {
            kl_glfb_request_flat_surface();
            if (kl_glfb_mtl_device() && kl_glfb_flat_surface(NULL, NULL)) {
                hw = 1;
                fprintf(stderr, "view: flat guest — IOSurface composite, "
                                "no readback\n");
            }
        }
        if (!hw) fprintf(stderr, "view: flat guest — readback path\n");
    } else if (getenv("KL_VIEW_CPU")) {
        fprintf(stderr, "view: KL_VIEW_CPU=1 — readback path\n");
    } else if (!kl_env_on("KL_GLFB", 0)) {
        // No GL renderer was asked for, so there is no readback to fall back
        // to and nothing to bring ANGLE up for. The compositor path is the only
        // one that can show anything, and it finds the guest's eye texture by
        // itself whichever API produced it — which for a Vulkan guest is the
        // only way a picture exists at all.
        hw = 1;
        fprintf(stderr, "view: no GL renderer — the compositor will wait for an "
                        "eye texture (Vulkan)\n");
    } else if (!kl_glfb_mtl_device()) {
        fprintf(stderr, "view: no MTLDevice from ANGLE — readback path\n");
    } else {
        kl_mtl_provider_install();          // installs unconditionally under KL_VIEW
        hw = kl_glfb_has_mtl_provider();
    }
    if (!hw) kl_glfb_set_frame_sink(kl_view_frame_sink, NULL);

    // What the display actually runs at, before the guest can ask. Without this
    // every host run describes the Quest 2's 72 Hz panel no matter what is on
    // the desk, and a guest that paces a video stream against the number is
    // asking its far end for frames at a rate nothing here chose.
    //
    // It is a ceiling, not a promise: a heavy target can deliver a third of
    // this. The viewer's HUD prints achieved against advertised every second,
    // and KL_DISPLAY_HZ (read inside kl_ovrp_display_frequency, hence after
    // this push) is how a run pins the number to what it can really sustain.
    // For Steam Link the number is also the far end's encode rate, and the
    // pixel count is the host's choice, so every hertz here divides a bitrate
    // it chose into thinner frames.
    float panel_hz = kl_view_display_hz();
    if (panel_hz > 0.0f) kl_ovrp_set_display_frequency(panel_hz);
    fprintf(stderr, "view: display %.1f Hz%s; guest will be told %.1f Hz\n",
            (double)panel_hz, panel_hz > 0.0f ? "" : " (SDL could not say)",
            (double)kl_ovrp_display_frequency());

    // The guest's frame clock, for the guests that own their own frame loop.
    // Nothing else on the host paces one: the compositor takes the newest frame
    // and never blocks its producer, so an OpenXR guest spins as fast as it can
    // render — Steam Link's VR client ran at ~1000 Hz against a 120 Hz window,
    // paying every mirror blit eight times over and reporting frame pacing to
    // its streaming host against a clock nobody drove.
    //
    // Which pacer a guest wants is a property of the XR RUNTIME it drives, not
    // of the door it came through, and this is the same pairing the device app
    // makes. A Unity guest is NOT in this set: kl_driver_frame already calls its
    // frame from this side, so a pacer there would be the compositor waiting on
    // itself. KL_XR_PACE=0 restores the free-running loop as the A/B.
    int owns_loop = (TARGET->kind == KL_GUEST_STEAMLINK && DOOR == KL_SLINK_VR) ||
                    TARGET->kind == KL_GUEST_JKXR;
    if (owns_loop && kl_env_on("KL_XR_PACE", 1)) {
        kl_openxr_set_frame_pacer(kl_view_pace_wait);
        fprintf(stderr, "view: the guest owns its frame loop — paced by the "
                        "composite at %.1f Hz\n",
                (double)kl_ovrp_display_frequency());
    }

    pthread_t guest;
    if (pthread_create(&guest, NULL, view_guest_thread, NULL)) {
        fprintf(stderr, "view: pthread_create failed\n");
        return 1;
    }
    int rc = kl_view_main(LIBDIR, hw);   // returns when the window closes
    g_view_quit = 1;
    // The flat guest is inside its own main() and does not return on its own;
    // the process exiting is what ends it. Detaching rather than joining makes
    // closing the window close the app, which is what a window close means.
    if (TARGET->kind == KL_GUEST_STEAMLINK) pthread_detach(guest);
    else                                    pthread_join(guest, NULL);
    kl_driver_report(stdout);
    kl_metadump_watch_report(stdout);   // host-only, so not the driver's
    return rc;
}

int main(int argc, char **argv) {
    TARGET = kl_target_resolve(argc > 1 ? argv[1] : NULL);
    if (!TARGET) {
        fprintf(stderr, "FAIL: unknown target '%s' — one of: %s "
                        "(or a path to a guest lib directory)\n",
                argv[1], kl_target_names());
        return 1;
    }
    LIBDIR = TARGET->libdir;
    if (argc > 0 && argv[0] && *argv[0]) g_argv0 = argv[0];
    printf("=== target: %s (%s, %s) ===\n", TARGET->name, LIBDIR, TARGET->apk);

    // Everything the guest is told about itself, from one row: the library path,
    // the assets, the APK it opens as a zip, and the userdata directory its saves
    // land in. Steam Link is told the same four things plus what only it has —
    // which front door, the activity class that goes with it, Qt's plugin path
    // and the panel size — so its description comes from kl_slink instead.
    if (TARGET->kind == KL_GUEST_STEAMLINK) {
        DOOR = kl_slink_door_from_env();
        if (kl_env_on("KL_SLINK_VR", 0) && kl_env_on("KL_SLINK_SHELL", 0))
            printf("(KL_SLINK_VR and KL_SLINK_SHELL are different front doors; "
                   "taking VR)\n");
        // One profile for all three doors: the pairing credential the shell
        // stores is exactly what the VR door is handed at the handoff, so
        // splitting them would make the app re-pair against itself.
        if (kl_slink_configure(DOOR, LIBDIR, TARGET->assets, TARGET->apk,
                               kl_userdata_dir(TARGET->userdata), stdout) != 0)
            return fail(kl_slink_error());
        // Only the shell can reach startVRLink, and only it should be able to
        // hand off — installing this unconditionally would let a client or VR
        // run take a path neither of them has any business on.
        if (DOOR == KL_SLINK_SHELL) kl_jni_set_vrlink_handoff(slink_vrlink_handoff);
    } else {
        kl_target_apply_host(TARGET, NULL);
    }
    kl_driver_init(TARGET, LIBDIR, DOOR);

    // Re-entry of the re-exec'd recon child (see below).
    if (getenv("KL_RECON_CHILD"))
        return recon_run(0);

    if (getenv("KL_VIEW"))
        return view_run();

    printf("=== DRM policy guard ===\n");
    // Under a debugger the forked child inherits the parent's Mach exception
    // ports, so its abort is intercepted and never reads as a SIGABRT to
    // waitpid. KL_SKIP_GUARD_TEST skips this self-test when running under
    // lldb — the guard itself in kl_ovrplat.c is unaffected.
    if (!getenv("KL_SKIP_GUARD_TEST") && check_drm_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_spacewarp_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_eye_cap_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_eye_rung_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_mrc_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_vk_budget_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_vk_format_guard()) return 1;
    if (!getenv("KL_SKIP_GUARD_TEST") && check_vk_lazy_guard()) return 1;

    // ---- phase 2: reconnaissance, in a child ----
    // Unimplemented JNI slots abort the process on purpose, so this runs in a
    // child. Whatever it prints before dying is the work list.
    //
    // The child is re-EXEC'd, not a bare fork. Metal's shader compiler is an
    // XPC service that refuses forked children, and AGX treats the resulting
    // cold-compile failure of one of its OWN internal blit shaders as fatal
    // (the long-standing "AGX abort"): the failure only showed once the driver
    // needed a compile — a blit in texture setup — and only ever in the child,
    // which is why every standalone recipe was clean. exec resets the XPC
    // state, so the re-exec'd child compiles like any other process.
    // KL_NOFORK=1 skips the child entirely: macOS lldb does not follow exec
    // any better than fork, so debugging the guest needs the guest threads in
    // the traced process, and the debugger becomes the reporter.
    printf("\n=== recon: spawning child ===\n");
    fflush(NULL);
    if (getenv("KL_NOFORK"))
        return recon_run(0);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("KL_RECON_CHILD", "1", 1);
        execl(argv[0], argv[0], LIBDIR, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) {
        // How it stopped is the first question every time: an unimplemented JNI
        // slot aborts (SIGABRT after the report), SIGALRM means the guest
        // blocked, and anything else is a real fault in guest code.
        if (WIFSIGNALED(st))
            printf("\n  (recon stopped on signal %d — %s)\n", WTERMSIG(st),
                   strsignal(WTERMSIG(st)));
        else
            printf("\n  (recon child exited %d)\n", WEXITSTATUS(st));
        printf("  (see the JNI surface report above)\n");
        // Say what was OBSERVED, not what is usually true. This used to report
        // every non-zero child as "an unimplemented JNI call was reached", which
        // is right only for the abort-by-name path — and it named a JNI stop for
        // a run whose actual cause was CPython dying on a refused getrandom,
        // sending the search to the JNI surface report, where there was nothing
        // to find. A diagnostic that asserts a cause it has not established is
        // worse than one that only reports the exit.
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT)
            return fail("guest init did not complete: aborted — an unimplemented "
                        "JNI call or unresolved import (the report names it)");
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM)
            return fail("guest init did not complete: the watchdog fired — the "
                        "guest blocked (KL_ALARM widens it)");
        if (WIFSIGNALED(st))
            return fail("guest init did not complete: a fault in guest code — "
                        "read the fault line above, not the JNI report");
        return fail("guest init did not complete: the guest EXITED on its own "
                    "(no signal) — something in it called exit(); the reason is "
                    "in the log above, not in the JNI report");
    }
    // Named for the door this run actually took. `initJni` is UnityPlayer's and
    // means nothing on a NativeActivity guest — a diagnostic asserting something
    // it never established sends the next reader to the wrong report.
    printf("\n=== EXIT CRITERION MET: %s ===\n",
           TARGET->kind == KL_GUEST_UE4       ? "the NativeActivity lifecycle ran with no unimplemented JNI calls"
           : TARGET->kind == KL_GUEST_JKXR    ? "the GLES3JNILib lifecycle ran with no unimplemented JNI calls"
           : TARGET->kind == KL_GUEST_STEAMLINK ? "the Steam Link chain ran with no unimplemented JNI calls"
           : TARGET->kind == KL_GUEST_SDL      ? "the SDL door ran with no unimplemented JNI calls"
                                               : "initJni completed with no unimplemented JNI calls");
    return 0;
}
