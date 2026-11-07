/*
 * vkcompute - headless Vulkan smoke test for the vgpu/Venus stack.
 *
 * Creates an instance and device on the first GPU, allocates a host-visible buffer,
 * runs a compute shader that writes out[i] = i*3+7, waits on a fence and checks the
 * result through the mapped pointer.  Then repeats the submission many times to
 * exercise fences and the sync emulation.  Loads vulkan-1.dll dynamically.
 *
 * SPDX-License-Identifier: MIT
 */
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <emmintrin.h>
#include <vulkan/vulkan.h>

#include "fill_spv.h"

#define N (1u << 20) /* 1M uints = 4 MiB */
#define LOOPS 200

static PFN_vkGetInstanceProcAddr gipa;
static int fails;
static int use_clflush;

/* write back + invalidate the CPU cache lines of [p, p+n) */
static void cflush(const void *p, size_t n)
{
    if (!use_clflush) return;
    for (size_t o = 0; o < n; o += 64) _mm_clflush((const char *)p + o);
    _mm_mfence();
}

#define CHECK(x, what)                                                              \
    do {                                                                            \
        VkResult r_ = (x);                                                          \
        if (r_ != VK_SUCCESS) {                                                     \
            printf("[FAIL] %s -> VkResult %d\n", what, (int)r_);                    \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define LOAD_I(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define LOAD_D(dev, name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

int main(int argc, char **argv)
{
    use_clflush = argc > 1 && !strcmp(argv[1], "--clflush");
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("[FAIL] LoadLibrary(vulkan-1.dll) %lu\n", GetLastError()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");

    LOAD_I(NULL, vkCreateInstance);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkcompute", 1, "none", 1, VK_API_VERSION_1_3 };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    f13.synchronization2 = VK_TRUE; f12.timelineSemaphore = VK_TRUE;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");
    printf("[PASS] vkCreateInstance\n");

    LOAD_I(inst, vkEnumeratePhysicalDevices);
    LOAD_I(inst, vkGetPhysicalDeviceProperties);
    LOAD_I(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(inst, vkGetPhysicalDeviceMemoryProperties);
    LOAD_I(inst, vkCreateDevice);
    LOAD_I(inst, vkGetDeviceProcAddr);
    LOAD_I(inst, vkDestroyInstance);

    uint32_t n = 1;
    VkPhysicalDevice pd;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) { printf("[FAIL] no physical device (%d)\n", er); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("[PASS] device: %s (api %u.%u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qf[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }
    if (qfi == UINT32_MAX) { printf("[FAIL] no compute queue\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfi, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
    printf("[PASS] vkCreateDevice (queue family %u)\n", qfi);

    LOAD_D(dev, vkGetDeviceQueue);
    LOAD_D(dev, vkCreateBuffer);
    LOAD_D(dev, vkGetBufferMemoryRequirements);
    LOAD_D(dev, vkAllocateMemory);
    LOAD_D(dev, vkBindBufferMemory);
    LOAD_D(dev, vkMapMemory);
    LOAD_D(dev, vkCreateShaderModule);
    LOAD_D(dev, vkCreateDescriptorSetLayout);
    LOAD_D(dev, vkCreatePipelineLayout);
    LOAD_D(dev, vkCreateComputePipelines);
    LOAD_D(dev, vkCreateDescriptorPool);
    LOAD_D(dev, vkAllocateDescriptorSets);
    LOAD_D(dev, vkUpdateDescriptorSets);
    LOAD_D(dev, vkCreateCommandPool);
    LOAD_D(dev, vkAllocateCommandBuffers);
    LOAD_D(dev, vkBeginCommandBuffer);
    LOAD_D(dev, vkCmdBindPipeline);
    LOAD_D(dev, vkCmdBindDescriptorSets);
    LOAD_D(dev, vkCmdPushConstants);
    LOAD_D(dev, vkCmdDispatch);
    LOAD_D(dev, vkCmdPipelineBarrier);
    LOAD_D(dev, vkEndCommandBuffer);
    LOAD_D(dev, vkCreateFence);
    LOAD_D(dev, vkQueueSubmit);
    LOAD_D(dev, vkWaitForFences);
    LOAD_D(dev, vkResetFences);
    LOAD_D(dev, vkDeviceWaitIdle);
    LOAD_D(dev, vkDestroyDevice);

    VkQueue q;
    vkGetDeviceQueue(dev, qfi, 0, &q);

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mti = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { mti = i; break; }
    if (mti == UINT32_MAX) { printf("[FAIL] no host visible coherent memory type\n"); return 1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mti };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &mem), "vkAllocateMemory (host visible)");
    CHECK(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");
    uint32_t *ptr = NULL;
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&ptr), "vkMapMemory");
    memset(ptr, 0xCD, N * 4);
    cflush(ptr, N * 4);
    printf("[PASS] host visible buffer %u KiB mapped at %p (type %u, flags 0x%x, heap %u)\n", (unsigned)(mr.size / 1024),
           (void *)ptr, mti, mp.memoryTypes[mti].propertyFlags, mp.memoryTypes[mti].heapIndex);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        printf("       memory type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fill_spv), fill_spv };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm), "vkCreateShaderModule");
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl), "vkCreateDescriptorSetLayout");
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl), "vkCreatePipelineLayout");
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                         { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                           VK_SHADER_STAGE_COMPUTE_BIT, sm, "main" },
                                         pl };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe), "vkCreateComputePipelines");
    printf("[PASS] compute pipeline created (shader compiled on the host)\n");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               NULL, &dbi };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfi };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    uint32_t count = N;
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &count);
    vkCmdDispatch(cb, N / 64, 1, 1);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence), "vkCreateFence");
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb };

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    CHECK(vkQueueSubmit(q, 1, &si, fence), "vkQueueSubmit");
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull), "vkWaitForFences");
    QueryPerformanceCounter(&t1);
    printf("[PASS] dispatch + fence wait: %.2f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);

    cflush(ptr, N * 4);
    uint32_t bad = 0, first_bad = 0;
    for (uint32_t i = 0; i < N; i++)
        if (ptr[i] != i * 3u + 7u) { if (!bad) first_bad = i; bad++; }
    if (bad) { printf("[FAIL] %u of %u values wrong (first at %u: 0x%08x)\n", bad, N, first_bad, ptr[first_bad]); fails++; }
    else printf("[PASS] all %u GPU results correct\n", N);

    /* repeated submit/wait to exercise fences and the timeline emulation */
    memset(ptr, 0, N * 4);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < LOOPS; i++) {
        CHECK(vkResetFences(dev, 1, &fence), "vkResetFences");
        CHECK(vkQueueSubmit(q, 1, &si, fence), "vkQueueSubmit (loop)");
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull), "vkWaitForFences (loop)");
    }
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    int ok = ptr[N - 1] == (N - 1) * 3u + 7u && ptr[12345] == 12345u * 3u + 7u;
    printf("[%s] %d submit+wait round trips: %.1f ms total, %.3f ms each\n", ok ? "PASS" : "FAIL", LOOPS, ms, ms / LOOPS);
    if (!ok) fails++;

    /* DXVK pattern: vkQueueSubmit2 signalling a timeline semaphore + fence, host waits on both */
    {
        LOAD_D(dev, vkCreateSemaphore);
        LOAD_D(dev, vkQueueSubmit2);
        LOAD_D(dev, vkWaitSemaphores);
        LOAD_D(dev, vkGetSemaphoreCounterValue);
        VkSemaphoreTypeCreateInfo stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci };
        VkSemaphore tl;
        CHECK(vkCreateSemaphore(dev, &sci, NULL, &tl), "vkCreateSemaphore (timeline)");
        int ok2 = 1;
        for (uint64_t v = 1; v <= 100 && ok2; v++) {
            VkCommandBufferSubmitInfo cbsi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0 };
            VkSemaphoreSubmitInfo ssi = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            VkSemaphoreSubmitInfo wsi = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, tl, v - 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            VkSubmitInfo2 si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, v > 1 ? 1u : 0u, &wsi, 1, &cbsi, 1, &ssi };
            CHECK(vkResetFences(dev, 1, &fence), "vkResetFences (submit2)");
            CHECK(vkQueueSubmit2(q, 1, &si2, (v % 3) ? fence : VK_NULL_HANDLE), "vkQueueSubmit2 (timeline + fence)");
            VkSemaphoreWaitInfo swi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &v };
            CHECK(vkWaitSemaphores(dev, &swi, 5000000000ull), "vkWaitSemaphores");
            uint64_t cur = 0;
            CHECK(vkGetSemaphoreCounterValue(dev, tl, &cur), "vkGetSemaphoreCounterValue");
            if (cur < v) ok2 = 0;
        }
        printf("[%s] 100 x vkQueueSubmit2 with timeline semaphore (+fence)\n", ok2 ? "PASS" : "FAIL");
        if (!ok2) fails++;
    }

    CHECK(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("RESULT: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
