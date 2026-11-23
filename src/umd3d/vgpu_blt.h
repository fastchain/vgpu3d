/* SPDX-License-Identifier: MIT */
#pragma once
#include "virtio-gpu/virgl_hw.h"
#include "virtio-gpu/wddm_hw.h"

static inline void
vgpuInitBltAllocation(VIOGPU_CREATE_ALLOCATION_EXCHANGE *ax, UINT width, UINT height)
{
   ax->Type = VIOGPU_RESOURCE_TYPE_3D;
   ax->Options3D.target = 2; /* PIPE_TEXTURE_2D */
   /* identical to the KMD's own coherent standard allocations (staging /
    * shadow surfaces), which the host accepts for transfers and copies */
   ax->Options3D.format = VIRGL_FORMAT_B8G8R8X8_UNORM;
   ax->Options3D.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                       VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT;
   ax->Options3D.width = width;
   ax->Options3D.height = height;
   ax->Options3D.depth = 1;
   ax->Options3D.array_size = 1;
   ax->Options3D.flags = VIRGL_RESOURCE_FLAG_MAP_COHERENT;
   ax->Size = (ULONGLONG)width * height * 4;
}
