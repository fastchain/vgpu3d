/*
 * d3d9test - Direct3D 9 smoke test: creates a HAL device on the default adapter, clears the back
 * buffer to a known colour, reads it back with GetRenderTargetData and checks the pixel, then
 * presents for a few seconds.  Reports the adapter description D3D9 sees.
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("[FAIL] Direct3DCreate9\n");
        return 1;
    }
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id);
    printf("[INFO] adapter: %s (driver %s)\n", id.Description, id.Driver);

    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL,
                     "d3d9test" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9test", "d3d9test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, NULL,
                              NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        printf("[FAIL] CreateDevice HAL = 0x%08lx\n", hr);
        return 1;
    }
    printf("[PASS] CreateDevice HAL\n");

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(64, 128, 191), 1.0f, 0);
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
    D3DLOCKED_RECT lr;
    int ok = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        const unsigned char *px = (const unsigned char *)lr.pBits + 32 * lr.Pitch + 32 * 4; /* BGRX */
        ok = abs(px[2] - 64) <= 1 && abs(px[1] - 128) <= 1 && abs(px[0] - 191) <= 1;
        printf("[%s] readback pixel R=%u G=%u B=%u (expect 64 128 191)\n", ok ? "PASS" : "FAIL", px[2], px[1], px[0]);
        IDirect3DSurface9_UnlockRect(sys);
    } else {
        printf("[FAIL] GetRenderTargetData/LockRect 0x%08lx\n", hr);
    }

    DWORD end = GetTickCount() + 3000;
    unsigned frames = 0;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(frames % 256, 80, 160), 1.0f, 0);
        if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) {
            printf("[FAIL] Present\n");
            return 1;
        }
        frames++;
    }
    printf("[PASS] presented %u frames (%.1f fps)\n", frames, frames / 3.0);
    return ok ? 0 : 1;
}
