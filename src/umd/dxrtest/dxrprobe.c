/* dxrprobe: ask vkd3d-proton directly (as Triton loads it) which DXR tier it supports on this device.
 * usage: dxrprobe.exe <driver-store-folder> */
#define COBJMACROS
#define INITGUID
#include <stdio.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

typedef struct fac fac;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(fac *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(fac *);
    ULONG(STDMETHODCALLTYPE *Release)(fac *);
    void *f[6];
    HRESULT(STDMETHODCALLTYPE *CreateDevice)(fac *, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
} fac_vtbl;
struct fac { const fac_vtbl *lpVtbl; };
static const GUID CLSID_Fac = { 0x114863bf, 0xc386, 0x4aee, { 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55 } };
static const GUID IID_Fac = { 0x61f307d3, 0xd34e, 0x4e7c, { 0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb } };

int main(int argc, char **argv)
{
    char p1[MAX_PATH], p2[MAX_PATH];
    if (argc < 2) return 2;
    snprintf(p1, sizeof(p1), "%s\\vkd3d_d3d12core.dll", argv[1]);
    snprintf(p2, sizeof(p2), "%s\\dxvk_dxgi.dll", argv[1]);
    HMODULE core = LoadLibraryExA(p1, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE dx = LoadLibraryExA(p2, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core || !dx) { printf("load failed %p %p\n", core, dx); return 2; }
    HRESULT(WINAPI *gi)(REFCLSID, REFIID, void **) = (void *)GetProcAddress(core, "D3D12GetInterface");
    HRESULT(WINAPI *cf)(REFIID, void **) = (void *)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1 *f; IDXGIAdapter *a = NULL;
    cf(&IID_IDXGIFactory1, (void **)&f); IDXGIFactory1_EnumAdapters(f, 0, &a);
    fac *fa; HRESULT hr = gi(&CLSID_Fac, &IID_Fac, (void **)&fa);
    ID3D12Device *d = NULL;
    if (SUCCEEDED(hr)) hr = fa->lpVtbl->CreateDevice(fa, (IUnknown *)a, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&d);
    if (FAILED(hr)) { printf("device 0x%08lx\n", hr); return 2; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
    hr = ID3D12Device_CheckFeatureSupport(d, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    printf("OPTIONS5 hr=0x%08lx RaytracingTier=0x%x RenderPassesTier=%d\n", hr, o5.RaytracingTier, o5.RenderPassesTier);
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_7 };
    ID3D12Device_CheckFeatureSupport(d, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    printf("HighestShaderModel=0x%x\n", sm.HighestShaderModel);
    return 0;
}
