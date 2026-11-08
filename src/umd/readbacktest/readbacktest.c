/*
 * readbacktest - D3D11 render-to-texture readback: fills every face/mip of a cube texture (and a
 * plain 2D mip chain) through render target views with known colours, copies it to a STAGING
 * texture (CopyResource and per-subresource CopySubresourceRegion) and maps each subresource back.
 * Unigine bakes environment probes this way at load; black probes meant missing indirect light.
 * Usage: readbacktest [format]   (DXGI format number, default 10 = R16G16B16A16_FLOAT)
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static float
half_to_float(unsigned short h)
{
    unsigned s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);
    return s ? -v : v;
}

static int
check(ID3D11DeviceContext *ctx, ID3D11Resource *staging, UINT sub, DXGI_FORMAT fmt, const float *want, const char *what)
{
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ID3D11DeviceContext_Map(ctx, staging, sub, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        printf("[FAIL] %s sub %u: Map 0x%08lx\n", what, sub, (unsigned long)hr);
        return 1;
    }
    float got[4];
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const unsigned short *p = (const unsigned short *)m.pData;
        for (int i = 0; i < 4; i++)
            got[i] = half_to_float(p[i]);
    } else if (fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        for (int i = 0; i < 4; i++)
            got[i] = ((const float *)m.pData)[i];
    } else {
        const unsigned char *p = (const unsigned char *)m.pData;
        for (int i = 0; i < 4; i++)
            got[i] = p[i] / 255.0f;
    }
    ID3D11DeviceContext_Unmap(ctx, staging, sub);
    int bad = 0;
    for (int i = 0; i < 4; i++)
        if (fabsf(got[i] - want[i]) > 0.01f)
            bad = 1;
    if (bad)
        printf("[FAIL] %s sub %u: got %.3f %.3f %.3f %.3f want %.3f %.3f %.3f %.3f\n", what, sub, got[0], got[1], got[2],
               got[3], want[0], want[1], want[2], want[3]);
    return bad;
}

static int
run(ID3D11Device *dev, ID3D11DeviceContext *ctx, DXGI_FORMAT fmt, BOOL cube, UINT size, UINT mips)
{
    const UINT faces = cube ? 6 : 1;
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = d.Height = size;
    d.MipLevels = mips;
    d.ArraySize = faces;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    ID3D11Texture2D *tex = NULL, *st1 = NULL, *st2 = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tex))) {
        printf("[FAIL] CreateTexture2D fmt %u\n", fmt);
        return 1;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st1);
    ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st2);
    for (UINT f = 0; f < faces; f++) {
        for (UINT m = 0; m < mips; m++) {
            D3D11_RENDER_TARGET_VIEW_DESC rd = { 0 };
            rd.Format = fmt;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice = m;
            rd.Texture2DArray.FirstArraySlice = f;
            rd.Texture2DArray.ArraySize = 1;
            ID3D11RenderTargetView *rtv = NULL;
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, &rd, &rtv);
            float c[4] = { 0.1f * (f + 1), 0.1f * (m + 1), 0.5f, 1.0f };
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
            ID3D11RenderTargetView_Release(rtv);
        }
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st1, (ID3D11Resource *)tex);
    for (UINT s = 0; s < faces * mips; s++)
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st2, s, 0, 0, 0, (ID3D11Resource *)tex, s, NULL);
    int bad = 0;
    char what[64];
    for (UINT f = 0; f < faces; f++) {
        for (UINT m = 0; m < mips; m++) {
            float want[4] = { 0.1f * (f + 1), 0.1f * (m + 1), 0.5f, 1.0f };
            UINT sub = m + f * mips; /* D3D11CalcSubresource */
            snprintf(what, sizeof(what), "%s fmt %u CopyResource face %u mip %u", cube ? "cube" : "2d", fmt, f, m);
            bad += check(ctx, (ID3D11Resource *)st1, sub, fmt, want, what);
            snprintf(what, sizeof(what), "%s fmt %u CopySubresource face %u mip %u", cube ? "cube" : "2d", fmt, f, m);
            bad += check(ctx, (ID3D11Resource *)st2, sub, fmt, want, what);
        }
    }
    printf("[%s] %s %ux%u fmt %u, %u faces x %u mips: %d bad subresources\n", bad ? "FAIL" : "PASS", cube ? "cube" : "2d",
           size, size, fmt, faces, mips, bad);
    ID3D11Texture2D_Release(st2);
    ID3D11Texture2D_Release(st1);
    ID3D11Texture2D_Release(tex);
    return bad;
}

/* Readback after polling: the app waits on an event query (or Map DO_NOT_WAIT) instead of a
 * blocking Map.  A heavy GPU workload in front of the copy makes an early "done" visible. */
static int
run_polled(ID3D11Device *dev, ID3D11DeviceContext *ctx, BOOL use_query)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = d.Height = 64;
    d.MipLevels = 1;
    d.ArraySize = 6;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    ID3D11Texture2D *probe = NULL, *staging = NULL, *big = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &probe);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging);
    D3D11_TEXTURE2D_DESC bd = d;
    bd.Width = 3840;
    bd.Height = 2160;
    bd.ArraySize = 1;
    bd.MiscFlags = 0;
    ID3D11Device_CreateTexture2D(dev, &bd, NULL, &big);
    ID3D11RenderTargetView *bigrtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)big, NULL, &bigrtv);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q = NULL;
    ID3D11Device_CreateQuery(dev, &qd, &q);
    int bad = 0;
    for (int round = 0; round < 8; round++) {
        float junk[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
        for (int i = 0; i < 300; i++)
            ID3D11DeviceContext_ClearRenderTargetView(ctx, bigrtv, junk);
        for (UINT f = 0; f < 6; f++) {
            D3D11_RENDER_TARGET_VIEW_DESC rd = { 0 };
            rd.Format = d.Format;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.FirstArraySlice = f;
            rd.Texture2DArray.ArraySize = 1;
            ID3D11RenderTargetView *rtv = NULL;
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)probe, &rd, &rtv);
            float c[4] = { 0.1f * (f + 1), 0.05f * (round + 1), 0.5f, 1.0f };
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
            ID3D11RenderTargetView_Release(rtv);
        }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)probe);
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr;
        if (use_query) {
            ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
            ID3D11DeviceContext_Flush(ctx);
            BOOL done = FALSE;
            while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof(done), 0) != S_OK)
                ;
        }
        int spins = 0;
        for (UINT f = 0; f < 6; f++) {
            while ((hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, f, D3D11_MAP_READ,
                                                 D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) == DXGI_ERROR_WAS_STILL_DRAWING)
                spins++;
            if (FAILED(hr)) {
                printf("[FAIL] Map 0x%08lx\n", (unsigned long)hr);
                bad++;
                continue;
            }
            const unsigned short *px = (const unsigned short *)m.pData;
            float r = half_to_float(px[0]), g = half_to_float(px[1]);
            if (fabsf(r - 0.1f * (f + 1)) > 0.01f || fabsf(g - 0.05f * (round + 1)) > 0.01f) {
                printf("[FAIL] %s round %d face %u: got %.3f %.3f want %.3f %.3f (spins %d)\n",
                       use_query ? "event query" : "Map DO_NOT_WAIT", round, f, r, g, 0.1f * (f + 1), 0.05f * (round + 1), spins);
                bad++;
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, f);
        }
    }
    printf("[%s] polled readback (%s): %d bad faces over 8 rounds\n", bad ? "FAIL" : "PASS",
           use_query ? "event query then Map DO_NOT_WAIT" : "Map DO_NOT_WAIT loop", bad);
    ID3D11Query_Release(q);
    ID3D11RenderTargetView_Release(bigrtv);
    ID3D11Texture2D_Release(big);
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(probe);
    return bad;
}

/* Shadow-map style depth cube (R16_TYPELESS, DSV D16_UNORM, SRV R16_UNORM) cleared to a depth value
 * through one DSV over all six faces (all_faces) or a DSV per face, then read back. */
static int
run_depth_cube(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT size, BOOL all_faces, BOOL array)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = d.Height = size;
    d.MipLevels = 1;
    d.ArraySize = array ? 12 : 6;
    d.Format = DXGI_FORMAT_R16_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tex))) {
        printf("[FAIL] create depth cube\n");
        return 1;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging);
    UINT nviews = all_faces ? 1 : d.ArraySize;
    for (UINT v = 0; v < nviews; v++) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = { 0 };
        dd.Format = DXGI_FORMAT_D16_UNORM;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray.FirstArraySlice = all_faces ? 0 : v;
        dd.Texture2DArray.ArraySize = all_faces ? d.ArraySize : 1;
        ID3D11DepthStencilView *dsv = NULL;
        if (FAILED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)tex, &dd, &dsv))) {
            printf("[FAIL] create DSV\n");
            return 1;
        }
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11DepthStencilView_Release(dsv);
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
    int bad = 0;
    for (UINT f = 0; f < d.ArraySize; f++) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, f, D3D11_MAP_READ, 0, &m))) {
            bad++;
            continue;
        }
        unsigned short v0 = ((const unsigned short *)m.pData)[0];
        unsigned short v1 = ((const unsigned short *)((const char *)m.pData + (size - 1) * m.RowPitch))[size - 1];
        if (v0 != 0xffff || v1 != 0xffff) {
            if (bad < 4)
                printf("  face %u: depth %04x %04x, want ffff\n", f, v0, v1);
            bad++;
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, f);
    }
    printf("[%s] depth %s %ux%u cleared to 1.0 via %s: %d bad faces\n", bad ? "FAIL" : "PASS", array ? "cube array" : "cube",
           size, size, all_faces ? "one DSV over all faces" : "a DSV per face", bad);
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(tex);
    return bad;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
        printf("[FAIL] D3D11CreateDevice\n");
        return 1;
    }
    int bad = 0;
    DXGI_FORMAT fmts[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT };
    if (argc > 1)
        fmts[0] = (DXGI_FORMAT)atoi(argv[1]);
    for (int i = 0; i < (argc > 1 ? 1 : 3); i++) {
        bad += run(dev, ctx, fmts[i], TRUE, 64, 7);
        bad += run(dev, ctx, fmts[i], FALSE, 64, 7);
    }
    bad += run_polled(dev, ctx, TRUE);
    bad += run_polled(dev, ctx, FALSE);
    for (int sz = 4; sz <= 128; sz *= 32)
        for (int arr = 0; arr < 2; arr++) {
            bad += run_depth_cube(dev, ctx, sz, TRUE, arr);
            bad += run_depth_cube(dev, ctx, sz, FALSE, arr);
        }
    printf("[%s] readback\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
