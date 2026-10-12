/* mvkcfg — print MoltenVK's effective MVKConfiguration for the given dylib. */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#define VK_NO_PROTOTYPES 1
#include <MoltenVK/mvk_private_api.h>

typedef VkResult (*getcfg_t)(VkInstance, MVKConfiguration*, size_t*);

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "vendor-moltenvk/out/macos/libMoltenVK.dylib";
    void* h = dlopen(path, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen fail: %s\n", dlerror()); return 1; }
    getcfg_t g = (getcfg_t)dlsym(h, "vkGetMoltenVKConfigurationMVK");
    if (!g) { fprintf(stderr, "no vkGetMoltenVKConfigurationMVK\n"); return 1; }
    MVKConfiguration cfg; memset(&cfg, 0, sizeof cfg);
    size_t sz = sizeof cfg;
    VkResult r = g(NULL, &cfg, &sz);
    printf("lib=%s res=%d cfgsize=%zu/%zu apiver=%u\n", path, r, sz, sizeof cfg, MVK_PRIVATE_API_VERSION);
    printf("  useMTLHeap              = %d   (env MVK_CONFIG_USE_MTLHEAP=%s)\n",
           (int)cfg.useMTLHeap, getenv("MVK_CONFIG_USE_MTLHEAP") ?: "<unset>");
    printf("  useMetalArgumentBuffers = %d   (env MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=%s)\n",
           (int)cfg.useMetalArgumentBuffers, getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS") ?: "<unset>");
    printf("  liveCheckAllResources   = %d\n", (int)cfg.liveCheckAllResources);
    return 0;
}
/* Round 20's gate 1. Build from the repo root:
 *   clang -o /tmp/mvkcfg tools/mvkcfg.c -Ivendor-moltenvk/out/include
 *   MVK_CONFIG_USE_MTLHEAP=0 /tmp/mvkcfg vendor-moltenvk/out/macos/libMoltenVK.dylib
 * Reads MoltenVK's EFFECTIVE configuration out of the running library rather
 * than out of release notes: this port lost eight rounds to a knob that
 * silently did nothing. The visionOS slice cannot be run here; disassemble
 * vendor-moltenvk/out/xros/MoltenVK.framework/MoltenVK and look for the
 * mvkGetEnvVarNumber call site that names the variable to check its default. */
