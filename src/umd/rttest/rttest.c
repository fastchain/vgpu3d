/* rttest: Vulkan ray tracing smoke test (VK_KHR_acceleration_structure, ray_query, ray_tracing_pipeline).
 *
 * Builds a BLAS (one quad) and a TLAS (two instances of it at different depths) on the GPU, then shoots a
 * 64x64 grid of rays along +Z twice: from a compute shader with ray queries, and through a ray tracing
 * pipeline (raygen/miss/closest-hit with a shader binding table).  Every ray's hit instance and distance are
 * compared with the CPU's answer.
 *
 * usage: rttest.exe    exit 0 = both paths correct, 1 = mismatch, 2 = setup failure
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "rttest_spv.h"

#define CHECK(x)                                                                 \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) {                                                  \
            printf("[FAIL] %s = %d (line %d)\n", #x, r_, __LINE__);              \
            exit(2);                                                             \
        }                                                                        \
    } while (0)

#define N 64

static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties memprops;
static PFN_vkGetBufferDeviceAddress pGetBufferDeviceAddress;
static PFN_vkCreateAccelerationStructureKHR pCreateAS;
static PFN_vkGetAccelerationStructureBuildSizesKHR pGetASBuildSizes;
static PFN_vkCmdBuildAccelerationStructuresKHR pCmdBuildAS;
static PFN_vkGetAccelerationStructureDeviceAddressKHR pGetASAddress;
static PFN_vkCreateRayTracingPipelinesKHR pCreateRTPipelines;
static PFN_vkGetRayTracingShaderGroupHandlesKHR pGetGroupHandles;
static PFN_vkCmdTraceRaysKHR pCmdTraceRays;

struct buf {
    VkBuffer b;
    VkDeviceMemory m;
    VkDeviceAddress addr;
    void *map;
};

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    printf("[FAIL] no memory type for 0x%x\n", want);
    exit(2);
}

static struct buf
make_buf(VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
    struct buf r = { 0 };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    CHECK(vkCreateBuffer(dev, &bi, NULL, &r.b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, r.b, &req);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits,
                                  host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                       : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(dev, &ai, NULL, &r.m));
    CHECK(vkBindBufferMemory(dev, r.b, r.m, 0));
    if (host)
        CHECK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.map));
    VkBufferDeviceAddressInfo da = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, r.b };
    r.addr = pGetBufferDeviceAddress(dev, &da);
    return r;
}

static VkShaderModule
module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

/* Instance layout: the quad [-0.5,0.5]^2 at z=0, placed at these offsets. */
static const float inst_off[2][3] = { { -0.4f, -0.4f, 1.0f }, { 0.4f, 0.4f, 2.0f } };
static const uint32_t inst_id[2] = { 7, 9 };

static int
verify(const char *name, const uint32_t *px)
{
    int bad = 0, hits = 0, misses = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float ox = -1.0f + (x + 0.5f) / 32.0f, oy = -1.0f + (y + 0.5f) / 32.0f;
            uint32_t want_id = 0xFFFF;
            float want_t = 0.0f;
            for (int i = 0; i < 2; i++)
                if (fabsf(ox - inst_off[i][0]) <= 0.5f && fabsf(oy - inst_off[i][1]) <= 0.5f) {
                    want_id = inst_id[i];
                    want_t = inst_off[i][2];
                    break; /* instance 0 is nearer */
                }
            uint32_t id = px[(y * N + x) * 2];
            float t;
            memcpy(&t, &px[(y * N + x) * 2 + 1], sizeof(t));
            int ok = id == want_id && (want_id == 0xFFFF || fabsf(t - want_t) < 1e-3f);
            if (!ok && bad++ < 5)
                printf("  %s ray (%d,%d): got id %u t %g, want id %u t %g\n", name, x, y, id, t, want_id, want_t);
            if (want_id == 0xFFFF)
                misses++;
            else
                hits++;
        }
    printf("[%s] %s: %d of %d rays correct (%d expected hits, %d misses)\n", bad ? "FAIL" : "PASS", name,
           N * N - bad, N * N, hits, misses);
    return bad;
}

int
main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "rttest", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
        printf("[FAIL] no Vulkan device\n");
        return 2;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    printf("device: %s\n", props.deviceName);

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR
    };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &rtp };
    vkGetPhysicalDeviceProperties2(pd, &p2);

    uint32_t qf = 0, nq = 8;
    VkQueueFamilyProperties qp[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            qf = i;
            break;
        }

    VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, &rq
    };
    rtf.rayTracingPipeline = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rtf
    };
    asf.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &asf };
    v12.bufferDeviceAddress = VK_TRUE;
    const char *exts[] = { "VK_KHR_acceleration_structure", "VK_KHR_ray_tracing_pipeline", "VK_KHR_ray_query",
                           "VK_KHR_deferred_host_operations" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12 };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 4;
    dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

#define LOAD(var, name) var = (void *)vkGetDeviceProcAddr(dev, name)
    LOAD(pGetBufferDeviceAddress, "vkGetBufferDeviceAddress");
    LOAD(pCreateAS, "vkCreateAccelerationStructureKHR");
    LOAD(pGetASBuildSizes, "vkGetAccelerationStructureBuildSizesKHR");
    LOAD(pCmdBuildAS, "vkCmdBuildAccelerationStructuresKHR");
    LOAD(pGetASAddress, "vkGetAccelerationStructureDeviceAddressKHR");
    LOAD(pCreateRTPipelines, "vkCreateRayTracingPipelinesKHR");
    LOAD(pGetGroupHandles, "vkGetRayTracingShaderGroupHandlesKHR");
    LOAD(pCmdTraceRays, "vkCmdTraceRaysKHR");

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = qf;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    /* --- BLAS: one quad (two triangles) --- */
    const float verts[] = { -0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0,
                            -0.5f, -0.5f, 0, 0.5f, 0.5f,  0, -0.5f, 0.5f, 0 };
    const VkBufferUsageFlags as_input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    struct buf vb = make_buf(sizeof(verts), as_input, 1);
    memcpy(vb.map, verts, sizeof(verts));

    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = vb.addr;
    geo.geometry.triangles.vertexStride = 12;
    geo.geometry.triangles.maxVertex = 5;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bgi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geo;
    uint32_t prims = 2;
    VkAccelerationStructureBuildSizesInfoKHR bsz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pGetASBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &prims, &bsz);
    struct buf blas_buf = make_buf(bsz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0);
    VkAccelerationStructureCreateInfoKHR aci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    aci.buffer = blas_buf.b;
    aci.size = bsz.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR blas;
    CHECK(pCreateAS(dev, &aci, NULL, &blas));
    struct buf scratch1 = make_buf(bsz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
    bgi.dstAccelerationStructure = blas;
    bgi.scratchData.deviceAddress = (scratch1.addr + 255) & ~(VkDeviceAddress)255;
    VkAccelerationStructureBuildRangeInfoKHR range = { prims, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;

    /* --- TLAS: two instances --- */
    VkAccelerationStructureDeviceAddressInfoKHR adi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                        NULL, blas };
    VkAccelerationStructureInstanceKHR insts[2];
    memset(insts, 0, sizeof(insts));
    for (int i = 0; i < 2; i++) {
        insts[i].transform.matrix[0][0] = insts[i].transform.matrix[1][1] = insts[i].transform.matrix[2][2] = 1.0f;
        insts[i].transform.matrix[0][3] = inst_off[i][0];
        insts[i].transform.matrix[1][3] = inst_off[i][1];
        insts[i].transform.matrix[2][3] = inst_off[i][2];
        insts[i].instanceCustomIndex = inst_id[i];
        insts[i].mask = 0xFF;
        insts[i].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        insts[i].accelerationStructureReference = pGetASAddress(dev, &adi);
    }
    struct buf ib = make_buf(sizeof(insts), as_input, 1);
    memcpy(ib.map, insts, sizeof(insts));
    VkAccelerationStructureGeometryKHR tgeo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.data.deviceAddress = ib.addr;
    VkAccelerationStructureBuildGeometryInfoKHR tbgi = bgi;
    tbgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbgi.pGeometries = &tgeo;
    uint32_t ninst = 2;
    VkAccelerationStructureBuildSizesInfoKHR tsz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pGetASBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tbgi, &ninst, &tsz);
    struct buf tlas_buf = make_buf(tsz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0);
    aci.buffer = tlas_buf.b;
    aci.size = tsz.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR tlas;
    CHECK(pCreateAS(dev, &aci, NULL, &tlas));
    struct buf scratch2 = make_buf(tsz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
    tbgi.dstAccelerationStructure = tlas;
    tbgi.scratchData.deviceAddress = (scratch2.addr + 255) & ~(VkDeviceAddress)255;
    VkAccelerationStructureBuildRangeInfoKHR trange = { ninst, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR *ptr = &trange;

    CHECK(vkBeginCommandBuffer(cb, &begin));
    pCmdBuildAS(cb, 1, &bgi, &pr);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, NULL, 0, NULL);
    pCmdBuildAS(cb, 1, &tbgi, &ptr);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(q));
    printf("acceleration structures built: BLAS %llu bytes, TLAS %llu bytes\n",
           (unsigned long long)bsz.accelerationStructureSize, (unsigned long long)tsz.accelerationStructureSize);

    /* --- shared descriptor set: TLAS + output buffer --- */
    struct buf out = make_buf(N * N * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
    VkDescriptorSetLayoutBinding lb[2] = {
        { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkWriteDescriptorSetAccelerationStructureKHR was = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, NULL, 1, &tlas
    };
    VkDescriptorBufferInfo dbi = { out.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &was, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi },
    };
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));

    int bad = 0;

    /* --- 1: ray query from a compute shader --- */
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module(spv_rq_comp, sizeof(spv_rq_comp));
    cpci.stage.pName = "main";
    cpci.layout = pl;
    VkPipeline cp;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &cp));
    memset(out.map, 0xAB, N * N * 8);
    CHECK(vkResetCommandBuffer(cb, 0));
    CHECK(vkBeginCommandBuffer(cb, &begin));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 8, N / 8, 1);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(q));
    bad += verify("ray query (compute)", out.map);

    /* --- 2: ray tracing pipeline --- */
    VkPipelineShaderStageCreateInfo st[3] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    st[0].module = module(spv_rt_rgen, sizeof(spv_rt_rgen));
    st[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    st[1].module = module(spv_rt_rmiss, sizeof(spv_rt_rmiss));
    st[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    st[2].module = module(spv_rt_rchit, sizeof(spv_rt_rchit));
    for (int i = 0; i < 3; i++)
        st[i].pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR g[3];
    for (int i = 0; i < 3; i++) {
        memset(&g[i], 0, sizeof(g[i]));
        g[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        g[i].generalShader = g[i].closestHitShader = g[i].anyHitShader = g[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    g[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    g[0].generalShader = 0;
    g[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    g[1].generalShader = 1;
    g[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    g[2].closestHitShader = 2;
    VkRayTracingPipelineCreateInfoKHR rpci = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
    rpci.stageCount = 3;
    rpci.pStages = st;
    rpci.groupCount = 3;
    rpci.pGroups = g;
    rpci.maxPipelineRayRecursionDepth = 1;
    rpci.layout = pl;
    VkPipeline rp;
    CHECK(pCreateRTPipelines(dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rpci, NULL, &rp));

    const uint32_t hs = rtp.shaderGroupHandleSize, ba = rtp.shaderGroupBaseAlignment;
    const uint32_t stride = (hs + rtp.shaderGroupHandleAlignment - 1) & ~(rtp.shaderGroupHandleAlignment - 1);
    const uint32_t region = (stride + ba - 1) & ~(ba - 1);
    uint8_t handles[3 * 64];
    CHECK(pGetGroupHandles(dev, rp, 0, 3, 3 * hs, handles));
    struct buf sbt = make_buf(3 * region + ba, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, 1);
    VkDeviceAddress sbt_base = (sbt.addr + ba - 1) & ~(VkDeviceAddress)(ba - 1);
    for (int i = 0; i < 3; i++)
        memcpy((uint8_t *)sbt.map + (sbt_base - sbt.addr) + i * region, handles + i * hs, hs);
    VkStridedDeviceAddressRegionKHR rgen = { sbt_base, region, region };
    VkStridedDeviceAddressRegionKHR miss = { sbt_base + region, stride, stride };
    VkStridedDeviceAddressRegionKHR hit = { sbt_base + 2 * region, stride, stride };
    VkStridedDeviceAddressRegionKHR callable = { 0, 0, 0 };

    memset(out.map, 0xAB, N * N * 8);
    CHECK(vkResetCommandBuffer(cb, 0));
    CHECK(vkBeginCommandBuffer(cb, &begin));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pl, 0, 1, &ds, 0, NULL);
    pCmdTraceRays(cb, &rgen, &miss, &hit, &callable, N, N, 1);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(q));
    bad += verify("ray tracing pipeline", out.map);

    vkDeviceWaitIdle(dev);
    printf("%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS");
    return bad ? 1 : 0;
}
