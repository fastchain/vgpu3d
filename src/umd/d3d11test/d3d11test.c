/*
 * d3d11test - headless Direct3D 11 smoke test (meant to run on DXVK -> Venus -> vgpu).
 *
 * Creates a D3D11 device, reports the DXGI adapter, compiles a vertex/pixel shader with
 * d3dcompiler_47, draws a full-screen-ish triangle into an offscreen 256x256 RGBA8
 * render target, copies it to a staging texture and checks pixels inside and outside
 * the triangle.  Then times N draw+readback iterations.  No window, so it runs over SSH.
 *
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#define INITGUID
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#define W 256
#define H 256

static int fails;

#define HR(x, what)                                                                 \
    do {                                                                            \
        HRESULT hr_ = (x);                                                          \
        if (FAILED(hr_)) {                                                          \
            printf("[FAIL] %s -> 0x%08lx\n", what, (unsigned long)hr_);             \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static const char *src =
    "struct V { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  V o; float2 p[3] = { float2(-0.5,-0.5), float2(0.0,0.5), float2(0.5,-0.5) };\n"
    "  o.pos = float4(p[id], 0, 1); o.col = float4(1, 0.5, 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return i.col; }\n";

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                         LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static int compile(PFN_D3DCompile c, const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = c(src, strlen(src), "d3d11test", NULL, NULL, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) {
        printf("[FAIL] compile %s: %s\n", entry, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    char path[MAX_PATH] = "?";
    if (d3d11) GetModuleFileNameA(d3d11, path, sizeof path);
    printf("[INFO] d3d11.dll loaded from %s\n", path);
    PFN_D3D11_CREATE_DEVICE create = d3d11 ? (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile d3dcompile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!create || !d3dcompile) { printf("[FAIL] missing d3d11.dll or d3dcompiler_47.dll\n"); return 1; }

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HR(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2, D3D11_SDK_VERSION, &dev, &fl, &ctx), "D3D11CreateDevice");
    printf("[PASS] D3D11CreateDevice, feature level %x.%x\n", (fl >> 12) & 0xf, (fl >> 8) & 0xf);

    IDXGIDevice *dxgidev;
    IDXGIAdapter *ad;
    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgidev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgidev, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
        printf("[INFO] adapter: %ls (vendor 0x%04x device 0x%04x, %llu MiB dedicated)\n", desc.Description, desc.VendorId,
               desc.DeviceId, (unsigned long long)desc.DedicatedVideoMemory >> 20);

    ID3DBlob *vsb, *psb;
    if (!compile(d3dcompile, "vs", "vs_5_0", &vsb) || !compile(d3dcompile, "ps", "ps_5_0", &psb)) return 1;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    HR(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs), "CreateVertexShader");
    HR(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps), "CreatePixelShader");
    printf("[PASS] shaders created\n");

    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET };
    ID3D11Texture2D *rt, *st;
    HR(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt), "CreateTexture2D (RT)");
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st), "CreateTexture2D (staging)");
    ID3D11RenderTargetView *rtv;
    HR(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv), "CreateRenderTargetView");

    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    const float clear[4] = { 0.0f, 0.25f, 1.0f, 1.0f };
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    D3D11_MAPPED_SUBRESOURCE m;
    for (int iter = 0; iter < 101; iter++) {
        if (iter == 1) QueryPerformanceCounter(&t0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
        HR(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m), "Map staging");
        if (iter == 0) {
            const unsigned char *c = (const unsigned char *)m.pData + (H / 2) * m.RowPitch + (W / 2) * 4;
            const unsigned char *e = (const unsigned char *)m.pData + 4 * m.RowPitch + 4 * 4;
            printf("[INFO] center pixel %02x %02x %02x %02x, corner pixel %02x %02x %02x %02x\n", c[0], c[1], c[2], c[3],
                   e[0], e[1], e[2], e[3]);
            int ok_c = c[0] == 0xff && c[1] >= 0x7e && c[1] <= 0x81 && c[2] == 0x00;
            int ok_e = e[0] == 0x00 && e[1] >= 0x3f && e[1] <= 0x41 && e[2] == 0xff;
            printf("[%s] triangle pixel (orange) correct\n", ok_c ? "PASS" : "FAIL");
            printf("[%s] clear pixel (blue) correct\n", ok_e ? "PASS" : "FAIL");
            fails += !ok_c + !ok_e;
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    QueryPerformanceCounter(&t1);
    printf("[INFO] 100 x (clear+draw+copy+map readback): %.3f ms each\n",
           (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 100);
    printf("RESULT: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
