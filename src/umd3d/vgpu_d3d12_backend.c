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

#include "triton_log.h"

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
