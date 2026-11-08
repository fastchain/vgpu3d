/*
 * blttest - windowed blt-model D3D11 swapchain (DXGI_SWAP_EFFECT_DISCARD) that clears to a
 * solid colour every frame; tests the classic redirected (blt) present path through DWM.
 * Usage: blttest [seconds [sync_interval [fullscreen [draw [buffers]]]]]   (default 30 s, interval 1, windowed)
 * fullscreen=1 switches the swapchain to exclusive fullscreen at the desktop mode; draw=1 also draws
 * a triangle with a vertex/pixel shader into the back buffer every frame, draw=2 through an sRGB
 * render-target view of the (UNORM) back buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3dcommon.h>

static const char *shader_src =
    "struct V { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  V o; float2 p[3] = { float2(-0.5,-0.5), float2(0.0,0.5), float2(0.5,-0.5) };\n"
    "  o.pos = float4(p[id], 0, 1); o.col = float4(0, 0.5, 1, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return i.col; }\n";

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                         LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    (void)show;
    int seconds = cmd && *cmd ? atoi(cmd) : 30;
    const char *sp = cmd ? strchr(cmd, ' ') : NULL;
    UINT interval = sp ? (UINT)atoi(sp + 1) : 1;
    const char *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
    int fullscreen = sp2 ? atoi(sp2 + 1) : 0;
    const char *sp3 = sp2 ? strchr(sp2 + 1, ' ') : NULL;
    int draw = sp3 ? atoi(sp3 + 1) : 0;
    const char *sp4 = sp3 ? strchr(sp3 + 1, ' ') : NULL;
    UINT buffers = sp4 ? (UINT)atoi(sp4 + 1) : 1; /* >1: rotated flip chain in fullscreen (Unigine uses 2) */
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = "blttest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("blttest", "blttest (blt-model D3D11)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              200, 200, 640, 400, NULL, NULL, inst, NULL);

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = buffers ? buffers : 1;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *sc;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "D3D11CreateDeviceAndSwapChain failed", "blttest", 0);
        return 1;
    }
    if (fullscreen) {
        /* fullscreen needs the foreground window; retry while a helper may still be raising it */
        for (int tries = 0; tries < 50; tries++) {
            SetForegroundWindow(hwnd);
            hr = IDXGISwapChain_SetFullscreenState(sc, TRUE, NULL);
            if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
                break;
            Sleep(200);
        }
        char m[96];
        snprintf(m, sizeof(m), "blttest: SetFullscreenState hr=0x%08lx\n", hr);
        OutputDebugStringA(m);
        IDXGISwapChain_ResizeBuffers(sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    }
    ID3D11Texture2D *bb;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11RenderTargetView *rtv;
    if (draw == 2) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_RTV_DIMENSION_TEXTURE2D };
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, &rd, &rtv);
        char m[96];
        snprintf(m, sizeof(m), "blttest: sRGB RTV hr=0x%08lx\n", hr);
        OutputDebugStringA(m);
        if (FAILED(hr))
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    } else {
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    }

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    if (draw) {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
        ID3DBlob *vsb = NULL, *psb = NULL;
        if (!compile ||
            FAILED(compile(shader_src, strlen(shader_src), "blttest", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, NULL)) ||
            FAILED(compile(shader_src, strlen(shader_src), "blttest", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, NULL))) {
            OutputDebugStringA("blttest: shader compile failed\n");
            return 1;
        }
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    }
    D3D11_TEXTURE2D_DESC bbd;
    ID3D11Texture2D_GetDesc(bb, &bbd);
    D3D11_VIEWPORT vp = { 0, 0, (float)bbd.Width, (float)bbd.Height, 0, 1 };

    DWORD end = GetTickCount() + seconds * 1000;
    UINT frame = 0;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* orange, pulsing slightly so successive frames differ */
        float c[4] = { 1.0f, 0.4f + 0.1f * (float)((frame / 30) & 1), 0.0f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        if (draw) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        IDXGISwapChain_Present(sc, interval, 0);
        frame++;
    }
    if (fullscreen)
        IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
    char note[96];
    snprintf(note, sizeof(note), "blttest frames=%u fps=%.1f interval=%u fullscreen=%d draw=%d\n", frame,
             frame / (double)seconds, interval, fullscreen, draw);
    OutputDebugStringA(note);
    return 0;
}
