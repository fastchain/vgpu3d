/*
 * vkprops - print the Vulkan features and format capabilities DXVK relies on for D3D11 (typed UAV
 * loads, cube arrays, custom border colours, robustness2 null descriptors, BC/HDR formats), as the
 * guest's Venus device reports them.  Build: build/build_tests.sh.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <vulkan/vulkan.h>

#define F(x) printf("  %-40s %u\n", #x, (unsigned)(x))

static void
fmt(VkPhysicalDevice pd, VkFormat f, const char *name)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    VkFormatFeatureFlags o = p.optimalTilingFeatures;
    printf("  %-28s sampled=%u filter=%u storage=%u color=%u blend=%u (optimal 0x%08x)\n", name,
           !!(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), !!(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
           !!(o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT), !!(o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT),
           !!(o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT), (unsigned)o);
}

int
main(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkprops", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("[FAIL] vkCreateInstance\n");
        return 1;
    }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("[FAIL] no device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("%s api %u.%u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    VkPhysicalDeviceRobustness2FeaturesEXT rb2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &cbc };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dce = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, &rb2 };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &dce };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceFeatures *f = &f2.features;
    printf("core:\n");
    F(f->robustBufferAccess); F(f->imageCubeArray); F(f->independentBlend); F(f->geometryShader);
    F(f->tessellationShader); F(f->sampleRateShading); F(f->dualSrcBlend); F(f->depthClamp);
    F(f->depthBiasClamp); F(f->samplerAnisotropy); F(f->textureCompressionBC); F(f->occlusionQueryPrecise);
    F(f->vertexPipelineStoresAndAtomics); F(f->fragmentStoresAndAtomics); F(f->shaderImageGatherExtended);
    F(f->shaderStorageImageExtendedFormats); F(f->shaderStorageImageReadWithoutFormat);
    F(f->shaderStorageImageWriteWithoutFormat); F(f->shaderClipDistance); F(f->shaderCullDistance);
    F(f->shaderFloat64); F(f->shaderInt64); F(f->shaderInt16); F(f->variableMultisampleRate);
    printf("1.2/1.3:\n");
    F(v12.shaderFloat16); F(v12.descriptorIndexing); F(v12.samplerMirrorClampToEdge); F(v12.timelineSemaphore);
    F(v12.bufferDeviceAddress); F(v12.vulkanMemoryModel); F(v13.shaderDemoteToHelperInvocation);
    F(v13.robustImageAccess); F(v13.maintenance4);
    printf("ext:\n");
    F(dce.depthClipEnable); F(rb2.robustBufferAccess2); F(rb2.robustImageAccess2); F(rb2.nullDescriptor);
    F(cbc.customBorderColors); F(cbc.customBorderColorWithoutFormat);
    printf("formats:\n");
    fmt(pd, VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10_FLOAT");
    fmt(pd, VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_FLOAT");
    fmt(pd, VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_FLOAT");
    fmt(pd, VK_FORMAT_A2B10G10R10_UNORM_PACK32, "R10G10B10A2_UNORM");
    fmt(pd, VK_FORMAT_R16G16_SFLOAT, "R16G16_FLOAT");
    fmt(pd, VK_FORMAT_R32_UINT, "R32_UINT");
    fmt(pd, VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UF16");
    fmt(pd, VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H_SF16");
    fmt(pd, VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM");
    fmt(pd, VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM");
    fmt(pd, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "R9G9B9E5_SHAREDEXP");
    fmt(pd, VK_FORMAT_D32_SFLOAT, "D32_FLOAT");
    fmt(pd, VK_FORMAT_D24_UNORM_S8_UINT, "D24S8");
    VkPhysicalDeviceLimits *l = &props.limits;
    printf("limits:\n");
    F(l->maxImageDimension3D); F(l->maxImageArrayLayers); F(l->maxSamplerAllocationCount);
    F(l->maxPerStageDescriptorSampledImages); F(l->maxPerStageDescriptorStorageImages);
    F(l->maxComputeSharedMemorySize); F(l->maxSamplerAnisotropy);
    return 0;
}
