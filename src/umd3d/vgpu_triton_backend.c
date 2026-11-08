/*
 * vgpu D3D10/11 UMD backend for Triton's DDI layer (osy/virtio-win-mesa
 * src/virtio/neptune/triton, MIT).
 *
 * Triton translates the WDDM D3D10/11 DDI onto an in-process ID3D11Device.
 * Upstream that device is Neptune (D3D11 remoted to a modified host); here it
 * is DXVK (dxvk_d3d11.dll + dxvk_dxgi.dll, D3D11 -> Vulkan) running on the
 * Venus Vulkan ICD of the same adapter, so the host stays stock QEMU.
 *
 * This file provides the handful of Neptune symbols the Triton D3D10/11 files
 * link against, including cross-process sharing (tritonSharedBridge*).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "triton.h" /* windows, winternl, d3dkmthk, d3d10umddi, d3d11_3/4, dxgiddi */
#include "triton_log.h"
#include "tritonSharedBridge.h"
#include "npt_env.h"
#include "virtio-gpu/virgl_hw.h"
#include "virtio-gpu/wddm_hw.h"
#include <vulkan/vulkan.h>

static HMODULE g_self;

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD fdwReason, LPVOID lpvReserved)
{
   (void)lpvReserved;
   if (fdwReason == DLL_PROCESS_ATTACH) {
      g_self = hInst;
      DisableThreadLibraryCalls(hInst);
      TR_LOG("vgpu UMD (Triton DDI + DXVK) DLL_PROCESS_ATTACH");
   }
   return TRUE;
}

/* ---- env / host workarounds ---------------------------------------------- */

struct npt_env npt_env;

void
npt_env_init(void)
{
}

/* DXVK on Venus needs none of Neptune's per-host-backend shader rewrites. */
uint32_t
npt_host_workaround_flags(void)
{
   return 0;
}

/* ---- device factory ------------------------------------------------------ */

typedef HRESULT(WINAPI *PFN_D3D11_CREATE_DEVICE_DXVK)(
   IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
   UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* The WOW64 (32-bit) UMD uses its own DXVK pair in the same DriverStore folder. */
#ifdef _WIN64
#define VGPU_DXVK_D3D11 L"dxvk_d3d11.dll"
#else
#define VGPU_DXVK_D3D11 L"dxvk32_d3d11.dll"
#endif

static PFN_D3D11_CREATE_DEVICE_DXVK
vgpu_dxvk_create_device(void)
{
   static PFN_D3D11_CREATE_DEVICE_DXVK pfn;
   static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
   BOOL pending;

   if (!InitOnceBeginInitialize(&once, 0, &pending, NULL))
      return NULL;
   if (pending) {
      /* dxvk_d3d11.dll and its import dxvk_dxgi.dll sit next to this UMD in
       * the DriverStore; LOAD_WITH_ALTERED_SEARCH_PATH resolves the import
       * from that directory, never from System32. */
      WCHAR path[MAX_PATH];
      DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
      while (n && path[n - 1] != L'\\')
         n--;
      path[n] = 0;
      if (n + 32 < MAX_PATH) {
         wcscat(path, VGPU_DXVK_D3D11);
         HMODULE dxvk = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
         if (dxvk)
            pfn = (PFN_D3D11_CREATE_DEVICE_DXVK)(void *)GetProcAddress(dxvk, "D3D11CreateDevice");
         else
            TR_LOG("vgpu: loading %ls failed (%lu)", VGPU_DXVK_D3D11, GetLastError());
      }
      InitOnceComplete(&once, 0, NULL);
   }
   return pfn;
}

HRESULT
npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter,
                                 D3D_DRIVER_TYPE DriverType,
                                 HMODULE Software,
                                 UINT Flags,
                                 const D3D_FEATURE_LEVEL *pFeatureLevels,
                                 UINT FeatureLevels,
                                 UINT SDKVersion,
                                 ID3D11Device **ppDevice,
                                 D3D_FEATURE_LEVEL *pFeatureLevel,
                                 ID3D11DeviceContext **ppImmediateContext)
{
   (void)pAdapter;
   (void)DriverType;
   (void)Software;

   PFN_D3D11_CREATE_DEVICE_DXVK create = vgpu_dxvk_create_device();
   if (!create)
      return E_FAIL;

   /* DXVK picks its own (only) Vulkan adapter: the Venus device of this GPU.
    * The runtime-side debug layer belongs to the app's d3d11.dll, not here. */
   Flags &= ~D3D11_CREATE_DEVICE_DEBUG;
   HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, Flags, pFeatureLevels,
                       FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
                       ppImmediateContext);
   if (FAILED(hr))
      TR_LOG("vgpu: DXVK D3D11CreateDevice failed 0x%08lx", hr);
   return hr;
}

/* D3D12: vkd3d-proton's d3d12core, loaded in-process like DXVK, creates the
 * ID3D12Device that Triton's D3D12 DDI layer drives.  Its Agility-SDK entry
 * point D3D12GetInterface hands out the device factory. */
#ifdef _WIN64
#define VGPU_VKD3D_CORE L"vkd3d_d3d12core.dll"
#define VGPU_DXVK_DXGI L"dxvk_dxgi.dll"
#else
#define VGPU_VKD3D_CORE L"vkd3d32_d3d12core.dll"
#define VGPU_DXVK_DXGI L"dxvk32_dxgi.dll"
#endif

/* ID3D12DeviceFactory (Agility SDK; not in the MinGW headers) */
typedef struct vgpu_d3d12_device_factory vgpu_d3d12_device_factory;
typedef struct {
   HRESULT(STDMETHODCALLTYPE *QueryInterface)(vgpu_d3d12_device_factory *, REFIID, void **);
   ULONG(STDMETHODCALLTYPE *AddRef)(vgpu_d3d12_device_factory *);
   ULONG(STDMETHODCALLTYPE *Release)(vgpu_d3d12_device_factory *);
   HRESULT(STDMETHODCALLTYPE *InitializeFromGlobalState)(vgpu_d3d12_device_factory *);
   HRESULT(STDMETHODCALLTYPE *ApplyToGlobalState)(vgpu_d3d12_device_factory *);
   HRESULT(STDMETHODCALLTYPE *SetFlags)(vgpu_d3d12_device_factory *, UINT);
   UINT(STDMETHODCALLTYPE *GetFlags)(vgpu_d3d12_device_factory *);
   HRESULT(STDMETHODCALLTYPE *GetConfigurationInterface)(vgpu_d3d12_device_factory *, REFCLSID, REFIID, void **);
   HRESULT(STDMETHODCALLTYPE *EnableExperimentalFeatures)(vgpu_d3d12_device_factory *, UINT, const IID *, void *, UINT *);
   HRESULT(STDMETHODCALLTYPE *CreateDevice)(vgpu_d3d12_device_factory *, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
} vgpu_d3d12_device_factory_vtbl;
struct vgpu_d3d12_device_factory {
   const vgpu_d3d12_device_factory_vtbl *lpVtbl;
};
static const GUID vgpu_CLSID_D3D12DeviceFactory = { 0x114863bf, 0xc386, 0x4aee, { 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55 } };
static const GUID vgpu_IID_ID3D12DeviceFactory = { 0x61f307d3, 0xd34e, 0x4e7c, { 0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb } };

typedef HRESULT(WINAPI *PFN_D3D12_GET_INTERFACE_VGPU)(REFCLSID, REFIID, void **);
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY1_VGPU)(REFIID, void **);

static HMODULE
vgpu_load_sibling(const WCHAR *name)
{
   WCHAR path[MAX_PATH];
   DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
   while (n && path[n - 1] != L'\\')
      n--;
   path[n] = 0;
   if (n + wcslen(name) + 1 >= MAX_PATH)
      return NULL;
   wcscat(path, name);
   HMODULE mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!mod)
      TR_LOG("vgpu: loading %ls failed (%lu)", name, GetLastError());
   return mod;
}

HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                 REFIID riid, void **ppDevice)
{
   static PFN_D3D12_GET_INTERFACE_VGPU get_interface;
   static PFN_CREATE_DXGI_FACTORY1_VGPU create_factory;
   static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
   BOOL pending;

   if (!ppDevice)
      return E_POINTER;
   *ppDevice = NULL;
   if (!InitOnceBeginInitialize(&once, 0, &pending, NULL))
      return E_FAIL;
   if (pending) {
      HMODULE core = vgpu_load_sibling(VGPU_VKD3D_CORE);
      if (core)
         get_interface = (PFN_D3D12_GET_INTERFACE_VGPU)(void *)GetProcAddress(core, "D3D12GetInterface");
      HMODULE dxgi = vgpu_load_sibling(VGPU_DXVK_DXGI);
      if (dxgi)
         create_factory = (PFN_CREATE_DXGI_FACTORY1_VGPU)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
      InitOnceComplete(&once, 0, NULL);
   }
   if (!get_interface)
      return E_FAIL;

   /* Hand vkd3d-proton DXVK's adapter for the Venus device: without one it
    * would enumerate through the system DXGI, from inside the runtime's own
    * device creation. */
   IDXGIAdapter *adapter = NULL;
   if (!pAdapter && create_factory) {
      IDXGIFactory1 *factory = NULL;
      if (SUCCEEDED(create_factory(&IID_IDXGIFactory1, (void **)&factory))) {
         IDXGIFactory1_EnumAdapters(factory, 0, &adapter);
         IDXGIFactory1_Release(factory);
      }
   }

   vgpu_d3d12_device_factory *factory = NULL;
   HRESULT hr = get_interface(&vgpu_CLSID_D3D12DeviceFactory, &vgpu_IID_ID3D12DeviceFactory, (void **)&factory);
   if (SUCCEEDED(hr)) {
      hr = factory->lpVtbl->CreateDevice(factory, pAdapter ? pAdapter : (IUnknown *)adapter,
                                         MinimumFeatureLevel, riid, ppDevice);
      factory->lpVtbl->Release(factory);
   }
   if (adapter)
      IDXGIAdapter_Release(adapter);
   if (FAILED(hr))
      TR_LOG("vgpu: vkd3d-proton D3D12 device creation failed 0x%08lx", hr);
   return hr;
}

/* ---- adapter probe ------------------------------------------------------- */

void
tritonSharedBridgeAdapterProbe(struct triton_adapter_probe *out)
{
   /* Only loaded for our adapter (INF UserModeDriverName); its KMD is the
    * WDDM2 viogpu3d.  The "host" is the in-process DXVK / vkd3d-proton pair:
    * vkd3d-proton is a D3D12 backend that consumes DXIL (SM 6.x) through
    * dxil-spirv. */
   *out = (struct triton_adapter_probe){
      .viogpu = true,
      .wddm2 = true,
      .host_ok = true,
      .caps_flags = TRITON_HOSTCAP_D3D12 | TRITON_HOSTCAP_DXIL,
   };
}

/* ---- cross-process sharing --------------------------------------------- */

/* Shared textures are DXVK MISC_SHARED textures: DXVK allocates exportable
 * memory and Venus backs it with a shareable blob allocation (its own D3DKMT
 * device), whose global D3DKMT handle IDXGIResource::GetSharedHandle returns.
 * The runtime-side KM allocation Triton creates (pfnAllocateCb) adopts that
 * blob's res_id (VIOGPU_SHARED_IMPORT_CTX) and carries the global handle in
 * the opaque modifier field; an opening process (DWM) re-creates the texture
 * from the handle with DXVK's own import path. */

static const GUID vgpu_IID_IDXGIResource =
   { 0x035f3ab4, 0x482e, 0x4e50, { 0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b } };
static const GUID vgpu_IID_IDXGIVkInteropDevice1 =
   { 0xe2ef5fa5, 0xdc21, 0x4af7, { 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24 } };

/* IDXGIVkInteropDevice1 (DXVK dxgi_interfaces.h + vgpu patch), C vtable */
typedef struct vgpu_interop vgpu_interop;
struct vgpu_interop_vtbl {
   HRESULT(STDMETHODCALLTYPE *QueryInterface)(vgpu_interop *, REFIID, void **);
   ULONG(STDMETHODCALLTYPE *AddRef)(vgpu_interop *);
   ULONG(STDMETHODCALLTYPE *Release)(vgpu_interop *);
   void *GetVulkanHandles;
   void *GetSubmissionQueue;
   void *TransitionSurfaceLayout;
   void *FlushRenderingCommands;
   void *LockSubmissionQueue;
   void *ReleaseSubmissionQueue;
   void *GetSubmissionQueue1;
   void *CreateTexture2DFromVkImage;
   HRESULT(STDMETHODCALLTYPE *CreateTexture2DFromSharedHandle)(vgpu_interop *,
                                                              const D3D11_TEXTURE2D_DESC1 *,
                                                              HANDLE, ID3D11Texture2D **);
};
struct vgpu_interop {
   const struct vgpu_interop_vtbl *lpVtbl;
};

/* IDXGIVkInteropSurface (DXVK dxgi_interfaces.h), C vtable */
static const GUID vgpu_IID_IDXGIVkInteropSurface =
   { 0x5546cf8c, 0x77e7, 0x4341, { 0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d } };
typedef struct vgpu_interop_surface vgpu_interop_surface;
struct vgpu_interop_surface_vtbl {
   HRESULT(STDMETHODCALLTYPE *QueryInterface)(vgpu_interop_surface *, REFIID, void **);
   ULONG(STDMETHODCALLTYPE *AddRef)(vgpu_interop_surface *);
   ULONG(STDMETHODCALLTYPE *Release)(vgpu_interop_surface *);
   HRESULT(STDMETHODCALLTYPE *GetDevice)(vgpu_interop_surface *, vgpu_interop **);
   HRESULT(STDMETHODCALLTYPE *GetVulkanImageInfo)(vgpu_interop_surface *, VkImage *,
                                                 VkImageLayout *, VkImageCreateInfo *);
};
struct vgpu_interop_surface {
   const struct vgpu_interop_surface_vtbl *lpVtbl;
};

/* Row pitch / offset of a shared texture's memory.  Shareable images are
 * LINEAR DRM-modifier images on the host (Venus vgpu patch), so plane 0 is
 * what QEMU scans out (SET_SCANOUT_BLOB needs the real stride). */
static bool
vgpu_linear_layout(IUnknown *tex, uint64_t *pitch, uint64_t *offset)
{
   vgpu_interop_surface *surf = NULL;
   if (FAILED(tex->lpVtbl->QueryInterface(tex, &vgpu_IID_IDXGIVkInteropSurface, (void **)&surf)))
      return false;
   VkImage img = VK_NULL_HANDLE;
   vgpu_interop *dev = NULL;
   bool ok = false;
   if (SUCCEEDED(surf->lpVtbl->GetVulkanImageInfo(surf, &img, NULL, NULL)) && img &&
       SUCCEEDED(surf->lpVtbl->GetDevice(surf, &dev)) && dev) {
      VkInstance inst;
      VkPhysicalDevice pd;
      VkDevice vkdev;
      ((void(STDMETHODCALLTYPE *)(vgpu_interop *, VkInstance *, VkPhysicalDevice *, VkDevice *))
          dev->lpVtbl->GetVulkanHandles)(dev, &inst, &pd, &vkdev);
      HMODULE vk = GetModuleHandleA("vulkan-1.dll");
      PFN_vkGetInstanceProcAddr gipa =
         vk ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr") : NULL;
      PFN_vkGetDeviceProcAddr gdpa =
         gipa ? (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr") : NULL;
      PFN_vkGetImageSubresourceLayout layout_fn =
         gdpa ? (PFN_vkGetImageSubresourceLayout)gdpa(vkdev, "vkGetImageSubresourceLayout") : NULL;
      if (layout_fn) {
         VkImageSubresource sub = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
         VkSubresourceLayout lay = { 0 };
         layout_fn(vkdev, img, &sub, &lay);
         *pitch = lay.rowPitch;
         *offset = lay.offset;
         ok = lay.rowPitch != 0;
      }
      dev->lpVtbl->Release(dev);
   }
   surf->lpVtbl->Release(surf);
   return ok;
}

/* A private D3DKMT device on our adapter, used to resolve a global share
 * handle to the KMD's res_id (RES_INFO). */
static struct {
   INIT_ONCE once;
   D3DKMT_HANDLE adapter, device;
   PFND3DKMT_QUERYRESOURCEINFO queryResourceInfo;
   PFND3DKMT_OPENRESOURCE openResource;
   PFND3DKMT_DESTROYALLOCATION destroyAllocation;
   PFND3DKMT_ESCAPE escape;
} vgpu_kmt = { INIT_ONCE_STATIC_INIT };

static BOOL CALLBACK
vgpu_kmt_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once;
   (void)param;
   (void)ctx;
   HMODULE gdi = GetModuleHandleA("gdi32.dll");
   if (!gdi)
      gdi = LoadLibraryA("gdi32.dll");
   PFND3DKMT_ENUMADAPTERS2 enumAdapters2 = (void *)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
   PFND3DKMT_QUERYADAPTERINFO queryAdapterInfo = (void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
   PFND3DKMT_CREATEDEVICE createDevice = (void *)GetProcAddress(gdi, "D3DKMTCreateDevice");
   PFND3DKMT_CLOSEADAPTER closeAdapter = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
   vgpu_kmt.queryResourceInfo = (void *)GetProcAddress(gdi, "D3DKMTQueryResourceInfo");
   vgpu_kmt.openResource = (void *)GetProcAddress(gdi, "D3DKMTOpenResource");
   vgpu_kmt.destroyAllocation = (void *)GetProcAddress(gdi, "D3DKMTDestroyAllocation");
   vgpu_kmt.escape = (void *)GetProcAddress(gdi, "D3DKMTEscape");
   if (!enumAdapters2 || !queryAdapterInfo || !createDevice || !vgpu_kmt.queryResourceInfo ||
       !vgpu_kmt.openResource || !vgpu_kmt.destroyAllocation || !vgpu_kmt.escape)
      return TRUE;

   D3DKMT_ADAPTERINFO infos[16];
   D3DKMT_ENUMADAPTERS2 e = { .NumAdapters = 16, .pAdapters = infos };
   if (!NT_SUCCESS(enumAdapters2(&e)))
      return TRUE;
   for (ULONG i = 0; i < e.NumAdapters; i++) {
      VIOGPU_ADAPTERINFO ai = { 0 };
      D3DKMT_QUERYADAPTERINFO q = {
         .hAdapter = infos[i].hAdapter,
         .Type = KMTQAITYPE_UMDRIVERPRIVATE,
         .pPrivateDriverData = &ai,
         .PrivateDriverDataSize = sizeof(ai),
      };
      if (!vgpu_kmt.adapter && NT_SUCCESS(queryAdapterInfo(&q)) && ai.IamVioGPU == VIOGPU_IAM) {
         D3DKMT_CREATEDEVICE cd = { .hAdapter = infos[i].hAdapter };
         if (NT_SUCCESS(createDevice(&cd))) {
            vgpu_kmt.adapter = infos[i].hAdapter;
            vgpu_kmt.device = cd.hDevice;
            continue;
         }
      }
      D3DKMT_CLOSEADAPTER c = { .hAdapter = infos[i].hAdapter };
      if (closeAdapter)
         closeAdapter(&c);
   }
   if (!vgpu_kmt.device)
      TR_LOG("vgpu: no viogpu D3DKMT adapter for share-handle lookups");
   return TRUE;
}

/* global share handle -> KMD res_id and size, via a transient open */
static bool
vgpu_kmt_res_info(D3DKMT_HANDLE global, uint32_t *res_id, uint64_t *size)
{
   InitOnceExecuteOnce(&vgpu_kmt.once, vgpu_kmt_init, NULL, NULL);
   if (!vgpu_kmt.device)
      return false;

   D3DKMT_QUERYRESOURCEINFO q = { .hDevice = vgpu_kmt.device, .hGlobalShare = global };
   NTSTATUS st = vgpu_kmt.queryResourceInfo(&q);
   if (!NT_SUCCESS(st) || q.NumAllocations < 1 || q.NumAllocations > 4) {
      TR_LOG("vgpu: D3DKMTQueryResourceInfo(0x%x) failed 0x%lx n=%u", global, st,
             q.NumAllocations);
      return false;
   }

   uint8_t drvdata[4][512];
   D3DDDI_OPENALLOCATIONINFO oai[4];
   memset(oai, 0, sizeof(oai));
   for (UINT i = 0; i < q.NumAllocations; i++) {
      oai[i].pPrivateDriverData = drvdata[i];
      oai[i].PrivateDriverDataSize = sizeof(drvdata[i]);
   }
   uint8_t rt[1024], res[1024];
   if (q.PrivateRuntimeDataSize > sizeof(rt) || q.ResourcePrivateDriverDataSize > sizeof(res) ||
       q.TotalPrivateDriverDataSize > sizeof(drvdata)) {
      TR_LOG("vgpu: share handle 0x%x private data too large", global);
      return false;
   }
   D3DKMT_OPENRESOURCE o = {
      .hDevice = vgpu_kmt.device,
      .hGlobalShare = global,
      .NumAllocations = q.NumAllocations,
      .pOpenAllocationInfo = oai,
      .pPrivateRuntimeData = rt,
      .PrivateRuntimeDataSize = q.PrivateRuntimeDataSize,
      .pResourcePrivateDriverData = res,
      .ResourcePrivateDriverDataSize = q.ResourcePrivateDriverDataSize,
      .pTotalPrivateDriverDataBuffer = drvdata,
      .TotalPrivateDriverDataBufferSize = q.TotalPrivateDriverDataSize,
   };
   st = vgpu_kmt.openResource(&o);
   if (!NT_SUCCESS(st)) {
      TR_LOG("vgpu: D3DKMTOpenResource(0x%x) failed 0x%lx", global, st);
      return false;
   }

   VIOGPU_ESCAPE esc;
   memset(&esc, 0, sizeof(esc));
   esc.Type = VIOGPU_RES_INFO;
   esc.DataLength = sizeof(esc.ResourceInfo);
   esc.ResourceInfo.ResHandle = oai[0].hAllocation;
   /* the creator's (Venus) lookup cookie from the create-time private data:
    * handle-based RES_INFO can resolve to a different allocation */
   if (oai[0].PrivateDriverDataSize >= sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX))
      esc.ResourceInfo.LookupCookie =
         ((const VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX *)oai[0].pPrivateDriverData)->LookupCookie;
   D3DKMT_ESCAPE e = {
      .hAdapter = vgpu_kmt.adapter,
      .hDevice = vgpu_kmt.device,
      .pPrivateDriverData = &esc,
      .PrivateDriverDataSize = sizeof(esc),
   };
   st = vgpu_kmt.escape(&e);
   bool ok = NT_SUCCESS(st) && esc.ResourceInfo.IsCreated && esc.ResourceInfo.Id;
   if (ok) {
      *res_id = esc.ResourceInfo.Id;
      *size = esc.ResourceInfo.Size;
   } else {
      TR_LOG("vgpu: RES_INFO for share 0x%x failed 0x%lx created=%d id=%u", global, st,
             esc.ResourceInfo.IsCreated, esc.ResourceInfo.Id);
   }

   D3DKMT_DESTROYALLOCATION d = { .hDevice = vgpu_kmt.device, .hResource = o.hResource };
   vgpu_kmt.destroyAllocation(&d);
   return ok;
}

typedef HANDLE(WINAPI *PFN_VKD3D_RESOURCE_KMT_VGPU)(IUnknown *);

/* KMT handle of a vkd3d-proton shared resource (exported by our vkd3d build);
 * NULL for anything else.  The resource must be an ID3D12Resource: only call
 * it on objects that are not DXVK textures. */
static HANDLE
vgpu_vkd3d_resource_kmt(IUnknown *res)
{
   static const GUID iid_d3d12_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
   HMODULE core = GetModuleHandleW(VGPU_VKD3D_CORE);
   PFN_VKD3D_RESOURCE_KMT_VGPU fn =
      core ? (PFN_VKD3D_RESOURCE_KMT_VGPU)(void *)GetProcAddress(core, "vkd3d_vgpu_resource_kmt") : NULL;
   IUnknown *d3d12 = NULL;
   if (!fn || FAILED(res->lpVtbl->QueryInterface(res, &iid_d3d12_resource, (void **)&d3d12)))
      return NULL;
   HANDLE h = fn(d3d12);
   d3d12->lpVtbl->Release(d3d12);
   return h;
}

bool
tritonSharedBridgeExportBlob(void *pResourceWrapper, struct triton_shared_texture_desc *desc)
{
   IUnknown *tex = pResourceWrapper;
   IDXGIResource *dxgi = NULL;
   HANDLE share = NULL;
   HRESULT hr = E_NOINTERFACE;
   bool d3d12 = false;
   if (SUCCEEDED(tex->lpVtbl->QueryInterface(tex, &vgpu_IID_IDXGIResource, (void **)&dxgi))) {
      hr = IDXGIResource_GetSharedHandle(dxgi, &share);
      IDXGIResource_Release(dxgi);
   } else {
      /* a vkd3d-proton ID3D12Resource on a shared heap: our vkd3d build
       * exports its memory's KMT handle */
      share = vgpu_vkd3d_resource_kmt(tex);
      hr = share ? S_OK : E_FAIL;
      d3d12 = true;
   }
   if (FAILED(hr) || !share || share == INVALID_HANDLE_VALUE) {
      TR_LOG("vgpu: export: no shared handle (hr=0x%08lx%s)", hr, d3d12 ? ", D3D12" : "");
      return false;
   }

   uint32_t res_id = 0;
   uint64_t size = 0;
   if (!vgpu_kmt_res_info((D3DKMT_HANDLE)(uintptr_t)share, &res_id, &size))
      return false;

   desc->blob_id = res_id;
   desc->create_ctx_id = VIOGPU_SHARED_IMPORT_CTX;
   desc->modifier = (uint64_t)(uintptr_t)share;
   desc->allocation_size = size;
   desc->plane_count = 1;
   desc->texture_layout = 0;
   uint64_t pitch = 0, offset = 0;
   /* D3D12 resources: no DXVK interop surface, and the stride only matters
    * for blob scanout */
   if (!d3d12 && !vgpu_linear_layout(tex, &pitch, &offset))
      TR_LOG("vgpu: export res_id=%u: no linear layout (scanout stride unknown)", res_id);
   desc->planes[0].offset = offset;
   desc->planes[0].pitch = pitch;
   TR_LOG("vgpu: export res_id=%u share=0x%x size=%llu pitch=%llu", res_id,
          (unsigned)(uintptr_t)share, (unsigned long long)size, (unsigned long long)pitch);
   return true;
}

bool
tritonSharedBridgeImportRes(void *pDeviceWrapper, uint32_t res_id, uint64_t size,
                            uint32_t *out_alloc, uint32_t *out_res_kmt)
{
   /* Venus attaches the memory to its own context when it imports the share
    * handle (CreateTexture2DFromSharedHandle); nothing to hold here. */
   (void)pDeviceWrapper;
   (void)res_id;
   (void)size;
   *out_alloc = 0;
   *out_res_kmt = 0;
   return true;
}

void
tritonSharedBridgeReleaseImportRes(void *pDeviceWrapper, uint32_t alloc, uint32_t res_kmt)
{
   (void)pDeviceWrapper;
   (void)alloc;
   (void)res_kmt;
}

void *
tritonSharedBridgeOpenRes(void *pDeviceWrapper, uint32_t res_id,
                          const struct triton_shared_texture_desc *desc)
{
   IUnknown *dev = pDeviceWrapper;
   if (desc->create_ctx_id != VIOGPU_SHARED_IMPORT_CTX || !desc->modifier) {
      TR_LOG("vgpu: open res_id=%u: not a vgpu export (ctx=0x%x)", res_id, desc->create_ctx_id);
      return NULL;
   }

   vgpu_interop *interop = NULL;
   if (FAILED(dev->lpVtbl->QueryInterface(dev, &vgpu_IID_IDXGIVkInteropDevice1,
                                          (void **)&interop))) {
      TR_LOG("vgpu: open: no DXVK interop interface");
      return NULL;
   }

   D3D11_TEXTURE2D_DESC1 d;
   memset(&d, 0, sizeof(d));
   d.Width = desc->width;
   d.Height = desc->height;
   d.MipLevels = desc->mip_levels ? desc->mip_levels : 1;
   d.ArraySize = desc->array_size ? desc->array_size : 1;
   d.Format = (DXGI_FORMAT)desc->format;
   d.SampleDesc.Count = desc->sample_count ? desc->sample_count : 1;
   d.Usage = (D3D11_USAGE)desc->usage;
   d.BindFlags = desc->bind_flags;
   d.CPUAccessFlags = desc->cpu_access_flags;
   d.MiscFlags = desc->misc_flags;
   d.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;

   ID3D11Texture2D *tex = NULL;
   HRESULT hr = interop->lpVtbl->CreateTexture2DFromSharedHandle(
      interop, &d, (HANDLE)(uintptr_t)desc->modifier, &tex);
   interop->lpVtbl->Release(interop);
   if (FAILED(hr) || !tex) {
      TR_LOG("vgpu: open res_id=%u share=0x%llx %ux%u fmt=%u failed 0x%08lx", res_id,
             (unsigned long long)desc->modifier, desc->width, desc->height, desc->format, hr);
      return NULL;
   }
   return tex;
}

/* ---- framebuffer present ------------------------------------------------- */

/* Frames of the display primaries reach the host display through the KMD's
 * 2D framebuffer resource (VIOGPU_FB_PRESENT): read back the finished frame
 * into a CPU staging texture and let the KMD copy it.  dma-buf scanout of the
 * GPU primary itself reaches QEMU but not the egl-headless display on an
 * NVIDIA host.  Called under tritonPresentLock.
 *
 * Asynchronous by default: the present only queues the GPU copy into one of
 * VGPU_FB_RING persistently mapped readback textures and signals a private
 * fence; a worker thread waits for the fence and does the escape, so the
 * presenting thread (DWM) does not wait for the GPU or the CPU copy.  Frames
 * reach the KMD in present order. */
#define VGPU_FB_RING 3
struct vgpu_fb_job {
   UINT64 value; /* fence value signalled after the copy */
   LONG x0, y0, x1, y1;
   UINT slot;
   LARGE_INTEGER queued;
};
static struct {
   PTRITON_DEVICE dev;
   ID3D11Texture2D *staging; /* synchronous path */
   UINT width, height;
   DXGI_FORMAT format;
   bool need_full; /* staging recreated or the KMD asked for a full frame */
   /* asynchronous path */
   bool async, async_failed;
   ID3D11Texture2D *ring[VGPU_FB_RING];
   uint8_t *ptr[VGPU_FB_RING];
   UINT pitch[VGPU_FB_RING];
   bool busy[VGPU_FB_RING];
   UINT dropped; /* queued frames replaced by a newer one */
   ID3D11Fence *fence;
   UINT64 fence_value;
   HANDLE thread;
   CRITICAL_SECTION lock;
   bool lock_init;
   CONDITION_VARIABLE cv_job, cv_done;
   struct vgpu_fb_job jobs[VGPU_FB_RING];
   UINT head, count;
   bool stop;
   TRITON_KM km;
   volatile LONG worker_need_full;
} vgpu_fb;

/* HKLM\SOFTWARE\vgpu  Scanout (REG_SZ): "blob" leaves scanout primaries to the KMD's blob
 * scanout (SET_SCANOUT_BLOB of the shared dma-buf, no CPU copy); anything else or missing uses
 * the 2D framebuffer copy below, which works on every host.  Read once per process. */
static bool
vgpu_blob_scanout(void)
{
   static int mode = -1;
   if (mode < 0) {
      char v[16] = "";
      DWORD n = sizeof(v);
      mode = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\vgpu", "Scanout", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, v, &n) ==
                   ERROR_SUCCESS &&
                !_stricmp(v, "blob");
      TR_LOG("vgpu: scanout mode %s", mode ? "blob (GPU)" : "framebuffer copy");
   }
   return mode;
}

static UINT vgpu_stats_every(void);

/* Every vgpu_stats_every() presents: average share of the frame that was updated and the time
 * spent waiting for the readback (Map) and in the KMD escape. */
static void
vgpu_fb_stats(UINT64 area, UINT64 full, const LARGE_INTEGER t[3])
{
   static LARGE_INTEGER freq;
   static double map_us, esc_us;
   static UINT64 sum_area, sum_full;
   static UINT n;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   const double us = 1e6 / (double)freq.QuadPart;
   map_us += (double)(t[1].QuadPart - t[0].QuadPart) * us;
   esc_us += (double)(t[2].QuadPart - t[1].QuadPart) * us;
   sum_area += area;
   sum_full += full;
   if (++n == vgpu_stats_every()) {
      TR_LOG("vgpu: fb stats n=%u updated=%u%% avg us: copy+map=%u escape=%u", n,
             (UINT)(sum_area * 100 / (sum_full ? sum_full : 1)), (UINT)(map_us / n), (UINT)(esc_us / n));
      map_us = esc_us = 0;
      sum_area = sum_full = 0;
      n = 0;
   }
}

/* HKLM\SOFTWARE\vgpu FbAsync (REG_DWORD) = 0 turns the asynchronous framebuffer
 * present off.  Read once per process. */
static bool
vgpu_fb_async_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      DWORD v = 1, n = sizeof(v);
      if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\vgpu", "FbAsync", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                       NULL, &v, &n) != ERROR_SUCCESS)
         v = 1;
      enabled = v != 0;
   }
   return enabled;
}

/* Worker-side statistics, logged every vgpu_stats_every() frames: queue-to-ready
 * (GPU copy latency) and the escape. */
static void
vgpu_fb_async_stats(UINT64 area, UINT64 full, const LARGE_INTEGER *queued, const LARGE_INTEGER *ready,
                    const LARGE_INTEGER *done)
{
   static LARGE_INTEGER freq;
   static double wait_us, esc_us;
   static UINT64 sum_area, sum_full;
   static UINT n;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   const double us = 1e6 / (double)freq.QuadPart;
   wait_us += (double)(ready->QuadPart - queued->QuadPart) * us;
   esc_us += (double)(done->QuadPart - ready->QuadPart) * us;
   sum_area += area;
   sum_full += full;
   if (++n == vgpu_stats_every()) {
      TR_LOG("vgpu: fb async stats n=%u updated=%u%% avg us: gpu=%u escape=%u, dropped %u", n,
             (UINT)(sum_area * 100 / (sum_full ? sum_full : 1)), (UINT)(wait_us / n), (UINT)(esc_us / n),
             vgpu_fb.dropped);
      vgpu_fb.dropped = 0;
      wait_us = esc_us = 0;
      sum_area = sum_full = 0;
      n = 0;
   }
}

static DWORD WINAPI
vgpu_fb_worker(void *arg)
{
   (void)arg;
   HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
   EnterCriticalSection(&vgpu_fb.lock);
   for (;;) {
      while (!vgpu_fb.count && !vgpu_fb.stop)
         SleepConditionVariableCS(&vgpu_fb.cv_job, &vgpu_fb.lock, INFINITE);
      if (!vgpu_fb.count)
         break; /* stopping, queue drained */
      const struct vgpu_fb_job job = vgpu_fb.jobs[vgpu_fb.head];
      LeaveCriticalSection(&vgpu_fb.lock);

      /* wait for the readback: yield first, then sleep in 200 us steps; give up
       * after 2 s (the frame is then copied as it is) */
      const ULONGLONG deadline = GetTickCount64() + 2000;
      for (UINT spins = 0; ID3D11Fence_GetCompletedValue(vgpu_fb.fence) < job.value; spins++) {
         if (GetTickCount64() > deadline) {
            static LONG n;
            if (InterlockedIncrement(&n) <= 3)
               TR_LOG("vgpu: fb async: fence %llu not reached after 2 s", (unsigned long long)job.value);
            break;
         }
         if (spins < 32 || !timer) {
            SwitchToThread();
         } else {
            LARGE_INTEGER due = { .QuadPart = -2000 }; /* 200 us */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
               WaitForSingleObject(timer, INFINITE);
         }
      }
      LARGE_INTEGER ready, done;
      QueryPerformanceCounter(&ready);

      VIOGPU_ESCAPE esc;
      memset(&esc, 0, sizeof(esc));
      esc.Type = VIOGPU_FB_PRESENT;
      esc.DataLength = sizeof(esc.FbPresent);
      esc.FbPresent.Pixels = VioGpuUmPtr(vgpu_fb.ptr[job.slot] + (SIZE_T)job.y0 * vgpu_fb.pitch[job.slot] +
                                         (SIZE_T)job.x0 * 4);
      esc.FbPresent.Width = (ULONG)(job.x1 - job.x0);
      esc.FbPresent.Height = (ULONG)(job.y1 - job.y0);
      esc.FbPresent.Pitch = vgpu_fb.pitch[job.slot];
      esc.FbPresent.Format = vgpu_fb.format;
      esc.FbPresent.DstX = (ULONG)job.x0;
      esc.FbPresent.DstY = (ULONG)job.y0;
      HRESULT hr = tritonKmEscape(&vgpu_fb.km, &esc);
      if (FAILED(hr) || esc.FbPresent.NeedFull)
         InterlockedExchange(&vgpu_fb.worker_need_full, 1);
      QueryPerformanceCounter(&done);
      vgpu_fb_async_stats((UINT64)(job.x1 - job.x0) * (UINT64)(job.y1 - job.y0),
                          (UINT64)vgpu_fb.width * vgpu_fb.height, &job.queued, &ready, &done);

      EnterCriticalSection(&vgpu_fb.lock);
      vgpu_fb.busy[job.slot] = false;
      vgpu_fb.head = (vgpu_fb.head + 1) % VGPU_FB_RING;
      vgpu_fb.count--;
      WakeAllConditionVariable(&vgpu_fb.cv_done);
   }
   LeaveCriticalSection(&vgpu_fb.lock);
   if (timer)
      CloseHandle(timer);
   return 0;
}

/* Stops the worker after it drained its queue and frees everything. */
static void
vgpu_fb_reset(void)
{
   if (vgpu_fb.thread) {
      EnterCriticalSection(&vgpu_fb.lock);
      vgpu_fb.stop = true;
      WakeAllConditionVariable(&vgpu_fb.cv_job);
      LeaveCriticalSection(&vgpu_fb.lock);
      WaitForSingleObject(vgpu_fb.thread, INFINITE);
      CloseHandle(vgpu_fb.thread);
   }
   for (UINT i = 0; i < VGPU_FB_RING; i++)
      if (vgpu_fb.ring[i])
         ID3D11Texture2D_Release(vgpu_fb.ring[i]);
   if (vgpu_fb.fence)
      ID3D11Fence_Release(vgpu_fb.fence);
   if (vgpu_fb.staging)
      ID3D11Texture2D_Release(vgpu_fb.staging);
   const bool lock_init = vgpu_fb.lock_init;
   const bool async_failed = vgpu_fb.async_failed;
   CRITICAL_SECTION lock = vgpu_fb.lock;
   memset(&vgpu_fb, 0, sizeof(vgpu_fb));
   vgpu_fb.lock = lock;
   vgpu_fb.lock_init = lock_init;
   vgpu_fb.async_failed = async_failed;
}

/* Called from DestroyDevice before the device's state is torn down. */
void
vgpuFbDeviceDestroy(PTRITON_DEVICE pD)
{
   if (vgpu_fb.dev == pD)
      vgpu_fb_reset();
}

/* Ring textures (persistently mapped: the pointer of one blocking Map at
 * creation stays valid), private fence and worker thread. */
static bool
vgpu_fb_async_setup(PTRITON_DEVICE pD, const D3D11_TEXTURE2D_DESC *sd)
{
   if (vgpu_fb.async_failed || !pD->pCtx4)
      return false;
   ID3D11Device5 *dev5 = NULL;
   if (FAILED(ID3D11Device1_QueryInterface(pD->pDev1, &IID_ID3D11Device5, (void **)&dev5)))
      goto fail;
   HRESULT hr = ID3D11Device5_CreateFence(dev5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence, (void **)&vgpu_fb.fence);
   ID3D11Device5_Release(dev5);
   if (FAILED(hr)) {
      vgpu_fb.fence = NULL;
      goto fail;
   }
   for (UINT i = 0; i < VGPU_FB_RING; i++) {
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(ID3D11Device1_CreateTexture2D(pD->pDev1, sd, NULL, &vgpu_fb.ring[i]))) {
         vgpu_fb.ring[i] = NULL;
         goto fail;
      }
      if (FAILED(ID3D11DeviceContext1_Map(pD->pCtx1, (ID3D11Resource *)vgpu_fb.ring[i], 0, D3D11_MAP_READ, 0, &m)))
         goto fail;
      vgpu_fb.ptr[i] = m.pData;
      vgpu_fb.pitch[i] = m.RowPitch;
      ID3D11DeviceContext1_Unmap(pD->pCtx1, (ID3D11Resource *)vgpu_fb.ring[i], 0);
   }
   if (!vgpu_fb.lock_init) {
      InitializeCriticalSection(&vgpu_fb.lock);
      vgpu_fb.lock_init = true;
   }
   InitializeConditionVariable(&vgpu_fb.cv_job);
   InitializeConditionVariable(&vgpu_fb.cv_done);
   vgpu_fb.km = pD->Km;
   vgpu_fb.thread = CreateThread(NULL, 0, vgpu_fb_worker, NULL, 0, NULL);
   if (!vgpu_fb.thread)
      goto fail;
   SetThreadPriority(vgpu_fb.thread, THREAD_PRIORITY_ABOVE_NORMAL);
   TR_LOG("vgpu: fb presents asynchronous (%u slots, %ux%u)", VGPU_FB_RING, sd->Width, sd->Height);
   return true;

fail:
   TR_LOG("vgpu: fb async setup failed; synchronous fb presents");
   for (UINT i = 0; i < VGPU_FB_RING; i++) {
      if (vgpu_fb.ring[i])
         ID3D11Texture2D_Release(vgpu_fb.ring[i]);
      vgpu_fb.ring[i] = NULL;
   }
   if (vgpu_fb.fence)
      ID3D11Fence_Release(vgpu_fb.fence);
   vgpu_fb.fence = NULL;
   vgpu_fb.async_failed = true;
   return false;
}

/* Queues the readback of the box and hands it to the worker.  Takes a free
 * slot; with none free, a queued job the worker has not started whose box lies
 * inside this one is dropped and its slot reused (the newer frame covers it),
 * like a sync-interval-0 flip queue.  Otherwise it waits for the worker.
 * FALSE: present synchronously (no slot within 1 s). */
static bool
vgpu_fb_async_present(PTRITON_DEVICE pD, PTRITON_RESOURCE src, LONG x0, LONG y0, LONG x1, LONG y1)
{
   UINT slot = VGPU_FB_RING;
   EnterCriticalSection(&vgpu_fb.lock);
   for (;;) {
      for (UINT i = 0; i < VGPU_FB_RING && slot == VGPU_FB_RING; i++)
         if (!vgpu_fb.busy[i])
            slot = i;
      if (slot == VGPU_FB_RING && vgpu_fb.count >= 2) {
         /* the head job may be in progress; the newest queued one is not */
         struct vgpu_fb_job *last = &vgpu_fb.jobs[(vgpu_fb.head + vgpu_fb.count - 1) % VGPU_FB_RING];
         if (last->x0 >= x0 && last->y0 >= y0 && last->x1 <= x1 && last->y1 <= y1) {
            slot = last->slot; /* stays busy: now owned by this present */
            vgpu_fb.count--;
            vgpu_fb.dropped++;
         }
      }
      if (slot != VGPU_FB_RING)
         break;
      if (!SleepConditionVariableCS(&vgpu_fb.cv_done, &vgpu_fb.lock, 1000)) {
         LeaveCriticalSection(&vgpu_fb.lock);
         TR_LOG("vgpu: fb async: no free slot after 1 s");
         return false;
      }
   }
   vgpu_fb.busy[slot] = true;
   LeaveCriticalSection(&vgpu_fb.lock);

   struct vgpu_fb_job job = { .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .slot = slot };
   QueryPerformanceCounter(&job.queued);
   D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, (ID3D11Resource *)vgpu_fb.ring[slot], 0, (UINT)x0,
                                              (UINT)y0, 0, src->pResource, 0, &box);
   job.value = ++vgpu_fb.fence_value;
   ID3D11DeviceContext4_Signal(pD->pCtx4, vgpu_fb.fence, job.value);
   ID3D11DeviceContext1_Flush(pD->pCtx1);

   EnterCriticalSection(&vgpu_fb.lock);
   vgpu_fb.jobs[(vgpu_fb.head + vgpu_fb.count) % VGPU_FB_RING] = job;
   vgpu_fb.count++;
   WakeConditionVariable(&vgpu_fb.cv_job);
   LeaveCriticalSection(&vgpu_fb.lock);
   vgpu_fb.need_full = false;
   return true;
}

/* Copies the changed part of the frame (the bounding box of the DXGI dirty rects, or the whole
 * frame) into the KMD framebuffer: only that box is read back, copied and uploaded to the host. */
void
vgpuFbPresent(PTRITON_DEVICE pD, PTRITON_RESOURCE src, const RECT *rects, UINT nrects)
{
   if (!src->pResource || !pD->pDev1 || !pD->pCtx1 || vgpu_blob_scanout())
      return;

   ID3D11Texture2D *tex = (ID3D11Texture2D *)src->pResource;
   D3D11_TEXTURE2D_DESC d;
   ID3D11Texture2D_GetDesc(tex, &d);
   if (d.SampleDesc.Count != 1)
      return;

   if (vgpu_fb.dev != pD || vgpu_fb.width != d.Width || vgpu_fb.height != d.Height ||
       vgpu_fb.format != d.Format) {
      vgpu_fb_reset();
      D3D11_TEXTURE2D_DESC sd = d;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      HRESULT hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &sd, NULL, &vgpu_fb.staging);
      if (FAILED(hr)) {
         TR_LOG("vgpu: fb present: staging %ux%u fmt=%u failed 0x%08lx", d.Width, d.Height,
                d.Format, hr);
         vgpu_fb.staging = NULL;
         return;
      }
      vgpu_fb.dev = pD;
      vgpu_fb.width = d.Width;
      vgpu_fb.height = d.Height;
      vgpu_fb.format = d.Format;
      vgpu_fb.need_full = true;
      vgpu_fb.async = vgpu_fb_async_enabled() && vgpu_fb_async_setup(pD, &sd);
   }
   if (InterlockedExchange(&vgpu_fb.worker_need_full, 0))
      vgpu_fb.need_full = true;

   /* bounding box of the dirty rects, clipped to the frame; whole frame when unknown */
   LONG x0 = 0, y0 = 0, x1 = (LONG)d.Width, y1 = (LONG)d.Height;
   if (rects && nrects && !vgpu_fb.need_full) {
      x0 = (LONG)d.Width, y0 = (LONG)d.Height, x1 = 0, y1 = 0;
      for (UINT i = 0; i < nrects; i++) {
         x0 = min(x0, max(rects[i].left, 0L));
         y0 = min(y0, max(rects[i].top, 0L));
         x1 = max(x1, min(rects[i].right, (LONG)d.Width));
         y1 = max(y1, min(rects[i].bottom, (LONG)d.Height));
      }
      if (x1 <= x0 || y1 <= y0)
         return; /* nothing visible changed */
   }

   if (vgpu_fb.async && vgpu_fb_async_present(pD, src, x0, y0, x1, y1))
      return;

   LARGE_INTEGER t[3];
   QueryPerformanceCounter(&t[0]);
   D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, (ID3D11Resource *)vgpu_fb.staging, 0,
                                              (UINT)x0, (UINT)y0, 0, src->pResource, 0, &box);
   D3D11_MAPPED_SUBRESOURCE m;
   HRESULT hr = ID3D11DeviceContext1_Map(pD->pCtx1, (ID3D11Resource *)vgpu_fb.staging, 0,
                                         D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) {
      TR_LOG("vgpu: fb present: map failed 0x%08lx", hr);
      return;
   }

   QueryPerformanceCounter(&t[1]);

   VIOGPU_ESCAPE esc;
   memset(&esc, 0, sizeof(esc));
   esc.Type = VIOGPU_FB_PRESENT;
   esc.DataLength = sizeof(esc.FbPresent);
   esc.FbPresent.Pixels = VioGpuUmPtr((const uint8_t *)m.pData + (SIZE_T)y0 * m.RowPitch + (SIZE_T)x0 * 4);
   esc.FbPresent.Width = (ULONG)(x1 - x0);
   esc.FbPresent.Height = (ULONG)(y1 - y0);
   esc.FbPresent.Pitch = m.RowPitch;
   esc.FbPresent.Format = d.Format;
   esc.FbPresent.DstX = (ULONG)x0;
   esc.FbPresent.DstY = (ULONG)y0;
   hr = tritonKmEscape(&pD->Km, &esc);
   ID3D11DeviceContext1_Unmap(pD->pCtx1, (ID3D11Resource *)vgpu_fb.staging, 0);
   QueryPerformanceCounter(&t[2]);
   vgpu_fb.need_full = FAILED(hr) || esc.FbPresent.NeedFull;
   vgpu_fb_stats((UINT64)(x1 - x0) * (UINT64)(y1 - y0), (UINT64)d.Width * d.Height, t);

   static LONG failures;
   if (FAILED(hr) && InterlockedIncrement(&failures) <= 3)
      TR_LOG("vgpu: fb present escape failed 0x%08lx (%ux%u)", hr, d.Width, d.Height);
}

/* ---- blt-model windowed presents ---------------------------------------- */

/* A blt-model present reaches DWM through the KMD: GenerateBltPresent copies
 * the source allocation into DWM's redirection surface (a guest-memory
 * "coherent" virgl 3D resource) with a virgl copy on the host.  That copy
 * cannot read Venus memory, so present from a coherent virgl 3D allocation
 * instead, filled on the CPU from the finished frame: the KMD then emits
 * TRANSFER_TO_HOST + copy, the path ordinary windows already use.
 * Called under tritonPresentLock. */

void
vgpuBltSourceRelease(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
   if (r->pVgpuStaging) {
      ID3D11Texture2D_Release(r->pVgpuStaging);
      r->pVgpuStaging = NULL;
   }
   if (r->pVgpuStagingPrev) {
      ID3D11Texture2D_Release(r->pVgpuStagingPrev);
      r->pVgpuStagingPrev = NULL;
   }
   r->vgpuPrevValid = FALSE;
   r->vgpuFastRun = 0;
   r->vgpuLastBltQpc = 0;
   if (r->hVgpuCoherent && pD && pD->KTCallbacks.pfnDeallocateCb) {
      D3DDDICB_DEALLOCATE da;
      memset(&da, 0, sizeof(da));
      da.NumAllocations = 1;
      da.HandleList = &r->hVgpuCoherent;
      pD->KTCallbacks.pfnDeallocateCb(pD->hRTDevice.handle, &da);
   }
   r->hVgpuCoherent = 0;
   r->vgpuCoherentW = r->vgpuCoherentH = 0;
}

static bool
vgpu_is_rgba8(DXGI_FORMAT f)
{
   return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
          f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

static bool
vgpu_is_bgra8(DXGI_FORMAT f)
{
   return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
          f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8X8_UNORM ||
          f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}

/* VGPU_STATS_EVERY=<n> overrides the 600-frame interval of both statistics. */
static UINT
vgpu_stats_every(void)
{
   static UINT every;
   if (!every) {
      const char *e = getenv("VGPU_STATS_EVERY");
      every = (e && atoi(e) > 1) ? (UINT)atoi(e) : 600;
   }
   return every;
}

/* Time spent after the GPU gate: the framebuffer copy (scanout presents) and
 * the runtime's present callback, where dxgkrnl throttles the caller; logged
 * every 300 presents of each kind. */
void
vgpuPresentTailStats(BOOL windowed, const LARGE_INTEGER *t1, const LARGE_INTEGER *t2, const LARGE_INTEGER *t3)
{
   static LARGE_INTEGER freq;
   static double fb[2], cb[2], cbmax[2];
   static UINT n[2];
   const int k = windowed ? 1 : 0;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   const double us = 1e6 / (double)freq.QuadPart;
   const double c = (double)(t3->QuadPart - t2->QuadPart) * us;
   fb[k] += (double)(t2->QuadPart - t1->QuadPart) * us;
   cb[k] += c;
   if (c > cbmax[k])
      cbmax[k] = c;
   if (++n[k] == 300) {
      TR_LOG("vgpu: %s present tail n=300 avg us: fb=%u cb=%u (max %u)", windowed ? "windowed" : "scanout",
             (UINT)(fb[k] / 300), (UINT)(cb[k] / 300), (UINT)cbmax[k]);
      fb[k] = cb[k] = cbmax[k] = 0;
      n[k] = 0;
   }
}

/* Per-process present statistics, logged every 600 windowed presents: count by
 * DXGI present flags, average FlushAndGate (GPU wait) time and the gap between
 * presents. */
void
vgpuPresentStats(UINT flags, BOOL windowed, const LARGE_INTEGER *t0, const LARGE_INTEGER *t1)
{
   static LARGE_INTEGER freq, last;
   static double gate, gap;
   static UINT n, blt, flip, other;
   if (!windowed) {
      /* scanout presents (DWM): own counters, logged every 300 */
      static LARGE_INTEGER plast;
      static double pgate, pgap;
      static UINT pn;
      if (!freq.QuadPart)
         QueryPerformanceFrequency(&freq);
      const double pus = 1e6 / (double)freq.QuadPart;
      pgate += (double)(t1->QuadPart - t0->QuadPart) * pus;
      if (plast.QuadPart)
         pgap += (double)(t0->QuadPart - plast.QuadPart) * pus;
      plast = *t1;
      if (++pn == 300) {
         TR_LOG("vgpu: scanout present stats n=%u avg us: gate=%u gap=%u (%.1f fps)", pn, (UINT)(pgate / pn),
                (UINT)(pgap / (pn - 1)), 1e6 * (pn - 1) / (pgap ? pgap : 1));
         pgate = pgap = 0;
         pn = 0;
      }
      return;
   }
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   const double us = 1e6 / (double)freq.QuadPart;
   gate += (double)(t1->QuadPart - t0->QuadPart) * us;
   if (last.QuadPart)
      gap += (double)(t0->QuadPart - last.QuadPart) * us;
   last = *t1;
   if (flags & 0x1)
      blt++;
   else if (flags & 0x2)
      flip++;
   else
      other++;
   if (++n == vgpu_stats_every()) {
      TR_LOG("vgpu: present stats n=%u blt=%u flip=%u other=%u avg us: gate=%u gap=%u", n, blt, flip, other,
             (UINT)(gate / n), (UINT)(gap / (n - 1)));
      gate = gap = 0;
      n = blt = flip = other = 0;
   }
}

/* VGPU_BLT_STATS: per-process averages of the blt-source stages, every 600 frames.
 * wait = GPU copy + Map (stall), lock = pfnLockCb, copy = CPU row copy,
 * unlock = Unlock + Unmap, gap = time between successive blt presents. */
static void
vgpu_blt_stats(UINT w, UINT h, const LARGE_INTEGER bt[5])
{
   static LARGE_INTEGER freq, last;
   static double acc[5];
   static UINT n;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   const double ms = 1000.0 / (double)freq.QuadPart;
   for (int i = 0; i < 4; i++)
      acc[i] += (double)(bt[i + 1].QuadPart - bt[i].QuadPart) * ms;
   if (last.QuadPart)
      acc[4] += (double)(bt[0].QuadPart - last.QuadPart) * ms;
   last = bt[4];
   if (++n == vgpu_stats_every()) {
      /* integer microseconds: TR_LOG has no floating-point formatting */
      TR_LOG("vgpu: blt stats %ux%u avg us: wait=%u lock=%u copy=%u unlock=%u gap=%u",
             w, h, (UINT)(acc[0] * 1000 / n), (UINT)(acc[1] * 1000 / n), (UINT)(acc[2] * 1000 / n),
             (UINT)(acc[3] * 1000 / n), (UINT)(acc[4] * 1000 / (n - 1)));
      memset(acc, 0, sizeof(acc));
      n = 0;
   }
}

/* Pipelined synchronous blt presents (see vgpuBltSource); on unless
 * HKLM\SOFTWARE\vgpu BltPipeline (REG_DWORD) = 0. */
static bool
vgpu_blt_pipeline(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      DWORD v = 1, n = sizeof(v);
      if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\vgpu", "BltPipeline", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                       NULL, &v, &n) != ERROR_SUCCESS)
         v = 1;
      enabled = v != 0;
      TR_LOG("vgpu: blt presents %s", enabled ? "pipelined" : "not pipelined");
   }
   return enabled;
}

D3DKMT_HANDLE
vgpuBltSource(PTRITON_DEVICE pD, PTRITON_RESOURCE src, BOOL *pAsync)
{
   *pAsync = FALSE;
   /* the companion holds the resolved / single-subresource copy of the frame */
   ID3D11Resource *copy_src = src->pPresentCompanion ? src->pPresentCompanion : src->pResource;
   if (!copy_src || !pD->pDev1 || !pD->pCtx1 || !pD->KTCallbacks.pfnAllocateCb ||
       !pD->KTCallbacks.pfnLockCb || !pD->KTCallbacks.pfnUnlockCb)
      return 0;

   D3D11_TEXTURE2D_DESC d;
   ID3D11Texture2D_GetDesc((ID3D11Texture2D *)copy_src, &d);
   const bool swizzle = vgpu_is_rgba8(d.Format);
   if (d.SampleDesc.Count != 1 || (!swizzle && !vgpu_is_bgra8(d.Format))) {
      static LONG n;
      if (InterlockedIncrement(&n) <= 3)
         TR_LOG("vgpu: blt source: unsupported fmt=%u samples=%u", (UINT)d.Format, d.SampleDesc.Count);
      return 0;
   }

   if (src->vgpuCoherentW != d.Width || src->vgpuCoherentH != d.Height || !src->hVgpuCoherent) {
      vgpuBltSourceRelease(pD, src);

      /* The trailing lookup cookie makes the KMD's DxgkDdiOpenAllocation
       * resolve THIS allocation; without one it falls back to the KMT handle
       * map, where a stale entry with the same handle value (another
       * process's allocation) wins and the blt reads the wrong surface.
       * Bit 31 keeps these apart from Venus' per-process cookie counter. */
      static volatile LONG cookie_counter;
      VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX axe;
      memset(&axe, 0, sizeof(axe));
      axe.LookupCookie = ((ULONGLONG)GetCurrentProcessId() << 32) | 0x80000000u |
                         (UINT)(InterlockedIncrement(&cookie_counter) & 0x7fffffff);
      VIOGPU_CREATE_ALLOCATION_EXCHANGE *ax = &axe.Base;
      ax->Type = VIOGPU_RESOURCE_TYPE_3D;
      ax->Options3D.target = 2; /* PIPE_TEXTURE_2D */
      /* identical to the KMD's own coherent standard allocations (staging /
       * shadow surfaces), which the host accepts for transfers and copies */
      ax->Options3D.format = VIRGL_FORMAT_B8G8R8X8_UNORM;
      ax->Options3D.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                          VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT;
      ax->Options3D.width = d.Width;
      ax->Options3D.height = d.Height;
      ax->Options3D.depth = 1;
      ax->Options3D.array_size = 1;
      ax->Options3D.flags = VIRGL_RESOURCE_FLAG_MAP_COHERENT;
      ax->Size = (ULONGLONG)d.Width * d.Height * 4;

      D3DDDI_ALLOCATIONINFO2 ai;
      memset(&ai, 0, sizeof(ai));
      ai.pPrivateDriverData = &axe;
      ai.PrivateDriverDataSize = sizeof(axe);
      D3DDDICB_ALLOCATE cb;
      memset(&cb, 0, sizeof(cb));
      cb.NumAllocations = 1;
      cb.pAllocationInfo2 = &ai;
      HRESULT hr = pD->KTCallbacks.pfnAllocateCb(pD->hRTDevice.handle, &cb);
      if (FAILED(hr) || !ai.hAllocation) {
         static LONG n;
         if (InterlockedIncrement(&n) <= 3)
            TR_LOG("vgpu: blt source: coherent %ux%u allocation failed 0x%08lx", d.Width, d.Height, hr);
         return 0;
      }
      tritonKmRequestResidency(&pD->Km, ai.hAllocation);
      /* GpuMmu: the KMD attaches a TYPE_3D allocation's guest pages to the
       * host resource only from UPDATE_PAGE_TABLE, i.e. once it has a GPU VA.
       * Without one the host resource has no backing and every TRANSFER of
       * the blt present fails ("Illegal resource"). */
      if (pD->KTCallbacks.pfnMapGpuVirtualAddressCb && pD->Km.hPagingQueue) {
         D3DDDI_MAPGPUVIRTUALADDRESS mv;
         memset(&mv, 0, sizeof(mv));
         mv.hPagingQueue = pD->Km.hPagingQueue;
         mv.hAllocation = ai.hAllocation;
         mv.SizeInPages = (ax->Size + 4095) / 4096;
         mv.Protection.Write = 1;
         HRESULT mhr = pD->KTCallbacks.pfnMapGpuVirtualAddressCb(pD->hRTDevice.handle, &mv);
         if (mhr == E_PENDING && pD->Km.PagingFenceVa) {
            const ULONGLONG deadline = GetTickCount64() + 5000;
            while (*pD->Km.PagingFenceVa < mv.PagingFenceValue && GetTickCount64() < deadline)
               Sleep(0);
         }
         TR_LOG("vgpu: blt source: alloc=0x%x %ux%u map hr=0x%08lx va=0x%llx",
                ai.hAllocation, d.Width, d.Height, mhr, (unsigned long long)mv.VirtualAddress);
      } else {
         TR_LOG("vgpu: blt source: no GPU VA map (cb=%p queue=0x%x)",
                pD->KTCallbacks.pfnMapGpuVirtualAddressCb, pD->Km.hPagingQueue);
      }

      D3D11_TEXTURE2D_DESC sd = d;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      if (FAILED(ID3D11Device1_CreateTexture2D(pD->pDev1, &sd, NULL, &src->pVgpuStaging)) ||
          FAILED(ID3D11Device1_CreateTexture2D(pD->pDev1, &sd, NULL, &src->pVgpuStagingPrev))) {
         if (src->pVgpuStaging)
            ID3D11Texture2D_Release(src->pVgpuStaging);
         src->pVgpuStaging = NULL;
         src->pVgpuStagingPrev = NULL;
         D3DDDICB_DEALLOCATE da;
         memset(&da, 0, sizeof(da));
         da.NumAllocations = 1;
         da.HandleList = &ai.hAllocation;
         pD->KTCallbacks.pfnDeallocateCb(pD->hRTDevice.handle, &da);
         return 0;
      }
      src->hVgpuCoherent = ai.hAllocation;
      src->vgpuCoherentW = d.Width;
      src->vgpuCoherentH = d.Height;
   }

   LARGE_INTEGER bt[5];
   QueryPerformanceCounter(&bt[0]);
   /* Pipelining: after a run of presents less than 50 ms apart, ship the
    * previous frame, whose copy was flushed a present ago and has finished,
    * instead of waiting for this one; the frame is shown one present late.
    * Isolated presents (UI updates) keep waiting, so only the last frame of
    * an animation can be one frame behind until the window presents again. */
   static LARGE_INTEGER qpf;
   if (!qpf.QuadPart)
      QueryPerformanceFrequency(&qpf);
   const bool fast = src->vgpuLastBltQpc && bt[0].QuadPart - src->vgpuLastBltQpc < qpf.QuadPart / 20;
   src->vgpuFastRun = fast ? min(src->vgpuFastRun + 1, 1000u) : 0;
   src->vgpuLastBltQpc = bt[0].QuadPart;
   const bool pipelined = vgpu_blt_pipeline() && src->vgpuPrevValid && src->vgpuFastRun >= 8;
   ID3D11Texture2D *cur = src->pVgpuStaging, *prev = src->pVgpuStagingPrev;
   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, (ID3D11Resource *)cur, 0, 0, 0, 0, copy_src, 0, NULL);
   ID3D11Texture2D *readback = pipelined ? prev : cur;
   /* this frame's texture becomes the next present's previous frame */
   src->pVgpuStagingPrev = cur;
   src->pVgpuStaging = prev;
   src->vgpuPrevValid = TRUE;
   D3D11_MAPPED_SUBRESOURCE m;
   if (FAILED(ID3D11DeviceContext1_Map(pD->pCtx1, (ID3D11Resource *)readback, 0,
                                       D3D11_MAP_READ, 0, &m)))
      return 0;
   QueryPerformanceCounter(&bt[1]);

   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof(lock));
   lock.hAllocation = src->hVgpuCoherent;
   lock.Flags.WriteOnly = 1;
   HRESULT hr = pD->KTCallbacks.pfnLockCb(pD->hRTDevice.handle, &lock);
   QueryPerformanceCounter(&bt[2]);
   bt[3] = bt[2];
   D3DKMT_HANDLE ret = 0;
   if (FAILED(hr) || !lock.pData) {
      static LONG n;
      if (InterlockedIncrement(&n) <= 5)
         TR_LOG("vgpu: blt source: Lock(0x%x) failed 0x%08lx", src->hVgpuCoherent, hr);
   }
   if (SUCCEEDED(hr) && lock.pData) {
      const UINT row = d.Width * 4;
      for (UINT y = 0; y < d.Height; y++) {
         const uint8_t *s = (const uint8_t *)m.pData + (SIZE_T)y * m.RowPitch;
         uint8_t *t = (uint8_t *)lock.pData + (SIZE_T)y * row;
         if (!swizzle) {
            memcpy(t, s, row);
         } else {
            const uint32_t *s32 = (const uint32_t *)s;
            uint32_t *t32 = (uint32_t *)t;
            for (UINT x = 0; x < d.Width; x++) {
               const uint32_t p = s32[x]; /* RGBA in memory -> BGRA */
               t32[x] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
            }
         }
      }
      {
         static LONG n;
         if (d.Width >= 2000 && InterlockedIncrement(&n) <= 6) {
            const uint32_t *c = (const uint32_t *)((const uint8_t *)m.pData +
                                                   (SIZE_T)(d.Height / 2) * m.RowPitch) + d.Width / 2;
            const uint32_t *c0 = (const uint32_t *)m.pData;
            TR_LOG("vgpu: blt source %ux%u fmt=%u center=0x%08x corner=0x%08x alloc=0x%x",
                   d.Width, d.Height, (UINT)d.Format, *c, *c0, src->hVgpuCoherent);
         }
      }
      QueryPerformanceCounter(&bt[3]);
      D3DDDICB_UNLOCK unlock;
      memset(&unlock, 0, sizeof(unlock));
      unlock.NumAllocations = 1;
      unlock.phAllocations = &src->hVgpuCoherent;
      pD->KTCallbacks.pfnUnlockCb(pD->hRTDevice.handle, &unlock);
      ret = src->hVgpuCoherent;
   } else {
      static LONG n;
      if (InterlockedIncrement(&n) <= 3)
         TR_LOG("vgpu: blt source: lock failed 0x%08lx", hr);
   }
   ID3D11DeviceContext1_Unmap(pD->pCtx1, (ID3D11Resource *)readback, 0);
   QueryPerformanceCounter(&bt[4]);
   vgpu_blt_stats(d.Width, d.Height, bt);
   return ret;
}

/* ---- format tables (from npt_shared_texture.c) --------------------------- */

uint32_t
npt_shared_texture_host_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
   default:
      return dxgi_format;
   }
}

uint32_t
npt_shared_texture_virgl_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM: return VIRGL_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_B8G8R8X8_UNORM: return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM: return VIRGL_FORMAT_R8G8B8A8_UNORM;
   default:
      TR_LOG("vgpu: shared texture: unmapped DXGI format %u, assuming BGRA8", dxgi_format);
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   }
}

uint32_t
npt_shared_texture_virgl_copy_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return VIRGL_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return VIRGL_FORMAT_R10G10B10A2_UNORM;
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return VIRGL_FORMAT_R16G16B16A16_FLOAT;
   case DXGI_FORMAT_B5G6R5_UNORM:   return VIRGL_FORMAT_B5G6R5_UNORM;
   case DXGI_FORMAT_B5G5R5A1_UNORM: return VIRGL_FORMAT_B5G5R5A1_UNORM;
   case DXGI_FORMAT_B4G4R4A4_UNORM: return VIRGL_FORMAT_B4G4R4A4_UNORM;
   default:                         return 0;
   }
}
