/* Gate 3: does MVK_CONFIG_USE_MTLHEAP=0 actually change the running library's
 * behaviour?  MVKDevice.mm:2466 sets _metalFeatures.placementHeaps from the
 * config, and MVKDevice.mm:741-742 reports that same bool back through Vulkan
 * as image2DViewOf3D / sampler2DViewOf3D (and :601 as portability
 * imageView2DOn3DImage).  So a plain vkGetPhysicalDeviceFeatures2 reads the
 * effective placement-heap decision out of the library, per physical device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "vendor-moltenvk/out/macos/libMoltenVK.dylib";
    void* h = dlopen(path, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    const char* exts[] = { "VK_KHR_portability_enumeration",
                           "VK_KHR_get_physical_device_properties2" };
    VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ic.flags = 0x00000001; /* ENUMERATE_PORTABILITY_BIT_KHR */
    ic.pApplicationInfo = &ai;
    ic.enabledExtensionCount = 1; exts[0] = exts[1]; ic.ppEnabledExtensionNames = exts;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = ci(&ic, NULL, &inst);
    if (r) { fprintf(stderr, "vkCreateInstance=%d\n", r); return 1; }
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2KHR");
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0; epd(inst, &n, NULL);
    VkPhysicalDevice pd[8]; if (n > 8) n = 8; epd(inst, &n, pd);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; gp(pd[i], &p);
        VkPhysicalDeviceImage2DViewOf3DFeaturesEXT f3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f3 };
        gf2(pd[i], &f2);
        printf("gpu[%u] %-28s vendor=0x%04x  image2DViewOf3D=%u sampler2DViewOf3D=%u  => placementHeaps=%s   (MVK_CONFIG_USE_MTLHEAP=%s)\n",
               i, p.deviceName, p.vendorID, f3.image2DViewOf3D, f3.sampler2DViewOf3D,
               f3.image2DViewOf3D ? "ON" : "OFF",
               getenv("MVK_CONFIG_USE_MTLHEAP") ?: "<unset>");
    }
    return 0;
}
