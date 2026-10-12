# Round 25 — the stereo path itself: `KL_UE4_MULTIVIEW=1`

Round 19 called `KL_UE4_MULTIVIEW=1` **a trap** and told later rounds not to
reach for it. This round VERIFIED that claim in MoltenVK's own source instead of
trusting it, and the verdict is split: **the mechanism r19 named is real, and the
conclusion it drew from the mechanism is wrong.**

## 1. What Klepton was doing, and why it does not apply here

`runtime/guest/kl_ue4.c:580-600` (`ue4_no_multiview`) zeroes two exported UE4
globals every pump tick:

    GSupportsMobileMultiView
    FOpenGLES::bSupportsMobileMultiView

The comment above it is an argument about **OpenGL ES**: UE4's
`GLSLToDeviceCompatibleGLSL` prepends `layout(num_views = 2) in;` to every cooked
shader when the flag is true, ANGLE's Metal backend implements no multiview, so
every such shader fails to compile and UE4 turns the Fatal into a ForceQuit. The
clear is UE4's own honest fallback for a device without `GL_OVR_multiview2`.

**Into the Radius is a Vulkan-only guest.** No cooked GLSL is rewritten, ANGLE is
not in the picture, and the gateway's GL extension string decides nothing. The
game ships `vr.MobileMultiView=True` (r19 §2, the packaged
`DefaultEngine.ini`), and Klepton has been clearing it since the first launch —
so **every one of the ten device launches ran a two-sequential-view mobile stereo
path (DoubleWide) that the title never shipped with**, on top of GPUScene and
dynamic instancing.

`KL_UE4_MULTIVIEW=1` leaves the guest's own decision alone.

## 2. What MoltenVK actually does with multiview — read, not assumed

Cloned `v1.4.2` (the exact version in `vendor-moltenvk`:
`out/include/MoltenVK/mvk_private_api.h:65-67` = 1.4.2).

**There is no vertex amplification anywhere in MoltenVK v1.4.2.** A grep of the
whole tree for `setVertexAmplificationCount`, `maxVertexAmplificationCount` and
`vertexAmplification` returns **zero hits**. (The xros dylib does carry the
selector string `supportsVertexAmplificationCount:`, but no MoltenVK code names
it — it is a Metal selector reference, not a call site.) So the option r19 did
not consider does not exist to be considered.

**Multiview is layered rendering + instance multiplication**, exactly as r19
said:

- `GPUObjects/MVKDevice.h:430` —
  `bool canUseInstancingForMultiview() { return _metalFeatures.layeredRendering; }`
  Apple-family GPUs have layered rendering, so this is the live path on visionOS.
- `Commands/MVKCmdDraw.mm:323-324` and `:611-612` —
  `uint32_t viewCount = subpass->isMultiview() ? subpass->getViewCountInMetalPass(...) : 1;`
  `uint32_t instanceCount = _instanceCount * viewCount;`
- `GPUObjects/MVKDevice.mm:4844-4858` (`getViewCountInMetalPass`) and
  `:4807-4822` (`getMultiviewMetalPassCount`) — with instancing available, a
  2-bit view mask is **one** Metal pass of view count 2; without it the pass is
  unrolled into two.
- `GPUObjects/MVKFramebuffer.mm:45` — `mtlTexDesc.arrayLength = subpass->getViewCountInMetalPass(passIdx);`
- `GPUObjects/MVKPipeline.mm:2138-2139` — `mslOptions.multiview` and
  `multiview_layered_rendering` are handed to SPIRV-Cross, which is what turns
  `gl_ViewIndex` into `instance_id % viewCount` and writes
  `render_target_array_index`.

**And the collision r19 predicted is explicitly compensated for.**
`GPUObjects/MVKPipeline.mm:1876-1884`:

    // Adjusts step rates for per-instance vertex buffers based on the number of views to be drawn.
    void MVKGraphicsPipeline::adjustVertexInputForMultiview(...)
        if (inputDesc.layouts[vbIdx].stepFunction == MTLVertexStepFunctionPerInstance) {
            inputDesc.layouts[vbIdx].stepRate = inputDesc.layouts[vbIdx].stepRate / oldViewCount * viewCount;

An instance-rate vertex stream — which is precisely what GPUScene's PrimitiveId
buffer is — has its Metal step rate multiplied by the view count, so instances
`2N` and `2N+1` both read element `N`. MoltenVK also compiles **a separate
pipeline per pass view count** for this reason (`:1105-1126`, with the source's
own "This really sucks." comment). `MVKDevice.mm:823` drops
`maxMultiviewInstanceIndex` to `UINT_MAX/32` to pay for it.

**So instance multiplication is not a head-on collision with an instance-rate
PrimitiveId stream — it is a case MoltenVK handles by construction.** r19's
"expect it to make things dramatically worse" was an inference from half the
code. That does not make the path *proven* correct, but it removes the reason
not to test it.

## 3. Nothing in Klepton strips the extension or the feature

- The xros MoltenVK advertises `VK_KHR_multiview`
  (`strings vendor-moltenvk/out/xros/MoltenVK.framework/MoltenVK`, one hit).
- `libUE4.so` names `VK_KHR_multiview` (one hit, UTF-8) — 4.27's Vulkan RHI asks
  for it by name.
- `runtime/gfx/kl_vulkan.c:800-808` keeps every requested device extension the
  physical device advertises and drops only unsupported ones. **The multiview run
  logs zero "NOT supported by MoltenVK — dropped" lines** and enables 10
  extensions.
- `vkCreateDevice`'s `pNext` chain is passed through untouched (`mine = *ci`;
  only the extension and layer arrays are replaced), so a
  `VkPhysicalDeviceMultiviewFeatures` in the chain reaches MoltenVK intact.
- `vkGetPhysicalDeviceFeatures2` is **not hooked** — the one call in the file
  (`:767-788`) is our own placement-heap readback on a local struct.

## 4. What the eye layer does under multiview

`runtime/xr/kl_ovrp.c:7466` already honours the Array layout for a Vulkan guest:

    if (asked == KLOVRP_LAYOUT_ARRAY && kl_vulkan_guest_active() && kl_ovrp_multiview())
        return asked;

The only refusal in the run is the pair of boot-time layers created **before the
Vulkan device exists** (`SetupLayer(device=0x0)`), which take the Stereo
description and are torn down. Every real eye layer is `layout=3`. `DoubleWide`
is never armed — `kl_glfb_set_doublewide` is not called once.

## 5. An instrument that was lying — fixed this round

`mains/m_boot.c:composite_audit_tick` decided "the two eyes share storage" with a
bare pointer comparison, `int shared = (t1 != NULL) && (t1 == t0);`. Under the
Array layout that is TRUE and CORRECT — one texture, two array layers, the eye
IS the slice (`runtime/gfx/kl_reproject.c:686-693` hands each eye its own
`slice`) — so the audit reported **3855 of 3855 samples DOUBLED** on a display
that was right. The slice is now part of the identity, and the report grew a
third state ("share one MTLTexture as ARRAY SLICES"). With the fix the multiview
arm reads **0 DOUBLED**, the same startup-transient profile as the baseline.

Worth carrying: the audit's job is to catch "the picture twice", and it had a
false positive that would have been read as a fatal for the whole Array path.
