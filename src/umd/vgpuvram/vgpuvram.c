/*
 * vgpuvram - print the host GPU's local memory in MiB: the largest DEVICE_LOCAL heap of the Venus
 * Vulkan device.  install.ps1 stores it as the adapter's VideoMemoryMB, which the kernel driver
 * reports as dedicated video memory.  Prints nothing and exits 1 when no Venus device is found.
 * Build: build/build_tests.sh (shipped in the package's tools/).
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

int
main(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vgpuvram", 1, NULL, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
        return 1;
    VkPhysicalDevice pds[8];
    uint32_t n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    unsigned long long best = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (!strstr(props.deviceName, "Venus"))
            continue;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pds[i], &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > best)
                best = mp.memoryHeaps[h].size;
    }
    vkDestroyInstance(inst, NULL);
    if (!best)
        return 1;
    printf("%llu\n", best >> 20);
    return 0;
}
