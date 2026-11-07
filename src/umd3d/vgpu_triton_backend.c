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
         wcscat(path, L"dxvk_d3d11.dll");
         HMODULE dxvk = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
         if (dxvk)
            pfn = (PFN_D3D11_CREATE_DEVICE_DXVK)(void *)GetProcAddress(dxvk, "D3D11CreateDevice");
         else
            TR_LOG("vgpu: loading dxvk_d3d11.dll failed (%lu)", GetLastError());
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

HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                 REFIID riid, void **ppDevice)
{
   (void)pAdapter;
   (void)MinimumFeatureLevel;
   (void)riid;
   if (ppDevice)
      *ppDevice = NULL;
   return E_NOTIMPL; /* D3D12 is M3 (vkd3d-proton) */
}

/* ---- adapter probe ------------------------------------------------------- */

void
tritonSharedBridgeAdapterProbe(struct triton_adapter_probe *out)
{
   /* Only loaded for our adapter (INF UserModeDriverName); its KMD is the
    * WDDM2 viogpu3d.  No Neptune host capabilities. */
   *out = (struct triton_adapter_probe){
      .viogpu = true,
      .wddm2 = true,
      .host_ok = true,
      .caps_flags = 0,
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

bool
tritonSharedBridgeExportBlob(void *pResourceWrapper, struct triton_shared_texture_desc *desc)
{
   IUnknown *tex = pResourceWrapper;
   IDXGIResource *dxgi = NULL;
   if (FAILED(tex->lpVtbl->QueryInterface(tex, &vgpu_IID_IDXGIResource, (void **)&dxgi)))
      return false;
   HANDLE share = NULL;
   HRESULT hr = IDXGIResource_GetSharedHandle(dxgi, &share);
   IDXGIResource_Release(dxgi);
   if (FAILED(hr) || !share || share == INVALID_HANDLE_VALUE) {
      TR_LOG("vgpu: export: no shared handle (hr=0x%08lx)", hr);
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
   if (!vgpu_linear_layout(tex, &pitch, &offset))
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
 * NVIDIA host.  Called under tritonPresentLock. */
static struct {
   PTRITON_DEVICE dev;
   ID3D11Texture2D *staging;
   UINT width, height;
   DXGI_FORMAT format;
} vgpu_fb;

void
vgpuFbPresent(PTRITON_DEVICE pD, PTRITON_RESOURCE src)
{
   if (!src->pResource || !pD->pDev1 || !pD->pCtx1)
      return;

   ID3D11Texture2D *tex = (ID3D11Texture2D *)src->pResource;
   D3D11_TEXTURE2D_DESC d;
   ID3D11Texture2D_GetDesc(tex, &d);
   if (d.SampleDesc.Count != 1)
      return;

   if (vgpu_fb.dev != pD || vgpu_fb.width != d.Width || vgpu_fb.height != d.Height ||
       vgpu_fb.format != d.Format) {
      if (vgpu_fb.staging)
         ID3D11Texture2D_Release(vgpu_fb.staging);
      memset(&vgpu_fb, 0, sizeof(vgpu_fb));
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
   }

   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, (ID3D11Resource *)vgpu_fb.staging, 0, 0,
                                              0, 0, src->pResource, 0, NULL);
   D3D11_MAPPED_SUBRESOURCE m;
   HRESULT hr = ID3D11DeviceContext1_Map(pD->pCtx1, (ID3D11Resource *)vgpu_fb.staging, 0,
                                         D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) {
      TR_LOG("vgpu: fb present: map failed 0x%08lx", hr);
      return;
   }

   VIOGPU_ESCAPE esc;
   memset(&esc, 0, sizeof(esc));
   esc.Type = VIOGPU_FB_PRESENT;
   esc.DataLength = sizeof(esc.FbPresent);
   esc.FbPresent.Pixels = VioGpuUmPtr(m.pData);
   esc.FbPresent.Width = d.Width;
   esc.FbPresent.Height = d.Height;
   esc.FbPresent.Pitch = m.RowPitch;
   esc.FbPresent.Format = d.Format;
   hr = tritonKmEscape(&pD->Km, &esc);
   ID3D11DeviceContext1_Unmap(pD->pCtx1, (ID3D11Resource *)vgpu_fb.staging, 0);

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

D3DKMT_HANDLE
vgpuBltSource(PTRITON_DEVICE pD, PTRITON_RESOURCE src)
{
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
      if (FAILED(ID3D11Device1_CreateTexture2D(pD->pDev1, &sd, NULL, &src->pVgpuStaging))) {
         src->pVgpuStaging = NULL;
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

   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, (ID3D11Resource *)src->pVgpuStaging, 0,
                                              0, 0, 0, copy_src, 0, NULL);
   D3D11_MAPPED_SUBRESOURCE m;
   if (FAILED(ID3D11DeviceContext1_Map(pD->pCtx1, (ID3D11Resource *)src->pVgpuStaging, 0,
                                       D3D11_MAP_READ, 0, &m)))
      return 0;

   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof(lock));
   lock.hAllocation = src->hVgpuCoherent;
   lock.Flags.WriteOnly = 1;
   HRESULT hr = pD->KTCallbacks.pfnLockCb(pD->hRTDevice.handle, &lock);
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
   ID3D11DeviceContext1_Unmap(pD->pCtx1, (ID3D11Resource *)src->pVgpuStaging, 0);
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
