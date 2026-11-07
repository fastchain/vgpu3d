/*
 * sharetest - cross-process D3D11 shared texture test (the path DWM uses for every window).
 *
 * Parent: creates a 64x64 BGRA8 MISC_SHARED texture on the default hardware adapter, clears it,
 * waits for the GPU (EVENT query), opens it again in-process, then starts "sharetest child
 * <handle>" which opens the handle with OpenSharedResource, copies it to a staging texture and
 * checks the pixel.  Headless; run in the interactive session (the Vulkan ICD needs a display).
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

#define W 64
#define H 64
static const float CLEAR[4] = { 0.25f, 0.5f, 0.75f, 1.0f };   /* B=0xbf G=0x80 R=0x40 */

static int
create_device(ID3D11Device **dev, ID3D11DeviceContext **ctx)
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                   dev, &fl, ctx);
    if (FAILED(hr)) {
        printf("[FAIL] D3D11CreateDevice 0x%08lx\n", hr);
        return 0;
    }
    return 1;
}

/* read pixel (0,0) of tex as BGRA */
static int
read_pixel(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, unsigned char px[4])
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(tex, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st)))
        return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        ID3D11Texture2D_Release(st);
        return 0;
    }
    memcpy(px, m.pData, 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
    return 1;
}

static int
check(const char *what, const unsigned char px[4])
{
    int ok = abs(px[0] - 0xbf) <= 1 && abs(px[1] - 0x80) <= 1 && abs(px[2] - 0x40) <= 1 && px[3] == 0xff;
    printf("[%s] %s: pixel B=%02x G=%02x R=%02x A=%02x\n", ok ? "PASS" : "FAIL", what, px[0], px[1], px[2],
           px[3]);
    return ok;
}

static int
child(HANDLE h)
{
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    if (!create_device(&dev, &ctx))
        return 2;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_OpenSharedResource(dev, h, &IID_ID3D11Texture2D, (void **)&tex);
    if (FAILED(hr) || !tex) {
        printf("[FAIL] child: OpenSharedResource(%p) 0x%08lx\n", h, hr);
        return 3;
    }
    printf("[PASS] child: OpenSharedResource\n");
    unsigned char px[4] = { 0 };
    if (!read_pixel(dev, ctx, tex, px)) {
        printf("[FAIL] child: readback\n");
        return 4;
    }
    return check("child (other process) sees the parent's clear", px) ? 0 : 5;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "child"))
        return child((HANDLE)(uintptr_t)strtoull(argv[2], NULL, 0));

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    if (!create_device(&dev, &ctx))
        return 1;

    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = W;
    d.Height = H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &tex);
    if (FAILED(hr)) {
        printf("[FAIL] CreateTexture2D(MISC_SHARED) 0x%08lx\n", hr);
        return 1;
    }
    printf("[PASS] CreateTexture2D(MISC_SHARED)\n");

    ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv))) {
        printf("[FAIL] CreateRenderTargetView\n");
        return 1;
    }
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, CLEAR);

    /* GPU sync before another process reads it */
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q = NULL;
    ID3D11Device_CreateQuery(dev, &qd, &q);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(ctx);
    BOOL done = FALSE;
    for (int i = 0; i < 5000 && ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof(done), 0) != S_OK; i++)
        Sleep(1);

    unsigned char px[4] = { 0 };
    if (!read_pixel(dev, ctx, tex, px) || !check("parent readback", px))
        return 1;

    IDXGIResource *dr = NULL;
    HANDLE h = NULL;
    if (FAILED(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dr)) ||
        FAILED(IDXGIResource_GetSharedHandle(dr, &h)) || !h) {
        printf("[FAIL] GetSharedHandle\n");
        return 1;
    }
    printf("[PASS] GetSharedHandle -> %p\n", h);

    /* same-process open */
    ID3D11Texture2D *same = NULL;
    hr = ID3D11Device_OpenSharedResource(dev, h, &IID_ID3D11Texture2D, (void **)&same);
    if (FAILED(hr) || !same) {
        printf("[FAIL] same-process OpenSharedResource 0x%08lx\n", hr);
    } else {
        memset(px, 0, 4);
        if (read_pixel(dev, ctx, same, px))
            check("same-process open", px);
        ID3D11Texture2D_Release(same);
    }

    char cmd[MAX_PATH + 64], self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cmd, sizeof(cmd), "\"%s\" child 0x%llx", self, (unsigned long long)(uintptr_t)h);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { /* inherit stdout */
        printf("[FAIL] CreateProcess %lu\n", GetLastError());
        return 1;
    }
    DWORD code = 99;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0)
        printf("[FAIL] child timed out\n");
    GetExitCodeProcess(pi.hProcess, &code);
    printf("RESULT: %s (child exit %lu)\n", code == 0 ? "cross-process sharing works" : "FAILED", code);
    return code == 0 ? 0 : 1;
}
