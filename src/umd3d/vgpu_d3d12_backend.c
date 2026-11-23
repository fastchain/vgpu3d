/*
 * vgpu D3D12 backend glue: the pieces of Triton's D3D12 DDI layer that the
 * upstream Neptune build gets from its wire protocol (host rings, the host's
 * root-signature serializer), answered in-process for the vkd3d-proton device
 * (see npt_d3d12_create_device_internal in vgpu_triton_backend.c).
 *
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#include "triton12.h"
#include "triton_log.h"
#include "vgpu_blt.h"

struct npt_ring;

/* No wire rings in-process: vkd3d-proton orders its own queues, so the
 * tile-mapping edges Triton keeps across rings have nothing to name. */
struct npt_ring *
npt_com_object_ring_seqno(void *self, UINT32 *out_seqno)
{
   (void)self;
   if (out_seqno)
      *out_seqno = 0;
   return NULL;
}

void
npt_ring_order_after_ring(struct npt_ring *ring, const struct npt_ring *target, UINT32 seqno)
{
   (void)ring;
   (void)target;
   (void)seqno;
}

/* Kernel gate tokens are retired by the Neptune host; the in-process queue
 * gate waits on the drain fence instead (triton12QueueGate). */
BOOL
npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value, UINT64 *kmd_token_out)
{
   (void)fence_wrapper;
   (void)value;
   if (kmd_token_out)
      *kmd_token_out = 0;
   return FALSE;
}

/* vkd3d-proton creates row-major (linear) textures only as buffers. */
BOOL
npt_d3d12_host_takes_row_major_texture(void *device_wrapper)
{
   (void)device_wrapper;
   return FALSE;
}

typedef HRESULT(WINAPI *PFN_SERIALIZE_VERSIONED_VGPU)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **,
                                                      ID3DBlob **);

/* The runtime's own serializer: this UMD always runs inside a process that
 * loaded d3d12.dll. */
HRESULT __stdcall
npt_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
                                         ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob)
{
   static PFN_SERIALIZE_VERSIONED_VGPU pfn;
   if (!pfn) {
      HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
      if (!d3d12)
         d3d12 = LoadLibraryW(L"d3d12.dll");
      if (d3d12)
         pfn = (PFN_SERIALIZE_VERSIONED_VGPU)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
   }
   if (!pfn) {
      TR_LOG("vgpu: D3D12SerializeVersionedRootSignature unavailable");
      return E_NOTIMPL;
   }
   return pfn(pRootSignature, ppBlob, ppErrorBlob);
}

struct vgpu_blt_frame {
   struct vgpu_blt_frame *next;
   struct vgpu_blt_frame *failedNext;
   LONG heldAfterFailure;
   LONG refs;
   D3DKMT_HANDLE allocation;
   ID3D12Resource *readback;
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
   UINT64 capacity;
   UINT rowBytes;
   BOOL published;
};

static ID3D12Resource *
vgpu_take_readback(PTRITON12_DEVICE dev, UINT64 size, UINT64 *capacity)
{
   ID3D12Resource *buffer = NULL;
   UINT best = ARRAYSIZE(dev->idleReadbacks);
   EnterCriticalSection(&dev->QueueLock);
   for (UINT i = 0; i < ARRAYSIZE(dev->idleReadbacks); ++i) {
      if (dev->idleReadbacks[i].buffer && dev->idleReadbacks[i].capacity >= size &&
          (best == ARRAYSIZE(dev->idleReadbacks) ||
           dev->idleReadbacks[i].capacity < dev->idleReadbacks[best].capacity))
         best = i;
   }
   if (best < ARRAYSIZE(dev->idleReadbacks)) {
      buffer = dev->idleReadbacks[best].buffer;
      *capacity = dev->idleReadbacks[best].capacity;
      dev->idleReadbacks[best].buffer = NULL;
   }
   LeaveCriticalSection(&dev->QueueLock);
   return buffer;
}

static void
vgpu_retire_readback(PTRITON12_DEVICE dev, struct vgpu_blt_frame *frame)
{
   ID3D12Resource *release = frame->readback;
   if (release && frame->published && !frame->heldAfterFailure && frame->capacity <= 32ull * 1024 * 1024) {
      UINT i;
      EnterCriticalSection(&dev->QueueLock);
      for (i = 0; i < ARRAYSIZE(dev->idleReadbacks); ++i)
         if (!dev->idleReadbacks[i].buffer)
            break;
      if (i == ARRAYSIZE(dev->idleReadbacks))
         i = dev->nextReadbackEviction++ % ARRAYSIZE(dev->idleReadbacks);
      release = dev->idleReadbacks[i].buffer;
      dev->idleReadbacks[i].buffer = frame->readback;
      dev->idleReadbacks[i].capacity = frame->capacity;
      LeaveCriticalSection(&dev->QueueLock);
   }
   if (release)
      ID3D12Resource_Release(release);
}

void
vgpuDestroyReadbacks(PTRITON12_DEVICE dev)
{
   for (UINT i = 0; i < ARRAYSIZE(dev->idleReadbacks); ++i)
      if (dev->idleReadbacks[i].buffer)
         ID3D12Resource_Release(dev->idleReadbacks[i].buffer);
}

/* The list owns the head and each frame owns its immutable successor. A
 * submitted batch retains its chain across list reset until the queue drain
 * and CPU copy finish. */
void
vgpuReleaseBlt(PTRITON12_DEVICE dev, struct vgpu_blt_frame *frame)
{
   while (frame && !InterlockedDecrement(&frame->refs)) {
      struct vgpu_blt_frame *next = frame->next;
      vgpu_retire_readback(dev, frame);
      triton12DeallocateKM(dev, &frame->allocation);
      HeapFree(GetProcessHeap(), 0, frame);
      frame = next;
   }
}

struct vgpu_blt_frame *
vgpuAcquireBlts(PTRITON12_LIST list)
{
   EnterCriticalSection(&list->pDev->QueueLock);
   struct vgpu_blt_frame *frames = list->blts;
   if (frames)
      InterlockedIncrement(&frames->refs);
   LeaveCriticalSection(&list->pDev->QueueLock);
   return frames;
}

void
vgpuDiscardBlts(PTRITON12_LIST list)
{
   EnterCriticalSection(&list->pDev->QueueLock);
   struct vgpu_blt_frame *frames = list->blts;
   list->blts = NULL;
   LeaveCriticalSection(&list->pDev->QueueLock);
   vgpuReleaseBlt(list->pDev, frames);
}

/* A failed drain can leave the host reading command-list resources after a
 * runtime reset. Retain the frame through native queue teardown in that case. */
void
vgpuHoldFailedBlts(PTRITON12_QUEUE queue, struct vgpu_blt_frame *frames)
{
   AcquireSRWLockExclusive(&queue->DrainLock);
   for (struct vgpu_blt_frame *f = frames; f; f = f->next) {
      if (InterlockedExchange(&f->heldAfterFailure, 1))
         continue;
      InterlockedIncrement(&f->refs);
      f->failedNext = queue->failedBlts;
      queue->failedBlts = f;
   }
   ReleaseSRWLockExclusive(&queue->DrainLock);
}

void
vgpuReleaseFailedBlts(PTRITON12_QUEUE queue)
{
   while (queue->failedBlts) {
      struct vgpu_blt_frame *f = queue->failedBlts;
      queue->failedBlts = f->failedNext;
      vgpuReleaseBlt(queue->pDev, f);
   }
}

static HRESULT
vgpu_allocate_blt_source(PTRITON12_DEVICE dev, UINT width, UINT height, ULONGLONG cookie,
                         D3DKMT_HANDLE *handle)
{
   VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX exchange = {0};
   vgpuInitBltAllocation(&exchange.Base, width, height);
   exchange.LookupCookie = cookie;
   D3D12DDI_ALLOCATION_INFO_0022 info = {0};
   info.pPrivateDriverData = &exchange;
   info.PrivateDriverDataSize = sizeof(exchange);
   D3D12DDICB_ALLOCATE_0022 alloc = {0};
   alloc.NumAllocations = 1;
   alloc.pAllocationInfo = &info;
   HRESULT hr = dev->pUMCallbacks->pfnAllocateCb(dev->hRTDevice, &alloc);
   if (FAILED(hr) || !info.hAllocation)
      return FAILED(hr) ? hr : E_FAIL;
   *handle = info.hAllocation;
   hr = tritonKmRequestResidency(&dev->Km, *handle);
   if (FAILED(hr))
      return hr;

   D3DDDI_MAPGPUVIRTUALADDRESS map = {0};
   map.hPagingQueue = dev->Km.hPagingQueue;
   map.hAllocation = *handle;
   map.SizeInPages = (exchange.Base.Size + 4095) / 4096;
   map.Protection.Write = 1;
   hr = dev->KTCallbacks.pfnMapGpuVirtualAddressCb(dev->Km.hRTDevice, &map);
   hr = tritonKmWaitPaging(&dev->Km, hr, map.PagingFenceValue);
   return SUCCEEDED(hr) && !map.VirtualAddress ? E_FAIL : hr;
}

HRESULT
vgpuRecordBlt(PTRITON12_LIST list, PTRITON12_RESOURCE src, ULONGLONG cookie, D3DKMT_HANDLE *handle)
{
   PTRITON12_DEVICE dev = list->pDev;
   if (!dev->pUMCallbacks->pfnAllocateCb || !dev->pUMCallbacks->pfnDeallocateCb ||
       !dev->KTCallbacks.pfnMapGpuVirtualAddressCb || !dev->KTCallbacks.pfnLock2Cb ||
       !dev->KTCallbacks.pfnUnlock2Cb)
      return E_NOTIMPL;

   D3D12_RESOURCE_DESC tex;
   src->pResource->lpVtbl->GetDesc(src->pResource, &tex);
   if (!tex.Width || tex.Width > UINT_MAX / 4 || !tex.Height)
      return E_INVALIDARG;

   struct vgpu_blt_frame *frame = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*frame));
   if (!frame)
      return E_OUTOFMEMORY;
   frame->refs = 1;
   frame->rowBytes = (UINT)tex.Width * 4;
   UINT rows;
   UINT64 rowSize, size;
   ID3D12Device_GetCopyableFootprints(dev->pDev, &tex, 0, 1, 0, &frame->footprint, &rows, &rowSize, &size);
   HRESULT hr = E_INVALIDARG;
   if (rows != tex.Height || rowSize != frame->rowBytes || size == UINT64_MAX || size > SIZE_MAX ||
       frame->footprint.Footprint.RowPitch < rowSize)
      goto fail;

   D3D12_HEAP_PROPERTIES heap = {0};
   heap.Type = D3D12_HEAP_TYPE_READBACK;
   heap.CreationNodeMask = heap.VisibleNodeMask = 1;
   D3D12_RESOURCE_DESC buf = {0};
   buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   buf.Width = size;
   buf.Height = buf.DepthOrArraySize = buf.MipLevels = buf.SampleDesc.Count = 1;
   buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   frame->readback = vgpu_take_readback(dev, size, &frame->capacity);
   if (!frame->readback) {
      hr = ID3D12Device_CreateCommittedResource(dev->pDev, &heap, D3D12_HEAP_FLAG_NONE, &buf,
                                                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                (void **)&frame->readback);
      if (FAILED(hr))
         goto fail;
      frame->capacity = size;
   }
   hr = vgpu_allocate_blt_source(dev, (UINT)tex.Width, tex.Height, cookie, &frame->allocation);
   if (FAILED(hr))
      goto fail;

   D3D12_TEXTURE_COPY_LOCATION from = {0}, to = {0};
   from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   from.pResource = src->pResource;
   to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   to.PlacedFootprint = frame->footprint;
   to.pResource = frame->readback;
   ID3D12GraphicsCommandList_CopyTextureRegion(list->pList, &to, 0, 0, 0, &from, NULL);
   InterlockedIncrement(&frame->refs);
   EnterCriticalSection(&dev->QueueLock);
   frame->next = list->blts;
   list->blts = frame;
   LeaveCriticalSection(&dev->QueueLock);

   /* The runtime submits the kernel present after the DDI returns. Keep its
    * handle alive across list reset. Each present gets immutable storage;
    * VidMm retires prior allocations behind outstanding kernel packets. */
   struct vgpu_blt_frame *previous = InterlockedExchangePointer((void *volatile *)&src->lastBlt, frame);
   *handle = frame->allocation;
   vgpuReleaseBlt(dev, previous);
   return S_OK;
fail:
   vgpuReleaseBlt(dev, frame);
   return hr;
}

HRESULT
vgpuFinishBlts(PTRITON12_DEVICE dev, struct vgpu_blt_frame *frames)
{
   for (struct vgpu_blt_frame *f = frames; f; f = f->next) {
      /* A command-list replay can refresh the readback while the kernel still
       * needs the pixels published for the original present. */
      if (f->published)
         continue;
      BYTE *mapped = NULL;
      D3D12_RANGE read = {(SIZE_T)f->footprint.Offset,
                          (SIZE_T)f->footprint.Offset +
                              (SIZE_T)(f->footprint.Footprint.Height - 1) * f->footprint.Footprint.RowPitch +
                              f->rowBytes};
      HRESULT hr = ID3D12Resource_Map(f->readback, 0, &read, (void **)&mapped);
      if (FAILED(hr))
         return hr;
      D3DDDICB_LOCK2 lock = {0};
      lock.hAllocation = f->allocation;
      hr = dev->KTCallbacks.pfnLock2Cb(dev->Km.hRTDevice, &lock);
      if (SUCCEEDED(hr)) {
         if (lock.pData && mapped) {
            BYTE *dst = lock.pData;
            const BYTE *src = mapped + f->footprint.Offset;
            for (UINT y = 0; y < f->footprint.Footprint.Height; y++) {
               memcpy(dst + (SIZE_T)y * f->rowBytes, src + (SIZE_T)y * f->footprint.Footprint.RowPitch,
                      f->rowBytes);
            }
         } else {
            hr = E_FAIL;
         }
         D3DDDICB_UNLOCK2 unlock = {0};
         unlock.hAllocation = f->allocation;
         HRESULT unlockHr = dev->KTCallbacks.pfnUnlock2Cb(dev->Km.hRTDevice, &unlock);
         if (FAILED(unlockHr))
            hr = unlockHr;
      }
      D3D12_RANGE written = {0};
      ID3D12Resource_Unmap(f->readback, 0, &written);
      if (FAILED(hr))
         return hr;
      f->published = TRUE;
      static LONG count;
      LONG n = InterlockedIncrement(&count);
      if (n <= 4 || !(n & 255))
         TR_LOG("12.Blt: readback #%ld %ux%u alloc=0x%x", n, f->rowBytes / 4, f->footprint.Footprint.Height,
                f->allocation);
   }
   return S_OK;
}
