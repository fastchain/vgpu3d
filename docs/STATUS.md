# Status (2026-11-03)

## Built and compile-checked (not yet run on Windows)

| Artifact | Path | What it is |
|---|---|---|
| `vgpu.sys` + `vgpu.pdb` | `out/kmd/` | Display-only WDDM driver (virtio-win base) + 3D control device `\\.\VGpu3D` |
| `vgputest.exe` | `out/tools/` | Smoke test: params, capsets, context, guest blob map, fenced submit |
| `vgpu.inf` | `src/kmd/inf/` | Binds PCI 1AF4:1050 |
| `vgpu-guest.zip` | `out/` | Everything for the Windows guest incl. signing/install scripts and Inf2Cat |
| `WIN11_3D.xml` | `/workspace/WIN11/` | libvirt domain: virtio-gpu GL + blob + Venus + memfd, Secure Boot not enforced |
| Host guide | `docs/HOST-SETUP.md` | What QEMU/virglrenderer/Vulkan on the host must provide |

Rebuild everything: `build/package.sh` (about 20 seconds).

## Tested here

* Compiles clean (warnings on) and links with lld; PE header, imports and section flags inspected.
* NOT tested: loading, any ioctl, any DMA. Nothing has run on Windows or against a real host device.

## First test on the Windows machine (order matters)

1. Host: follow `docs/HOST-SETUP.md`, start `WIN11_3D`, install Windows 11.
2. Guest: unzip `vgpu-guest.zip`, run `install.ps1` (reboot once when asked), run `vgputest.exe -v`.
3. Report the full `vgputest.exe` output. If the guest bluescreens, send the minidump and
   `vgpu.pdb`/`vgpu.sys` from the same build.

## Known unknowns to verify on first run (marked VERIFY in `vgpu3d.cpp`)

* `DxgkDdiEscape`-free control device: replacing the driver's IRP dispatch entries after
  `DxgkInitializeDisplayOnlyDriver` must not disturb dxgkrnl (open question: does dxgkrnl set
  entries later, e.g. at AddDevice?).
* `MmAllocateMdlForIoSpace` MDL mapped to user mode, and that `ExFreePool` is the right free.
* Whether the host answers an empty fenced `SUBMIT_3D`; if not, the fence step of `vgputest`
  fails even though the path works (Venus needs a real command stream).
* Interrupt delivery to the new completions uses the existing control-queue DPC; MSI message
  numbers are unchanged.
* Large BAR (hostmem 4G): upstream `CPciBar` stores sizes in 32 bits; the driver only uses the PA
  plus the capability's length, but verify the BAR resource appears at all.

## Next (Stage B)

Mesa Venus winsys on top of `vgpu_abi.h` (MinGW cross build), Vulkan ICD registration, swapchain
presentation by copying to a GDI surface. Starts once step 2 above passes on the Windows machine.

## Independent review (2026-11-03)

A separate reviewer read `vgpu3d.cpp` and the queue code before any test run. Fixed from its
findings: caller-owned buffers were freed by the queue (double free, wrong pool tag), the DPC read
a vbuf after a waiter could free it, shutdown ran after the virtqueue was destroyed, shutdown vs
close/IOCTL races, dead-file checks made under the lock, unmap against an exited process, a
capset size overflow, unreferenced guest pages freed when no UNREF could be sent, unaligned
host-visible window. Not changed: inline command buffers straddling a page (pre-existing upstream
behavior, pool blocks of this size normally do not cross pages) and the async ctx-attach result
being ignored.

## First Windows run (2026-11-06) - Stage A display path WORKS

Guest: WinDev2301Eval (Win11 Enterprise Eval 22H2, build 22621), libvirt WIN11_3D, host GL OFF
(`<gl enable="no">`, no blob/venus) - so only the 2D/display path and graceful 3D fallback are exercised.

Result: vgpu.sys loads at boot, drives the display (1280x800), survives reboot, no bugcheck.
vgpu3d logs `features: virgl=0 blob=0 context_init=0` -> control device not created (expected);
vgputest fails cleanly with "open \\.\VGpu3D: error 3".

Bugs found and fixed on the way (all toolchain/packaging, none in driver logic):
1. **lld-link sets IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE (0x8000) by default.** The NT kernel
   refuses such a driver: `sc start` -> 487 (invalid address), PnP -> 0xC000026C / 0xC0000365, and
   DriverEntry never runs. Fix: `/tsaware:no`. Bisected with tests/probe (minimal driver, link variants).
2. `/section:INIT,d` REPLACES the section flags in lld (INIT lost execute/read). Now `/section:INIT,erd`,
   plus MSVC-equivalent NOT_PAGED flags on .text/.rdata/.data/.pdata and `.retplne` merged into .rdata.
3. install.ps1: `$PSScriptRoot` is empty as a param default in PS 5.1 -> resolve from $MyInvocation.
4. package.sh: unique DriverVer per build (pnputil ignores same-version updates); date backdated one day
   because Inf2Cat rejects a DriverVer date ahead of the guest clock.

Debug aids now in the driver: DriverEntry writes `InitStage`/`InitStatus` under
HKLM\SYSTEM\CurrentControlSet\Services\VGpu; DbgView (scheduled task `vgpu-dbgview`, session 1) logs to
C:\vgpu\dbg.log.

Next: enable host GL (virtio-gpu-gl + virglrenderer on the RTX 4090) so VIRGL/blob/Venus are offered,
then vgputest exercises the 3D control path.

## 3D control path PASSES on Windows (2026-11-06)

Host: QEMU virtio-vga-gl + virglrenderer on the RTX 4090 (NVIDIA 615.78.08), `egl-headless` (renderD128)
+ VNC, blob=true, hostmem=4G, **venus off**. vgputest: all checks passed - capsets virgl(1)/virgl2(2),
CONTEXT_INIT, guest blob create/map/zero/pattern/unmap/unref, fenced EXECBUFFER, virgl texture
TRANSFER_TO/FROM_HOST round trip. Display path keeps working (EDID, display info OK).

Host-side findings:
- libvirt (qemu:///system) hides /dev/nvidia* from QEMU -> `eglInitialize failed`. Fixed via
  cgroup_device_acl in /etc/libvirt/qemu.conf (only list nodes that exist, or every VM fails to start).
- SPICE `<gl>` in virt-manager showed black on NVIDIA -> use `<graphics type="egl-headless">` + VNC
  (VNC must be the FIRST graphics element or virt-manager refuses to open the console).
- `venus=true` -> "failed to initialize venus renderer / virgl could not be initialized: -1", after which
  QEMU silently drops EVERY control command (looks like a guest hang). Venus needs host work.

Guest fixes in this round: CmResourceTypeMemoryLarge BARs (the 4 GiB hostmem BAR) in viogpu_pci.cpp;
vgputest used CreateFileA with a wide path.

## Venus works (2026-11-06)

Host: `seccomp_sandbox = 0` in /etc/libvirt/qemu.conf (QEMU's `-sandbox ...,spawn=deny` blocked
virglrenderer's Venus render-server process -> "failed to initialize venus renderer"), venus=true back on.
Guest sees capsets virgl(1), virgl2(2), **Venus(4, max_version 0, 160 bytes)**. vgputest: all checks pass,
including a Venus CONTEXT_INIT with 4 rings and a fenced empty submit on ring 1 (per-ring timeline).
Fixes: DoGetCaps and vgputest rejected/skipped capset version 0 (Venus advertises max_version 0).

Stage A is complete. Next: Stage B - Mesa Venus Vulkan ICD for Windows over vgpu_abi.h.

## Stage B: Vulkan + Direct3D 11 on the host GPU WORK (2026-11-06)

Mesa Venus cross-built for Windows (MinGW-w64, `build/build_venus.sh`), new renderer backend
`mesa/src/virtio/vulkan/vn_renderer_vgpu.c` over \\.\VGpu3D (patch: patches/mesa-venus-windows-vgpu.patch,
mesa worktree branch `vgpu-win`, uncommitted).  Guest package `out/vgpu-venus.zip` (`build/package_venus.sh`):
install-venus.ps1 registers the ICD (HKLM\SOFTWARE\Khronos\Vulkan\Drivers), installs vulkan-1.dll (LunarG
1.4.328.1 runtime), copies DXVK 3.1.1 + tools to C:\Program Files\vgpu.

Results in the Win11 guest:
- vulkaninfo: "Virtio-GPU Venus (NVIDIA GeForce RTX 4090)", Vulkan 1.4.334, discrete GPU.
- vkcompute: 1M-element compute shader, results verified through mapped memory; 200 submit/wait loops OK.
- vkcube: renders correctly to a Win32 window (common WSI, CPU present path), ~125 fps.
- d3d11test on DXVK: device FL 11_1, adapter = RTX 4090 via Venus, offscreen draw + readback pixel-exact.

Fixes/findings:
- Win32 build gaps in Venus: sync_wait/sync_valid_fd stubs, renderer selection, KHR_win32_surface enabled,
  vtn_* stubs (PE ld keeps the unreferenced vk_spec_info_to_nir_spirv), -static (no libwinpthread-1.dll).
- **Coherency**: host-visible non-cached types (NVIDIA WC sysmem, ReBAR VRAM) are NOT coherent in the guest:
  KVM ignores the guest PAT for host RAM (guest mapping is effectively WB) while the GPU writes without
  snooping -> stale cache lines (verified: wrong values without clflush, correct with). Fix: renderer flag
  `needs_cached_host_visible` -> Venus hides HOST_VISIBLE on non-HOST_CACHED types; apps use the cached
  (snooped, coherent) type. Override VN_VGPU_ALLOW_UNCACHED=1 once the host honors guest PAT (KVM quirk
  KVM_X86_QUIRK_IGNORE_GUEST_PAT disabled by the VMM). Plus sfence before every submit/flush.
- vkWaitForFences round trip ~10 ms although the kernel fence round trip is 0.02-0.07 ms (vgpulat.exe):
  host-side (QEMU/virglrenderer retire Venus queue fences on a ~10 ms poll). Latency only, not throughput.
- The Vulkan loader ignores VK_DRIVER_FILES in elevated processes -> register the ICD in HKLM.

Next: Autodesk app test with DXVK; OpenGL via Mesa Zink (opengl32.dll on Venus); faster present path
(GPU->DIB without CPU copy / DXGI interop); host fence latency; full WDDM render driver (long term).

## Kernel driver stability fixes (2026-11-06, step 1 of the unified-driver plan)

Kernel debugger: kd.exe + extensions extracted from the WinDbg msixbundle (upstream/windbg/kd ->
C:\vgpu\kd in the guest); run `kd -z <dmp> -y "C:\vgpu\pdb;srv*C:\symbols*https://msdl.microsoft.com/download/symbols"
-logo log -c ".load C:\vgpu\kd\winext\ext.dll; !analyze -v; q"` (stdout through PowerShell is empty: use -logo).

1. BAD_POOL_HEADER (0x19/0x22) in VioGpuBuf::Close: FreeBuf left caller-owned resp/data pointers on
   pooled vbufs when no_free_bufs was set; Close() freed them again (e.g. DoGetCaps' page). FreeBuf now
   always clears them.
2. Driver restart with a live 3D client took 50 s (UNMAP_BLOB/UNREF/CTX_DESTROY each timing out at 5 s
   because the stopping device no longer completes) and left \\.\VGpu3D unusable (old device object
   kept the name) -> next stop called IoDeleteDevice twice -> SYSTEM_THREAD_EXCEPTION_NOT_HANDLED in
   IoDeleteDevice. Now: `host_gone` makes all sends fail fast during shutdown (guest-side teardown only;
   the device reset drops host state), and the control device lives for the driver lifetime
   (VGpu3DShutdown detaches, VGpu3DUnload from DxgkDdiUnload deletes it).
3. Installer: a test-signing cert created while the guest clock ran ahead is "not yet valid" ->
   unsigned catalog -> pnputil "No signature was present". install.ps1 now requires NotBefore < now and
   fails loudly if the catalog signature is not Valid.

Verified: restart-while-rendering takes 0 s, 3D usable right after, second restart OK; 15/15
vgputest + vkcompute + device-restart cycles; no bugcheck. The unkillable Fusion process seen earlier
was not reproduced; LiveKD + kd are now available in the guest to capture its kernel stack if it recurs.

## Step 2: OpenGL (Mesa WGL + Zink) - per-app works, system-wide needs WDDM (2026-11-06)

Build: /workspace/driver/out/mesa-gl-build (meson, MinGW, -Dgallium-drivers=zink,softpipe -Dopengl=true
-Dplatforms=windows, -static) -> libgallium_wgl.dll (17 MB stripped) + opengl32.dll; out/gl-win/.
- App-local (opengl32.dll + libgallium_wgl.dll next to the .exe): "zink Vulkan 1.4 (Virtio-GPU Venus
  (NVIDIA GeForce RTX 4090))", OpenGL 4.6 compatibility profile, gltest pixel-exact.
- System-wide ICD experiment: OpenGLDriverName/OpenGLVersion/OpenGLFlags on the adapter's class key
  ({4d36e968...}\0001) -> Windows opengl32 still uses "GDI Generic" (also in the desktop session):
  Windows does not load an OpenGL ICD for a display-only (DOD) adapter. => needs the full WDDM driver (step 3).
- **Autodesk Fusion works on the GPU**: Graphics driver preference = OpenGL + the two Mesa DLLs in
  Fusion's webdeploy folder. Fusion log: "Graphics Card: zink Vulkan 1.4(Virtio-GPU Venus (NVIDIA GeForce
  RTX 4090))". Chromium/Qt keep Windows' own D3D11/DComp (WARP), so none of the DXVK crashes apply.
  Fusion auto-updates into a new hash folder: the DLLs must be copied again after an update.

DXVK route for Fusion abandoned: Fusion's in-process Chromium also gets DXVK's d3d11 -> DirectComposition
crash (fixed with a dcomp.dll proxy, src/umd/dcompshim) -> NULL call inside DXVK from Chromium.
Venus bug found on the way: async present thread raced DXVK's submit thread on the queue scratch storage
-> renderer flag no_async_present for the vgpu backend.

Fusion benchmark (2,547,485-triangle mesh scene: 9x xyzrgb_dragon + lucy + armadillo + happy, loaded via
Fusion MCP; 72-step scripted orbit with viewport refresh):
- OpenGL -> Mesa Zink -> Venus -> RTX 4090: avg 16.8 ms/frame, ~60 fps (39 fps in a first run with 2 docs open)
- DirectX -> WARP (CPU, no vgpu components loaded): avg 706 ms/frame, 1.4 fps  => ~42x faster on the GPU path

## Roadmap (agreed 2026-11-06) - goal: one driver that any Windows software uses by default (like VBox)

Done: stage A transport; Venus Vulkan (system-wide ICD); step 1 kernel stability; step 2 OpenGL via Mesa
WGL+Zink (per app; Fusion on the 4090, ~42x vs WARP).
Remaining:
3. Full WDDM render miniport (replace display-only): allocations, GPU scheduler submission, fences,
   present; INF registers UMDs: Vulkan (Venus), OpenGL (Mesa WGL+Zink), D3D10/11 (Mesa d3d10umd on Zink).
   Design document first (docs/PHASE3_WDDM_DESIGN.md), user approval before code.
4. D3D9 and D3D12.
5. Performance: zero-copy present, host fence latency (~10 ms poll), review VNC/egl-headless setup.
6. Release: production signing, single installer, commit Mesa changes (Generated-by: LLM), licence decision.
Loose ends: Fusion updates need the 2 Mesa DLLs re-copied; SPICE auto-resize untested; unkillable process
not reproduced (kd/LiveKD ready in guest).
Saved artefacts: patches/mesa-venus-windows-vgpu.patch, build/{build_kmd,package,build_venus,package_venus,build_gl}.sh,
tools/fusion/*, tools/windows/{stress,restart_mid,deploy}.ps1, src/umd/{vkcompute,vgpulat,d3d11test,gltest,dcompshim}.

## Step 3 / M1 progress (2026-11-06, VM now Windows 11 Pro 26H2 build 26300, upgraded in place from 22H2 eval)

- Upgrade fixed the 22H2-only adapter start failure: our viogpu3d KMD build (src/kmd3d) starts on 26300
  (CM_PROB_NONE, boot guard clears). Package: build/package_kmd3d.sh -> out/vgpu3d-m1.zip, install.ps1 in guest.
- Vulkan system-wide through the WDDM KMD WORKS: vulkaninfo with no local DLLs -> "Virtio-GPU Venus (NVIDIA
  GeForce RTX 4090)", Vulkan 1.4.334; vkcompute2: 1M results correct, fences + timeline semaphores, 15.7 ms
  submit+wait round trip.  Needed (all in patches/triton-mesa-vgpu-m1.patch, build/build_triton_venus.sh):
  1. ICD manifest library_path ".\\vulkan_virtio.dll" (bare name -> LoadLibrary error 87 from DriverStore).
  2. Venus built from osy/virtio-win-mesa with MinGW: WDK DDI headers staged in out/winddk + SAL shim,
     pid_t guard, "static inline" in wddm_hw.h.
  3. Standalone ICD: dxgkrnl rejects D3DKMTRender on this GpuMmu KMD (STATUS_UNSUCCESSFUL before
     DxgkDdiRender; LegacyMode / OpenGL client hint do not help).  Ported Neptune's WDDM2 path: virtual
     context (D3DKMTCreateContextVirtual), commands in D3DKMTSubmitCommand private data (VIOGPU_DMA_PRIVATE,
     max 1024 B), monitored fence + hAsyncEvent for CPU events, MAP/UNMAP_BLOB_BY_ID, RES_INFO UserVa
     instead of D3DKMTLock.
  4. Our KVM coherency fix (needs_cached_host_visible) and no_async_present ported.
  Venus/Mesa logs: VN_DEBUG=init,result MESA_LOG_LEVEL=debug -> stderr; run tests in session 1 via the
  scheduled task vgpu-run (C:\vgpu\run1.ps1 runs C:\vgpu\job.ps1 -> C:\vgpu\job.txt); SSH = session 0, where
  EnumDisplayDevices finds no adapter.
- BLOCKER: with our KMD driving the display, DWM crash-loops (dwmcore 0x889800b0, ~250 restarts / 15 min,
  black desktop).  DWM needs a working D3D11 UMD on the display adapter; Triton's neptune_umd advertises D3D11
  but CreateDevice fails without Triton's host; "<>" in all UMD slots does not make DWM fall back to WARP.
  The design (PHASE3_WDDM_DESIGN.md) missed this: a render+display adapter needs D3D11 for the desktop in M1.
  VM rolled back to Microsoft Basic Display (all vgpu3d oem*.inf deleted); reinstall = unzip vgpu3d-m1.zip +
  install.ps1.  vkcube present path untested because of this (window black = whole desktop black).
- Boot guard false positive: a driver update that needs a restart leaves VgpuBootGuard=1 -> next boot refuses
  (2).  Re-arm: reg add HKLM\SYSTEM\CurrentControlSet\Services\viogpu3d /v VgpuBootGuard /t REG_DWORD /d 0 /f

## M2 (plan A, approved 2026-11-06): D3D10/11 UMD = Triton DDI layer + DXVK on Venus

- Step 1 done: DXVK v3.1.1 in upstream/dxvk (vgpu patch: dxvk_ prefix on Windows, dxvk_d3d11 links
  dxvk_dxgi, .def LIBRARY names) -> build/build_dxvk.sh -> out/dxvk.  Triton D3D10/11 files (no D3D12)
  + src/umd3d/vgpu_triton_backend.c (DXVK device factory, format tables, sharing stubs) -> build/build_umd.sh
  -> out/umd3d/vgpu_d3d11.dll (MinGW; d3d12umddi.h staged in out/winddk; --allow-multiple-definition for
  the umddi const UINTs).  INF: UserModeDriverName = "<>", vgpu_d3d11.dll x3; Wow = "<>" (no 32-bit yet).
- Step 2 PASSED: d3d11test through C:\Windows\System32\d3d11.dll -> vgpu_d3d11 -> DXVK -> Venus: FL 11.1,
  adapter "vgpu VirtIO GPU 3D (WDDM)", triangle pixels correct, 14.5 ms per draw+readback.
  DWM also loaded the stack and created its device (no crash loop any more) but then hung in present
  (screen capture hung) -> step 4.
- Rollback while DWM hung -> BUGCHECK 0x50 in VioGpu3DCloseAllocation (HandleBase::FromHandle on a freed
  VioGpuDeviceAllocation; dxgkrnl DXGDEVICE::DestroyCoreAllocations during DxgkRemoveAdapter).  Cause:
  device-allocation entries live in the parent's list and died with it when DestroyAllocation ran first.
  Fix (src/kmd3d viogpu_allocation.cpp): each device entry holds a parent ref (AddRef in Open, Release
  after the lock in Close via CloseEntry).  The reboot also logged the user out (no autologon).
  tools/windows/vgpu3d-rollback.ps1 = remove all vgpu3d packages.
- Step 3 design (sharing, stock host): creator makes the VkImage itself on DXVK's VkDevice
  (ID3D11VkInteropDevice1::GetVulkanHandles) with exportable OPAQUE_WIN32_KMT dedicated memory,
  vkGetMemoryWin32HandleKHR -> Venus allocation's global KMT handle, wraps with CreateTexture2DFromVkImage;
  runtime allocation (pfnAllocateCb) = VIOGPU_RESOURCE_TYPE_IMPORT{res_id} + trailing desc + KMT handle.
  Opener (DWM DDI OpenResource): import KMT handle into Venus (existing D3DKMTOpenResource path), same
  VkImage desc, CreateTexture2DFromVkImage.  Sync first by CPU wait at hand-over, then shared semaphores.
- Step 3 PASSED (2026-11-07): sharetest (src/umd/sharetest) - parent creates a MISC_SHARED BGRA8 texture,
  clears it, child process OpenSharedResource -> correct pixel on the 4090.  What it took:
  * Export: DXVK MISC_SHARED texture -> IDXGIResource::GetSharedHandle = Venus allocation's global KMT
    handle; backend resolves it to the KMD res_id (private D3DKMT device: OpenResource + RES_INFO) and
    Triton's runtime allocation becomes VIOGPU_RESOURCE_TYPE_SHARED with create_ctx_id =
    VIOGPU_SHARED_IMPORT_CTX (KMD adopts the res_id, never destroys it), KMT handle in `modifier`.
  * Open (DDI OpenResource): DXVK interop addition CreateTexture2DFromSharedHandle (patches/dxvk-vgpu.patch)
    = DXVK's own OpenSharedResource construction with the creator's desc.
  * Venus (patches/triton-mesa-vgpu-m1.patch): advertise OPAQUE_WIN32(_KMT)/D3D11_TEXTURE(_KMT) on top of
    the dma_buf renderer type; shareable OPTIMAL images -> LINEAR DRM-modifier images (virglrenderer's
    proxy only attaches dma-buf/shm resources to other contexts - opaque fds are refused); allocation
    lookup cookie + RES_INFO retry (handle lookup unreliable under GpuMmu); control-queue sync after an
    import's OpenResource; GetMemoryWin32Handle without bo -> error instead of assert.
  * Triton: runtime ctx on capset 4 (Venus) instead of 7; LINEAR_EXPORT misc bit off (VGPU_DXVK_INPROC).
  Test loop: tools/windows/vgpu3d-cycle.ps1 (install, run C:\vgpu\job.ps1 in session 1, roll back).
  Host-side errors only appear in /var/log/libvirt/qemu/WIN11_3D.log (ask the user).
  Sync between processes is still the app's job (sharetest waits on an EVENT query); DWM path = step 4.
- Step 4 (2026-11-07), DWM on our stack:
  * DWM hang was a Venus assert -> MinGW msvcrt MessageBox inside DWM (invisible, waits forever); the stuck
    DWM survived every later round.  Venus now built with -D_assert=vgpu_mesa_assert (log via
    OutputDebugString + abort; __imp_ slot provided in vn_renderer_virtgpu_win32.c).
  * With a fresh DWM: device, 3-buffer flip chain (1024x768 B8G8R8A8, LINEAR, pitch 4096, 3 MiB each),
    present fence, flips -> KMD SET_SCANOUT_BLOB per frame.  In-guest screenshot = correct desktop.
  * QEMU rejected SET_SCANOUT_BLOB (0x1205) while stride was 0 -> backend now queries the LINEAR plane
    layout (DXVK IDXGIVkInteropSurface + vkGetImageSubresourceLayout MEMORY_PLANE_0).  Accepted since.
  * egl-headless only forwards a GL/dma-buf scanout when the console has a 2D surface, which only a
    classic SET_SCANOUT creates; KMD now re-creates it with a 3840x2160 helper 2D resource before a blob
    scanout of a new size (CtrlQueue::SetScanoutBlob; QEMU checks the rect against the resource size).
  * STILL BLACK on the host display (SPICE via egl-headless): QEMU imports the dma-buf without EGL error
    (DRM_FORMAT_MOD_INVALID = implicit modifier) and refreshes on RESOURCE_FLUSH, image is black.
    `virsh screenshot` cannot help ("no surface" for any GL scanout).  Next: SPICE gl=on (dma-buf passed
    to the client) - needs a host XML change.
  * Also: DWM crashes (vn_ring_load_head AV) when the adapter is removed under it (KMD unmaps ring
    shmem) - rollback only; A8_UNORM 32x32 shared textures cannot be exported (DXVK sharing formats).
  * KMD DebugLevel=4 via INF (verbose DPC tracing made DbgView eat >90% RAM); dwm-diag-cycle.ps1 starts
    each round with a fresh dbg.log.
- Step 4 DONE (2026-11-07 ~09:30 VM time): **Windows desktop runs on the 4090 through our driver**, visible
  and responsive in the user's SPICE viewer.  Display path (option B, no host change): Triton present hook
  (tritonDxgi.c, VGPU_DXVK_INPROC) -> vgpuFbPresent (backend): CopySubresourceRegion primary -> CPU staging,
  Map, escape VIOGPU_FB_PRESENT -> KMD VioGpuVidPN::PresentToFrameBuffer copies into the 2D framebuffer
  (m_pFrameBuf, guest RAM), TRANSFER_TO_HOST_2D, classic SET_SCANOUT (once), RESOURCE_FLUSH; blob scanout
  disabled once active (m_FbPresentActive).  Cost: GPU->CPU readback + 2 memcpy of the frame per present.
  With the driver installed: d3d11test passes, vkcube renders in a window on the composed desktop
  (~58 fps windowed), DWM stable.
  Next (step 5/6): stability soak + reboot with the driver installed, resize/modes, A8 shared textures,
  32-bit UMD, Fusion DirectX, OpenGL system-wide, performance of the fb path (dirty rects, async copy).
- Fusion 360 (DirectX 11 mode) on the WDDM stack, 2026-11-07:
  * OGS initialises on "vgpu VirtIO GPU 3D (WDDM)"; Qt UI renders correctly.
  * Host ring fatal (vkQueueSubmit2 CS error / DEVICE_LOST) = NVIDIA Xid 69 (graphics class error, class
    c997, offset 0x274) from rendering into LINEAR shared images (works with VN_VGPU_NO_LINEAR=1).
    Fixes: Venus LINEAR rewrite only for 1-sample/1-mip/1-layer 2D colour images (others not shareable,
    export dropped from their memory); Triton (VGPU_DXVK_INPROC) keeps every windowed back buffer private
    + optimal and presents through a shared LINEAR companion (GPU copy at present).  Also: Venus no longer
    advertises VK_KHR_win32_keyed_mutex and strips win32 export info from host semaphore/fence creates.
  * Viewport still black: classic (blt-model) windowed presents go through the KMD GenerateBltPresent
    virgl copy (src = our Venus companion, dst = DWM's coherent redirection surface), which cannot read
    Venus memory.  SwapEffectUpgradeEnable=1 (HKCU DirectX UserGpuPreferences) set; Fusion still blts.
    Giving the virgl SET_TYPE import the real LINEAR modifier HUNG THE WHOLE VM (host GL copy) -> reverted.
    Next: blt presents via CPU readback into a guest-coherent source allocation (KMD's coherent path).
  * Diagnostics left on for the user account: VN_DEBUG=result,init, VN_PERF=no_async_*, MESA_LOG_LEVEL.
    Venus logs a rejected submit's contents (vn_queue_submission_dump).

### Blt-model windows (Fusion viewport) visible (2026-11-07)
Blt-model swapchains (DXGI DISCARD/SEQUENTIAL, e.g. the Fusion 3D viewport) were black. The UMD (`vgpuBltSource`)
copies each frame into a per-swapchain coherent TYPE_3D allocation; the KMD then blts it into DWM's redirection surface
with virgl transfers/copy. Two bugs:
1. The allocation had no GPU VA. Under GpuMmu the KMD attaches a TYPE_3D allocation's guest pages to the host resource
   only from UPDATE_PAGE_TABLE, so the host resource had no backing. vrend reports a missing iov as
   `vrend_renderer_transfer_iov: ... Illegal resource N` (check_transfer_iovec), not as a backing error.
   Fix: `pfnMapGpuVirtualAddressCb` right after MakeResident.
2. The allocation carried no lookup cookie, so DxgkDdiOpenAllocation resolved it through the KMT handle map. A stale
   entry with the same handle value (another process's 1281x1032 shared blob) won, and the present blted the wrong
   surface. Fix: `VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX` with a per-process cookie (bit 31 set, apart from Venus'
   counter). Any UMD-created allocation without a cookie has the same risk.
Result: blttest shows its orange clear; Fusion's viewport renders (grid, axes, ViewCube) through DXVK on the 4090.
The frame takes a GPU->CPU staging copy plus a CPU copy, so this path is correct but not fast.

Fusion benchmark rerun on the system-wide driver (2026-11-07; same 2,547,485-triangle scene + 72-step orbit;
Fusion Graphics = DirectX -> vgpu_d3d11 (Triton DDI + DXVK) -> Venus -> RTX 4090, no per-app DLLs involved):
- run 1: avg 58.6 ms, median 50.3, p90 84.5, ~17.1 fps; run 2: avg 73.6 ms, median 69.9, p90 106.1, ~13.6 fps
  (2 docs open). ~10-12x faster than WARP, ~3.5-4x slower than per-app OpenGL/Zink: every windowed frame takes
  the blt path (GPU->staging readback, CPU swizzle, CPU copy, virgl transfer into the DWM redirection surface).
