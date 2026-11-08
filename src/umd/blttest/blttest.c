/*
 * blttest - windowed blt-model D3D11 swapchain (DXGI_SWAP_EFFECT_DISCARD) that clears to a
 * solid colour every frame; tests the classic redirected (blt) present path through DWM.
 * Usage: blttest [seconds [sync_interval [fullscreen]]]   (default 30 s, interval 1, windowed)
 * fullscreen=1 switches the swapchain to exclusive fullscreen at the desktop mode.
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
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = "blttest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("blttest", "blttest (blt-model D3D11)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              200, 200, 640, 400, NULL, NULL, inst, NULL);

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = 1;
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
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);

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
        IDXGISwapChain_Present(sc, interval, 0);
        frame++;
    }
    if (fullscreen)
        IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
    char note[80];
    snprintf(note, sizeof(note), "blttest frames=%u fps=%.1f interval=%u fullscreen=%d\n", frame,
             frame / (double)seconds, interval, fullscreen);
    OutputDebugStringA(note);
    return 0;
}
