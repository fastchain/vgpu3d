/*
 * d3d12test - D3D12 smoke test on the default adapter: creates a device, clears a render target to
 * a known colour, copies it to a readback buffer, waits on a fence and checks the pixel.  With
 * "present" as argument it also clears and presents a flip-model swapchain for a few seconds.
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                     \
    do {                                                                                             \
        HRESULT hr_ = (x);                                                                           \
        if (FAILED(hr_)) {                                                                           \
            printf("[FAIL] %s = 0x%08lx\n", #x, hr_);                                                \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

static void
wait_fence(ID3D12CommandQueue *q, ID3D12Fence *f, UINT64 *v, HANDLE evt)
{
    ++*v;
    ID3D12CommandQueue_Signal(q, f, *v);
    if (ID3D12Fence_GetCompletedValue(f) < *v) {
        ID3D12Fence_SetEventOnCompletion(f, *v, evt);
        WaitForSingleObject(evt, 5000);
    }
}

static D3D12_RESOURCE_BARRIER
transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int do_present = argc > 1 && !strcmp(argv[1], "present");

    IDXGIFactory4 *factory;
    CHECK(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory));
    IDXGIAdapter1 *adapter;
    CHECK(IDXGIFactory4_EnumAdapters1(factory, 0, &adapter));
    DXGI_ADAPTER_DESC1 ad;
    IDXGIAdapter1_GetDesc1(adapter, &ad);
    printf("[INFO] adapter: %ls\n", ad.Description);

    ID3D12Device *dev;
    CHECK(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    printf("[PASS] D3D12CreateDevice\n");

    D3D12_FEATURE_DATA_D3D12_OPTIONS opt;
    memset(&opt, 0, sizeof(opt));
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    printf("[INFO] ResourceBindingTier=%d TiledResourcesTier=%d\n", opt.ResourceBindingTier,
           opt.TiledResourcesTier);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                              (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12Fence *fence;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    HANDLE evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    UINT64 fv = 0;

    /* 64x64 RGBA8 render target */
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 64;
    rd.Height = 64;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
                                               (void **)&rt));
    D3D12_HEAP_PROPERTIES rhp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256 * 64;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                               NULL, &IID_ID3D12Resource, (void **)&rb));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvs, &h);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, h);

    const float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, color, 0, NULL);
    D3D12_RESOURCE_BARRIER b = transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof(dst));
    memset(&src, 0, sizeof(src));
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 64;
    dst.PlacedFootprint.Footprint.Height = 64;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    wait_fence(queue, fence, &fv, evt);
    printf("[INFO] fence completed %llu (want %llu)\n", (unsigned long long)ID3D12Fence_GetCompletedValue(fence),
           (unsigned long long)fv);

    unsigned char *px;
    D3D12_RANGE range = { 0, 256 * 64 };
    CHECK(ID3D12Resource_Map(rb, 0, &range, (void **)&px));
    unsigned char *c = px + 32 * 256 + 32 * 4;
    int ok = abs(c[0] - 64) <= 1 && abs(c[1] - 128) <= 1 && abs(c[2] - 191) <= 1 && c[3] == 255;
    printf("[%s] readback pixel %u %u %u %u (expect 64 128 191 255)\n", ok ? "PASS" : "FAIL", c[0], c[1], c[2],
           c[3]);
    ID3D12Resource_Unmap(rb, 0, NULL);
    if (!ok)
        return 1;
    if (!do_present)
        return 0;

    /* flip-model swapchain: clear to cycling colours for 5 s */
    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL,
                     "d3d12test" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d12test", "d3d12test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 600, NULL,
                              NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof(sd));
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1;
    CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain3 *sc;
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
    ID3D12Resource *bb[2];
    D3D12_CPU_DESCRIPTOR_HANDLE bh[2];
    for (int i = 0; i < 2; i++) {
        CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bb[i]));
        bh[i].ptr = h.ptr + (i + 1) * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        ID3D12Device_CreateRenderTargetView(dev, bb[i], NULL, bh[i]);
    }
    DWORD end = GetTickCount() + 5000;
    unsigned frames = 0;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        b = transition(bb[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        float cc[4] = { (frames % 120) / 120.0f, 0.3f, 0.6f, 1.0f };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, bh[i], cc, 0, NULL);
        b = transition(bb[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        if (FAILED(IDXGISwapChain3_Present(sc, 1, 0))) {
            printf("[FAIL] Present\n");
            return 1;
        }
        wait_fence(queue, fence, &fv, evt);
        frames++;
    }
    printf("[PASS] presented %u frames (%.1f fps)\n", frames, frames / 5.0);
    return 0;
}
