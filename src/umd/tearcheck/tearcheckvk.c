/*
 * tearcheckvk - tearcheck through the Vulkan Win32 WSI (the path Vulkan apps and Zink windows take):
 * each frame clears a large offscreen image several times (GPU load) and then clears the swapchain
 * image to solid red or blue (alternating); a capture thread BitBlts the window from the screen every
 * ~10 ms and counts captures that contain both colours.  The first mixed captures are saved as
 * tearcheckvk-N.bmp.
 * Usage: tearcheckvk [seconds [load_clears [present_mode]]]   (default 20 40 0=IMMEDIATE, 2=FIFO)
 * Build: build/build_tests.sh.
 * SPDX-License-Identifier: MIT
 */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

static HWND g_hwnd;
static volatile LONG g_stop;
static LONG g_captures, g_mixed, g_red, g_blue, g_other;

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void
save_bmp(const char *name, const BITMAPINFO *bi, const void *bits, int bytes)
{
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    BITMAPFILEHEADER fh = { 0x4d42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
    fh.bfSize = fh.bfOffBits + bytes;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi->bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(bits, bytes, 1, f);
    fclose(f);
}

static DWORD WINAPI
capture_thread(void *arg)
{
    (void)arg;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    POINT o = { 0, 0 };
    ClientToScreen(g_hwnd, &o);
    const int w = rc.right, h = rc.bottom;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(mem, bmp);
    Sleep(1500);
    int saved = 0;
    while (!g_stop) {
        BitBlt(mem, 0, 0, w, h, screen, o.x, o.y, SRCCOPY);
        GdiFlush();
        int red = 0, blue = 0, other = 0, n = 0;
        for (int y = h / 40; y < h; y += h / 20)
            for (int x = w / 64; x < w; x += w / 32) {
                const BYTE *p = (const BYTE *)bits + ((size_t)y * w + x) * 4; /* B G R A */
                if (p[2] > 180 && p[0] < 80 && p[1] < 80)
                    red++;
                else if (p[0] > 180 && p[2] < 80 && p[1] < 80)
                    blue++;
                else
                    other++;
                n++;
            }
        g_captures++;
        g_other += other > n / 2;
        if (red > n / 33 && blue > n / 33) {
            g_mixed++;
            if (saved < 5) {
                char name[32];
                snprintf(name, sizeof(name), "tearcheckvk-%d.bmp", ++saved);
                save_bmp(name, &bi, bits, w * h * 4);
            }
        } else if (red >= blue) {
            g_red++;
        } else {
            g_blue++;
        }
        Sleep(10);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return 0;
}

#define CHECK(x)                                                                                                      \
    do {                                                                                                              \
        VkResult r_ = (x);                                                                                            \
        if (r_ != VK_SUCCESS) {                                                                                       \
            printf("[FAIL] %s = %d\n", #x, r_);                                                                       \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

static void
barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

int
main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int load = argc > 2 ? atoi(argv[2]) : 40;
    VkPresentModeKHR mode = argc > 3 ? (VkPresentModeKHR)atoi(argv[3]) : VK_PRESENT_MODE_IMMEDIATE_KHR;
    SetProcessDPIAware();

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "tearcheckvk";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("tearcheckvk", "tearcheckvk (Vulkan WSI)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1600,
                           900, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(g_hwnd);

    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "tearcheckvk", 1, NULL, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 2, iext };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, wc.hInstance, g_hwnd };
    VkSurfaceKHR surf;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("[FAIL] no Vulkan device\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dext };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    VkSwapchainCreateInfoKHR swi = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swi.surface = surf;
    swi.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    swi.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swi.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swi.imageExtent = caps.currentExtent;
    swi.imageArrayLayers = 1;
    swi.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swi.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swi.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swi.presentMode = mode;
    swi.clipped = VK_TRUE;
    VkSwapchainKHR sc;
    CHECK(vkCreateSwapchainKHR(dev, &swi, NULL, &sc));
    uint32_t nimg = 8;
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);

    /* GPU load target: a 4K RGBA16F image cleared `load` times per frame */
    VkImageCreateInfo lci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    lci.imageType = VK_IMAGE_TYPE_2D;
    lci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    lci.extent = (VkExtent3D){ 3840, 2160, 1 };
    lci.mipLevels = lci.arrayLayers = 1;
    lci.samples = VK_SAMPLE_COUNT_1_BIT;
    lci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage loadimg;
    CHECK(vkCreateImage(dev, &lci, NULL, &loadimg));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, loadimg, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mt = i;
            break;
        }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    VkDeviceMemory loadmem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &loadmem));
    vkBindImageMemory(dev, loadimg, loadmem, 0);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkFence fences[2];
    VkSemaphore acq[2], done[2];
    for (int i = 0; i < 2; i++) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };
        vkCreateFence(dev, &fci, NULL, &fences[i]);
        VkSemaphoreCreateInfo sei = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(dev, &sei, NULL, &acq[i]);
        vkCreateSemaphore(dev, &sei, NULL, &done[i]);
    }

    HANDLE cap = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    const ULONGLONG start = GetTickCount64(), end = start + (ULONGLONG)seconds * 1000;
    uint32_t frame = 0;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        const int s = frame & 1;
        /* adapter restarts (driver updates) lose the device; stop cleanly */
        VkResult vr = vkWaitForFences(dev, 1, &fences[s], VK_TRUE, UINT64_MAX);
        if (vr < 0) {
            printf("device lost: wait %d\n", vr);
            break;
        }
        vkResetFences(dev, 1, &fences[s]);
        uint32_t idx;
        vr = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq[s], VK_NULL_HANDLE, &idx);
        if (vr < 0) {
            printf("device lost: acquire %d\n", vr);
            break;
        }
        VkCommandBuffer cb = cbs[s];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cb, &bi);
        barrier(cb, loadimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < load; i++) {
            VkClearColorValue lc = { .float32 = { (float)i, 0, 0, 1 } };
            vkCmdClearColorImage(cb, loadimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &lc, 1, &rng);
            barrier(cb, loadimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue cc = { .float32 = { s ? 0.f : 1.f, 0.f, s ? 1.f : 0.f, 1.f } };
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acq[s], &ws, 1, &cb, 1, &done[s] };
        vr = vkQueueSubmit(q, 1, &si, fences[s]);
        if (vr < 0) {
            printf("device lost: submit %d\n", vr);
            break;
        }
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &done[s], 1, &sc, &idx };
        vr = vkQueuePresentKHR(q, &pi);
        if (vr < 0 && vr != VK_ERROR_OUT_OF_DATE_KHR) {
            printf("device lost: present %d\n", vr);
            break;
        }
        frame++;
    }
    vkDeviceWaitIdle(dev);
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(cap, INFINITE);
    double secs = (GetTickCount64() - start) / 1000.0;
    printf("frames %u (%.1f fps, %d load clears, present mode %d, %u images)\n", frame, frame / secs, load, mode, nimg);
    printf("captures %ld: red %ld, blue %ld, mixed %ld, mostly-other %ld\n", g_captures, g_red, g_blue, g_mixed,
           g_other);
    printf("[%s] mixed-frame captures: %ld of %ld\n", g_mixed ? "FAIL" : "PASS", g_mixed, g_captures);
    return g_mixed ? 2 : 0;
}
