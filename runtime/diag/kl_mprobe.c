#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_mprobe.h"

// ---- the slice of IL2CPP's embedding API this needs ----
// All exported by libil2cpp.so (checked with nm), so they resolve through the
// image registry like any other guest symbol.
typedef struct Il2CppDomain   Il2CppDomain;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage    Il2CppImage;
typedef struct Il2CppClass    Il2CppClass;
typedef struct MethodInfo     MethodInfo;
typedef struct Il2CppObject   Il2CppObject;

static struct {
    Il2CppDomain     *(*domain_get)(void);
    const Il2CppAssembly **(*domain_get_assemblies)(const Il2CppDomain *, size_t *);
    const Il2CppImage *(*assembly_get_image)(const Il2CppAssembly *);
    Il2CppClass      *(*class_from_name)(const Il2CppImage *, const char *, const char *);
    const MethodInfo *(*class_get_method_from_name)(Il2CppClass *, const char *, int);
    Il2CppObject     *(*runtime_invoke)(const MethodInfo *, void *, void **, Il2CppObject **);
    void             *(*object_unbox)(Il2CppObject *);
    Il2CppObject     *(*string_new)(const char *);
    uint32_t          (*array_length)(Il2CppObject *);
    Il2CppClass      *(*object_get_class)(Il2CppObject *);
    const char       *(*class_get_name)(Il2CppClass *);
    void             *(*class_get_type)(Il2CppClass *);
    Il2CppObject     *(*type_get_object)(void *);
} il;

// Managed object layouts (v24 / Unity 2019.4, LP64). Only the fields read here.
//   Il2CppObject { Il2CppClass *klass; void *monitor; }              16 B
//   Il2CppString { Il2CppObject; int32_t length; uint16_t chars[]; } chars at 20
//   Il2CppArray  { Il2CppObject; void *bounds; uintptr_t max_length; } data at 32
#define OBJ_HDR   16
#define STR_LEN   16
#define STR_CHARS 20
#define ARR_DATA  32

static int g_state;          // 0 = untried, 1 = up, -1 = unavailable

static int mprobe_init(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return 0;
#define BIND(field, sym) \
    do { *(void **)&il.field = kl_sym(img, sym); \
         if (!il.field) { fprintf(stderr, "  [mprobe] libil2cpp exports no %s\n", sym); \
                          return 0; } } while (0)
    BIND(domain_get,                "il2cpp_domain_get");
    BIND(domain_get_assemblies,     "il2cpp_domain_get_assemblies");
    BIND(assembly_get_image,        "il2cpp_assembly_get_image");
    BIND(class_from_name,           "il2cpp_class_from_name");
    BIND(class_get_method_from_name,"il2cpp_class_get_method_from_name");
    BIND(runtime_invoke,            "il2cpp_runtime_invoke");
    BIND(object_unbox,              "il2cpp_object_unbox");
    BIND(string_new,                "il2cpp_string_new");
    BIND(array_length,              "il2cpp_array_length");
    BIND(object_get_class,          "il2cpp_object_get_class");
    BIND(class_get_name,            "il2cpp_class_get_name");
    BIND(class_get_type,            "il2cpp_class_get_type");
    BIND(type_get_object,           "il2cpp_type_get_object");
#undef BIND
    g_state = 1;
    return 1;
}

// Search every loaded assembly rather than naming one: which module a type
// lives in is a Unity-version detail (Input moved to UnityEngine.CoreModule,
// InputTracking to UnityEngine.XRModule), and 87 images is a cheap scan done
// once per method.
static Il2CppClass *find_class(const char *ns, const char *name) {
    Il2CppDomain *dom = il.domain_get();
    if (!dom) return NULL;
    size_t n = 0;
    const Il2CppAssembly **as = il.domain_get_assemblies(dom, &n);
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage *im = il.assembly_get_image(as[i]);
        if (!im) continue;
        Il2CppClass *k = il.class_from_name(im, ns, name);
        if (k) return k;
    }
    return NULL;
}

static const MethodInfo *find_method(const char *ns, const char *cls,
                                     const char *meth, int argc) {
    Il2CppClass *k = find_class(ns, cls);
    if (!k) { fprintf(stderr, "  [mprobe] no class %s.%s\n", ns, cls); return NULL; }
    const MethodInfo *m = il.class_get_method_from_name(k, meth, argc);
    if (!m) fprintf(stderr, "  [mprobe] %s.%s has no %s/%d\n", ns, cls, meth, argc);
    return m;
}

// Invoke, reporting a managed throw by class name instead of letting it
// escape. A probe that aborts the run it is measuring is worse than no probe.
static Il2CppObject *invoke_on(Il2CppObject *self, const MethodInfo *m,
                               void **args, const char *what) {
    if (!m) return NULL;
    Il2CppObject *exc = NULL;
    Il2CppObject *r = il.runtime_invoke(m, self, args, &exc);
    if (exc) {
        Il2CppClass *k = il.object_get_class(exc);
        fprintf(stderr, "  [mprobe] %s threw %s\n", what,
                k ? il.class_get_name(k) : "?");
        return NULL;
    }
    return r;
}

static Il2CppObject *invoke(const MethodInfo *m, void **args, const char *what) {
    return invoke_on(NULL, m, args, what);   // static methods
}

static float invoke_float(const MethodInfo *m, void **args, const char *what) {
    Il2CppObject *r = invoke(m, args, what);
    if (!r) return 0.0f;
    return *(float *)il.object_unbox(r);
}

// Managed string -> ASCII, for the joystick names.
static void str_ascii(Il2CppObject *s, char *out, size_t cap) {
    if (!s) { snprintf(out, cap, "(null)"); return; }
    int32_t len = *(int32_t *)((uint8_t *)s + STR_LEN);
    const uint16_t *ch = (const uint16_t *)((uint8_t *)s + STR_CHARS);
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 1 < cap; i++)
        out[o++] = (ch[i] >= 0x20 && ch[i] < 0x7f) ? (char)ch[i] : '?';
    out[o] = 0;
}

// ---- KL_PROBE_ENUM: read an enum's VALUES out of the running guest --------
//
// The values of a C# enum are the one thing about the guest's ABI that is
// neither in its exported symbols nor in its strings: global-metadata.dat holds
// the names, and the numbers live in a constant table whose layout is a
// metadata-version detail. But the runtime knows them, and it will answer
// through ordinary reflection — `Enum.GetNames(t)` and `Enum.GetValues(t)`.
//
// This exists because kl_ovrplat has to hand the guest a MESSAGE, and a message
// carries `Message.MessageType`, which the guest's own `ParseMessageHandle`
// switches on: a number we guessed lands in its `default` arm, which logs
// "Unrecognized message type" and produces no message at all. Same argument as
// OVRP_HEADSET_OCULUS_QUEST_2 in kl_ovrp.c — read the value out of the guest
// rather than out of a header we do not have — with reflection instead of a
// metadata scan, because that also works for a nested type.
//
//   KL_PROBE_ENUM="Oculus.Platform.Message/MessageType,Oculus.Platform.User..."
//
// Comma-separated, `Namespace.Type` (nested types with `/`), printed once at
// the first probe tick. Diagnostic only; nothing in the shipping path reads it.
static void probe_dump_enum(const char *spec) {
    char ns[128] = "", name[128] = "";
    const char *dot = strrchr(spec, '.');
    if (dot) {
        size_t nl = (size_t)(dot - spec);
        if (nl >= sizeof ns) nl = sizeof ns - 1;
        memcpy(ns, spec, nl); ns[nl] = 0;
        snprintf(name, sizeof name, "%s", dot + 1);
    } else {
        snprintf(name, sizeof name, "%s", spec);   // the global namespace
    }
    Il2CppClass *k = find_class(ns, name);
    if (!k) { fprintf(stderr, "  [mprobe] enum: no class %s.%s\n", ns, name); return; }
    void         *ty  = il.class_get_type(k);
    Il2CppObject *tyo = ty ? il.type_get_object(ty) : NULL;
    if (!tyo) { fprintf(stderr, "  [mprobe] enum: no System.Type for %s\n", spec); return; }

    const MethodInfo *m_names  = find_method("System", "Enum", "GetNames", 1);
    const MethodInfo *m_values = find_method("System", "Enum", "GetValues", 1);
    void *args[1] = { tyo };
    Il2CppObject *names  = invoke(m_names,  args, "Enum.GetNames");
    Il2CppObject *values = invoke(m_values, args, "Enum.GetValues");
    if (!names || !values) return;
    uint32_t n = il.array_length(names);
    // GetValues returns an array OF THE ENUM TYPE, so its elements are the
    // underlying integer, not boxed objects. Four bytes unless the enum has an
    // explicit wider base, which none of the ones this exists for do — the
    // stride is checked against the array length rather than assumed, so a
    // long-backed enum prints as such instead of silently reading halves.
    const uint8_t *vdata = (const uint8_t *)values + ARR_DATA;
    const void   **ndata = (const void **)((const uint8_t *)names + ARR_DATA);
    fprintf(stderr, "  [mprobe] enum %s: %u value(s)\n", spec, n);
    for (uint32_t i = 0; i < n; i++) {
        char nm[128];
        str_ascii((Il2CppObject *)ndata[i], nm, sizeof nm);
        int32_t v; memcpy(&v, vdata + (size_t)i * 4, sizeof v);
        fprintf(stderr, "  [mprobe]   %-56s = %d (0x%08x)\n", nm, v, (unsigned)v);
    }
}

// Public, and callable BEFORE the frame pump exists — see kl_mprobe.h. The
// gating below (KL_PROBE_INPUT, KL_PROBE_FROM=1200) is right for a probe that
// reads live input state and wrong for this one: an enum's values are fixed the
// moment the runtime is up, and a guest that never reaches frame 1200 on the
// host (Arkham Shadow stops at initJni there) could not be asked at all.
void kl_mprobe_enums(void) {
    const char *spec = kl_env_str("KL_PROBE_ENUM", NULL);
    if (!spec || !*spec) return;
    if (!mprobe_init()) { fprintf(stderr, "  [mprobe] enum: IL2CPP not up\n"); return; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf, *tok; (tok = strsep(&p, ",")) != NULL; )
        if (*tok) probe_dump_enum(tok);
}

// The axes this title binds (parsed out of globalgamemanagers).
static const char *const AXES[] = {
    "TriggerLeftHand", "TriggerRightHand",
    "HorizontalLeftHand", "VerticalLeftHand",
    "HorizontalRightHand", "VerticalRightHand",
    "MenuButtonLeftHand", "MenuButtonRightHand",
};
#define N_AXES (sizeof AXES / sizeof AXES[0])

// ---- KL_PROBE_STACKTRACE: make Unity's own exception log say WHERE ---------
//
// A managed exception that prints its message and nothing else is a name
// without an address. BONELAB throws `ArgumentOutOfRangeException` once a
// frame out of Unity's SRP `XRSystem` and the log carries three blank lines
// where the stack should be — which has two very different causes, and they
// are not distinguishable from the native side: either the build strips
// managed stack traces, or the game called `Application.SetStackTraceLogType`
// to turn them off. `Application.GetStackTraceLogType` answers that, so this
// PRINTS the setting it found before changing it: a probe that reports
// "already ScriptOnly" is telling you the traces are genuinely unavailable and
// that this instrument is a dead end, which is worth as much as a trace.
//
// StackTraceLogType { None = 0, ScriptOnly = 1, Full = 2 } and
// LogType { Error = 0, Assert = 1, Warning = 2, Log = 3, Exception = 4 } — both
// are UnityEngine enums stable since 5.x. Enums marshal as their underlying
// int, so il2cpp_runtime_invoke takes the ADDRESS of each value.
//
// Diagnostic only, and it changes the guest's logging configuration, so it is
// off unless asked for.
static void probe_stack_traces(void) {
    static const char *const LOGTYPE[] = { "Error", "Assert", "Warning", "Log",
                                           "Exception" };
    static const char *const STLT[] = { "None", "ScriptOnly", "Full" };

    const MethodInfo *m_get = find_method("UnityEngine", "Application",
                                          "GetStackTraceLogType", 1);
    const MethodInfo *m_set = find_method("UnityEngine", "Application",
                                          "SetStackTraceLogType", 2);
    if (!m_get || !m_set) return;

    // What the guest asked for, before we touch it.
    int want = (int)kl_env_uint("KL_PROBE_STACKTRACE", 1);
    if (want < 0 || want > 2) want = 1;
    for (int lt = 0; lt < 5; lt++) {
        int arg = lt;
        void *ga[1] = { &arg };
        Il2CppObject *r = invoke(m_get, ga, "Application.GetStackTraceLogType");
        int had = r ? *(int32_t *)il.object_unbox(r) : -1;
        int set = want;
        void *sa[2] = { &arg, &set };
        invoke(m_set, sa, "Application.SetStackTraceLogType");
        fprintf(stderr, "  [mprobe] stack traces for %-9s: %s -> %s\n",
                LOGTYPE[lt],
                (had >= 0 && had <= 2) ? STLT[had] : "?",
                STLT[want]);
    }
}

// ---- KL_PROBE_XR: what Unity thinks the XR display IS ---------------------
//
// Every number on this path is one WE answered — the eye texture size through
// ovrp_CalculateEyeLayerDesc, the layout through ovrp_GetSystemMultiViewSupported,
// the viewport through ovrp_CalculateEyeViewportRect — and they travel through
// libunity and the Oculus XR provider before Unity's own render loop reads them
// back. That round trip is where they can stop agreeing, and nothing on the
// native side can see the far end of it: our log says what we said, not what
// Unity concluded. `XRSettings` is Unity's conclusion, in Unity's words.
//
// The reason this exists: BONELAB throws `ArgumentOutOfRangeException` once per
// frame out of the SRP render loop with an EMPTY stack trace (the build carries
// none — `KL_PROBE_STACKTRACE` measured that and is a dead end), so the throw
// site cannot be named from either side. What can be named is a disagreement,
// and the first candidate is the stereo mode: a title authored for Single Pass
// Instanced whose display hands back two separate 2D textures is a shape its
// renderer may never have run.
static void probe_xr_display(unsigned frame) {
    // UnityEngine.XR.StereoRenderingMode { MultiPass = 0, SinglePass = 1,
    // SinglePassInstanced = 2, SinglePassMultiview = 3 }.
    static const char *const SRM[] = { "MultiPass", "SinglePass",
                                       "SinglePassInstanced",
                                       "SinglePassMultiview" };
    static const MethodInfo *m_mode, *m_w, *m_h, *m_vps, *m_en, *m_dev, *m_occ;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_mode = find_method("UnityEngine.XR", "XRSettings",
                             "get_stereoRenderingMode", 0);
        m_w    = find_method("UnityEngine.XR", "XRSettings", "get_eyeTextureWidth", 0);
        m_h    = find_method("UnityEngine.XR", "XRSettings", "get_eyeTextureHeight", 0);
        m_vps  = find_method("UnityEngine.XR", "XRSettings",
                             "get_renderViewportScale", 0);
        m_en   = find_method("UnityEngine.XR", "XRSettings", "get_enabled", 0);
        m_dev  = find_method("UnityEngine.XR", "XRSettings", "get_loadedDeviceName", 0);
        m_occ  = find_method("UnityEngine.XR", "XRSettings",
                             "get_useOcclusionMesh", 0);
    }
    Il2CppObject *mo = invoke(m_mode, NULL, "XRSettings.stereoRenderingMode");
    Il2CppObject *wo = invoke(m_w,    NULL, "XRSettings.eyeTextureWidth");
    Il2CppObject *ho = invoke(m_h,    NULL, "XRSettings.eyeTextureHeight");
    Il2CppObject *so = invoke(m_vps,  NULL, "XRSettings.renderViewportScale");
    Il2CppObject *eo = invoke(m_en,   NULL, "XRSettings.enabled");
    Il2CppObject *oo = invoke(m_occ,  NULL, "XRSettings.useOcclusionMesh");
    Il2CppObject *dv = invoke(m_dev,  NULL, "XRSettings.loadedDeviceName");
    int mode = mo ? *(int32_t *)il.object_unbox(mo) : -1;
    char dn[128] = "?";
    if (dv) str_ascii(dv, dn, sizeof dn);
    fprintf(stderr, "  [mprobe f%u] XRSettings: device=\"%s\" enabled=%s "
                    "stereo=%d (%s) eyeTexture=%dx%d viewportScale=%.3f "
                    "occlusionMesh=%s\n",
            frame, dn,
            eo ? (*(uint8_t *)il.object_unbox(eo) ? "1" : "0") : "?",
            mode, (mode >= 0 && mode <= 3) ? SRM[mode] : "?",
            wo ? *(int32_t *)il.object_unbox(wo) : -1,
            ho ? *(int32_t *)il.object_unbox(ho) : -1,
            so ? (double)*(float *)il.object_unbox(so) : -1.0,
            oo ? (*(uint8_t *)il.object_unbox(oo) ? "1" : "0") : "?");
}

// ---------------------------------------------------------------------------
// KL_PROBE_RESOLVE=0xoff[,0xoff...]: name the managed method CONTAINING each
// given libil2cpp offset. The sampler reports parked pcs as libil2cpp+0x...;
// on metadata v29 its own resolver cannot name them, but the RUNTIME can:
// enumerate every image -> class -> method through the embedding API, read
// each MethodInfo's methodPointer (its first field - stable across il2cpp
// versions), and keep the nearest method at or below each target. One pass,
// a few seconds, printed once.
static void mprobe_resolve_offsets(void) {
    const char *spec = kl_env_str("KL_PROBE_RESOLVE", NULL);
    if (!spec || !*spec) return;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return;
    typedef const MethodInfo *(*get_methods_fn)(Il2CppClass *, void **);
    typedef size_t (*class_count_fn)(const Il2CppImage *);
    typedef Il2CppClass *(*get_class_fn)(const Il2CppImage *, size_t);
    typedef const char *(*m_name_fn)(const MethodInfo *);
    typedef const char *(*i_name_fn)(const Il2CppImage *);
    get_methods_fn get_methods = (get_methods_fn)kl_sym(img, "il2cpp_class_get_methods");
    class_count_fn class_count = (class_count_fn)kl_sym(img, "il2cpp_image_get_class_count");
    get_class_fn   get_class   = (get_class_fn)kl_sym(img, "il2cpp_image_get_class");
    m_name_fn      m_name      = (m_name_fn)kl_sym(img, "il2cpp_method_get_name");
    i_name_fn      i_name      = (i_name_fn)kl_sym(img, "il2cpp_image_get_name");
    if (!get_methods || !class_count || !get_class || !m_name) {
        fprintf(stderr, "  [mprobe] resolve: enumeration exports missing\n");
        return;
    }
    uintptr_t base = (uintptr_t)kl_base(img);
    enum { MAXT = 8 };
    uintptr_t tgt[MAXT]; int nt = 0;
    for (const char *p = spec; *p && nt < MAXT; ) {
        tgt[nt++] = base + (uintptr_t)strtoull(p, NULL, 16);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    struct { uintptr_t best; const MethodInfo *m; Il2CppClass *k; const Il2CppImage *im; } hit[MAXT];
    memset(hit, 0, sizeof hit);
    size_t na = 0;
    const Il2CppAssembly **as = il.domain_get_assemblies(il.domain_get(), &na);
    unsigned long long nmeth = 0;
    for (size_t a = 0; a < na; a++) {
        const Il2CppImage *im = il.assembly_get_image(as[a]);
        if (!im) continue;
        size_t nc = class_count(im);
        for (size_t c = 0; c < nc; c++) {
            Il2CppClass *k = get_class(im, c);
            if (!k) continue;
            void *iter = NULL;
            const MethodInfo *m;
            while ((m = get_methods(k, &iter)) != NULL) {
                nmeth++;
                uintptr_t p = *(uintptr_t *)m;   // MethodInfo.methodPointer
                if (!p) continue;
                for (int t = 0; t < nt; t++)
                    if (p <= tgt[t] && p > hit[t].best) {
                        hit[t].best = p; hit[t].m = m; hit[t].k = k; hit[t].im = im;
                    }
            }
        }
    }
    fprintf(stderr, "  [mprobe] resolve: scanned %llu methods in %zu assemblies\n",
            nmeth, na);
    for (int t = 0; t < nt; t++) {
        if (!hit[t].m) { fprintf(stderr, "  [mprobe] resolve %#llx: no method at or below\n",
                                 (unsigned long long)(tgt[t] - base)); continue; }
        fprintf(stderr, "  [mprobe] resolve libil2cpp+%#llx -> %s::%s (+%#llx, image %s)\n",
                (unsigned long long)(tgt[t] - base),
                il.class_get_name(hit[t].k), m_name(hit[t].m),
                (unsigned long long)(tgt[t] - hit[t].best),
                i_name && hit[t].im ? i_name(hit[t].im) : "?");
    }
}

void kl_mprobe_tick(unsigned frame) {
    // Independent of KL_PROBE_INPUT and of KL_PROBE_FROM: this one has to land
    // BEFORE the throw it exists to describe, and the throw here is on the
    // first frame. It runs once, on the first tick that finds IL2CPP up.
    static int st_done;
    if (!st_done && kl_env_str("KL_PROBE_STACKTRACE", NULL) && mprobe_init()) {
        st_done = 1;
        probe_stack_traces();
    }
    // pc->method naming, once, late enough that the game's assemblies are all
    // loaded (frame 600 ~= ten seconds in).
    static int rs_done;
    if (!rs_done && frame >= 600 && kl_env_str("KL_PROBE_RESOLVE", NULL) && mprobe_init()) {
        rs_done = 1;
        mprobe_resolve_offsets();
    }
    // Also independent, and for the same reason: the disagreement it looks for
    // is settled long before KL_PROBE_FROM's default, and reading XRSettings
    // runs no cctor the guest has not already run.
    static unsigned xr_from, xr_every;
    if (kl_env_str("KL_PROBE_XR", NULL)) {
        if (!xr_every) {
            xr_from  = kl_env_uint("KL_PROBE_XR_FROM", 60u);
            xr_every = kl_env_uint("KL_PROBE_XR_EVERY", 300u);
            if (!xr_every) xr_every = 300;
        }
        if (frame >= xr_from && frame % xr_every == 0 && mprobe_init())
            probe_xr_display(frame);
    }

    static int on = -1;
    if (on < 0) on = kl_env_on("KL_PROBE_INPUT", 0);
    // ...and act on it. This gate was computed and then not consulted, so the
    // probe — which the header and the comment below both describe as off by
    // default, BECAUSE it has side effects — ran on every host boot past frame
    // 1200 of every title. It was still running the day it stopped being
    // harmless: it is in the stack of the out-of-memory abort SUPERHOT hit the
    // first time it got far enough to load its main scene.
    if (!on) return;
    static unsigned every, from;
    if (!every) {
        every = kl_env_uint("KL_PROBE_EVERY", 120);
        if (!every) every = 120;
        // Probing is not free of side effects: the first call into a managed
        // class runs its cctor, and doing that to OVRInput/OVRPlugin before the
        // guest has loaded the plugin drags the runtime down paths it would not
        // take on its own (one such run aborted on an ovrp entry point the game
        // reaches only from there). Wait for the game to be up.
        from = kl_env_uint("KL_PROBE_FROM", 1200u);
    }
    if (frame < from || frame % every) return;
    if (!mprobe_init()) { on = 0; return; }

    static int dumped_enums;
    if (!dumped_enums) { dumped_enums = 1; kl_mprobe_enums(); }

    // Input.GetJoystickNames and InputTracking.GetLocalPosition/Rotation are
    // NOT here: IL2CPP's managed stripping removed them, which is itself the
    // measurement — this title reaches poses through OVRInput, below.
    static const MethodInfo *m_axis;
    static const MethodInfo *m_cpos, *m_crot, *m_conn, *m_pvalid, *m_ptracked;
    static Il2CppObject *axis_str[N_AXES];
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_axis  = find_method("UnityEngine", "Input", "GetAxis", 1);
        // The game's own pose seam. VRController::Update asks
        // IVRPlatformHelper::GetNodePose, which lands in OVRInput, which asks
        // OVRNodeStateProperties -> InputTracking::GetNodeStates -> libunity's
        // node table -> our ovrp node poses. Static, so callable from here.
        m_cpos     = find_method("", "OVRInput", "GetLocalControllerPosition", 1);
        m_crot     = find_method("", "OVRInput", "GetLocalControllerRotation", 1);
        m_conn     = find_method("", "OVRInput", "GetConnectedControllers", 0);
        m_pvalid   = find_method("", "OVRInput", "GetControllerPositionValid", 1);
        m_ptracked = find_method("", "OVRInput", "GetControllerPositionTracked", 1);
        for (unsigned i = 0; i < N_AXES; i++) axis_str[i] = il.string_new(AXES[i]);
    }

    // Which VR device does Unity think is loaded? This title ships four
    // IVRPlatformHelper implementations (Oculus, OpenVR, PSVR and
    // Deviceless), and DevicelessVRHelper::GetNodePose reports no pose BY
    // DESIGN — a game running on that helper has a working menu, working
    // Input.GetAxis and no controllers at all, which is exactly the symptom.
    // The choice keys on this surface, and OculusVRHelper also picks its
    // controller-transform offset from the device model.
    static const MethodInfo *m_devname, *m_xrenabled, *m_present, *m_model;
    static int resolved_xr;
    if (!resolved_xr) {
        resolved_xr = 1;
        m_devname   = find_method("UnityEngine.XR", "XRSettings", "get_loadedDeviceName", 0);
        m_xrenabled = find_method("UnityEngine.XR", "XRSettings", "get_enabled", 0);
        m_present   = find_method("UnityEngine.XR", "XRDevice", "get_isPresent", 0);
        m_model     = find_method("UnityEngine.XR", "XRDevice", "get_model", 0);
    }
    // Focus, as MANAGED OVRPlugin computes it. The game acts on these: it has
    // inputFocusWasCaptured/Released events and hides its menu controllers
    // while focus is away. Every one of these getters is a candidate for the
    // ovrpResult-vs-ovrpBool trap that made node validity read false.
    static const MethodInfo *m_infocus, *m_vrfocus, *m_userp;
    static int resolved_focus;
    if (!resolved_focus) {
        resolved_focus = 1;
        m_infocus = find_method("", "OVRPlugin", "get_hasInputFocus", 0);
        m_vrfocus = find_method("", "OVRPlugin", "get_hasVrFocus", 0);
        m_userp   = find_method("", "OVRPlugin", "get_userPresent", 0);
    }
    {
        Il2CppObject *a = invoke(m_infocus, NULL, "OVRPlugin.hasInputFocus");
        Il2CppObject *b = invoke(m_vrfocus, NULL, "OVRPlugin.hasVrFocus");
        Il2CppObject *c2 = invoke(m_userp, NULL, "OVRPlugin.userPresent");
        fprintf(stderr, "  [mprobe f%u] focus: input=%s vr=%s userPresent=%s\n", frame,
                a ? (*(uint8_t *)il.object_unbox(a) ? "1" : "0") : "?",
                b ? (*(uint8_t *)il.object_unbox(b) ? "1" : "0") : "?",
                c2 ? (*(uint8_t *)il.object_unbox(c2) ? "1" : "0") : "?");
    }
    {
        char dn[128] = "?", md[128] = "?";
        Il2CppObject *d = invoke(m_devname, NULL, "XRSettings.loadedDeviceName");
        Il2CppObject *m = invoke(m_model, NULL, "XRDevice.model");
        if (d) str_ascii(d, dn, sizeof dn);
        if (m) str_ascii(m, md, sizeof md);
        Il2CppObject *en = invoke(m_xrenabled, NULL, "XRSettings.enabled");
        Il2CppObject *pr = invoke(m_present, NULL, "XRDevice.isPresent");
        fprintf(stderr, "  [mprobe f%u] xr: device=\"%s\" model=\"%s\" enabled=%s present=%s\n",
                frame, dn, md,
                en ? (*(uint8_t *)il.object_unbox(en) ? "1" : "0") : "?",
                pr ? (*(uint8_t *)il.object_unbox(pr) ? "1" : "0") : "?");
    }

    // The game's own controller objects, read straight out of the live scene.
    // FindObjectsOfType returns only components on ACTIVE objects while
    // Resources.FindObjectsOfTypeAll includes inactive ones, so the pair
    // separates "the controllers were never built" from "they exist and
    // something disabled them" — which is what "no saber hilts render" is
    // asking. VRController::get_active is the game's own verdict on whether
    // its node has a pose.
    // KL_PROBE_TYPES: comma-separated managed types to census, each as
    // Namespace.Type (bare name for the global namespace). The default set is
    // the menu pointer chain.
    static const MethodInfo *m_find, *m_findall, *m_vactive, *m_vnode, *m_vpos;
    static Il2CppObject *vrc_type;
    static Il2CppObject *types[12];
    static char          tnames[12][64];
    static unsigned      n_types;
    static int resolved_scene;
    if (!resolved_scene) {
        resolved_scene = 1;
        m_find    = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_findall = find_method("UnityEngine", "Resources", "FindObjectsOfTypeAll", 1);
        m_vactive = find_method("", "VRController", "get_active", 0);
        m_vnode   = find_method("", "VRController", "get_node", 0);
        m_vpos    = find_method("", "VRController", "get_position", 0);
        Il2CppClass *k = find_class("", "VRController");
        if (k) vrc_type = il.type_get_object(il.class_get_type(k));

        const char *spec = kl_env_str("KL_PROBE_TYPES", "VRUIControls.VRPointer,VRUIControls.VRInputModule,"
                                                        "VRUIControls.VRLaserPointer,VRUIControls.VRGraphicRaycaster");
        char buf[512];
        snprintf(buf, sizeof buf, "%s", spec);
        for (char *tok = strtok(buf, ","); tok && n_types < 12;
             tok = strtok(NULL, ",")) {
            char ns[64] = "", nm[64];
            const char *dot = strrchr(tok, '.');
            if (dot) {
                size_t l = (size_t)(dot - tok);
                if (l >= sizeof ns) l = sizeof ns - 1;
                memcpy(ns, tok, l); ns[l] = 0;
                snprintf(nm, sizeof nm, "%s", dot + 1);
            } else {
                snprintf(nm, sizeof nm, "%s", tok);
            }
            Il2CppClass *kk = find_class(ns, nm);
            if (!kk) continue;
            snprintf(tnames[n_types], sizeof tnames[0], "%s", nm);
            types[n_types++] = il.type_get_object(il.class_get_type(kk));
        }
    }
    if (n_types) {
        fprintf(stderr, "  [mprobe f%u] scene:", frame);
        for (unsigned i = 0; i < n_types; i++) {
            void *ta[1] = { types[i] };
            Il2CppObject *a = invoke(m_find, ta, "FindObjectsOfType");
            Il2CppObject *b = invoke(m_findall, ta, "FindObjectsOfTypeAll");
            fprintf(stderr, " %s=%d/%d", tnames[i],
                    a ? (int)il.array_length(a) : -1,
                    b ? (int)il.array_length(b) : -1);
        }
        fprintf(stderr, "  (active/all)\n");
    }
    // Walk each VRController up its parent chain, naming every object and
    // whether it is switched off in its own right (activeSelf). An object can
    // be inactive because a PARENT is inactive, and only the ancestor that is
    // actually off is the thing to explain — the rest are consequences.
    static const MethodInfo *m_go, *m_tf, *m_parent, *m_aself, *m_ahier, *m_ptr_ctrl;
    static const MethodInfo *m_objname;
    static int resolved_walk;
    if (!resolved_walk) {
        resolved_walk = 1;
        m_objname = find_method("UnityEngine", "Object", "get_name", 0);
        m_go     = find_method("UnityEngine", "Component", "get_gameObject", 0);
        m_tf     = find_method("UnityEngine", "Component", "get_transform", 0);
        m_parent = find_method("UnityEngine", "Transform", "get_parent", 0);
        m_aself  = find_method("UnityEngine", "GameObject", "get_activeSelf", 0);
        m_ahier  = find_method("UnityEngine", "GameObject", "get_activeInHierarchy", 0);
        m_ptr_ctrl = find_method("VRUIControls", "VRPointer", "get_vrController", 0);
    }
    // Which controller does the live pointer actually want? A null here means
    // the pointer was never wired to one, which is a different bug from a
    // controller that exists and is switched off.
    if (vrc_type && m_ptr_ctrl && n_types) {
        void *ta[1] = { types[0] };            // VRPointer is first by default
        Il2CppObject *ps = invoke(m_find, ta, "FindObjectsOfType(VRPointer)");
        if (ps && il.array_length(ps)) {
            Il2CppObject *p0 = ((Il2CppObject **)((uint8_t *)ps + ARR_DATA))[0];
            Il2CppObject *vc = invoke_on(p0, m_ptr_ctrl, NULL, "VRPointer.vrController");
            fprintf(stderr, "  [mprobe f%u] VRPointer.vrController=%s\n", frame,
                    vc ? "set" : "<null>");
        }
    }
    if (vrc_type) {
        void *targs[1] = { vrc_type };
        Il2CppObject *act = invoke(m_find, targs, "Object.FindObjectsOfType");
        Il2CppObject *all = invoke(m_findall, targs, "Resources.FindObjectsOfTypeAll");
        fprintf(stderr, "  [mprobe f%u] VRController: active=%d all=%d", frame,
                act ? (int)il.array_length(act) : -1,
                all ? (int)il.array_length(all) : -1);
        // Report each one the game built, however it was found.
        Il2CppObject *list = all ? all : act;
        uint32_t n = list ? il.array_length(list) : 0;
        for (uint32_t i = 0; i < n && i < 4; i++) {
            Il2CppObject *vc = ((Il2CppObject **)((uint8_t *)list + ARR_DATA))[i];
            if (!vc) continue;
            Il2CppObject *a = invoke_on(vc, m_vactive, NULL, "VRController.active");
            Il2CppObject *nd = invoke_on(vc, m_vnode, NULL, "VRController.node");
            Il2CppObject *p = invoke_on(vc, m_vpos, NULL, "VRController.position");
            fprintf(stderr, " | node=%s active=%s",
                    nd ? (*(int32_t *)il.object_unbox(nd) == 4 ? "L" :
                          *(int32_t *)il.object_unbox(nd) == 5 ? "R" : "?") : "?",
                    a ? (*(uint8_t *)il.object_unbox(a) ? "YES" : "no") : "?");
            if (p) {
                const float *v = (const float *)il.object_unbox(p);
                fprintf(stderr, " pos=(%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
            }
            fprintf(stderr, "\n      chain:");
            // Up the parents, naming each and flagging the ones switched off.
            Il2CppObject *tf = invoke_on(vc, m_tf, NULL, "Component.transform");
            for (unsigned d = 0; tf && d < 12; d++) {
                Il2CppObject *go = invoke_on(tf, m_go, NULL, "Component.gameObject");
                if (!go) break;
                Il2CppObject *nm2 = invoke_on(go, m_objname, NULL, "Object.get_name");
                Il2CppObject *as = invoke_on(go, m_aself, NULL, "activeSelf");
                Il2CppObject *ah = invoke_on(go, m_ahier, NULL, "activeInHierarchy");
                char nb[96];
                str_ascii(nm2, nb, sizeof nb);
                int self = as ? *(uint8_t *)il.object_unbox(as) : -1;
                int hier = ah ? *(uint8_t *)il.object_unbox(ah) : -1;
                fprintf(stderr, " %s%s[self=%d,hier=%d]", d ? "< " : "", nb, self, hier);
                tf = invoke_on(tf, m_parent, NULL, "Transform.parent");
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
    }

    // The other end of the chain: does the pointer ray actually land on UI?
    // Everything above measures what we hand the game; this reads back what
    // the game's own event system concluded, which is the difference between
    // "the controllers are wrong" and "the controllers are fine and the ray is
    // aimed somewhere else".
    static const MethodInfo *m_es_cur, *m_over, *m_sel, *m_name;
    static int resolved_es;
    if (!resolved_es) {
        resolved_es = 1;
        m_es_cur = find_method("UnityEngine.EventSystems", "EventSystem", "get_current", 0);
        m_over   = find_method("UnityEngine.EventSystems", "EventSystem",
                               "IsPointerOverGameObject", 0);
        m_sel    = find_method("UnityEngine.EventSystems", "EventSystem",
                               "get_currentSelectedGameObject", 0);
        m_name   = find_method("UnityEngine", "Object", "get_name", 0);
    }

    // 1. Does any of the fill survive into Input.GetAxis? This was the one
    //    unproven link in the chain from our ovrp state to the game's click.
    //    Gated behind its OWN knob (default off) even under KL_PROBE_INPUT: on a
    //    title whose Input Manager has no axis of the queried name, il2cpp's
    //    Input.GetAxis walks off a buffer inside libunity and SIGSEGVs — an
    //    intermittent native fault the managed try/catch cannot stop, and it
    //    killed some Unity guests mid-run. The answer here is always
    //    "InvalidOperationException" anyway, so it is off unless asked for.
    if (kl_env_on("KL_PROBE_GETAXIS", 0)) {
        fprintf(stderr, "  [mprobe f%u] axes:", frame);
        for (unsigned i = 0; i < N_AXES; i++) {
            void *args[1] = { axis_str[i] };
            fprintf(stderr, " %s=%.3f", AXES[i], invoke_float(m_axis, args, "Input.GetAxis"));
        }
        fprintf(stderr, "\n");
    }

    // 2. And do the hand poses? The pointer ray is built from these, so a ray
    //    that never hits is either aimed wrong or cast from nowhere. Asked
    //    exactly the way VRController::Update asks it.
    Il2CppObject *c = invoke(m_conn, NULL, "OVRInput.GetConnectedControllers");
    fprintf(stderr, "  [mprobe f%u] OVRInput: connected=", frame);
    if (c) fprintf(stderr, "0x%x", *(uint32_t *)il.object_unbox(c));
    else   fprintf(stderr, "?");

    // OVRInput.Controller: LTouch = 0x01, RTouch = 0x02.
    static const struct { const char *name; int ctrl; } HANDS[] = {
        { "L", 0x01 }, { "R", 0x02 },
    };
    for (unsigned i = 0; i < 2; i++) {
        int ctrl = HANDS[i].ctrl;
        void *args[1] = { &ctrl };
        Il2CppObject *p  = invoke(m_cpos, args, "OVRInput.GetLocalControllerPosition");
        Il2CppObject *q  = invoke(m_crot, args, "OVRInput.GetLocalControllerRotation");
        Il2CppObject *pv = invoke(m_pvalid, args, "OVRInput.GetControllerPositionValid");
        Il2CppObject *pt = invoke(m_ptracked, args, "OVRInput.GetControllerPositionTracked");
        fprintf(stderr, " %s:", HANDS[i].name);
        if (pv) fprintf(stderr, "valid=%d", *(uint8_t *)il.object_unbox(pv));
        if (pt) fprintf(stderr, ",tracked=%d", *(uint8_t *)il.object_unbox(pt));
        if (p) {
            const float *v = (const float *)il.object_unbox(p);
            fprintf(stderr, ",pos=(%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
        }
        if (q) {
            const float *r = (const float *)il.object_unbox(q);
            fprintf(stderr, ",rot=(%.2f,%.2f,%.2f,%.2f)", r[0], r[1], r[2], r[3]);
        }
    }
    fprintf(stderr, "\n");

    // 3. The verdict: is the pointer over UI, and what does the game consider
    //    selected? A hit here is the whole point of the controller work.
    Il2CppObject *es = invoke(m_es_cur, NULL, "EventSystem.get_current");
    fprintf(stderr, "  [mprobe f%u] eventsystem:", frame);
    if (!es) {
        fprintf(stderr, " none\n");
    } else {
        Il2CppObject *over = invoke_on(es, m_over, NULL, "IsPointerOverGameObject");
        fprintf(stderr, " overUI=%s",
                over ? (*(uint8_t *)il.object_unbox(over) ? "YES" : "no") : "?");
        Il2CppObject *sel = invoke_on(es, m_sel, NULL, "get_currentSelectedGameObject");
        if (!sel) {
            fprintf(stderr, " selected=<none>");
        } else {
            Il2CppObject *nm = invoke_on(sel, m_name, NULL, "Object.get_name");
            char buf[128];
            str_ascii(nm, buf, sizeof buf);
            fprintf(stderr, " selected=\"%s\"", buf);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

// ===========================================================================
// KL_PROBE_TIMELINE — read the fields of a running state machine
// ===========================================================================
//
// Everything above CALLS managed methods. This half READS managed fields,
// which is what a stalled sequence needs: SUPERHOT's launch is a
// `LevelTimeline` walking a `List<LevelPhase>`, and disassembly can enumerate
// the phases the coroutine queues but cannot say which index the walk is
// sitting on 20 seconds later. The index is a field; so is every reason a
// phase has for not finishing (its duration, its elapsed time, its done byte).
//
// The offsets below were read out of `LevelTimeline::Update` (0xdc05bc) and
// `LevelPhase::Update` (0xd39744) and are printed back with the field NAME
// il2cpp's own metadata puts at each one — so a wrong offset shows up as a
// mismatched name in the log rather than as a plausible wrong number.

typedef struct FieldInfo FieldInfo;

static struct {
    FieldInfo   *(*class_get_field_from_name)(Il2CppClass *, const char *);
    FieldInfo   *(*class_get_fields)(Il2CppClass *, void **);
    size_t       (*field_get_offset)(FieldInfo *);
    const char  *(*field_get_name)(FieldInfo *);
    void         (*field_static_get_value)(FieldInfo *, void *);
    int          (*field_get_flags)(FieldInfo *);
    Il2CppClass *(*class_get_parent)(Il2CppClass *);
    Il2CppClass *(*class_get_nested_types)(Il2CppClass *, void **);
} ilf;

#define FIELD_ATTR_STATIC 0x0010   // ECMA-335 FieldAttributes.Static

static int g_tl_state;   // 0 = untried, 1 = up, -1 = unavailable

static int tl_init(void) {
    if (g_tl_state) return g_tl_state > 0;
    g_tl_state = -1;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return 0;
#define BINDF(field, sym) \
    do { *(void **)&ilf.field = kl_sym(img, sym); \
         if (!ilf.field) { fprintf(stderr, "  [mprobe] libil2cpp exports no %s\n", sym); \
                           return 0; } } while (0)
    BINDF(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    BINDF(class_get_fields,          "il2cpp_class_get_fields");
    BINDF(field_get_offset,          "il2cpp_field_get_offset");
    BINDF(field_get_name,            "il2cpp_field_get_name");
    BINDF(field_static_get_value,    "il2cpp_field_static_get_value");
    BINDF(field_get_flags,           "il2cpp_field_get_flags");
    BINDF(class_get_parent,          "il2cpp_class_get_parent");
    BINDF(class_get_nested_types,    "il2cpp_class_get_nested_types");
#undef BINDF
    g_tl_state = 1;
    return 1;
}

// The name metadata gives the field at `off`, searching up the inheritance
// chain. "" when nothing lives there — which is the finding, not a formatting
// problem: it means the offset was read wrong.
static const char *field_name_at(Il2CppClass *k, size_t off) {
    for (; k; k = ilf.class_get_parent(k)) {
        void *it = NULL;
        FieldInfo *f;
        while ((f = ilf.class_get_fields(k, &it)))
            if (ilf.field_get_offset(f) == off) return ilf.field_get_name(f);
    }
    return "";
}

static size_t field_off(Il2CppClass *k, const char *name) {
    FieldInfo *f = k ? ilf.class_get_field_from_name(k, name) : NULL;
    return f ? ilf.field_get_offset(f) : 0;
}

// LevelTimeline, from LevelTimeline::Update.
#define TL_PHASES  0x38   // List<LevelPhase>
#define TL_INDEX   0x40   // int, the walk position
#define TL_DONE    0x4c   // bool, set when index runs off the end
#define TL_MODE    0x74   // int; 2 checks OmniVRSubScene.Pause, 1 Cfiszenplac.IsPaused
// LevelPhase, from LevelPhase::Update.
#define PH_ONSTART 0x10
#define PH_ONUPD   0x20
#define PH_ENDCOND 0x30
#define PH_ADDCOND 0x38
#define PH_DUR     0x40   // float; NewPhase defaults it to -1 (= ends at once)
#define PH_ELAPSED 0x44
#define PH_DELAY   0x48
#define PH_NAME    0x50
#define PH_DONE    0x58
#define PH_SKIPPED 0x59
#define PH_STARTED 0x5a
#define PH_REALT   0x5b

static void dump_phase(unsigned frame, const char *tag, int i, Il2CppObject *ph,
                       Il2CppClass *k_phase) {
    if (!ph) { fprintf(stderr, "  [tl f%u] %s %2d: <null>\n", frame, tag, i); return; }
    uint8_t *p = (uint8_t *)ph;
    char nm[128];
    str_ascii(*(Il2CppObject **)(p + PH_NAME), nm, sizeof nm);
    fprintf(stderr, "  [tl f%u] %s %2d: \"%s\" dur=%.3f elapsed=%.3f delay=%.3f "
                    "done=%d started=%d skipped=%d realtime=%d "
                    "onStart=%s onUpdate=%s endCond=%s addCond=%s\n",
            frame, tag, i, nm,
            *(float *)(p + PH_DUR), *(float *)(p + PH_ELAPSED),
            *(float *)(p + PH_DELAY),
            p[PH_DONE], p[PH_STARTED], p[PH_SKIPPED], p[PH_REALT],
            *(void **)(p + PH_ONSTART) ? "y" : "-",
            *(void **)(p + PH_ONUPD)   ? "y" : "-",
            *(void **)(p + PH_ENDCOND) ? "y" : "-",
            *(void **)(p + PH_ADDCOND) ? "y" : "-");
    (void)k_phase;
}

// KL_PROBE_CLASSREF: turn `adrp x8, 0x2f83000 / ldr x8, [x8, #0xec0]` into a
// type name. The slot holds an Il2CppClass* once il2cpp's metadata-usage table
// is populated; statics hang off it at +0xb8 and the embedding API names them.
//
// Note the guest dereferences TWICE — `ldr x20,[x20,#0xec0]` then `ldr x0,[x20]`
// — because the image's own table holds the ADDRESS of the usage slot, not the
// class. Reading one level, as the first cut of this did, hands class_get_name
// a pointer to a pointer and bus-errors.
static void dump_classrefs(unsigned frame) {
    static int done;
    static char spec[256];
    if (!done) {
        done = 1;
        snprintf(spec, sizeof spec, "%s", kl_env_str("KL_PROBE_CLASSREF", ""));
    }
    if (!spec[0]) return;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return;
    uint8_t *base = (uint8_t *)kl_base(img);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long off = strtoul(tok, NULL, 0);
        Il2CppClass **slot = *(Il2CppClass ***)(base + off);
        Il2CppClass *k = slot ? *slot : NULL;
        // il2cpp fills its metadata-usage slots lazily, as the code that needs
        // each one first runs. Before that the slot is not merely null — early
        // in the boot it can hold a value that is neither null nor a class,
        // and reading fields off it bus-errors (it did, on the first run of
        // this probe). A name is the cheap proof that it is a class now.
        const char *kn = k ? il.class_get_name(k) : NULL;
        if (!kn) { fprintf(stderr, "  [tl f%u] classref 0x%lx: <not yet resolved>\n",
                           frame, off); continue; }
        fprintf(stderr, "  [tl f%u] classref 0x%lx -> %s  statics:", frame, off, kn);
        void *it = NULL;
        FieldInfo *f;
        int n = 0;
        while ((f = ilf.class_get_fields(k, &it))) {
            // Only the statics: il2cpp_field_static_get_value on an instance
            // field reads through a null static block.
            if (!(ilf.field_get_flags(f) & FIELD_ATTR_STATIC)) continue;
            uint64_t v = 0;
            ilf.field_static_get_value(f, &v);
            fprintf(stderr, " %s@0x%zx=0x%llx", ilf.field_get_name(f),
                    ilf.field_get_offset(f), (unsigned long long)v);
            if (++n >= 16) break;
        }
        fprintf(stderr, "\n");
    }
}

// KL_PROBE_ENUM=<Namespace.Type>[:substring] — the guest's own number for a
// name. Compiled il2cpp keeps enum constants as bare integers, so every
// protocol constant this runtime has to answer with (an ovrMessageType, say)
// is otherwise a value to be guessed or dug out of global-metadata.dat by
// hand. The metadata is loaded and Enum.GetNames/Enum.Parse read it for us.
// Printed once, on the first report.
static void dump_enum(unsigned frame) {
    static int done;
    static char spec[192];
    if (!done) {
        done = 1;
        snprintf(spec, sizeof spec, "%s", kl_env_str("KL_PROBE_ENUM", ""));
    }
    if (!spec[0]) return;
    static int printed;
    if (printed) return;

    char buf[192];
    snprintf(buf, sizeof buf, "%s", spec);
    char *filter = strchr(buf, ':');
    if (filter) *filter++ = 0;
    char ns[96] = "", nm[96];
    const char *dot = strrchr(buf, '.');
    if (dot) {
        size_t l = (size_t)(dot - buf);
        if (l >= sizeof ns) l = sizeof ns - 1;
        memcpy(ns, buf, l); ns[l] = 0;
        snprintf(nm, sizeof nm, "%s", dot + 1);
    } else {
        snprintf(nm, sizeof nm, "%s", buf);
    }
    // "Outer/Inner": protocol enums are usually nested inside the class that
    // speaks the protocol (Oculus's is Message.MessageType), and
    // il2cpp_class_from_name only finds top-level types.
    char *inner = strchr(nm, '/');
    if (inner) *inner++ = 0;
    Il2CppClass *k = find_class(ns, nm);
    if (k && inner) {
        void *it = NULL;
        Il2CppClass *nk, *found = NULL;
        while ((nk = ilf.class_get_nested_types(k, &it)))
            if (strcmp(il.class_get_name(nk), inner) == 0) { found = nk; break; }
        if (!found) fprintf(stderr, "  [tl] %s has no nested type %s\n", nm, inner);
        k = found;
    }
    if (!k) { fprintf(stderr, "  [tl] no enum %s\n", spec); printed = 1; return; }
    printed = 1;
    Il2CppObject *type = il.type_get_object(il.class_get_type(k));
    const MethodInfo *m_names = find_method("System", "Enum", "GetNames", 1);
    const MethodInfo *m_parse = find_method("System", "Enum", "Parse", 2);
    void *ta[1] = { type };
    Il2CppObject *names = invoke(m_names, ta, "Enum.GetNames");
    uint32_t n = names ? il.array_length(names) : 0;
    fprintf(stderr, "  [tl f%u] enum %s: %u names%s%s\n", frame, spec, n,
            filter ? ", matching " : "", filter ? filter : "");
    for (uint32_t i = 0; i < n; i++) {
        Il2CppObject *s = ((Il2CppObject **)((uint8_t *)names + ARR_DATA))[i];
        char nb[160];
        str_ascii(s, nb, sizeof nb);
        if (filter && *filter && !strstr(nb, filter)) continue;
        // Parse rather than index Enum.GetValues: the values array is unboxed
        // and its element width is the enum's underlying type, which is a
        // second thing to get right. A boxed return needs no such guess.
        void *pa[2] = { type, s };
        Il2CppObject *v = invoke(m_parse, pa, "Enum.Parse");
        if (v) fprintf(stderr, "      %s = 0x%08x (%u)\n", nb,
                       *(uint32_t *)il.object_unbox(v),
                       *(uint32_t *)il.object_unbox(v));
        else   fprintf(stderr, "      %s = <parse failed>\n", nb);
    }
}

void kl_mprobe_timeline(unsigned frame) {
    static int every = -1;
    if (every < 0) every = (int)kl_env_uint("KL_PROBE_TIMELINE", 0);
    if (every <= 0) return;
    if (!frame || frame % (unsigned)every) return;   // frame 0 has no managed world yet
    if (!mprobe_init() || !tl_init()) { every = 0; return; }

    static const MethodInfo *m_findall, *m_find;
    static const MethodInfo *m_tscale, *m_udt, *m_rt;
    static const MethodInfo *m_launching, *m_fog;
    static Il2CppObject *tl_type;
    static Il2CppClass *k_tl, *k_phase;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_findall = find_method("UnityEngine", "Resources", "FindObjectsOfTypeAll", 1);
        m_find    = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_tscale  = find_method("UnityEngine", "Time", "get_timeScale", 0);
        m_udt     = find_method("UnityEngine", "Time", "get_unscaledDeltaTime", 0);
        m_rt      = find_method("UnityEngine", "Time", "get_realtimeSinceStartup", 0);
        m_launching = find_method("", "VRLaunch", "get_isLaunching", 0);
        m_fog       = find_method("", "VrFog", "get__enabled", 0);
        k_tl    = find_class("", "LevelTimeline");
        k_phase = find_class("", "LevelPhase");
        if (k_tl) tl_type = il.type_get_object(il.class_get_type(k_tl));
        // Print the metadata's own names for every offset this reads, once.
        // A field map that came out of a disassembler is a claim; this is the
        // check on it, and it costs one log block.
        if (k_tl && k_phase) {
            fprintf(stderr, "  [tl] LevelTimeline fields: phases@0x%x=%s index@0x%x=%s "
                            "done@0x%x=%s mode@0x%x=%s\n",
                    TL_PHASES, field_name_at(k_tl, TL_PHASES),
                    TL_INDEX,  field_name_at(k_tl, TL_INDEX),
                    TL_DONE,   field_name_at(k_tl, TL_DONE),
                    TL_MODE,   field_name_at(k_tl, TL_MODE));
            fprintf(stderr, "  [tl] LevelPhase fields: dur@0x%x=%s elapsed@0x%x=%s "
                            "name@0x%x=%s done@0x%x=%s started@0x%x=%s realtime@0x%x=%s\n",
                    PH_DUR, field_name_at(k_phase, PH_DUR),
                    PH_ELAPSED, field_name_at(k_phase, PH_ELAPSED),
                    PH_NAME, field_name_at(k_phase, PH_NAME),
                    PH_DONE, field_name_at(k_phase, PH_DONE),
                    PH_STARTED, field_name_at(k_phase, PH_STARTED),
                    PH_REALT, field_name_at(k_phase, PH_REALT));
        }
    }
    if (!k_tl || !tl_type) return;

    dump_classrefs(frame);
    dump_enum(frame);

    {
        Il2CppObject *ts = invoke(m_tscale, NULL, "Time.timeScale");
        Il2CppObject *dt = invoke(m_udt, NULL, "Time.unscaledDeltaTime");
        Il2CppObject *rt = invoke(m_rt, NULL, "Time.realtimeSinceStartup");
        Il2CppObject *la = invoke(m_launching, NULL, "VRLaunch.isLaunching");
        Il2CppObject *fg = invoke(m_fog, NULL, "VrFog._enabled");
        fprintf(stderr, "  [tl f%u] time: scale=%.3f unscaledDt=%.4f realtime=%.2f "
                        "| VRLaunch.isLaunching=%s VrFog._enabled=%s\n", frame,
                ts ? *(float *)il.object_unbox(ts) : -1.0f,
                dt ? *(float *)il.object_unbox(dt) : -1.0f,
                rt ? *(float *)il.object_unbox(rt) : -1.0f,
                la ? (*(uint8_t *)il.object_unbox(la) ? "TRUE" : "false") : "?",
                fg ? (*(uint8_t *)il.object_unbox(fg) ? "TRUE" : "false") : "?");
    }

    // Inactive ones count: a timeline whose GameObject got switched off stops
    // ticking without ever finishing, and FindObjectsOfType would hide exactly
    // that case behind an empty list.
    void *targs[1] = { tl_type };
    Il2CppObject *all = invoke(m_findall, targs, "Resources.FindObjectsOfTypeAll");
    if (!all) all = invoke(m_find, targs, "Object.FindObjectsOfType");
    uint32_t n = all ? il.array_length(all) : 0;
    if (!n) { fprintf(stderr, "  [tl f%u] no LevelTimeline in the scene\n", frame); return; }

    static int listed;                       // print the full phase list once
    for (uint32_t i = 0; i < n && i < 8; i++) {
        Il2CppObject *tl = ((Il2CppObject **)((uint8_t *)all + ARR_DATA))[i];
        if (!tl) continue;
        uint8_t *t = (uint8_t *)tl;
        Il2CppClass *kc = il.object_get_class(tl);
        Il2CppObject *list = *(Il2CppObject **)(t + TL_PHASES);
        int32_t idx = *(int32_t *)(t + TL_INDEX);
        int32_t mode = *(int32_t *)(t + TL_MODE);
        int32_t count = 0;
        Il2CppObject *items = NULL;
        if (list) {
            Il2CppClass *kl = il.object_get_class(list);
            size_t o_items = field_off(kl, "_items"), o_size = field_off(kl, "_size");
            if (o_items && o_size) {
                items = *(Il2CppObject **)((uint8_t *)list + o_items);
                count = *(int32_t *)((uint8_t *)list + o_size);
            }
        }
        fprintf(stderr, "  [tl f%u] %s: phases=%d index=%d mode=%d finished=%d\n",
                frame, kc ? il.class_get_name(kc) : "?", count, idx, mode,
                t[TL_DONE]);
        if (!items || count <= 0) continue;
        Il2CppObject **arr = (Il2CppObject **)((uint8_t *)items + ARR_DATA);
        if (!listed) {
            for (int32_t j = 0; j < count; j++)
                dump_phase(frame, "  phase", j, arr[j], k_phase);
        }
        if (idx >= 0 && idx < count)
            dump_phase(frame, "CURRENT", idx, arr[idx], k_phase);
    }
    listed = 1;
    fflush(stderr);
}
