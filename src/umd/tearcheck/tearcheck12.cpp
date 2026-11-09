/*
 * tearcheck12 - tearcheck through D3D12 (vkd3d-proton, which presents through the Vulkan Win32 WSI):
 * a flip-model D3D12 swap chain shows full-window solid red and blue on alternate frames, each drawn
 * with a deliberately heavy pixel shader, while a capture thread BitBlts the window's client area from
 * the screen every ~10 ms and counts captures that contain both colours (two frames mixed).  The first
 * mixed captures are saved as tearcheck12-N.bmp.
 * Usage: tearcheck12 [seconds [shader_loops [sync_interval [buffers]]]]   (default 20 400 0 3)
 * Build: build/build_tests.sh.
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *shader_src =
    "cbuffer C : register(b0) { float4 col; uint loops; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float a = pos.x * 0.001 + pos.y * 0.002;\n"
    "  [loop] for (uint i = 0; i < loops; i++) a = frac(sin(a * 12.9898 + i) * 43758.5453);\n"
    "  return col + a * 1e-6; }\n";

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                         LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static HWND g_hwnd;
static volatile LONG g_stop;
static LONG g_captures, g_mixed, g_red, g_blue, g_other;
static const UINT MAX_BUFFERS = 4;

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
capture_thread(void *)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    POINT o = { 0, 0 };
    ClientToScreen(g_hwnd, &o);
    const int w = rc.right, h = rc.bottom;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
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
                snprintf(name, sizeof(name), "tearcheck12-%d.bmp", ++saved);
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

int
main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    UINT loops = argc > 2 ? (UINT)atoi(argv[2]) : 400;
    UINT interval = argc > 3 ? (UINT)atoi(argv[3]) : 0;
    UINT buffers = argc > 4 ? (UINT)atoi(argv[4]) : 3;
    if (buffers < 2 || buffers > MAX_BUFFERS)
        buffers = 3;
    SetProcessDPIAware();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "tearcheck12";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("tearcheck12", "tearcheck12 (flip-model D3D12)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                           1600, 900, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(g_hwnd);

    ID3D12Device *dev;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        printf("[FAIL] D3D12CreateDevice\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ID3D12CommandQueue *queue;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    IDXGIFactory4 *fac;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = buffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1;
    if (FAILED(fac->CreateSwapChainForHwnd(queue, g_hwnd, &sd, NULL, NULL, &sc1))) {
        printf("[FAIL] CreateSwapChainForHwnd\n");
        return 1;
    }
    IDXGISwapChain3 *sc;
    sc1->QueryInterface(IID_PPV_ARGS(&sc));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, buffers };
    ID3D12DescriptorHeap *heap;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *bb[MAX_BUFFERS];
    ID3D12CommandAllocator *alloc[MAX_BUFFERS];
    UINT64 used[MAX_BUFFERS] = {};
    for (UINT i = 0; i < buffers; i++) {
        sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * inc;
        dev->CreateRenderTargetView(bb[i], NULL, h);
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
    }
    D3D12_RESOURCE_DESC rd = bb[0]->GetDesc();

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 5;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3DBlob *rsb, *err = NULL;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
    ID3D12RootSignature *rs;
    dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs));

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    if (!compile ||
        FAILED(compile(shader_src, strlen(shader_src), "tearcheck12", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, NULL)) ||
        FAILED(compile(shader_src, strlen(shader_src), "tearcheck12", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, NULL))) {
        printf("[FAIL] shader compile\n");
        return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
    pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        printf("[FAIL] CreateGraphicsPipelineState\n");
        return 1;
    }
    ID3D12GraphicsCommandList *cl;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], pso, IID_PPV_ARGS(&cl));
    cl->Close();
    ID3D12Fence *fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT64 fv = 0;

    HANDLE cap = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    const ULONGLONG start = GetTickCount64(), end = start + (ULONGLONG)seconds * 1000;
    UINT frame = 0;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        UINT i = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < used[i]) {
            fence->SetEventOnCompletion(used[i], ev);
            WaitForSingleObject(ev, INFINITE);
        }
        alloc[i]->Reset();
        cl->Reset(alloc[i], pso);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb[i];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * inc;
        cl->OMSetRenderTargets(1, &h, FALSE, NULL);
        D3D12_VIEWPORT vp = { 0, 0, (float)rd.Width, (float)rd.Height, 0, 1 };
        D3D12_RECT sr = { 0, 0, (LONG)rd.Width, (LONG)rd.Height };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sr);
        cl->SetGraphicsRootSignature(rs);
        float col[4] = { (frame & 1) ? 0.f : 1.f, 0.f, (frame & 1) ? 1.f : 0.f, 1.f };
        cl->SetGraphicsRoot32BitConstants(0, 4, col, 0);
        cl->SetGraphicsRoot32BitConstants(0, 1, &loops, 4);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList *lists[] = { cl };
        queue->ExecuteCommandLists(1, lists);
        sc->Present(interval, 0);
        queue->Signal(fence, ++fv);
        used[i] = fv;
        frame++;
    }
    queue->Signal(fence, ++fv);
    fence->SetEventOnCompletion(fv, ev);
    WaitForSingleObject(ev, 5000);
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(cap, INFINITE);
    double secs = (GetTickCount64() - start) / 1000.0;
    printf("frames %u (%.1f fps, %u shader loops, interval %u, %u buffers)\n", frame, frame / secs, loops, interval,
           buffers);
    printf("captures %ld: red %ld, blue %ld, mixed %ld, mostly-other %ld\n", g_captures, g_red, g_blue, g_mixed,
           g_other);
    printf("[%s] mixed-frame captures: %ld of %ld\n", g_mixed ? "FAIL" : "PASS", g_mixed, g_captures);
    return g_mixed ? 2 : 0;
}
