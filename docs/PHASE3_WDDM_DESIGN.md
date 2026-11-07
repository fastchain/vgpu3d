# Step 3 design: a WDDM driver every Windows program uses by default — option B (stock QEMU)

Status: approved direction "option B without QEMU changes" (user, 2026-11-06). This document is the
design for review. No code yet.

## 1. Goal and constraint

Windows programs reach the host RTX 4090 through Windows' own runtimes (`d3d11.dll`, `opengl32.dll`,
`vulkan-1.dll`), with no per-application DLLs.

The host stays **stock**: QEMU 10 `virtio-vga-gl` (`blob=true, hostmem=4G, venus=true`) and stock
virglrenderer with Venus. That is today's setup, plus `seccomp_sandbox = 0`. All translation happens in
the guest.

```
app ─ d3d11.dll ─ D3D10/11 UMD (DDI → D3D11 API) ─ DXVK (in-process) ─┐
app ─ opengl32.dll ─ Mesa WGL + Zink ICD ─────────────────────────────┼─ Venus ICD (D3DKMT backend)
app ─ vulkan-1.dll ───────────────────────────────────────────────────┘        │ D3DKMT*
                                                         WDDM render KMD (viogpu3d-based)
                                                                │ virtio-gpu (ctx/blob/submit/scanout)
                                                    stock QEMU + virglrenderer (Venus) → host Vulkan → RTX 4090
```

## 2. What we reuse

| Piece | Source | Licence | Use |
|---|---|---|---|
| WDDM render KMD | osy/kvm-guest-drivers-windows `neptune` → `viogpu/viogpu3d` (~13k lines; anonymix007's Venus KMD lineage) | BSD-3 | base of our KMD. Drop Neptune specifics (gates/present-fence escapes), keep allocation/context/submit/VidPn/present |
| Venus over D3DKMT | osy/virtio-win-mesa `neptune` → `src/virtio/vulkan/vn_renderer_virtgpu_win32.c` (2.3k lines: CreateAllocation, Render, Lock, Escape, OpenResource, sync objects) | MIT (Mesa) | replaces our `\\.\VGpu3D` backend once the KMD is WDDM. Port our fixes into it: coherency (`needs_cached_host_visible`), `no_async_present`, sfence, capset version 0 |
| D3D10/11 DDI layer | same fork → `src/virtio/neptune/triton` (DDI ↔ ID3D11Device; DXBC signature rebuild `tritonDxbc.c`; DXGI DDI/present `tritonDxgi.c`) | MIT | swap the device factory `npt_d3d11_create_device_internal()` (Neptune) for **DXVK's** D3D11 device creation |
| D3D11 → Vulkan | DXVK (upstream; built as a library for MinGW) | zlib | linked into the UMD, running on the guest Venus ICD |
| OpenGL | our Mesa WGL + Zink build (`build/build_gl.sh`), proven on Fusion | MIT | registered as `OpenGLDriverName` |
| Already ours | stability fixes, kd/LiveKD workflow, stress/benchmark scripts, Fusion MCP harness | — | testing |

Our display-only `vgpu.sys` and its `\\.\VGpu3D` ABI become the fallback and reference. Only one driver
can bind `PCI\VEN_1AF4&DEV_1050`.

## 3. Milestones

### M1 — WDDM KMD + system-wide Vulkan and OpenGL (no Direct3D yet)

1. Build the viogpu3d KMD with our Linux cross toolchain (clang-cl + lld-link + NuGet WDK). Apply the
   lld lessons: `/tsaware:no`, explicit section flags, MemoryLarge BARs.
2. Make it run on **stock** QEMU: Venus/virgl capsets only, no Neptune capset. Verify:
   - desktop/display: modes, resize, cursor
   - Venus contexts from user mode
   - blob allocations mappable (host-visible window)
   - fences
3. Rebuild Venus with the D3DKMT backend, plus our fixes. Register it through the INF
   (`VulkanDriverName`).
4. Register our Zink ICD (`OpenGLDriverName`, WOW64 variant later).
5. Present path for GL/Vulkan windowed apps: render → blit into the window's DWM redirection surface
   (CPU or KMD blit first, the same as today), then a shared-allocation path.

Exit test:

- `vulkaninfo`/`vkcube`/`vkcompute` and `gltest` with **no** local DLLs.
- Fusion in **OpenGL** mode with no DLLs in its folder: ≥ the 60 fps measured in step 2.
- 15-cycle restart stress, no bugcheck.

### M2 — Direct3D 10/11 by default

1. Build DXVK (d3d11 + dxgi internals) as a static library for the UMD (MinGW, same toolchain as Mesa).
2. Triton's DDI layer with the DXVK device factory. Present/DXGI DDI (`tritonDxgi.c`) adapted to
   DXVK + KMD present.
3. Shared resources for DWM: allocations created shareable by the KMD (one virtio blob resource,
   attached to every context that opens it). Venus imports by resource id. Expose
   `VK_KHR_external_memory_win32` in Venus by mapping D3DKMT shared handles to those allocations.
   **This is the critical path; prototype it first.**

Exit test:

- `d3d11test` with no local DLLs.
- DWM-composited windowed D3D11 apps.
- **Fusion in DirectX mode on the 4090**. Today's baseline: WARP 1.4 fps; the GPU path should be in the
  ~60 fps class.

### M3 — D3D9, then D3D12

- D3D9: DXVK d3d9 behind the D3D9 DDI.
- D3D12: vkd3d-proton behind the D3D12 DDI. Triton has D3D12 pieces (`triton12*`, `npt_d3d12*`).

## 4. Risks and how we check them early

| Risk | Early check |
|---|---|
| viogpu3d KMD needs Neptune-era host features | M1 step 2 on stock QEMU, first thing |
| Venus D3DKMT backend vs. our NVIDIA coherency finding | `vkcompute` stale-value test (passes only with our fix) |
| Cross-process sharing (DWM) with Venus | M2 step 3 prototype before the full DDI work |
| DXVK inside a UMD (it normally *is* `d3d11.dll`): loader recursion, threading, TLS | load-only spike: UMD that creates a DXVK device and clears a render target |
| Host fence latency (~10 ms, QEMU polling) limits CPU↔GPU sync-heavy apps | measured with `vgpulat`; fix belongs in a newer QEMU/virglrenderer, not a fork |
| Stability of experimental upstream code | kd/LiveKD, minidumps, snapshot before each install |

## 5. Not in this step

- Release signing.
- Installer.
- Zero-copy scanout tuning (step 5).

Sources:

- https://github.com/osy/kvm-guest-drivers-windows (branch `neptune`, BSD-3)
- https://github.com/osy/virtio-win-mesa (branch `neptune`, MIT)
- https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/
- https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943
- https://www.phoronix.com/news/Mesa-Drops-Direct3D-10-UMD
