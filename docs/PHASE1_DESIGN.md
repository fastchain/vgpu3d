# Phase 1 design: source map, ABI and staging decision

Sources analysed (all under `upstream/`, shallow clones taken 2026-11-03):

| Project | Commit | License | Used for |
|---|---|---|---|
| VirtualBox (`vbox`) | b6bea721 (7.2.22) | GPLv3 | VBoxWddm: WDDM miniport structure, VidPn, command manager |
| virtio-win (`virtio-win`) | bd196cd | BSD-3 / GPLv2 | VirtIO library, `viogpudo` display-only driver (our working base) |
| Mesa (`mesa`) | 7d52dbb | MIT | Venus (`src/virtio/vulkan`), virgl, `virtgpu_drm.h`, DXVK-style translation later |

## 1. Key finding that shaped the staging

`viogpudo` is a **display-only** WDDM driver (4k lines, boots, signed upstream). It can scan out
frames but cannot own a render node: no contexts, paging, or D3D user-mode driver. VBoxWddm is a
**full** WDDM driver (16.7k lines in `mp/wddm` plus 13.5k in `mp/wddm/gallium`) and is the right
base for the Direct3D phases, but it cannot be exercised until a Windows machine is available and
every phase before it was previously blind.

Decision (a re-ordering of plan phases 2/3, not a change of goal):

* **Stage A (done, testable now):** keep the proven viogpudo display path and add the plan's
  phase 3 transport on top of it, exposed as a control device that mirrors Linux DRM virtgpu.
  This validates the whole host chain (QEMU, virglrenderer, Venus capset, blob memory, fences)
  from a Windows guest, and is exactly what a Mesa Venus/virgl winsys needs.
* **Stage B:** Mesa winsys for Windows over this ABI (Vulkan ICD, then OpenGL) built with MinGW.
* **Stage C (plan phases 2 and 7):** replace the display-only front end with the VBoxWddm-derived
  full WDDM miniport (render capable, native D3D11 UMD). The control ABI is transport neutral
  and moves to `DxgkDdiEscape` there; the virtio-gpu back end written in Stage A is reused.

## 2. VBoxWddm keep / strip / rewrite map (for Stage C)

Counts are lines or matching references per file in `mp/wddm`.

| File | Lines | HGSMI/VBVA/VBox-HW refs | Verdict |
|---|---|---|---|
| `VBoxMPVidPn.cpp/.h` | 3397+397 | 4 VBVA | **Keep.** Hardware neutral VidPn topology and mode negotiation. Biggest single saving. |
| `VBoxMPVModes.cpp/.h` | 777+64 | 6 VMMDev | **Keep**, replace the VMMDev custom-resolution hook with the virtio-gpu EDID/`events_read` path (already in viogpudo). |
| `VBoxMPSa.cpp/.h` | 425+115 | 0 | **Keep.** Generic allocation/GPU-segment helper. |
| `VBoxMPCm.cpp/.h` | 707+73 | many `VBoxVideoHw` names, no HGSMI | **Keep and rename.** Command manager (contexts, submission queue); needs the transport swapped. |
| `VBoxMPMisc.cpp/.h` | 1701+180 | 8 HGSMI, 4 VBVA | **Rewrite partly.** Utility code with HGSMI helper pieces mixed in. |
| `VBoxMPWddm.cpp/.h` | 5690+229 | 39 HGSMI, 44 VBVA, 109 SVGA/gallium | **Port selectively.** The DDI entry-point skeleton (AddDevice/StartDevice/QueryAdapterInfo/CreateAllocation/BuildPagingBuffer/SubmitCommand/Escape/...) is the value; every device-touching body is replaced. |
| `VBoxMPShgsmi.*`, `VBoxMPVbva.*`, `VBoxMPVdma.*`, `VBoxMPLegacy.*` | 302+107+92+1714 | 100+ | **Strip.** HGSMI/VBVA transport and legacy (pre-WDDM1.1) paths. |
| `VBoxMPRegistry.cpp`, `VBoxWddm.inf/.rc` | 81 | 1 | **Port** (settings keys, INF). |
| `mp/wddm/gallium/*` | 13465 | SVGA specific | **Strip entirely** (SVGA FIFO, SVGA3D, VBoxMPDX). Reference only for how a Gallium UMD consumes kernel contexts/fences. |
| `disp/wddm/dx/` (VBoxDX UMD) | | SVGA3D back end | **Reuse the DDI front end only** in Stage C phase 7. |

## 3. What the virtio-win base was missing for 3D (now added)

Verified against `/usr/include/linux/virtio_gpu.h`:

| Gap in upstream viogpu | Added in `src/kmd/viogpu` |
|---|---|
| Features negotiated: only VERSION_1 / ACCESS_PLATFORM | `VIRTIO_GPU_F_VIRGL`, `RESOURCE_BLOB`, `CONTEXT_INIT` |
| Command/response numbers for blob, map, UUID | added to `viogpu.h` (0x10b-0x10d, 0x208-0x209, 0x1105-0x1106) |
| Control header had no `ring_idx` | split `padding` into `ring_idx` + 3 bytes, added `FLAG_INFO_RING_IDX` |
| 3D encoders (ctx create/destroy, attach, submit, create_3d/blob, map/unmap, capsets) | `vgpu3d.cpp` |
| Host-visible shared-memory region (PCI cap type 8, id 1) not parsed | `FindHostVisibleShm()` walks the config-space capability list |
| `QueueBuffer` returned 0 for both success and setup failure | setup failures now return `(UINT)-1` |
| Capset/response limited to one page by the SG builder | documented; `GET_CAPS` rejects capsets > 4072 bytes |

## 4. Control ABI (`src/include/vgpu_abi.h`)

One open handle = one virtio-gpu context, like a DRM file. Ioctls mirror `virtgpu_drm.h`:
GETPARAM, GET_CAPSET_INFO, GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE_BLOB, RESOURCE_CREATE
(virgl 3D), RESOURCE_INFO, MAP/UNMAP (into the caller's address space), RESOURCE_CLOSE,
TRANSFER_TO/FROM_HOST, EXECBUFFER (handles + command stream inline), WAIT_FENCE, QUERY_FENCE.
Fences are per (context, ring) timelines with 64-bit ids; ring `VGPU_RING_NONE` is the global
timeline. No user pointers cross the boundary, so 32-bit callers work unchanged.

### Why an IOCTL device rather than DxgkDdiEscape for Stage A

`MmMapLockedPagesSpecifyCache(UserMode)` mappings must be unmapped before their process dies. An
escape has no per-handle cleanup hook; a file object has `IRP_MJ_CLEANUP`, which runs in the
closing process while its address space still exists. That also gives crash-safe teardown of
contexts and resources for free.

## 5. Toolchain decision

The whole kernel driver cross-builds on Linux: `clang-cl` + `lld-link` against the WDK and SDK
NuGet packages (`Microsoft.Windows.WDK.x64` and `Microsoft.Windows.SDK.CPP`, 10.0.26100.6584).
Case-sensitivity of the headers is handled by symlinks inside the toolchain cache and an include
overlay (`build/casefix_overlay.py`). Two link shims live in `build/kshim.c` (port I/O intrinsics
and a static `__security_cookie`; the cookie must be seeded in `DriverEntry` before a release build).
No MSVC, Wine or Windows machine is needed to produce `vgpu.sys` and the PDB (CodeView, usable in
WinDbg).
