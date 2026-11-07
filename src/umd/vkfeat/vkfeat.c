/*
 * vkfeat - find advertised-but-unusable Vulkan features.
 * Creates a device with every feature vkGetPhysicalDeviceFeatures2 reports (core 1.0 and the
 * Vulkan 1.1/1.2/1.3/1.4 feature structs). If that fails, retries with one feature at a time and
 * prints each one vkCreateDevice rejects. Apps that "enable everything supported" (FurMark 2)
 * fail with VK_ERROR_FEATURE_NOT_PRESENT on such a feature.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;

typedef struct {
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features v11;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;
    VkPhysicalDeviceVulkan14Features v14;
} feats_t;

static void chain(feats_t *f, uint32_t api)
{
    memset(f, 0, sizeof(*f));
    f->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f->v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f->v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f->v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f->v14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    f->f2.pNext = &f->v11;
    f->v11.pNext = &f->v12;
    f->v12.pNext = api >= VK_API_VERSION_1_3 ? (void *)&f->v13 : NULL;
    f->v13.pNext = api >= VK_API_VERSION_1_4 ? (void *)&f->v14 : NULL;
}

/* the VkBool32 members of each struct: everything after sType/pNext */
struct range { const char *name; VkBool32 *first; size_t count; };

static int ranges(feats_t *f, uint32_t api, struct range *r)
{
    int n = 0;
    r[n++] = (struct range){"1.0", (VkBool32 *)&f->f2.features, sizeof(f->f2.features) / sizeof(VkBool32)};
    r[n++] = (struct range){"1.1", &f->v11.storageBuffer16BitAccess,
                            (sizeof(f->v11) - offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess)) / sizeof(VkBool32)};
    r[n++] = (struct range){"1.2", &f->v12.samplerMirrorClampToEdge,
                            (sizeof(f->v12) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32)};
    if (api >= VK_API_VERSION_1_3)
        r[n++] = (struct range){"1.3", &f->v13.robustImageAccess,
                                (sizeof(f->v13) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32)};
    if (api >= VK_API_VERSION_1_4)
        r[n++] = (struct range){"1.4", &f->v14.globalPriorityQuery,
                                (sizeof(f->v14) - offsetof(VkPhysicalDeviceVulkan14Features, globalPriorityQuery)) / sizeof(VkBool32)};
    return n;
}

static VkResult try_create(feats_t *f)
{
    PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f->f2, 0, 1, &q, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    VkResult r = cd(pd, &ci, NULL, &dev);
    if (r == VK_SUCCESS)
        dd(dev, NULL);
    return r;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("[FAIL] vulkan-1.dll %lu\n", GetLastError()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkfeat", 1, NULL, 0, VK_API_VERSION_1_4};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst) != VK_SUCCESS) { printf("[FAIL] instance\n"); return 1; }
    uint32_t n = 1;
    ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd);
    VkPhysicalDeviceProperties props;
    ((PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"))(pd, &props);
    uint32_t api = props.apiVersion;
    printf("device: %s, Vulkan %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
    PFN_vkGetPhysicalDeviceFeatures2 gf = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");

    feats_t all;
    chain(&all, api);
    gf(pd, &all.f2);
    VkResult r = try_create(&all);
    printf("all advertised features: %d\n", r);
    if (r == VK_SUCCESS) { printf("RESULT: every advertised feature can be enabled\n"); return 0; }

    struct range ra[5];
    int nr = ranges(&all, api, ra);
    int bad = 0;
    for (int i = 0; i < nr; i++) {
        for (size_t j = 0; j < ra[i].count; j++) {
            if (!ra[i].first[j])
                continue;
            feats_t one;
            chain(&one, api);
            struct range ro[5];
            ranges(&one, api, ro);
            ro[i].first[j] = VK_TRUE;
            VkResult rr = try_create(&one);
            if (rr != VK_SUCCESS) {
                printf("[BAD] Vulkan %s feature #%zu advertised but vkCreateDevice -> %d\n", ra[i].name, j, rr);
                bad++;
            }
        }
    }
    printf("RESULT: %d advertised feature(s) cannot be enabled\n", bad);
    return bad ? 2 : 0;
}
