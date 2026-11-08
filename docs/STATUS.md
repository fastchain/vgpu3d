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

### Per-frame GPU wait: 25-30 ms -> 3 ms (2026-11-07)
Stage timers (vgpuPresentStats / vgpu_blt_stats in src/umd3d/vgpu_triton_backend.c, logged every 600 frames)
showed the frame copy costs only ~4 ms. Every GPU wait cost 25-30 ms, even for blttest (a bare clear).
Cause: Venus waits by polling (vn_relax: yields, then sleeps of 160 us << n), and Mesa's os_time_sleep is
Sleep(ms) on Windows, which rounds each step up to the 15.6 ms scheduler tick. Fix in vn_common.c: a
per-thread CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer for the relax sleep. Also: windowed blt presents
no longer arm and wait on the present fence (the staging Map in vgpuBltSource already waits for the GPU).
Fusion benchmark (same scene/orbit): 13.4-17 fps -> 48-64 fps (avg 15.6-20.8 ms), level with per-app
OpenGL/Zink (16.8 ms). blttest present interval 40 ms -> 14 ms. Regression: sharetest, d3d11test,
vkcompute, vkcube pass; host log clean (only DXVK descriptor-pool growth warnings).
Benchmark scripts: tools/fusion-bench/bench.sh load|check|orbit (MCP tunnelled over the VM ssh).

### 4K support (2026-11-07)
- Framebuffer segment minimum 16 MiB -> 64 MiB (VIOGPU_FRAMEBUFFER_SIZE, system memory; BAR 0 is 16 MiB).
- VioGpuVidPN::ModeFits hides modes whose 32 bpp framebuffer does not fit or that exceed
  VIOGPU_MAX_MODE_W x H (5120x2880, also the scanout helper surface size); host-resize/custom modes that
  do not fit are ignored instead of blacking the desktop. Offered now: up to 3840x2160, 4096x2160, 5120x2160.
- Blt presents into DWM's redirection surface are copied on the CPU in the KMD (VioGpuAllocation::CpuBltTo,
  kernel mapping of the GpuMmu-reported pages) instead of TRANSFER_TO_HOST + host copy + TRANSFER_FROM_HOST.
  Falls back to the virgl path for blobs, primaries or non-resident pages.
- Fusion benchmark maximized at 3840x2160 (viewport 3840x1822): 17 -> 31 fps. Maximized at 2560x1080: 66-69 fps.
  4K remaining cost: GPU readback incl. frame render ~11 ms, CPU swizzle copy ~4-5 ms.
- Tools: src/umd/modes (list / switch modes), tools/windows/fusion-4k-job.ps1; VGPU_STATS_EVERY=<n> env var
  sets the UMD stats interval (default 600).

### 32-bit (WOW64) app support (2026-11-07)
- New 32-bit user-mode stack in the same package/DriverStore folder, same 64-bit KMD:
  vgpu_d3d11_32.dll (build_umd.sh 32) + dxvk32_d3d11/dxvk32_dxgi (build_dxvk.sh 32),
  vulkan_virtio32.dll + virtio_icd32.json (build_triton_venus.sh 32), libgallium_wgl32.dll (build_gl.sh 32).
  INF: UserModeDriverNameWow, VulkanDriverNameWow, OpenGLDriverNameWow/VersionWow/FlagsWow.
- tests/abi32/check.sh: DWARF layout of every wddm_hw.h struct identical for i686 and x86_64 (138 members).
- Tests: d3d11test32, vkcompute32, blttest32, sharetest 32->32, 32->64 and 64->32 all pass; gltest/gltest32
  with the system opengl32 (ICD, no app-local DLLs) report zink on the 4090.
- Mesa fixes: driconf executable match case-insensitive on Windows (Heaven.exe never matched heaven.exe, so
  its "#extension mid-shader" workaround was off and every material shader failed -> white window);
  Heaven/Valley on Windows also get disable_blend_func_extended.
  OPEN ISSUE: dual-source blending (ARB_blend_func_extended) renders lit geometry black on zink/Venus.
- Unigine benchmarks, 32-bit binaries, 1920x1080 windowed, QUALITY_HIGH, Phoronix automation define
  (tools/windows/unigine-run.ps1, mid-run screenshot checked for each):
    Heaven 4.0  DirectX 11: 48.4 FPS   OpenGL: 95.7 FPS
    Valley 1.0  DirectX 11: 52.0 FPS   OpenGL: 76.8 FPS
  The guest+host CPU path (DXVK/Zink translation, Venus encode/decode, blt copies) is the limit, not the GPU
  (host nvtop: ~200% CPU, 6-8% GPU).
- Lesson: after a toolchain reinstall, partial rebuilds mixed old and new objects -> 64-bit dxvk_dxgi.dll
  failed DLL init (1114). Clean-rebuild build dirs after any toolchain change.

### Speed-up step 1b + driver restart fix (2026-11-07)
- Framebuffer present uses DWM's dirty rects (bounding box) and one virtqueue kick for upload+flush.
  Heaven at a 5120x2160 desktop: DX11 35.6 -> 60.3 fps, OpenGL 55.5 -> 72.7 fps.
- GPU scanout (SET_SCANOUT_BLOB) under egl-headless froze the VM on the NVIDIA host: QEMU imports the
  dma-buf with DRM_FORMAT_MOD_INVALID. Kept behind HKLM\SOFTWARE\vgpu\Scanout=blob, off.
- Driver restarts broke the host renderer until QEMU restarted (virtio_gpu_virgl_reset() aborts while a
  deferred blob destroy is pending). Fixed: unmap orphaned blobs before unref, drain the control queue
  before the virtio reset. DXVK Singleton no longer returns null after a failed instance creation.
- Step 2 (asynchronous blt present, PRESENT_WAIT + KMD CPU_BLT) works but stays OPT-IN
  (HKLM\SOFTWARE\vgpu\BltAsync=1). The first version waited on the thread's present token, which is
  always 0 in our stack (DXVK's ID3D11Fence never reaches the KMD); now VIOGPU_BLT_ASYNC hands out a
  token and a thread-pool wait on the readback fence sends VIOGPU_BLT_READY. Correct output, no stalls,
  but Heaven DX11 only 60.3 -> 62.0 fps: the KMD commander runs packets one at a time and a parked
  PRESENT_WAIT holds its slot, so the next frame's submissions wait behind it. Real gain needs the
  commander to execute later packets while an earlier present is parked (complete in order only).
- Tried: a parked PRESENT_WAIT releasing the commander's running slot (later packets execute while it
  waits, completions still in fence order). Heaven DX11 unchanged (61.4 fps) -> reverted. With the async
  blt path the app now waits in the present pacing (<= 3 frames ahead of the GPU): the guest->host GPU
  pipeline (DXVK conversion, Venus encode, host decode, GPU) completes one Heaven frame per ~16 ms.
  Next: profile that pipeline (guest WPR on the app's DXVK threads, host perf on virgl_render_server).

### Guest profiling of Heaven DX11 (2026-11-07)
- WPR CPU trace (20 s mid-run): Heaven main thread busy 28% (0.28 core), DXVK conversion thread 0.11 core,
  whole process 0.7 core; the VM is 96.5% idle. Main thread is woken ~61x/s, mostly by DXVK's worker and
  submission threads: one pipeline wait per frame. The guest CPU is not the bottleneck.
- blttest (trivial clear): 980 presents/s with sync interval 0, 60 with interval 1 -> the copy path is cheap.
- Heaven DX11 tracks DWM's composition rate: 72.5 fps at a 2560x1080 desktop (DWM ~72-75), 60-62 fps at
  5120x2160 (DWM ~60), also with -video_vsync 0 and with the vsync source forced to 120 Hz
  (HKLM\SOFTWARE\vgpu VsyncHz, experiment knob). Old-style windowed presents are paced by DWM, and DWM's
  own frame at 5K takes ~16 ms while using little CPU: it is latency-bound on the guest->host GPU round
  trip as well. Next lever: DWM's per-frame GPU round trip (host-side latency: perf on the render server).

### Guest-only speed-up round (2026-11-07, host changes ruled out)
- Correction to the two sections above: the 32-bit UMD read HKLM\SOFTWARE\vgpu through the WOW64 view,
  so BltAsync/Scanout never reached 32-bit apps. The "async blt 60.3 -> 62.0" Heaven result was really
  the synchronous path. Fixed (RRF_SUBKEY_WOW6464KEY). And Heaven DX11 was not DWM-paced: per-frame
  timing showed 7.3 ms of each 16.7 ms frame waiting for the synchronous readback of its own frame.
- New per-present timing (UMD "present tail": time in the framebuffer copy and in the runtime's present
  callback; DWM's own present costs ~3.4 ms of its ~15 ms frame at 5K).
- Pipelined blt presents (default on, HKLM\SOFTWARE\vgpu BltPipeline=0 turns off): after 8 presents less
  than 50 ms apart, a windowed blt present ships the previous frame, whose readback has finished, instead
  of waiting for the current one (one frame of latency; isolated presents stay synchronous).
  Heaven DX11 at a 5120x2160 desktop: 58.7 -> 88.5 fps (readback wait 7.3 ms -> 0.4 ms). Run-to-run
  noise in later runs is about +-5 fps (Defender scans, host load).
- KMD async blt (BltAsync=1) now really active for 32-bit apps: Heaven 29.7 fps with 1.2-1.5 s present
  stalls (tokens retiring only at the KMD deadline) and the KMD CPU copy slowing DWM. Stays off.
- Asynchronous framebuffer present (default on, FbAsync=0 turns off): the scanout present queues the
  readback into one of 3 persistently mapped textures, signals a private fence and returns; a worker
  thread waits for the fence and does the escape. A queued frame the worker has not started is
  replaced by a newer frame that covers its box (sync-interval-0 semantics). Windowed: no measurable
  change (DWM's rate is not set by our present). Exclusive fullscreen at 5120x2160 (blttest, 64- and
  32-bit): 31 -> 77 fps app rate; the display shows what the host can upload (~15 ms per full 44 MB
  frame on the host side: queue+kick in "fb kmd" stats), and flips are paced by the 75 Hz refresh.
- DWM uploads a blt-model app's whole window with UpdateSubresource every frame (8 MB at 1920x1080):
  its present has no destination, so Windows uses the GDI-style redirection bitmap. Avoiding that needs
  DXGI to use a DX redirection surface for these swapchains (not investigated yet).
- VsyncHz=120 does not lift DWM or fullscreen pacing reliably (mode stayed 75 Hz after a driver
  restart); no default change. At 5K the host upload limits full frames to ~45/s anyway.
- Known issue: Heaven DX11 in exclusive fullscreen fails: the host logs "vkQueueSubmit2 resulted in CS
  error" (a submit references an object the host does not know), the app's Vulkan queue hangs.
  blttest fullscreen works. Not yet diagnosed (host gives no more detail).
- Superposition 1.1 (engine arguments, 1920x1080 windowed, shaders/textures quality 2, DX11) at the
  5120x2160 desktop: avg 69.8 fps (min 35.4, max 107.5) over 17 scenes. At 1280x720 the HUD showed avg
  117.3 fps pipelined vs ~72 with BltPipeline=0 at the same point. One mid-run frame showed a black streak
  over a wall poster (possible rendering glitch, to check). superposition_cli.exe exits silently here.
- Geekbench 6 free CLI runs only the CPU test (no GPU compute without Pro/GUI); not used.

### Bug-fix round and Direct3D 12 (2026-11-08)
- OpenGL dual-source blending (Heaven/Valley black scene): the WGL target created the Zink screen without a
  config, so none of Zink's driconf options (dual_color_blend_by_location for Unigine) applied on Windows.
  wgl.c now hands Zink its option table; the disable_blend_func_extended workaround is gone. Heaven/Valley
  GL render correctly with dual-source blending (67 fps each at the 5K desktop).
- FurMark 2 Vulkan (VK_ERROR_FEATURE_NOT_PRESENT): a Vulkan 1.1 app chained VkPhysicalDeviceVulkan12Features;
  Venus queries a <1.2 renderer through the extension structs, which leave the 1.2 "extension supported"
  members (descriptorIndexing, drawIndirectCount, ...) false. Derived from the extensions now.
  FurMark Vulkan 1080p: 7882 (130.9 fps avg).
- GravityMark OpenGL (ARB_gl_spirv): four Mesa fixes -- image queries on an OpTypeSampledImage operand
  (Tellusim compiler), snorm/unorm pack/unpack in Zink's SPIR-V emitter (an assert in a compile thread showed
  a hidden message box and deadlocked the app), nir_opt_cse merging derefs of different sizes, and
  util_queue_destroy joining threads under the loader lock (opengl32.dll unloaded by the app ->
  deadlock at exit). GravityMark OpenGL: 11064 (66.2 fps), exits cleanly (4 of 5 runs; one earlier
  timeout not reproduced).
- Superposition D3D11 black patches: confirmed a D3D11-only rendering bug (OpenGL renders the same scenes
  correctly). Not fixed yet; needs frame capture.
- KMD async blt (BltAsync=1) stalls: root cause is a dependency cycle -- DXVK submits the readback copy
  from its own thread after the PRESENT_WAIT packet entered the single-slot commander, so the fence it
  waits for queues behind it until the 1.2 s deadline. Stays off (the pipelined path replaced it).
- Exclusive-fullscreen Heaven D3D11: the app's GPU work never completes in fullscreen (with async Venus
  calls the host also reports "vkQueueSubmit2 resulted in CS error"). Not fixed yet. Test-environment
  note: after a reboot, windows started by the scheduled task cannot take the foreground and DXGI refuses
  fullscreen (DXGI_ERROR_NOT_CURRENTLY_AVAILABLE / STATUS_OCCLUDED); C:\vgpu\fg.ps1 raises the window.
- Direct3D 12 (M3): Triton's D3D12 DDI layer is built into vgpu_d3d11[_32].dll (OpenAdapter12) and drives
  vkd3d-proton v3.1 (vkd3d_d3d12core.dll / vkd3d32_d3d12core.dll, build/build_vkd3d.sh) in-process, created
  through its D3D12GetInterface device factory with DXVK's DXGI adapter. Patches: shared heaps export KMT
  handles (the only kind our Venus exports) via a vkd3d_vgpu_resource_kmt export; the backend shares them
  like DXVK textures. The queue gate waits on the CPU for the drain fence (no host to retire a kernel
  token in-process). Host caps advertise DXIL, so SM 6.x shaders are accepted.
  Results: d3d12test clear/readback (64- and 32-bit) and flip-model present pass; GravityMark D3D12
  22313 (133.6 fps, renders correctly); PassMark DirectX 12: 27.

### Task list round (2026-11-08)
- Re-measured on the current driver: GravityMark DX11 10369 (62.1 fps, was 6574 before the pipelined
  present), Superposition OpenGL avg 56.4 fps (DX11 69.8).
- CPU-only (Microsoft Basic Display / WARP): GravityMark refuses to start ("no Direct3D11/12 devices",
  no GL 3+ context) and Heaven aborts with out-of-memory, so no CPU-only benchmark numbers exist; Fusion's
  1.4 fps stays the only one. The rollback also removed the ~27 old vgpu3d driver packages.
- Superposition DX11 black patches: also with BltPipeline=0, with DXVK descriptor buffer/heap and graphics
  pipeline libraries off, and with forced compute/UAV barriers -- none of those is the cause. Needs a frame
  capture.
- D3D12 hang and GravityMark GL exit: d3d12test 5/5 clean; GravityMark GL exit hung in 3 of 11 runs after
  the loader-lock fix (not caught with a debugger yet).
- D3D12 queue gate: a UMD-fired kernel gate (worker thread firing a VIOGPU gate token at the drain value)
  made the parked GATE packet stall the work it waits for -- 107 gates hit the 1.2 s deadline, GravityMark
  D3D12 133 -> 17 fps. Reverted to the CPU wait (GravityMark D3D12 20471 / 122.5 fps).
- Kernel-side async blt present removed (UMD ring, BLT_ASYNC/BLT_READY escapes, CPU_BLT, BLT_JOB); the
  numbers stay reserved in the ABI.  VsyncHz experiment knob removed.
- Direct3D 9: works with no driver code -- Windows routes D3D9 to D3D9On12 on our D3D12 driver (no D3D9
  UMD).  d3d9test 64/32-bit passes (clear/readback/present); PassMark DirectX 9: 37.
- OpenCL: blocked.  Microsoft's Compatibility Pack (OpenCLOn12) registers Dozen as a Vulkan driver; Vulkan
  loader -> Dozen -> D3D12 -> our UMD -> DXVK/vkd3d -> Vulkan loader -> Dozen deadlocks every Vulkan app
  (VK_LOADER_DRIVERS_DISABLE did not stop it).  The pack was removed again.  A native route would be
  Mesa rusticl on Zink (Rust/LLVM/libclc cross toolchain).
- Fullscreen Heaven: fullscreen blttest drawing triangles (also through an sRGB RTV) works, and Heaven
  windowed at 5120x2160 works -- so not linear primaries, sRGB views or memory size.  Next: the Vulkan
  validation layer in the guest.
- The KMD now logs pid and first word of short escapes ("buffer too small") to name the sender.

### Release round: installer and README (2026-11-08)
- Windows installer: `build/build_installer.sh` wraps the staged package in an NSIS installer
  (`src/installer/vgpu3d.nsi`) -> `out/vgpu3d-setup-<version>.exe`, ~20 MB. It checks Windows 11 x64 and
  the virtio-gpu PCI device, unpacks to Program Files, runs install.ps1 (64-bit PowerShell via sysnative),
  registers an Apps & features uninstaller (uninstall3d.ps1 -RemoveCert), and handles test signing with a
  reboot plus a RunOnce resume. install.ps1 exit codes: 0 / 1 error / 3 test signing just enabled /
  3010 reboot needed (pnputil 3010, or the adapter left at Code 14 while pnputil returned 0; pnputil 259
  "no device updated" counts as success).
  Tested in the VM: silent install over an existing install, uninstall (all packages + certificate
  removed, Basic Display), fresh reinstall (new certificate), GUI run with reboot, upgrade.
- Code 39 after uninstall + reinstall: uninstalling within 45 s of an adapter start left
  VgpuBootGuard=1 in the service key, so the next DriverEntry refused to load (0xC0000365). install.ps1
  now resets the guard before pnputil; the KMD also clears it in its unload path (not observed to run
  when the adapter falls back to Basic Display, so the installer reset is the effective fix).
- OpenGL exit hang (dsbtest reliably, GravityMark GL 3/11): the earlier u_queue loader-lock change waited
  for worker threads to leave the queue code, but at ExitProcess they are already terminated, so
  DLL_PROCESS_DETACH spun in Sleep(1) forever. It now also stops when all killed threads' handles are
  signaled. dsbtest 64/32-bit exits cleanly 5/5 (hung every run before). GravityMark GL not re-run yet.
- First release-build (`package_kmd3d.sh release`) smoke round: d3d12test, d3d9test (64/32), vkcompute2,
  dsbtest (64/32), blttest pass.
- README rewritten for the release (use cases, API table, benchmarks, host and guest steps).

### Release build without debug overhead (2026-11-08)
- Kernel driver: trace.h hard-codes `DBG 1` (no WPP), so `/DDBG=0` never removed anything and the INF's
  DebugLevel=4 (= TRACE_LEVEL_INFORMATION, not "warnings") kept ~65 prints live, several per frame or per
  paging operation (BuildPagingBuffer fill/discard/residency, blt present, scanout flush, ring full).
  Release (NDEBUG) now caps the compiled-in level at ERROR, so the rest is compiled out (viogpu3d.sys
  204800 -> 171520 bytes), and `package_kmd3d.sh release` sets the packaged INF to DebugLevel=2.
- Venus and Zink were debugoptimized with asserts (and NIR validation after every pass in Zink).
  `VGPU_RELEASE=1 build_triton_venus.sh / build_gl.sh [32]` build NDEBUG variants in out/*-build-rel ->
  out/*-rel; the release package uses those. Debug packages are unchanged.
- DXVK: default log level none (was info, which wrote <app>_d3d11.log/_dxgi.log into app folders);
  DXVK_LOG_LEVEL turns it back on.
- Release smoke round passes (d3d12test, d3d9test 64/32, vkcompute2, dsbtest 64/32, blttest; no DXVK
  log files). The before/after benchmark is still open: since ~12:50 GravityMark fails in every API with
  host "vkr: failed to look up object N of type 8" (VkDeviceMemory) on vkBind*Memory2 and aborts on the
  fatal ring, with the debug package too and after a guest reboot. The host had 119 of 125 GB RAM in use
  (108 GB anonymous, processes outside the container), so host allocations failing is the likely cause;
  re-run the comparison once host memory is back.

### Benchmark round, fullscreen, OpenCL probe (2026-11-08)
- tools/windows/fg.ps1 sent a synthetic Alt key (keybd_event) to lift the foreground lock. GravityMark then
  rendered with its window never shown and reported inflated scores (D3D11 37644, D3D12 27973): presents
  were not composited. fg.ps1 now uses no input (foreground lock timeout 0 + AttachThreadInput +
  SwitchToThisWindow). Any earlier benchmark or fullscreen result taken through fg.ps1 is suspect.
- Release vs debug package, GravityMark 1.89 1920x1080 window on a 3840x2160 desktop, screenshot-checked,
  two runs each (mean): D3D12 25105 vs 24696, Vulkan 17249 vs 17862, OpenGL 12684 vs 13105, D3D11 11516 vs
  11860. Debug varied up to 7.6% between its own runs, so the difference is noise; GravityMark is not
  limited by the debug prints/asserts. All APIs are well above the older README numbers (host RAM was
  nearly full earlier in the day, see the release-build notes).
- Exclusive fullscreen 3840x2160: GravityMark works in every API (D3D12 12587 / Vulkan 5539 / OpenGL 5295 /
  D3D11 4767; D3D11 28.9 fps also with FbAsync=1). blttest draw in fullscreen works 32- and 64-bit (63 fps).
  Unigine fails: Heaven D3D11 (32-bit) aborts after host "vkQueueSubmit2 resulted in CS error" with
  FbAsync=1 and hangs with FbAsync=0 -- the main thread waits in vgpuFbPresent's staging Map
  (vgpu_triton_backend.c:1022), dxvk-queue waits on a Venus fence that never signals, right after
  "present-fence: armed"; same at 1920x1080 (mode change). Superposition D3D11 (64-bit) also hits the CS
  error. The Khronos validation layer (SDK 1.4.363, registered under HKLM\SOFTWARE\Khronos\Vulkan\
  ExplicitLayers because the loader ignores VK_LAYER_PATH in elevated processes) reported nothing in one
  Superposition run, but that run may not have reached exclusive fullscreen. Open.
- GravityMark OpenGL exit: 6/6 clean exits after the u_queue ExitProcess fix (was 3/11 hangs).
- install.ps1 now stages the package, removes every other vgpu3d.inf package, then installs: Windows keeps
  the highest DriverVer, so installing an older build over a newer one silently changed nothing (and each
  install left another copy in the DriverStore; the VM had 6). Downgrade tested.
- OpenCL option 1 (OpenCLOn12 without Dozen): mesa-dist-win 26.2.6 openclon12.dll enumerates our adapter
  ("OpenCLOn12: vgpu VirtIO GPU 3D (WDDM)") but is a Debug build (MSVCP140D/ucrtbased) and crashes in
  clCreateContext on a null mutex -- also on the WARP device, so it is that binary, not our D3D12. Building
  OpenCLOn12 needs MSVC. Moving to option 2 (clvk). build/build_opencl_loader.sh cross-builds the Khronos
  ICD loader (OpenCL.dll, 64/32-bit); cltest now checks every step and takes CLTEST_DEVICE=<n>.
- DirectML works on our D3D12: src/umd/dmltest (build/build_tests.sh) creates a DirectML device through
  System32 DirectML.dll (1.15.6) on the vgpu adapter and runs an element-wise add of 65536 floats, 0
  mismatches. MinGW note: COM methods returning structs > 8 bytes (IDMLDispatchable::GetBindingProperties)
  pass the hidden result pointer in a different position than MSVC; dmltest calls that vtable slot directly.
- OpenCL works (option 2, clvk): build/build_clvk.sh cross-builds clvk + clspv + LLVM with MinGW in two stages
  (native clspv build for the LLVM/clang tablegens and libclc, ~4 min; Windows build ~6 min on 32 cores) ->
  out/clvk/OpenCL.dll (102 MB, imports only system DLLs + vulkan-1). clspv's dllexport disables MinGW's
  export-all, so the cl* entry points come from a .def generated from api.cpp.obj; one clvk source fix
  (Windows.h case, patches/clvk-vgpu.patch). Vulkan import library from the guest's vulkan-1.dll (gendef).
  The INF ships clvk.dll and sets OpenCLDriverName; the installer installs/upgrades the Khronos loader in
  System32 (and removes it on uninstall only if it added it). cltest through the installed loader:
  "clvk: Virtio-GPU Venus (NVIDIA GeForce RTX 4090)", vector add of 1M floats, 0 mismatches. 64-bit only.
- clpeak (bundled with PassMark as clpeak64.exe) on clvk: single precision 2983-3828 GFLOPS, half 3283-3898,
  int 1559-1943 GIOPS; global memory bandwidth only 10-12 GB/s and transfers 12-16 GB/s. clvk reports 1
  compute unit and no profiling (clGetEventProfileInfo -7). Likely next steps: buffer memory type selection
  under Venus (bandwidth suggests system-memory backing), compute-unit count, timestamp queries. One host
  "vkDestroyDescriptorPool resulted in CS error" at clpeak exit.
- PassMark GPU Compute (G3D_DIRECTCOMPUTE): Mandelbrot/QJulia4D (DirectCompute) run (QJulia4D ~620-690 fps),
  then PerformanceTest64 waits indefinitely with no subtest process alive and nothing in our DLLs on any
  stack -- not diagnosed (its own orchestration; subtest exes need PassMark's arguments).
- Fullscreen Unigine remains open (see above); the validation layer is installed in the VM for the next try.

### Exclusive fullscreen fixed (2026-11-08)
- Cause: in-process DXVK builds rendered exclusive-fullscreen apps straight into the scanout primary,
  which Triton creates pitch-LINEAR (shared, LINEAR_EXPORT) for blob scanout. NVIDIA cannot use linear
  colour targets in every configuration (the same Xid 69 class Triton already avoided for windowed back
  buffers); Unigine's first fullscreen frame hung the host GPU channel silently: a Venus submit trace
  (new VN_VGPU_TRACE_SUBMIT) showed the 14th submission, with no semaphore waits, never completing.
  Fix (tritonResource.c): with the framebuffer-copy scanout the app renders into an optimal private
  texture and the linear primary only keeps the KM allocation as the present companion, synced only
  for blob scanout (tritonDxgi.c).
- Results, 3840x2160 desktop: Heaven DX11 exclusive fullscreen 3840x2160 72.0 fps, 1920x1080 (mode change)
  187.2 fps, both complete; Superposition fullscreen presents 100-114 fps with real frame content (centre
  pixels logged by the fb worker); GravityMark D3D11 fullscreen unchanged (28.0 fps).
- Test-environment traps found on the way: a killed DbgView left DBWIN_BUFFER owned so OutputDebugString
  blocked every caller (blttest "hung" in OpenAdapter10_2); GDI screenshots of a blt-model exclusive-
  fullscreen app show DWM's black desktop; fg.ps1 now keeps raising the app's largest window (Superposition
  opens a console first).

### Superposition D3D11 black patches: narrowed, still open (2026-11-08)
- What it is: D3D11 renders sun-shadowed areas pure black and the whole image greyer -- the indirect /
  environment lighting is missing. Superposition's OpenGL renderer (Zink, same Venus) shows the warm
  bounce light and dim (not black) shadows at the same moments. Measured as the share of near-black
  pixels in the lower frame: D3D11 4-12 %, OpenGL 0-1 %.
- Not Triton's DDI layer: the same patches with DXVK loaded app-local (d3d11.dll/dxgi.dll next to the exe,
  straight on Venus). Not Venus async/batching (VN_PERF no_async_*,no_cmd_batching,no_multi_ring) nor its
  feedback paths. Not feature exposure: src/umd/vkprops shows every feature/format DXVK relies on
  (typed UAV loads without format, cube arrays, robustness2 null descriptors, custom border colours,
  BC6H/BC7/R11G11B10/RGBA16F). DXVK options without effect: d3d11.zeroWorkgroupMemory,
  d3d11.enableRtOutputNanFixup, d3d11.cachedDynamicResources=vicra, dxvk.enableUnifiedImageLayouts=False,
  dxvk.enableNvRawAccessChains=False, dxvk.enableMemoryDefrag=False (earlier: descriptor buffer/heap, GPL,
  BltPipeline). DXVK's NVIDIA-keyed workarounds are off under Venus's driver ID, but the host driver
  (615.78) is past their version limits except renderPassResolveFormatBug (MSAA only).
- Launch-time console commands (render_ssao/ssgi/shadows 0) are overridden by the preset, so per-effect
  bisection needs another route. Next: a RenderDoc capture of one frame to find the pass or texture
  that comes out zero.

### clvk memory bandwidth (2026-11-08)
- Why clpeak sees 10-12 GB/s: clvk allocates every buffer from a host-visible memory type and prefers
  HOST_CACHED ones (it was written for UMA/mobile GPUs), and our Venus hides host visibility on all
  non-cached types (KVM maps host RAM write-back in the guest while the host GPU accesses uncached memory
  unsnooped). The only host-visible type left (memory type 3) lives in the 96 GB system heap, so kernels
  read system memory over PCIe. The 4090's resizable-BAR type (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT,
  24 GB) exists on the host and appears with VN_VGPU_ALLOW_UNCACHED=1.
- Tried: a clvk option buffer_prefer_device_local (patches/clvk-vgpu.patch) that puts buffers into that
  type. With VN_VGPU_ALLOW_UNCACHED=1 the kernel compiles but the first kernel never completes (host VRAM
  mapped into the guest through hostmem does not work as a CL buffer path here). The option stays, off by
  default.
- Real fix: device-local (not host-visible) buffers in clvk with staging copies for map/read/write/
  fill/copy-host-ptr -- a larger change to clvk's buffer paths, not done yet.
