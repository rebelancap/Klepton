// The OpenVR runtime gate. `make openvr`, in `make check`.
//
// kl_openvr serves Valve's C++ interfaces to a guest that calls them as C++
// virtual calls, so this calls them the same way: fetch the object, read word
// 0 as the vtable, call slot N with `this` in x0. Nothing here goes around the
// vtables — a slot in the wrong place answers the wrong question and this is
// where that has to show, because the guest it was built for (Half-Life:
// Alyx's libvr.so) would show it only as a wrong picture, or a wrong button,
// thousands of calls later.
//
// It runs against kl_openxr's REAL headless path: VR_InitInternal2 brings up
// an OpenXR instance, a session (Vulkan binding deferred, as on a first boot),
// spaces and actions, and walks the session to FOCUSED. Controller input comes
// in the way a frontend publishes it (kl_ovrp_set_controller_input), so the
// IVRInput answers below are read out of kl_openxr's action state, not a fake.
//
// The x8 return (HmdMatrix34/44 by value) and the unwidened-bool argument are
// the two places a Darwin callee and a Linux caller disagree; both are driven
// here with the caller-side shape: struct returns through a by-value function
// pointer, and bools carried in a uint32_t with garbage above bit 7.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "../runtime/xr/kl_openvr_int.h"
#include "../runtime/xr/kl_ovrp.h"
#include "../runtime/klepton.h"
#include "../runtime/gfx/kl_vulkan.h"

static int g_fail;
static void check(int ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

#define VT(obj) (*(void *const *const *)(obj))
#define SLOT(obj, i, type) ((type)VT(obj)[i])

static FILE *g_logf;
static char *log_text(void) {
    static char buf[1 << 20];
    fflush(g_logf);
    long n = ftell(g_logf);
    rewind(g_logf);
    size_t got = fread(buf, 1, n > 0 && n < (long)sizeof buf - 1 ? (size_t)n : sizeof buf - 1,
                       g_logf);
    buf[got] = 0;
    fseek(g_logf, 0, SEEK_END);
    return buf;
}

// The interfaces and slot counts hla/OPENVR-VTABLES.md gives libvr.so's
// requests (private ones: the served width).
static const struct { const char *version; unsigned slots; } g_expect[] = {
    { "IVRApplications_007", 30 },  { "IVRSystem_026", 51 },   { "IVROverlay_028", 82 },
    { "IXrProto_001", 32 },         { "IVRInput_011", 34 },    { "IVROverlayView_003", 4 },
    { "IVRInputInternal_002", 32 }, { "IVRMailbox_001", 32 },  { "IVRChaperone_004", 9 },
    { "IVRCompositor_029", 53 },    { "IVRRenderModels_006", 19 },
    { "IVRHeadsetView_001", 9 },
};

// ---- a Vulkan renderer of the test's own, through kl_vulkan's door ----------
//
// The eye copy is only real against a real device: the test creates instance,
// device and queue exactly as the guest's renderer does (vkGetInstanceProcAddr
// from kl_vulkan's synthetic libvulkan, MoltenVK underneath), fills its own
// images, hands them to Submit, and reads the swapchain images back on its own
// queue. Nothing here goes around kl_openvr to reach kl_openxr.
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#define T_HAVE_VK 1
static struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue q;
    uint32_t qfam;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkPhysicalDeviceMemoryProperties mp;
#define T_VKFN(X) X(CreateImage) X(GetImageMemoryRequirements) X(AllocateMemory)            \
    X(BindImageMemory) X(CreateBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)     \
    X(MapMemory) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)        \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage)  \
    X(CmdCopyImageToBuffer) X(CmdClearColorImage) X(QueueSubmit) X(QueueWaitIdle)            \
    X(GetDeviceQueue)
#define X(n) PFN_vk##n n;
    T_VKFN(X)
#undef X
} T;

static int t_vk_up(void) {
    T.gipa = (PFN_vkGetInstanceProcAddr)kl_vulkan_sym("vkGetInstanceProcAddr");
    if (!T.gipa) return -1;
    PFN_vkCreateInstance ci_fn = (PFN_vkCreateInstance)T.gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    if (!ci_fn || ci_fn(&ici, NULL, &T.inst) != VK_SUCCESS) return -2;
    PFN_vkEnumeratePhysicalDevices epd =
        (PFN_vkEnumeratePhysicalDevices)T.gipa(T.inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 1;
    if (!epd || epd(T.inst, &n, &T.phys) < 0 || !n) return -3;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        T.gipa(T.inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    VkQueueFamilyProperties qf[8];
    uint32_t nq = 8;
    qfp(T.phys, &nq, qf);
    T.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && T.qfam == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) T.qfam = i;
    if (T.qfam == UINT32_MAX) return -4;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = T.qfam, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    PFN_vkCreateDevice cd = (PFN_vkCreateDevice)T.gipa(T.inst, "vkCreateDevice");
    if (!cd || cd(T.phys, &dci, NULL, &T.dev) != VK_SUCCESS) return -5;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)T.gipa(T.inst, "vkGetDeviceProcAddr");
#define X(n) if (!(T.n = (PFN_vk##n)gdpa(T.dev, "vk" #n))) return -6;
    T_VKFN(X)
#undef X
    ((PFN_vkGetPhysicalDeviceMemoryProperties)T.gipa(T.inst, "vkGetPhysicalDeviceMemoryProperties"))(
        T.phys, &T.mp);
    T.GetDeviceQueue(T.dev, T.qfam, 0, &T.q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = T.qfam };
    if (T.CreateCommandPool(T.dev, &pci, NULL, &T.pool) != VK_SUCCESS) return -7;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = T.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    return T.AllocateCommandBuffers(T.dev, &cai, &T.cb) == VK_SUCCESS ? 0 : -8;
}

static VkDeviceMemory t_mem(VkMemoryRequirements req, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < T.mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (T.mp.memoryTypes[i].propertyFlags & want) == want) {
            VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .allocationSize = req.size, .memoryTypeIndex = i };
            VkDeviceMemory m = VK_NULL_HANDLE;
            return T.AllocateMemory(T.dev, &ai, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
        }
    return VK_NULL_HANDLE;
}

static void t_begin(void) {
    T.ResetCommandBuffer(T.cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    T.BeginCommandBuffer(T.cb, &bi);
}
static void t_end(void) {
    T.EndCommandBuffer(T.cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                        .pCommandBuffers = &T.cb };
    T.QueueSubmit(T.q, 1, &si, VK_NULL_HANDLE);
    T.QueueWaitIdle(T.q);
}
static void t_layout(VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    T.CmdPipelineBarrier(T.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkBuffer t_buffer(VkDeviceSize size, void **map) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf = VK_NULL_HANDLE;
    T.CreateBuffer(T.dev, &bci, NULL, &buf);
    VkMemoryRequirements req;
    T.GetBufferMemoryRequirements(T.dev, buf, &req);
    VkDeviceMemory m = t_mem(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    T.BindBufferMemory(T.dev, buf, m, 0);
    T.MapMemory(T.dev, m, 0, size, 0, map);
    return buf;
}

// The guest's eye image: one R8G8B8A8_SRGB image two eyes wide, left half
// `c0`, right half `c1`, handed over in TRANSFER_SRC_OPTIMAL (OpenVR's
// contract). samples > 1: an MSAA image cleared to `c0` throughout.
static VkImage t_eye_image(uint32_t w, uint32_t h, uint32_t samples, const uint8_t c0[4],
                           const uint8_t c1[4]) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
                              .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
                              .samples = (VkSampleCountFlagBits)samples,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       (samples == 1 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE;
    if (T.CreateImage(T.dev, &ici, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    T.GetImageMemoryRequirements(T.dev, img, &req);
    T.BindImageMemory(T.dev, img, t_mem(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
    t_begin();
    t_layout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (samples == 1) {
        void *map = NULL;
        VkBuffer buf = t_buffer((VkDeviceSize)w * h * 4, &map);
        uint8_t *px = map;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                memcpy(px + ((size_t)y * w + x) * 4, x < w / 2 ? c0 : c1, 4);
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                .imageExtent = { w, h, 1 } };
        T.CmdCopyBufferToImage(T.cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else {
        // Encoded bytes 0 and 255 are their own sRGB decode, so a clear of
        // c0/255 lands as exactly c0 when c0 has no mid-tones.
        VkClearColorValue cv = { .float32 = { c0[0] / 255.f, c0[1] / 255.f, c0[2] / 255.f,
                                              c0[3] / 255.f } };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        T.CmdClearColorImage(T.cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
    }
    t_layout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    t_end();
    return img;
}

// Three texels across the middle row of an eye's swapchain image (at 1/4, 1/2,
// 3/4 of its width), read back on the test's queue: 1 when all match `want`
// within 2 per channel.
static int t_eye_is(int eye, const uint8_t want[4], uint8_t got[4]) {
    uint64_t img = 0;
    uint32_t w = 0, h = 0;
    int64_t fmt = 0;
    if (!kl_openvr_eye_image(eye, &img, &w, &h, &fmt)) return 0;
    static VkBuffer buf;
    static void *map;
    if (!buf) buf = t_buffer(64, &map);
    VkBufferImageCopy r[3];
    memset(r, 0, sizeof r);
    for (int i = 0; i < 3; i++) {
        r[i].bufferOffset = (VkDeviceSize)i * 4;
        r[i].imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r[i].imageOffset = (VkOffset3D){ (int32_t)(w * (uint32_t)(i + 1) / 4), (int32_t)h / 2, 0 };
        r[i].imageExtent = (VkExtent3D){ 1, 1, 1 };
    }
    memset(map, 0xee, 12);
    t_begin();
    t_layout((VkImage)(uintptr_t)img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    T.CmdCopyImageToBuffer(T.cb, (VkImage)(uintptr_t)img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 3, r);
    t_layout((VkImage)(uintptr_t)img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    t_end();
    const uint8_t *p = map;
    memcpy(got, p + 4, 4);
    int ok = fmt == VK_FORMAT_R8G8B8A8_SRGB;
    for (int i = 0; i < 12; i++) ok &= abs((int)p[i] - (int)want[i % 4]) <= 2;
    return ok;
}
#else
#define T_HAVE_VK 0
#endif

typedef uint32_t (*fn_init)(int32_t *, uint32_t, const char *);
typedef void *(*fn_get)(const char *, int32_t *);
typedef uint8_t (*fn_valid)(const char *);
typedef const char *(*fn_errsym)(int32_t);

#if T_HAVE_VK
#include <pthread.h>
// The teardown race (review of 49213fc6): a Submit settling an eye's copy on
// one thread while VR_ShutdownInternal tears the eye copy down on another.
static struct {
    void *comp;
    const void *tex;
    const klvr_VRTextureBounds_t *b[2];
    int32_t r[2];
    int go;
} g_race;
static void *t_race_submit(void *arg) {
    (void)arg;
    typedef int32_t (*fn_s)(void *, uint32_t, const void *, const klvr_VRTextureBounds_t *,
                            uint32_t);
    __atomic_store_n(&g_race.go, 1, __ATOMIC_RELEASE);
    for (uint32_t e = 0; e < 2; e++)
        g_race.r[e] = SLOT(g_race.comp, 6, fn_s)(g_race.comp, e, g_race.tex, g_race.b[e], 0);
    return NULL;
}
#endif

// ---- the reference door's pose math, transcribed from libalyxhost_f1_m1 -----
//
// Written from the disassembly, not from kl_openvr, so the differential below
// compares two independent codings of the same conversion:
//   PoseMatrix   AlyxOpenVrSystemGetEyeToHeadTransform+0x7c / WaitGetPoses
//                +0x648: row-major 3x4, r00 = 1-2(yy+zz), r01 = 2(xy-zw), ...
//   RelativePose alyx_view::RelativePose (+0xfb7d8): a^-1 o b — (b.p - a.p)
//                rotated by conj(a.q), conj(a.q) * b.q.
//   Calibrate    alyx_view::Calibrate (+0x10478c): eye-to-head =
//                RelativePose(head, view located in the tracking space).
//   raw space    alyx_input::TouchRaw::Attach (+0x102b5c): the hand pose is the
//                grip action located with poseInActionSpace = RelativePose(A,
//                identity), A = (0.17880222,0,0,0.98388505) at
//                (+-0.007, -0.00182941, 0.10194820) m — a runtime composes that
//                as grip o offset (OpenXR's action-space rule).
typedef struct { float q[4], p[3]; } ref_pose;
static ref_pose ref_of(klxr_Posef x) {
    ref_pose r = { { x.orientation.x, x.orientation.y, x.orientation.z, x.orientation.w },
                   { x.position.x, x.position.y, x.position.z } };
    return r;
}
static void ref_qmul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void ref_qrot(const float q[4], const float v[3], float o[3]) {
    float u[4] = { v[0], v[1], v[2], 0 }, c[4] = { -q[0], -q[1], -q[2], q[3] }, t[4], r[4];
    ref_qmul(q, u, t);
    ref_qmul(t, c, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
static klvr_HmdMatrix34_t ref_matrix(ref_pose p) {
    float x = p.q[0], y = p.q[1], z = p.q[2], w = p.q[3];
    klvr_HmdMatrix34_t m = { { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p.p[0] },
                               { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p.p[1] },
                               { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p.p[2] } } };
    return m;
}
static ref_pose ref_relative(ref_pose a, ref_pose b) {
    float c[4] = { -a.q[0], -a.q[1], -a.q[2], a.q[3] }, d[3] = { b.p[0] - a.p[0], b.p[1] - a.p[1],
                                                               b.p[2] - a.p[2] };
    ref_pose o;
    ref_qmul(c, b.q, o.q);
    ref_qrot(c, d, o.p);
    return o;
}
static ref_pose ref_compose(ref_pose a, ref_pose b) {
    ref_pose o;
    ref_qmul(a.q, b.q, o.q);
    ref_qrot(a.q, b.p, o.p);
    for (int i = 0; i < 3; i++) o.p[i] += a.p[i];
    return o;
}
static float ref_diff(const klvr_HmdMatrix34_t *a, const klvr_HmdMatrix34_t *b) {
    float worst = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            float d = fabsf(a->m[i][j] - b->m[i][j]);
            if (d > worst) worst = d;
        }
    return worst;
}
static void ref_axis_angle(float ax, float ay, float az, float deg, float q[4]) {
    float n = sqrtf(ax * ax + ay * ay + az * az), h = deg * 3.14159265f / 360.0f;
    q[0] = ax / n * sinf(h); q[1] = ay / n * sinf(h); q[2] = az / n * sinf(h); q[3] = cosf(h);
}
// The deterministic motion script: head yaw/pitch/roll and sway sinusoids, a
// canted asymmetric eye pair, two controllers rotating about tilted axes while
// they move. Frame k of it.
static void motion_frame(int k, ref_pose *head, ref_pose hand[2]) {
    float qy[4], qp[4], qr[4], t[4];
    ref_axis_angle(0, 1, 0, 40.0f * sinf(k * 0.21f), qy);
    ref_axis_angle(1, 0, 0, 15.0f * sinf(k * 0.13f), qp);
    ref_axis_angle(0, 0, 1, 5.0f * sinf(k * 0.07f), qr);
    ref_qmul(qy, qp, t);
    ref_qmul(t, qr, head->q);
    head->p[0] = 0.10f * sinf(k * 0.17f);
    head->p[1] = 1.60f + 0.05f * cosf(k * 0.11f);
    head->p[2] = -0.10f * sinf(k * 0.05f);
    for (int h = 0; h < 2; h++) {
        ref_axis_angle(0.3f + h, 0.8f, 0.5f - h, 17.0f * k + 30.0f * h, hand[h].q);
        hand[h].p[0] = (h ? 0.25f : -0.25f) + 0.05f * sinf(k * 0.3f);
        hand[h].p[1] = 1.1f + 0.08f * cosf(k * 0.23f);
        hand[h].p[2] = -0.35f + 0.06f * sinf(k * 0.19f + h);
    }
}

int main(void) {
    printf("=== OpenVR runtime (kl_openvr over kl_openxr's headless path) ===\n");
    // The canted-eye case below needs kl_ovrp to honour a pushed eye rotation;
    // with none pushed (everything before it) the knob is the identity.
    setenv("KL_OVRP_EYE_CANT", "1", 0);
    g_logf = tmpfile();
    kl_openvr_set_log(g_logf);

    // The door's configuration: the stock SteamVR manifest reader, on a fixture.
    kl_openvr_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.flatten_manifest = kl_openvr_manifest_flatten;
    kl_openvr_configure(&cfg);

    // ---- the exports, through the soname claim the loader uses
    static const char *const names[] = {
        "VR_InitInternal2", "VR_ShutdownInternal", "VR_GetInitToken", "VR_GetGenericInterface",
        "VR_IsInterfaceVersionValid", "VR_GetVRInitErrorAsSymbol",
        "VR_GetVRInitErrorAsEnglishDescription", "VRControlPanel" };
    int bound = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        const char *who = NULL;
        void *v = kl_glibc_claim_sym("libopenvr_api.so", names[i], &who);
        bound += v && v == kl_openvr_sym(names[i]) && who && !strcmp(who, "kl_openvr");
    }
    check(bound == 8, "libopenvr_api.so's 8 names bind to kl_openvr through kl_dl's claim");
    check(!kl_openvr_sym("VR_Init"), "a name libopenvr_api does not export is refused");

    fn_init   init  = (fn_init)kl_openvr_sym("VR_InitInternal2");
    fn_get    get   = (fn_get)kl_openvr_sym("VR_GetGenericInterface");
    fn_valid  valid = (fn_valid)kl_openvr_sym("VR_IsInterfaceVersionValid");
    fn_errsym esym  = (fn_errsym)kl_openvr_sym("VR_GetVRInitErrorAsSymbol");
    uint32_t (*token_fn)(void) = (uint32_t (*)(void))kl_openvr_sym("VR_GetInitToken");
    void (*shutdown)(void) = (void (*)(void))kl_openvr_sym("VR_ShutdownInternal");
    void *(*ctl)(void) = (void *(*)(void))kl_openvr_sym("VRControlPanel");

    // libvr asks for Applications BEFORE VR_InitInternal2.
    int32_t err = -1;
    void *apps = get("IVRApplications_007", &err);
    check(apps && err == 0, "IVRApplications_007 is served before init");

    err = -1;
    uint32_t token = init(&err, 1 /* VRApplication_Scene */, NULL);
    check(token == 1 && err == 0, "VR_InitInternal2 -> token 1, no error");
    check(token_fn() == 1, "VR_GetInitToken -> 1");
    check(strstr(log_text(), "session READY -> xrBeginSession 0") != NULL,
          "the OpenXR session reached READY and began inside VR_InitInternal2");

    // ---- every interface the doc lists, its slot count, no empty slot
    void *obj[sizeof g_expect / sizeof *g_expect];
    int all = 1;
    for (size_t i = 0; i < sizeof g_expect / sizeof *g_expect; i++) {
        err = -1;
        obj[i] = get(g_expect[i].version, &err);
        int n = kl_openvr_iface_count(), found = 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(kl_openvr_iface_version(k), g_expect[i].version)) {
                found = kl_openvr_iface_slots(k) == g_expect[i].slots;
                for (unsigned s = 0; found && s < g_expect[i].slots; s++)
                    found = VT(obj[i])[s] != NULL;
            }
        int ok = obj[i] && err == 0 && valid(g_expect[i].version) && found;
        if (!ok) printf("       %s: object %p err %d slots-ok %d\n", g_expect[i].version,
                        obj[i], err, found);
        all &= ok;
    }
    check(all, "12 interfaces served with the doc's slot counts, every slot filled, all valid");
    check(kl_openvr_iface_count() == 12, "...and nothing else is served");
    static const char *const nulls[] = { "IVRIPCResourceManagerClient_004",
                                         "IVRControlPanel_006", "IVRSystem_022",
                                         "FnTable:IVRSystem_026" };
    int nok = 1;
    for (size_t i = 0; i < sizeof nulls / sizeof *nulls; i++) {
        err = 0;
        nok &= get(nulls[i], &err) == NULL && err == 105 && !valid(nulls[i]);
    }
    check(nok, "the NULL/105 interfaces (IPCResourceManagerClient, ControlPanel, others)");
    check(ctl() == NULL, "VRControlPanel -> NULL");
    check(!strcmp(esym(105), "VRInitError_Init_InterfaceNotFound"), "VR_GetVRInitErrorAsSymbol(105)");

    void *sys = get("IVRSystem_026", NULL), *comp = get("IVRCompositor_029", NULL);
    void *input = get("IVRInput_011", NULL), *chap = get("IVRChaperone_004", NULL);
    void *mail = get("IVRMailbox_001", NULL), *xrp = get("IXrProto_001", NULL);

    // ---- IVRSystem
    uint32_t w = 0, h = 0;
    SLOT(sys, 0, void (*)(void *, uint32_t *, uint32_t *))(sys, &w, &h);
    check(w > 0 && h > 0, "System.GetRecommendedRenderTargetSize answers a size");
    printf("       recommended %ux%u\n", w, h);
    check(SLOT(sys, 50, int32_t (*)(void *, uint32_t, uint32_t, uint32_t))(sys, 2, 16, 0) == 0,
          "System.SetSDKVersion(2,16,0) -> None");

    // x8: a 64-byte struct by value, with a sentinel the callee must overwrite.
    typedef klvr_HmdMatrix44_t (*fn_proj)(void *, uint32_t, float, float);
    klvr_HmdMatrix44_t p;
    memset(&p, 0x7f, sizeof p);
    p = SLOT(sys, 1, fn_proj)(sys, 0, 0.1f, 100.0f);
    float l, r, t, b;
    SLOT(sys, 2, void (*)(void *, uint32_t, float *, float *, float *, float *))(sys, 0, &l, &r,
                                                                                &t, &b);
    int proj_ok = fabsf(p.m[0][0] - 2 / (r - l)) < 1e-4f && fabsf(p.m[1][1] - 2 / (b - t)) < 1e-4f &&
                  fabsf(p.m[2][2] + 100.0f / 99.9f) < 1e-4f && p.m[3][2] == -1.0f &&
                  p.m[3][3] == 0.0f && p.m[0][1] == 0.0f;
    check(proj_ok, "System.GetProjectionMatrix returns through x8 and is ComposeProjection(raw)");
    printf("       eye 0 raw l=%.4f r=%.4f top=%.4f bottom=%.4f; m00=%.4f m22=%.6f\n",
           l, r, t, b, p.m[0][0], p.m[2][2]);
    typedef klvr_HmdMatrix34_t (*fn_e2h)(void *, uint32_t);
    klvr_HmdMatrix34_t e0 = SLOT(sys, 5, fn_e2h)(sys, 0), e1 = SLOT(sys, 5, fn_e2h)(sys, 1);
    check(e0.m[0][3] < 0 && e1.m[0][3] > 0 && fabsf(e0.m[0][3] + e1.m[0][3]) < 1e-5f &&
          e0.m[0][0] == 1.0f && e0.m[1][3] == 0.0f,
          "System.GetEyeToHeadTransform: eyes either side of the head, no standing height");
    printf("       eye-to-head x %.4f / %.4f\n", e0.m[0][3], e1.m[0][3]);
    klvr_HiddenAreaMesh_t ham = SLOT(sys, 34, klvr_HiddenAreaMesh_t (*)(void *, uint32_t, uint32_t))(
        sys, 0, 0);
    check(!ham.pVertexData && !ham.unTriangleCount, "System.GetHiddenAreaMesh -> {NULL, 0} (x0:x1)");
    {   // KL_OPENVR_HIDDEN_AREA (round N15): corner mask of the asked-for area.
        typedef klvr_HiddenAreaMesh_t (*fn_ham)(void *, uint32_t, uint32_t);
        int ok = 1;
        for (int fi = 0; fi < 3; fi++) {
            static const char *fs[3] = { "0.10", "0.15", "0.20" };
            setenv("KL_OPENVR_HIDDEN_AREA", fs[fi], 1);
            double want = atof(fs[fi]), area[2] = { 0, 0 };
            for (uint32_t ty = 0; ty < 2; ty++) {
                klvr_HiddenAreaMesh_t h = SLOT(sys, 34, fn_ham)(sys, 1, ty);
                if (!h.pVertexData || !h.unTriangleCount) { ok = 0; continue; }
                for (uint32_t i = 0; i < h.unTriangleCount; i++) {
                    const float *a = h.pVertexData[3 * i].v, *b = h.pVertexData[3 * i + 1].v,
                                *c = h.pVertexData[3 * i + 2].v;
                    for (int k = 0; k < 3; k++) {
                        const float *v = h.pVertexData[3 * i + k].v;
                        if (v[0] < 0 || v[0] > 1 || v[1] < 0 || v[1] > 1) ok = 0;
                    }
                    area[ty] += fabs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2;
                }
            }
            klvr_HiddenAreaMesh_t ll = SLOT(sys, 34, fn_ham)(sys, 0, 2);
            // The polygon areas are inscribed in the curve: within ~1% of the
            // analytic area, hidden slightly over, visible slightly under.
            if (fabs(area[0] - want) > 0.01 || fabs(area[0] + area[1] - 1) > 1e-4 || ll.unTriangleCount != 64) ok = 0;
            printf("       hidden-area %s: hidden %.4f visible %.4f lineloop %u vertices\n", fs[fi],
                   area[0], area[1], ll.unTriangleCount);
        }
        setenv("KL_OPENVR_HIDDEN_AREA", "0", 1);
        klvr_HiddenAreaMesh_t off = SLOT(sys, 34, fn_ham)(sys, 0, 0);
        unsetenv("KL_OPENVR_HIDDEN_AREA");
        check(ok && !off.unTriangleCount,
              "KL_OPENVR_HIDDEN_AREA=<f>: corner mesh hides f of the eye (types 0/1/2), 0 = empty");
    }
    check(SLOT(sys, 20, uint32_t (*)(void *, uint32_t))(sys, 0) == 1 &&
          SLOT(sys, 20, uint32_t (*)(void *, uint32_t))(sys, 2) == 2 &&
          SLOT(sys, 20, uint32_t (*)(void *, uint32_t))(sys, 7) == 0,
          "System.GetTrackedDeviceClass: HMD, controller, invalid");
    check(SLOT(sys, 18, uint32_t (*)(void *, uint32_t))(sys, 2) == 2 &&
          SLOT(sys, 19, uint32_t (*)(void *, uint32_t))(sys, 1) == 1,
          "System controller role <-> index");
    typedef uint32_t (*fn_sprop)(void *, uint32_t, uint32_t, char *, uint32_t, uint32_t *);
    char sbuf[64];
    uint32_t perr = 99;
    uint32_t need = SLOT(sys, 28, fn_sprop)(sys, 0, 1000, sbuf, sizeof sbuf, &perr);
    check(need == 7 && perr == 0 && !strcmp(sbuf, "oculus"),
          "System.GetStringTrackedDeviceProperty(HMD, TrackingSystemName) -> \"oculus\", 7");
    need = SLOT(sys, 28, fn_sprop)(sys, 0, 1001, sbuf, 3, &perr);
    check(need == 8 && perr == 3, "...a short buffer -> required 8, BufferTooSmall");
    SLOT(sys, 28, fn_sprop)(sys, 1, 1001, sbuf, sizeof sbuf, &perr);
    check(perr == 4, "...a controller's ModelNumber -> UnknownProperty (as the reference)");
    float hz = SLOT(sys, 23, float (*)(void *, uint32_t, uint32_t, uint32_t *))(sys, 0, 2002, &perr);
    check(hz > 0 && perr == 0, "System.GetFloatTrackedDeviceProperty(DisplayFrequency) > 0");

    // ---- the frame loop
    typedef int32_t (*fn_wgp)(void *, klvr_TrackedDevicePose_t *, uint32_t,
                              klvr_TrackedDevicePose_t *, uint32_t);
    static klvr_TrackedDevicePose_t rp[64], gp[64];
    int32_t wr = SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
    check(wr == 0 && rp[0].bPoseIsValid && rp[0].bDeviceIsConnected && !rp[5].bDeviceIsConnected &&
          rp[0].eTrackingResult == 200 && !memcmp(&rp[0], &gp[0], sizeof rp[0]),
          "Compositor.WaitGetPoses(64, 64): a valid HMD pose, render == game");
    printf("       HMD at (%.3f %.3f %.3f)\n", rp[0].mDeviceToAbsoluteTracking.m[0][3],
           rp[0].mDeviceToAbsoluteTracking.m[1][3], rp[0].mDeviceToAbsoluteTracking.m[2][3]);
    uint32_t rid = 0, gid = 0;
    check(SLOT(comp, 51, int32_t (*)(void *, uint32_t *, uint32_t *))(comp, &rid, &gid) == 0 &&
          rid == 1 && gid == 1, "Compositor.GetLastPosePredictionIDs -> frame 1");
    typedef int32_t (*fn_pff)(void *, uint32_t, klvr_TrackedDevicePose_t *, uint32_t);
    static klvr_TrackedDevicePose_t fp[64];
    check(SLOT(comp, 52, fn_pff)(comp, rid, fp, 64) == 0 && !memcmp(fp, rp, sizeof rp) &&
          SLOT(comp, 52, fn_pff)(comp, rid + 5, fp, 64) == 1,
          "Compositor.GetPosesForFrame: the frame's poses, RequestFailed for one never made");
    wr = SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
    check(wr == 0 && strstr(log_text(), "xrEndFrame") == NULL,
          "a second WaitGetPoses ends the first frame (empty, accepted) and opens frame 2");
    {
        char *rep = NULL;
        size_t rn = 0;
        FILE *mf = open_memstream(&rep, &rn);
        kl_openvr_report(mf);
        fclose(mf);
        check(rep && strstr(rep, "frames ended 0 with layers / 1 empty"),
              "...the report counts it: 0 with layers, 1 empty");
        free(rep);
    }
    check(SLOT(comp, 1, uint32_t (*)(void *))(comp) == 1, "Compositor.GetTrackingSpace -> Standing");

    // ---- the frame clock (round N13-B): the pose prediction id counts DISPLAY
    // INTERVALS. Source 2 advances one tick per id step (libvr+0x9bafc), so a
    // frame that missed the display must advance the id by the intervals it
    // spanned, or the world runs at fps/Hz (the headset's "slow motion").
    {
        typedef int32_t (*fn_ids)(void *, uint32_t *, uint32_t *);
        uint32_t a = 0, b = 0, c = 0, g = 0;
        usleep((useconds_t)(10e6 / hz));             // let the display catch up with the test's frames
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        SLOT(comp, 51, fn_ids)(comp, &a, &g);
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        SLOT(comp, 51, fn_ids)(comp, &b, &g);
        check(b == a + 1 && g == b, "frame clock: back-to-back frames -> id + 1 (one tick)");
        // The id never runs behind the frame count (a free-running caller
        // still gets a tick a frame), so a frame may start up to one interval
        // ahead of the display: a 10-period frame advances 9..11.
        usleep((useconds_t)(10e6 / hz));             // 10 display periods of work
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        SLOT(comp, 51, fn_ids)(comp, &c, &g);
        check(c >= b + 9 && c <= b + 11,
              "frame clock: a frame 10 periods long -> id + 9..11 (the intervals it spanned)");
        static klvr_TrackedDevicePose_t cp[64];
        check(SLOT(comp, 52, fn_pff)(comp, c, cp, 64) == 0 && !memcmp(cp, rp, sizeof rp),
              "...and GetPosesForFrame finds that frame by its id");
        printf("       ids %u -> %u -> %u at %.0f Hz\n", a, b, c, hz);
    }

    // ---- Submit: the eye copy, against a real device (A5)
    typedef int32_t (*fn_submit)(void *, uint32_t, const void *, const klvr_VRTextureBounds_t *,
                                 uint32_t);
    klvr_VRTextureBounds_t lhalf = { 0, 0, 0.5f, 1 }, rhalf = { 0.5f, 0, 1, 1 };
    {
        // Not Vulkan: refused by name, nothing bound, no frame touched.
        klvr_Texture_t gl = { (void *)0x1234, 1, 0 };
        check(SLOT(comp, 6, fn_submit)(comp, 0, &gl, NULL, 0) == 102 &&
              strstr(log_text(), "texture type 1 handle 0x1234 flags=0x0 -> InvalidTexture"),
              "Compositor.Submit(OpenGL texture) -> InvalidTexture (102), logged");
    }
#if T_HAVE_VK
    // The checks below up to the async block run the reference host's scheme
    // (the fence waited inside Submit), whose layer carries THIS frame's
    // pictures; the async default is exercised after them.
    setenv("KL_OPENVR_EYE_COPY_ASYNC", "0", 1);
    int vkr = kl_vulkan_available() ? t_vk_up() : -100;
    check(vkr == 0, "the test's renderer: VkInstance, VkDevice and a graphics queue via kl_vulkan");
    if (vkr == 0) {
        static const uint8_t red[4] = { 255, 0, 0, 255 }, green[4] = { 0, 255, 0, 255 },
                             blue[4] = { 0, 0, 255, 255 };
        uint8_t got[4];
        VkImage src = t_eye_image(64, 32, 1, red, green);
        klvr_VRVulkanTextureData_t vk = { (uint64_t)(uintptr_t)src, T.dev, T.phys, T.inst, T.q,
                                          T.qfam, 64, 32, VK_FORMAT_R8G8B8A8_SRGB, 1 };
        // The reference's shape: flags 0x218, one image, uv halves, pose +
        // depth + motion riding behind Texture_t.
        klvr_VRTextureWithMotion_t tex;
        memset(&tex, 0, sizeof tex);
        tex.tex.handle = &vk;
        tex.tex.eType = 2;
        tex.mDeviceToAbsoluteTracking = rp[0].mDeviceToAbsoluteTracking;
        tex.depth.handle = (void *)0xde97;
        tex.motion.handle = (void *)0x3017;

        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t s0 = SLOT(comp, 6, fn_submit)(comp, 0, &tex, &lhalf, 0x218);
        char bound[160];
        snprintf(bound, sizeof bound, "bind_vulkan: instance %p physical %p device %p queue %u.0",
                 (void *)T.inst, (void *)T.phys, (void *)T.dev, T.qfam);
        check(s0 == 0 && strstr(log_text(), bound) != NULL,
              "Submit(left, 0x218) -> None; the FIRST Submit bound the session to the test's device");
        check(strstr(log_text(), "image=0x") && strstr(log_text(), "size=64x32 format=43 samples=1 "
                                                                    "flags=0x218") &&
              strstr(log_text(), "depth=0xde97 motion=0x3017"),
              "...VRTextureWithMotion_t decoded: depth at +64, motion at +144");
        char *rep = NULL;
        size_t rn = 0;
        FILE *mf = open_memstream(&rep, &rn);
        kl_openvr_report(mf);
        fclose(mf);
        check(rep && strstr(rep, "frames ended 0 with layers"), "...one eye in: the frame is still open");
        free(rep);
        int32_t s1 = SLOT(comp, 6, fn_submit)(comp, 1, &tex, &rhalf, 0x218);
        kl_ovrp_proj_layer pl[4];
        int npl = kl_ovrp_proj_layers_snapshot(pl, 4);
        check(s1 == 0 && npl == 1 && pl[0].slot[0] >= 0 && pl[0].slot[1] >= 0,
              "Submit(right) -> None and xrEndFrame took ONE projection layer naming both eyes");
        printf("       layer: slots %d/%d, head (%.3f %.3f %.3f), eye 0 tan L%.3f R%.3f\n",
               npl ? pl[0].slot[0] : -9, npl ? pl[0].slot[1] : -9, pl[0].pose[0], pl[0].pose[1],
               pl[0].pose[2], pl[0].tangents[0][0], pl[0].tangents[0][1]);
        rep = NULL;
        mf = open_memstream(&rep, &rn);
        kl_openvr_report(mf);
        fclose(mf);
        check(rep && strstr(rep, "frames ended 1 with layers"), "...the report counts it with layers");
        free(rep);
        int e0 = t_eye_is(0, red, got);
        printf("       eye 0 swapchain texel %u,%u,%u,%u\n", got[0], got[1], got[2], got[3]);
        int e1 = t_eye_is(1, green, got);
        printf("       eye 1 swapchain texel %u,%u,%u,%u\n", got[0], got[1], got[2], got[3]);
        check(e0 && e1, "pixel readback: eye 0's swapchain image is the left half (red), "
                        "eye 1's the right (green)");
        check(SLOT(comp, 6, fn_submit)(comp, 0, &tex, &lhalf, 0x218) == 1,
              "a Submit after the frame ended (no WaitGetPoses) -> RequestFailed");

        // One eye only: the next WaitGetPoses ends it with no layer.
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t a = SLOT(comp, 6, fn_submit)(comp, 0, &tex, &lhalf, 0x218);
        int32_t again = SLOT(comp, 6, fn_submit)(comp, 0, &tex, &lhalf, 0x218);
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        rep = NULL;
        mf = open_memstream(&rep, &rn);
        kl_openvr_report(mf);
        fclose(mf);
        check(a == 0 && again == 108 && kl_ovrp_proj_layers_snapshot(pl, 4) == 0 && rep &&
              strstr(rep, "frames ended 1 with layers / 6 empty"),
              "one-eye frame: the eye twice -> AlreadySubmitted (108); ended with NO layer");
        free(rep);

        // Submit_TextureWithPose alone (0x08): the layer is placed where that
        // pose says, not where WaitGetPoses' head was.
        struct { klvr_Texture_t t; klvr_HmdMatrix34_t pose; } wp;
        memset(&wp, 0, sizeof wp);
        wp.t = tex.tex;
        wp.pose = rp[0].mDeviceToAbsoluteTracking;
        wp.pose.m[0][3] = 1.0f; wp.pose.m[1][3] = 2.0f; wp.pose.m[2][3] = 3.0f;
        int32_t p0 = SLOT(comp, 6, fn_submit)(comp, 0, &wp, &lhalf, 0x08);
        int32_t p1 = SLOT(comp, 6, fn_submit)(comp, 1, &wp, &rhalf, 0x08);
        npl = kl_ovrp_proj_layers_snapshot(pl, 4);
        printf("       Submit_TextureWithPose: %d/%d, %d layer(s), head (%.3f %.3f %.3f)\n", p0, p1,
               npl, pl[0].pose[0], pl[0].pose[1], pl[0].pose[2]);
        check(p0 == 0 && p1 == 0 && npl == 1 && fabsf(pl[0].pose[0] - 1) < 0.01f &&
              fabsf(pl[0].pose[1] - 2) < 0.01f && fabsf(pl[0].pose[2] - 3) < 0.01f &&
              strstr(log_text(), "pose=(1.000 2.000 3.000) depth=0x0 motion=0x0"),
              "flags 0x08 (VRTextureWithPose_t): the views carry the Submit's pose (1,2,3)");

        // The depth info's projection (0x218, as Source 2 submits): the layer
        // states the frustum the guest DREW with, not GetProjectionRaw's — the
        // reference's ProjectionFov. An asymmetric one (l -1.2, r 0.9, down
        // -1.1, up 0.8 as tangents) in OpenVR's ComposeProjection shape.
        {
            float L = -1.2f, R = 0.9f, T = -1.1f, B = 0.8f, zn = 0.05f, zf = 100.0f;
            klvr_VRTextureWithMotion_t tp = tex;
            memset(&tp.depth.mProjection, 0, sizeof tp.depth.mProjection);
            tp.depth.mProjection.m[0][0] = 2.0f / (R - L);
            tp.depth.mProjection.m[0][2] = (R + L) / (R - L);
            tp.depth.mProjection.m[1][1] = 2.0f / (B - T);
            tp.depth.mProjection.m[1][2] = (B + T) / (B - T);
            tp.depth.mProjection.m[2][2] = -zf / (zf - zn);
            tp.depth.mProjection.m[2][3] = -zf * zn / (zf - zn);
            tp.depth.mProjection.m[3][2] = -1.0f;
            SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
            int32_t q0 = SLOT(comp, 6, fn_submit)(comp, 0, &tp, &lhalf, 0x218);
            int32_t q1 = SLOT(comp, 6, fn_submit)(comp, 1, &tp, &rhalf, 0x218);
            npl = kl_ovrp_proj_layers_snapshot(pl, 4);
            const float *t0 = pl[0].tangents[0];
            printf("       Submit projection: %d/%d, %d layer(s), eye 0 tan L%.3f R%.3f U%.3f D%.3f\n",
                   q0, q1, npl, t0[0], t0[1], t0[2], t0[3]);
            check(q0 == 0 && q1 == 0 && npl == 1 && fabsf(t0[0] - 1.2f) < 1e-3f &&
                  fabsf(t0[1] - 0.9f) < 1e-3f && fabsf(t0[2] - 0.8f) < 1e-3f &&
                  fabsf(t0[3] - 1.1f) < 1e-3f &&
                  strstr(log_text(), "THEY DIFFER; the layer states the drawn one"),
                  "flags 0x218 with a projection: the layer's frustum is the Submit's own "
                  "(asymmetric l/r/up/down = 1.2/0.9/0.8/1.1)");
        }

        // Submit_TextureWithDepth alone (0x10): VRTextureWithDepth_t, depth at +16.
        struct { klvr_Texture_t t; void *depth; float rest[18]; } wd;
        memset(&wd, 0, sizeof wd);
        wd.t = tex.tex;
        wd.depth = (void *)0xd16;
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t d0 = SLOT(comp, 6, fn_submit)(comp, 0, &wd, &lhalf, 0x10);
        int32_t d1 = SLOT(comp, 6, fn_submit)(comp, 1, &wd, &rhalf, 0x10);
        check(d0 == 0 && d1 == 0 && strstr(log_text(), "pose=none depth=0xd16 motion=0x0"),
              "flags 0x10 (VRTextureWithDepth_t): depth decoded at +16, accepted and ignored");

        // Multisampled: resolved, then blitted.
        VkImage ms = t_eye_image(64, 32, 4, blue, blue);
        klvr_VRVulkanTextureData_t vms = vk;
        vms.m_nImage = (uint64_t)(uintptr_t)ms;
        vms.m_nSampleCount = 4;
        klvr_Texture_t tms = { &vms, 2, 0 };
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t m0 = SLOT(comp, 6, fn_submit)(comp, 0, &tms, &lhalf, 0);
        int32_t m1 = SLOT(comp, 6, fn_submit)(comp, 1, &tms, &rhalf, 0);
        int b0 = t_eye_is(0, blue, got), b1 = t_eye_is(1, blue, got);
        check(ms && m0 == 0 && m1 == 0 && b0 && b1 &&
              strstr(log_text(), "4 sample(s), 64x32 -> swapchain VkFormat 43") &&
              strstr(log_text(), "RESOLVE (vkCmdResolveImage) then vkCmdBlitImage"),
              "a 4x MSAA source is resolved (vkCmdResolveImage) and both eyes read back blue");

        // PostPresentHandoff ends what Submit left open: here, nothing.
        SLOT(comp, 9, void (*)(void *))(comp);

        // The present counter every 60 frames, in the shape round8-rate.py reads
        // (`source2-xr-present frames=(\d+) pair=(\d+)`): run frames to 60.
        for (int i = 0; i < 60 && !strstr(log_text(), "source2-xr-present frames=60 "); i++) {
            SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
            SLOT(comp, 6, fn_submit)(comp, 0, &tex, &lhalf, 0x218);
            SLOT(comp, 6, fn_submit)(comp, 1, &tex, &rhalf, 0x218);
            SLOT(comp, 9, void (*)(void *))(comp);
        }
        const char *pr = strstr(log_text(), "source2-xr-present frames=60 pair=");
        int pair = pr ? atoi(pr + strlen("source2-xr-present frames=60 pair=")) : -1;
        printf("       %.*s\n", pr ? (int)strcspn(pr, "\n") : 4, pr ? pr : "none");
        check(pair > 40 && pair < 60, "source2-xr-present frames=60 pair=N: every frame counted, "
                                      "pairs = the frames that carried both eyes");
        check(strstr(log_text(), "guest-eye-copy call=0 eye=0 copied=1 result=0 reason=complete "
                                 "stage=wait-fence src=64x32 samples=1 layer=0 rect=0,0,32,32") != NULL,
              "the per-eye copy line, in the reference host's guest-eye-copy shape");

        // ---- the async eye copy (round N8, the default): Submit does not wait,
        // the eye's next Submit settles the copy, so a frame's layer shows the
        // PREVIOUS frame's pictures, each placed at the head it was drawn at.
        setenv("KL_OPENVR_EYE_COPY_ASYNC", "1", 1);
        VkImage src2 = t_eye_image(64, 32, 1, blue, red);
        klvr_VRVulkanTextureData_t vk2 = vk;
        vk2.m_nImage = (uint64_t)(uintptr_t)src2;
        struct { klvr_Texture_t t; klvr_HmdMatrix34_t pose; } ap;
        memset(&ap, 0, sizeof ap);
        ap.t = (klvr_Texture_t){ &vk2, 2, 0 };
        ap.pose = rp[0].mDeviceToAbsoluteTracking;
        ap.pose.m[0][3] = 4.0f; ap.pose.m[1][3] = 5.0f; ap.pose.m[2][3] = 6.0f;
        klvr_Texture_t plain = { &vk, 2, 0 };
        // Frame A: red | green, submitted, not waited: no complete picture yet.
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t aa0 = SLOT(comp, 6, fn_submit)(comp, 0, &plain, &lhalf, 0);
        int32_t aa1 = SLOT(comp, 6, fn_submit)(comp, 1, &plain, &rhalf, 0);
        int npa = kl_ovrp_proj_layers_snapshot(pl, 4);
        check(aa0 == 0 && aa1 == 0 && npa == 0 &&
              strstr(log_text(), "eye copy: ASYNC (stage=submitted)") != NULL,
              "async frame A: both Submits -> None without waiting (stage=submitted), the "
              "frame-end queue drain off, and NO layer (nothing complete yet)");
        // Frame B: blue | red with a Submit pose; its layer shows frame A.
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t ab0 = SLOT(comp, 6, fn_submit)(comp, 0, &ap, &lhalf, 0x08);
        int32_t ab1 = SLOT(comp, 6, fn_submit)(comp, 1, &ap, &rhalf, 0x08);
        int npb = kl_ovrp_proj_layers_snapshot(pl, 4);
        float bz = npb ? pl[0].pose[2] : -99.f;
        int r0 = t_eye_is(0, red, got), r1 = t_eye_is(1, green, got);
        check(ab0 == 0 && ab1 == 0 && npb == 1 && r0 && r1 && fabsf(bz - 3.0f) > 0.5f,
              "async frame B: the layer shows frame A's pictures (red | green) at frame A's head");
        // Frame C: red | green again, no pose; its layer shows frame B at B's pose.
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        int32_t ac0 = SLOT(comp, 6, fn_submit)(comp, 0, &plain, &lhalf, 0);
        int32_t ac1 = SLOT(comp, 6, fn_submit)(comp, 1, &plain, &rhalf, 0);
        int npc = kl_ovrp_proj_layers_snapshot(pl, 4);
        int c0 = t_eye_is(0, blue, got), c1 = t_eye_is(1, red, got);
        printf("       async layers: A %d, B %d (z %.3f), C %d head (%.3f %.3f %.3f)\n", npa, npb,
               bz, npc, npc ? pl[0].pose[0] : -9, npc ? pl[0].pose[1] : -9,
               npc ? pl[0].pose[2] : -9);
        check(ac0 == 0 && ac1 == 0 && npc == 1 && c0 && c1 && fabsf(pl[0].pose[0] - 4) < 0.01f &&
              fabsf(pl[0].pose[1] - 5) < 0.01f && fabsf(pl[0].pose[2] - 6) < 0.01f,
              "async frame C: the layer shows frame B's pictures (blue | red) at B's Submit "
              "pose (4,5,6), not C's head");
        check(strstr(log_text(), "fence did not signal") == NULL,
              "async: no fence timed out");
        // Frame C's copies stay outstanding (held) until the end: shutdown
        // below races a Submit against them.
        static klvr_VRVulkanTextureData_t race_vk;
        static klvr_Texture_t race_tex;
        race_vk = vk;
        race_tex = (klvr_Texture_t){ &race_vk, 2, 0 };
        g_race.comp = comp;
        g_race.tex = &race_tex;
        g_race.b[0] = &lhalf;
        g_race.b[1] = &rhalf;
    }
#else
    printf("       (no Vulkan headers in this build: the eye copy is not exercised)\n");
#endif

    // ---- bool arguments with garbage above bit 7 (a Linux caller's w1)
    typedef void (*fn_fade)(void *, float, uint32_t);
    typedef float (*fn_alpha)(void *);
    SLOT(comp, 16, fn_fade)(comp, 1.5f, 0x12345600u);         // false, dirty upper bits
    float a0 = SLOT(comp, 17, fn_alpha)(comp);
    SLOT(comp, 16, fn_fade)(comp, 0.0f, 0xabcdef01u);         // true, dirty upper bits
    float a1 = SLOT(comp, 17, fn_alpha)(comp);
    check(a0 == 0.0f && a1 == 1.0f, "Compositor.FadeGrid masks its bool (0x12345600 = false)");
    SLOT(chap, 7, void (*)(void *, uint32_t))(chap, 0xffffff00u);
    check(strstr(log_text(), "ForceBoundsVisible 0") != NULL,
          "Chaperone.ForceBoundsVisible(0xffffff00) reads false");
    check(SLOT(chap, 0, uint32_t (*)(void *))(chap) == 1, "Chaperone.GetCalibrationState -> OK");

    // ---- events, at the game's size and at a short one
    klvr_VREvent_t ev;
    while (SLOT(sys, 30, uint8_t (*)(void *, void *, uint32_t))(sys, &ev, 60)) {}
    kl_openvr_post_event(700, 0);
    kl_openvr_post_event(700, 0);
    memset(&ev, 0xee, sizeof ev);
    int got = SLOT(sys, 30, uint8_t (*)(void *, void *, uint32_t))(sys, &ev, 60);
    check(got && ev.eventType == 700 && ev.trackedDeviceIndex == 0,
          "System.PollNextEvent(60) delivers VREvent_Quit");
    struct { uint8_t ev[16]; uint8_t canary[48]; } shortev;
    memset(&shortev, 0xcc, sizeof shortev);
    got = SLOT(sys, 30, uint8_t (*)(void *, void *, uint32_t))(sys, &shortev, 16);
    int canary = 1;
    for (int i = 0; i < 48; i++) canary &= shortev.canary[i] == 0xcc;
    check(got && canary, "...a caller's smaller VREvent_t is never written past its size");
    check(!SLOT(sys, 30, uint8_t (*)(void *, void *, uint32_t))(sys, &ev, 60), "...then empty");

    // ---- the mailbox
    typedef int32_t (*fn_reg)(void *, const char *, uint64_t *);
    typedef int32_t (*fn_send)(void *, uint64_t, const char *, const char *);
    typedef int32_t (*fn_read)(void *, uint64_t, char *, uint32_t, uint32_t *);
    uint64_t mb = 0, peer = 0;
    check(SLOT(mail, 0, fn_reg)(mail, "hlvr/mailbox", &mb) == 0 && mb == 1 &&
          SLOT(mail, 0, fn_reg)(mail, "test/peer", &peer) == 0 && peer == 2,
          "Mailbox.Register hands out handles 1, 2");
    check(SLOT(mail, 2, fn_send)(mail, peer, "hlvr/mailbox", "{\"type\":\"ready\"}") == 0 &&
          SLOT(mail, 2, fn_send)(mail, mb, "hlvr/interstitials", "x") == 1,
          "Mailbox.Send: to a registered name 0, to an unregistered one 1 (the reference)");
    char mbuf[1024];
    uint32_t req = 0;
    int r1 = SLOT(mail, 3, fn_read)(mail, mb, mbuf, 4, &req);
    uint32_t req1 = req;
    int r2 = SLOT(mail, 3, fn_read)(mail, mb, mbuf, sizeof mbuf, &req);
    int r3 = SLOT(mail, 3, fn_read)(mail, mb, mbuf, sizeof mbuf, &req);
    check(r1 == 2 && req1 == 17 && r2 == 0 && !strcmp(mbuf, "{\"type\":\"ready\"}") && r3 == 1,
          "Mailbox.Read: too small 2 (+required), then the message 0, then nothing 1");
    check(SLOT(mail, 1, int32_t (*)(void *, uint64_t))(mail, peer) == 0,
          "Mailbox.Unregister");

    // ---- IVRInput, over the fixture manifest and kl_openxr's actions
    char manifest[PATH_MAX];
    if (!realpath("tests/data/openvr/actions.json", manifest))
        snprintf(manifest, sizeof manifest, "tests/data/openvr/actions.json");
    check(SLOT(input, 0, int32_t (*)(void *, const char *))(input, manifest) == 0,
          "Input.SetActionManifestPath (kl_openvr_manifest_flatten -> binding table)");
    typedef int32_t (*fn_handle)(void *, const char *, uint64_t *);
    uint64_t s_def = 0, s_move = 0, a_fire = 0, a_pull = 0, a_move = 0, a_jump = 0, a_turn = 0,
             a_menu = 0, a_pose = 0, a_hap = 0, src_l = 0, src_r = 0, src_x = 1;
    SLOT(input, 1, fn_handle)(input, "/actions/default", &s_def);
    SLOT(input, 1, fn_handle)(input, "/actions/move", &s_move);
    SLOT(input, 2, fn_handle)(input, "/actions/default/in/Fire", &a_fire);
    SLOT(input, 2, fn_handle)(input, "/actions/default/in/TriggerPull", &a_pull);
    SLOT(input, 2, fn_handle)(input, "/actions/move/in/Move", &a_move);
    SLOT(input, 2, fn_handle)(input, "/actions/move/in/Jump", &a_jump);
    SLOT(input, 2, fn_handle)(input, "/actions/move/in/TurnLeft", &a_turn);
    SLOT(input, 2, fn_handle)(input, "/actions/dev/in/ToggleMenu", &a_menu);
    SLOT(input, 2, fn_handle)(input, "/actions/default/in/HandPoseRight", &a_pose);
    SLOT(input, 2, fn_handle)(input, "/actions/default/in/HapticVibration", &a_hap);
    SLOT(input, 3, fn_handle)(input, "/user/hand/left", &src_l);
    SLOT(input, 3, fn_handle)(input, "/user/hand/right", &src_r);
    int32_t gp_err = SLOT(input, 3, fn_handle)(input, "/user/gamepad", &src_x);
    check(s_def == 0x10001 && s_move == 0x10002 && (a_fire >> 16) == 2 && src_l == 0x30001 &&
          src_r == 0x30002 && gp_err == 1 && src_x == 0,
          "Input handles: sets 0x1000n, actions 0x2000n, sources 0x3000n, /user/gamepad NameNotFound");

    // The frontend publishes: both hands tracked, right trigger at 0.9, A held,
    // left stick full forward, right stick hard left.
    kl_ovrp_set_hand_pose(0, -0.2f, 1.0f, -0.3f, 0, 0, 0, 1);
    kl_ovrp_set_hand_pose(1,  0.2f, 1.0f, -0.3f, 0, 0, 0, 1);
    kl_ovrp_set_controller_input(0, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f);
    kl_ovrp_set_controller_input(1, KL_OVRP_RAW_A, 0, 0.9f, 0.0f, -1.0f, 0.0f);
    // A frame boundary, as the game has one between frames: kl_openxr latches
    // the published hands at xrWaitFrame.
    SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
    klvr_VRActiveActionSet_t act[2];
    memset(act, 0, sizeof act);
    act[0].ulActionSet = s_def;
    act[1].ulActionSet = s_move;
    typedef int32_t (*fn_update)(void *, void *, uint32_t, uint32_t);
    check(SLOT(input, 4, fn_update)(input, act, sizeof act[0], 2) == 0, "Input.UpdateActionState(32, 2)");
    typedef int32_t (*fn_dig)(void *, uint64_t, klvr_InputDigitalActionData_t *, uint32_t, uint64_t);
    typedef int32_t (*fn_ana)(void *, uint64_t, klvr_InputAnalogActionData_t *, uint32_t, uint64_t);
    klvr_InputDigitalActionData_t dd;
    klvr_InputAnalogActionData_t ad;
    SLOT(input, 5, fn_dig)(input, a_fire, &dd, 24, 0);
    check(dd.bActive && dd.bState && dd.bChanged && dd.activeOrigin == src_r,
          "digital: right trigger 0.9 past its 0.8 click -> pressed, changed, origin right");
    SLOT(input, 5, fn_dig)(input, a_fire, &dd, 24, src_l);
    check(!dd.bState, "...restricted to the left hand -> not pressed");
    SLOT(input, 6, fn_ana)(input, a_pull, &ad, 48, 0);
    check(ad.bActive && fabsf(ad.x - 0.9f) < 1e-3f, "analog: trigger pull 0.9");
    SLOT(input, 6, fn_ana)(input, a_move, &ad, 48, 0);
    check(ad.bActive && fabsf(ad.y - 1.0f) < 1e-3f && fabsf(ad.x) < 1e-3f,
          "analog: left stick forward through the 10%/90% zones -> y 1.0");
    SLOT(input, 5, fn_dig)(input, a_jump, &dd, 24, 0);
    int jump = dd.bState;
    SLOT(input, 5, fn_dig)(input, a_turn, &dd, 24, 0);
    int turn = dd.bState;
    SLOT(input, 5, fn_dig)(input, a_menu, &dd, 24, 0);
    check(jump && turn && !dd.bActive, "digital: A click, dpad west; an inactive set reads inactive");
    typedef int32_t (*fn_pose)(void *, uint64_t, uint32_t, float, klvr_InputPoseActionData_t *,
                               uint32_t, uint64_t);
    klvr_InputPoseActionData_t pd;
    SLOT(input, 7, fn_pose)(input, a_pose, 1, 0.0f, &pd, sizeof pd, 0);
    printf("       right grip at (%.3f %.3f %.3f) active %d valid %d\n",
           pd.pose.mDeviceToAbsoluteTracking.m[0][3], pd.pose.mDeviceToAbsoluteTracking.m[1][3],
           pd.pose.mDeviceToAbsoluteTracking.m[2][3], pd.bActive, pd.pose.bPoseIsValid);
    // kl_openxr applies its grip correction (KL_XR_GRIP_*) to the published
    // hand, so the side is the assertion, not the exact metre.
    check(pd.bActive && pd.pose.bPoseIsValid && pd.activeOrigin == src_r &&
          pd.pose.mDeviceToAbsoluteTracking.m[0][3] > 0.1f,
          "pose: HandPoseRight follows the right grip (tracked, right of centre)");
    typedef int32_t (*fn_hap)(void *, uint64_t, float, float, float, float, uint64_t);
    check(SLOT(input, 23, fn_hap)(input, a_hap, 0, 0.011f, 1, 0.2f, src_r) == 0 &&
          strstr(log_text(), "hands=2") != NULL, "haptic: one pulse on the right hand");
    kl_ovrp_set_controller_input(1, 0, 0, 0.75f, 0.0f, 0.0f, 0.0f);   // inside the hysteresis
    SLOT(input, 4, fn_update)(input, act, sizeof act[0], 2);
    SLOT(input, 5, fn_dig)(input, a_fire, &dd, 24, 0);
    int held = dd.bState && !dd.bChanged;
    kl_ovrp_set_controller_input(1, 0, 0, 0.6f, 0.0f, 0.0f, 0.0f);
    SLOT(input, 4, fn_update)(input, act, sizeof act[0], 2);
    SLOT(input, 5, fn_dig)(input, a_fire, &dd, 24, 0);
    check(held && !dd.bState && dd.bChanged, "digital: 0.75 holds the click (off at 0.7), 0.6 releases");

    // ---- the differential: what the game is handed vs the reference door
    //
    // A MOVING head (yaw/pitch/roll + sway), canted asymmetric eyes (the
    // Vision Pro's ~1 deg cant about a mostly-X axis, an off-centre IPD) and two
    // rotating controllers, frame by frame. For each frame the reference door's
    // answer is rebuilt from kl_openxr directly with the transcription above —
    // head = VIEW in STAGE, eyes = RelativePose(head, views in STAGE), hands =
    // grip o RelativePose(A, I) — and compared with what kl_openvr hands the
    // game through WaitGetPoses, GetEyeToHeadTransform and the pose action.
    {
        float c0[4], c1[4];
        ref_axis_angle(0.962f, -0.270f, -0.030f, 1.061f, c0);
        ref_axis_angle(0.947f, -0.312f, 0.082f, 1.083f, c1);
        kl_ovrp_set_eye_rotation(0, c0[0], c0[1], c0[2], c0[3]);
        kl_ovrp_set_eye_rotation(1, c1[0], c1[1], c1[2], c1[3]);
        kl_ovrp_set_eye_offset(0, -0.0307f, 0.0005f, -0.0008f);
        kl_ovrp_set_eye_offset(1, 0.0312f, -0.0005f, 0.0011f);
        float w_turn = 0, w_hmd = 0, w_e2h = 0, w_hand = 0, w_act = 0, w_round = 0, w_cant = 0;
        int frames = 0, valid = 0;
        for (int k = 0; k < 48; k++) {
            ref_pose head, hand[2];
            motion_frame(k, &head, hand);
            kl_ovrp_set_head_pose(head.p[0], head.p[1], head.p[2], head.q[0], head.q[1],
                                  head.q[2], head.q[3]);
            for (int h = 0; h < 2; h++)
                kl_ovrp_set_hand_pose(h, hand[h].p[0], hand[h].p[1], hand[h].p[2], hand[h].q[0],
                                      hand[h].q[1], hand[h].q[2], hand[h].q[3]);
            if (SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64) != 0) continue;
            frames++;
            // The reference door, out of the same OpenXR answers.
            klxr_SpaceLocation hl = { KLXR_T_SPACE_LOCATION, NULL, 0, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
            klvr_xr.xrLocateSpace(klvr.space_view, klvr.space_stage, klvr.display_time, &hl);
            struct { int32_t type; const void *next; int32_t cfg; int64_t time; void *space; } li =
                { 6, NULL, 2, klvr.display_time, klvr.space_stage };
            struct { int32_t type; void *next; uint64_t flags; } vs = { 11, NULL, 0 };
            struct { int32_t type; void *next; klxr_Posef pose; klxr_Fovf fov; } v[2];
            memset(v, 0, sizeof v);
            v[0].type = v[1].type = 7;
            uint32_t nv = 0;
            klvr_xr.xrLocateViews(klvr.session, &li, &vs, 2, &nv, v);
            ref_pose rhead = ref_of(hl.pose);
            klvr_HmdMatrix34_t want = ref_matrix(rhead);
            float d = ref_diff(&want, &rp[0].mDeviceToAbsoluteTracking);
            if (d > w_hmd) w_hmd = d;
            // The head the game was handed must actually move with the script.
            float hy = atan2f(rp[0].mDeviceToAbsoluteTracking.m[0][2],
                              rp[0].mDeviceToAbsoluteTracking.m[2][2]) * 57.29578f;
            float sy = atan2f(want.m[0][2], want.m[2][2]) * 57.29578f;
            if (fabsf(hy) > w_turn) w_turn = fabsf(hy);
            (void)sy;
            for (int e = 0; e < 2; e++) {
                klvr_HmdMatrix34_t got = SLOT(sys, 5, fn_e2h)(sys, (uint32_t)e);
                klvr_HmdMatrix34_t ref = ref_matrix(ref_relative(rhead, ref_of(v[e].pose)));
                d = ref_diff(&ref, &got);
                if (d > w_e2h) w_e2h = d;
                // ...and the cant survives into what the game is told.
                float tilt = acosf(fminf(1.0f, got.m[1][1] > got.m[2][2] ? got.m[2][2] : got.m[1][1]));
                if (tilt > w_cant) w_cant = tilt;
            }
            for (int h = 0; h < 2; h++) {
                klxr_SpaceLocation gl = { KLXR_T_SPACE_LOCATION, NULL, 0, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
                klvr_xr.xrLocateSpace(klvr.grip_space[h], klvr.space_stage, klvr.display_time, &gl);
                ref_pose a = ref_of(klvr_touch_grip_in_raw(h)), id = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
                ref_pose raw = ref_compose(ref_of(gl.pose), ref_relative(a, id));
                klvr_HmdMatrix34_t ref = ref_matrix(raw);
                const klvr_TrackedDevicePose_t *dp = &rp[KLVR_DEVICE_LEFT + h];
                valid += dp->bPoseIsValid;
                d = ref_diff(&ref, &dp->mDeviceToAbsoluteTracking);
                if (d > w_hand) w_hand = d;
                // Round trip: the game's Touch raw->grip offset (A) applied to
                // what it was handed lands exactly on the grip — the pivot.
                klvr_HmdMatrix34_t back = ref_matrix(ref_compose(raw, a)), grip = ref_matrix(ref_of(gl.pose));
                d = ref_diff(&back, &grip);
                if (d > w_round) w_round = d;
                if (h == 1) {
                    klvr_InputPoseActionData_t ap;
                    SLOT(input, 7, fn_pose)(input, a_pose, 1, 0.0f, &ap, sizeof ap, 0);
                    d = ref_diff(&ref, &ap.pose.mDeviceToAbsoluteTracking);
                    if (d > w_act) w_act = d;
                }
            }
        }
        printf("       %d moving frames: worst |delta| HMD %.2g, eye-to-head %.2g, hands %.2g, "
               "pose action %.2g, raw o A vs grip %.2g; eye cant told %.3f deg\n",
               frames, w_hmd, w_e2h, w_hand, w_act, w_round, w_cant * 57.29578f);
        printf("       the handed head turned up to %.1f deg of yaw (script: 40)\n", w_turn);
        check(frames == 48 && w_hmd < 2e-5f && w_turn > 30.0f,
              "differential: the HMD matrix == the reference's PoseMatrix(VIEW in STAGE), moving head");
        check(w_e2h < 2e-5f && w_cant > 0.5f / 57.29578f,
              "differential: eye-to-head == RelativePose(head, view) — canted, asymmetric eyes, cant kept");
        check(valid == 96 && w_hand < 2e-5f && w_act < 2e-5f,
              "differential: controllers == the reference's raw-pose space (grip o A^-1), "
              "WaitGetPoses and the pose action");
        check(w_round < 2e-5f, "...and raw o A is the grip again: the hand pivots where the grip is");
        // A linear velocity is moved to the raw origin: v + w x r.
        klxr_Posef g = { { 0, 0, 0, 1 }, { 0, 1, 0 } };
        float ang[3] = { 0, 2.0f, 0 }, lin[3] = { 0, 0, 0 };
        klxr_Posef rr = klvr_hand_raw_from_grip(1, g, ang, lin);
        float want_x = 2.0f * (rr.position.z - g.position.z), want_z = -2.0f * (rr.position.x - g.position.x);
        check(fabsf(lin[0] - want_x) < 1e-6f && fabsf(lin[2] - want_z) < 1e-6f && lin[1] == 0.0f &&
              fabsf(rr.position.z - g.position.z + 0.10194820f) < 0.02f,
              "the raw pose's linear velocity is the grip's plus w x r; raw sits ~10 cm ahead");
        kl_ovrp_set_eye_rotation(0, 0, 0, 0, 1);
        kl_ovrp_set_eye_rotation(1, 0, 0, 0, 1);
        kl_ovrp_set_eye_offset(0, 0, 0, 0);
        kl_ovrp_set_eye_offset(1, 0, 0, 0);
    }

    // ---- unimplemented slots: each logs itself and returns its zero
    uint64_t before = 0, after = 0;
    kl_openvr_slot_info("IVRSystem_026", 16, NULL, NULL, &before);
    uint32_t lvl = SLOT(sys, 16, uint32_t (*)(void *, uint32_t))(sys, 0);
    const char *nm = NULL;
    int stub = 0;
    kl_openvr_slot_info("IVRSystem_026", 16, &nm, &stub, &after);
    check(lvl == 0 && stub && after == before + 1 && !strcmp(nm, "GetTrackedDeviceActivityLevel") &&
          strstr(log_text(), "unimplemented IVRSystem_026 slot=16 GetTrackedDeviceActivityLevel"),
          "System slot 16 (stub): returns 0, counts, logs its own interface/slot/name");
    check(SLOT(input, 11, int32_t (*)(void *, uint32_t))(input, 1) == 13 &&
          strstr(log_text(), "IVRInput_011 slot=11 SetDominantHand"),
          "Input slot 11 (stub): NoData (13), as the reference");
    check(SLOT(comp, 12, float (*)(void *))(comp) == 0.0f, "Compositor slot 12 (float stub) -> 0.0");
    klvr_HmdColor_t c = SLOT(comp, 15, klvr_HmdColor_t (*)(void *))(comp);
    check(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0, "Compositor slot 15 (HmdColor_t stub) -> 0");
    const char *en = SLOT(sys, 29, const char *(*)(void *, uint32_t))(sys, 4);
    check(en && !*en, "System slot 29 (name-of-enum stub) -> \"\", never NULL");
    check(SLOT(xrp, 4, uint64_t (*)(void *))(xrp) == 0 &&
          strstr(log_text(), "IXrProto_001 slot=4 XrProtoSlot4"), "XrProto slot 4 (stub) -> 0");

#if T_HAVE_VK
    if (g_race.comp) {
        // Teardown with copies outstanding, racing a Submit that settles them:
        // teardown must wait the settle out and every outstanding fence before
        // destroying fences and swapchains (no NULL WaitForFences, no write
        // into a destroyed image); the late Submit fails cleanly.
        static klvr_VRTextureBounds_t lb = { 0, 0, 0.5f, 1 }, rb = { 0.5f, 0, 1, 1 };
        g_race.b[0] = &lb; g_race.b[1] = &rb;
        SLOT(comp, 2, fn_wgp)(comp, rp, 64, gp, 64);
        pthread_t th;
        pthread_create(&th, NULL, t_race_submit, NULL);
        while (!__atomic_load_n(&g_race.go, __ATOMIC_ACQUIRE)) { }  // into Submit, then down
        shutdown();
        pthread_join(th, NULL);
        printf("       teardown race: Submit results %d %d\n", g_race.r[0], g_race.r[1]);
        check(strstr(log_text(), "eye copy teardown: waited ") != NULL &&
              strstr(log_text(), "fence(s), all signalled") != NULL &&
              strstr(log_text(), "fence did not signal") == NULL,
              "shutdown with copies outstanding (racing a Submit): every outstanding copy "
              "fence waited before teardown, all signalled");
        unsetenv("KL_OPENVR_EYE_COPY_ASYNC");
    } else
#endif
    shutdown();
    check(strstr(log_text(), "VR_ShutdownInternal") != NULL, "VR_ShutdownInternal");

    kl_openvr_report(stdout);
    if (getenv("KL_OPENVR_TEST_LOG")) fputs(log_text(), stdout);
    printf("%s: kl_openvr answers through its vtables as libvr.so calls them\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
