/*
 * tearcheck - detects frames that mix two app frames in the desktop composition.  A flip-model D3D11
 * swap chain (3 buffers, like games use) shows full-window solid red and blue on alternate frames,
 * each drawn with a deliberately heavy pixel shader; a capture thread BitBlts the window's client area
 * from the screen (DWM's composed output) every ~10 ms and counts captures that contain both colours.
 * Any such capture means DWM composed a buffer the app was still rendering, or one the app had already
 * started overwriting with the next frame.  The first mixed captures are saved as tearcheck-N.bmp.
 * Usage: tearcheck [seconds [shader_loops [sync_interval [buffers [blt]]]]]   (default 20 400 0 3 0)
 * blt=1: legacy blt-model swap chain (DXGI_SWAP_EFFECT_DISCARD, 1 buffer) instead of flip-discard.
 * Build: build/build_tests.sh.
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *shader_src =
    "cbuffer C : register(b0) { float4 col; uint loops; uint3 pad; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float a = pos.x * 0.001 + pos.y * 0.002;\n"
    "  [loop] for (uint i = 0; i < loops; i++) a = frac(sin(a * 12.9898 + i) * 43758.5453);\n"
    "  return col + a * 1e-6; }\n";

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                         LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static HWND g_hwnd;
static volatile LONG g_stop, g_frames_shown;
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
    Sleep(1500); /* let the window appear and the chain fill */
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
                snprintf(name, sizeof(name), "tearcheck-%d.bmp", ++saved);
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
    int blt = argc > 5 ? atoi(argv[5]) : 0;
    SetProcessDPIAware();

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "tearcheck";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("tearcheck", "tearcheck (flip-model D3D11)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                           1600, 900, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(g_hwnd);

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL,
                                 &ctx))) {
        printf("[FAIL] D3D11CreateDevice\n");
        return 1;
    }
    IDXGIDevice *xdev;
    IDXGIAdapter *ad;
    IDXGIFactory2 *fac;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xdev);
    IDXGIDevice_GetAdapter(xdev, &ad);
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = blt ? 1 : buffers;
    sd.SwapEffect = blt ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc;
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)dev, g_hwnd, &sd, NULL, NULL, &sc))) {
        printf("[FAIL] CreateSwapChainForHwnd\n");
        return 1;
    }

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    if (!compile ||
        FAILED(compile(shader_src, strlen(shader_src), "tearcheck", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, NULL)) ||
        FAILED(compile(shader_src, strlen(shader_src), "tearcheck", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, NULL))) {
        printf("[FAIL] shader compile\n");
        return 1;
    }
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    ID3D11Buffer *cb;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);

    HANDLE cap = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    const ULONGLONG start = GetTickCount64(), end = start + (ULONGLONG)seconds * 1000;
    UINT frame = 0;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        ID3D11Texture2D *bb;
        IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
        ID3D11RenderTargetView *rtv;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(bb, &td);
        struct { float col[4]; UINT loops, pad[3]; } c = { { (frame & 1) ? 0.f : 1.f, 0.f, (frame & 1) ? 1.f : 0.f, 1.f },
                                                          loops };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, &c, 0, 0);
        D3D11_VIEWPORT vp = { 0, 0, (float)td.Width, (float)td.Height, 0, 1 };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11RenderTargetView_Release(rtv);
        ID3D11Texture2D_Release(bb);
        IDXGISwapChain1_Present(sc, interval, 0);
        frame++;
    }
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(cap, INFINITE);
    double secs = (GetTickCount64() - start) / 1000.0;
    printf("frames %u (%.1f fps, %u shader loops, interval %u, %u buffers, %s)\n", frame, frame / secs, loops, interval,
           buffers, blt ? "blt model" : "flip model");
    printf("captures %ld: red %ld, blue %ld, mixed %ld, mostly-other %ld\n", g_captures, g_red, g_blue, g_mixed,
           g_other);
    printf("[%s] mixed-frame captures: %ld of %ld\n", g_mixed ? "FAIL" : "PASS", g_mixed, g_captures);
    return g_mixed ? 2 : 0;
}
