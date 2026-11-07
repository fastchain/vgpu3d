# Windows Guest GPU Driver for QEMU/KVM (virtio-gpu) — Build Plan

Goal: a GPL-licensed, signable Windows 10/11 guest driver for QEMU's virtio-gpu
device that provides Vulkan, OpenGL and Direct3D acceleration through
virglrenderer (virgl + Venus) on the host. Reuse the VirtualBox Guest Additions
WDDM driver where it helps.

## 1. What to reuse and what to leave

### Reuse from VirtualBox (GPLv3, `src/VBox/Additions/WINNT/Graphics/Video/`)
- **VBoxWddm miniport** (`mp/wddm/`): complete shipping WDDM display driver for a
  paravirtual device. DxgkDdi entry points, VidPn and mode enumeration,
  allocation/paging callbacks, present and submission scheduling, PnP/power,
  multi-monitor, hotplug, UMD<->KMD escape framework. The hardest part of any
  Windows GPU driver; the main reason to start here rather than from scratch.
- **Gallium-aware kernel path** (`mp/wddm/gallium/`): models what a Mesa UMD
  needs from the kernel (contexts, surfaces, fences, command buffers). Maps
  closely to virtio-gpu semantics.
- **UMD winsys glue** for the Gallium drivers (VBoxGL OpenGL ICD, VBoxNine):
  shows how to run Mesa Gallium inside a Windows UMD on top of D3DKMT calls.
  Template for a virgl winsys and a Venus winsys.
- **VBoxDX** (`disp/wddm/dx/`), the Direct3D 10/11 UMD: DDI front end reusable
  later (phase 7); SVGA3D back end is not.

### Reuse from virtio-win (BSD / GPLv2)
- Shared VirtIO library: PCI transport, virtqueues, interrupt handling.
- Existing `viogpu` display-only driver: virtio-gpu 2D command encoders,
  modeset and cursor handling, INF.

### Reuse from upstream (MIT)
- Mesa: virgl Gallium driver, Venus Vulkan driver, WGL frontend, Vulkan ICD
  loader support.
- DXVK and vkd3d-proton for Direct3D 9-12 on Vulkan.

### Leave behind
- HGSMI, VBVA, VMMDev, VBoxGuest and HGCM dependencies.
- VMSVGA FIFO and SVGA3D command encoding.
- VirtualBox's Mesa fork and kBuild. Use upstream Mesa + Meson and the EWDK.

## 2. Phases

### Phase 1 — Feasibility and code mapping (3-4 weeks)
- Build VBoxWddm from source, test-sign it, confirm it runs in VirtualBox.
  Build viogpu and Mesa for Windows. Document the build chains.
- Map every VBoxWddm dependency on HGSMI, VBVA and VBoxGuest. Produce a
  keep / strip / rewrite list per file.
- Verify the host: QEMU with `virtio-gpu-gl`, blob resources and hostmem
  enabled, virglrenderer built with Venus. Validate with a Linux guest so later
  bugs are known to be guest-side.
- License audit. The combined kernel driver will be GPLv3, so it lives outside
  the virtio-win repository.
- Decide the UMD<->KMD ABI: mirror the Linux virtgpu ioctls (resource create,
  map, execbuffer, wait, get caps) as WDDM escapes so Mesa's existing virtgpu
  winsys logic ports with minimal change.
- Start the Microsoft Partner Center / EV certificate paperwork for signing.

### Phase 2 — Kernel miniport, 2D only (2-3 months)
- Fork VBoxWddm, remove VirtualBox transport code, add a HAL backed by the
  virtio-win VirtIO library.
- Modeset, cursor, VidPn and present via virtio-gpu 2D commands:
  RESOURCE_CREATE_2D, ATTACH_BACKING, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH.
- **Milestone:** full WDDM driver replaces viogpu; desktop runs on WARP;
  multi-monitor and resolution changes work.

### Phase 3 — 3D submission path (2-3 months)
- Context creation with capset selection (virgl2 / Venus), 3D and blob
  resource creation, guest memory backing via SG lists, SUBMIT_3D, fences wired
  into WDDM ISR/DPC completion.
- Memory manager: guest system-memory segment plus a host-visible segment
  mapping the virtio-gpu hostmem BAR (needed by Venus for its ring and
  host-coherent allocations).
- Expose virtgpu-style escapes to user mode.
- **Milestone:** test tool creates a context, submits a command stream, waits
  on a fence.

### Phase 4 — Vulkan (1-2 months)
- Windows virtgpu winsys for Mesa Venus using D3DKMT + phase 3 escapes.
  Register the ICD (`HKLM\SOFTWARE\Khronos\Vulkan\Drivers`).
- **Milestone:** vulkaninfo, vkcube, Vulkan CTS smoke subset pass.

### Phase 5 — OpenGL (1-2 months)
- virgl Gallium driver + Mesa WGL frontend with a winsys sharing the phase 4
  transport. Evaluate Zink-over-Venus as the alternative; keep the faster one.
- **Milestone:** OpenGL ICD passes piglit basics, runs Blender.

### Phase 6 — Direct3D via translation (1 month)
- Ship DXVK and vkd3d-proton builds in the guest tools with a per-application
  deployment helper. Gives D3D 9-12 to games and CAD immediately.
- Document that system components (incl. DWM) stay on WARP at this stage.

### Phase 7 — Native Direct3D 11 UMD (4-6 months, highest risk)
- Take the VBoxDX DDI front end and give it a Vulkan back end on Venus. In
  practice: reimplement DXVK's core as a WDDM user-mode driver.
- Alternative to evaluate: a D3D11 front end on Gallium (does not exist
  upstream).
- **Milestone:** DWM reports hardware acceleration; D3D11 apps run without
  DXVK DLLs.

### Phase 8 — Hardening (2 months, overlaps 5-7)
- TDR / timeout detection and recovery, fence timeouts, suspend/resume,
  hotplug, TDR-triggered resets.
- Fuzz the host side: a guest driver bug must not crash QEMU. Run
  virglrenderer's fuzzers against the command streams the driver produces.
- Performance: hostmem blobs, Venus ring tuning, zero-copy present path.

### Phase 9 — Packaging and signing (1 month)
- INF, driver store layout, installer integrated with virtio-win guest tools.
- Test-signing for development; Microsoft attestation signing via Partner
  Center with an EV certificate so the driver loads under Secure Boot.
- Plan HLK runs for the display tests.

### Phase 10 — Upstreaming
- Mesa winsys changes to Mesa. QEMU and virglrenderer fixes upstream.
- Kernel driver in its own GPLv3 repository with CI building against each Mesa
  release.

## 3. Risks and resourcing

| Risk | Note |
|------|------|
| WDDM internals | Even on VBoxWddm, memory manager / scheduler interactions are the main bluescreen source. Budget kernel-debugger time (serial or network KD into the VM). |
| Venus on Windows | Venus assumes Linux-style shared memory for ring and shmem. The hostmem BAR covers this on paper but has never been exercised from a Windows guest. |
| D3D11 UMD (phase 7) | The only phase with no existing template. Treat as a separate project; ship phases 1-6 first. |
| Signing | Attestation signing needs an organisation account and an EV certificate. Start paperwork in phase 1. |

**Team:** two engineers with Windows kernel + graphics driver experience, one
Mesa contributor.

**Timeline:** phases 1-6 roughly 9-12 months; phase 7 adds 4-6 months.

## 4. Next step
Phase 1 design document: list of VBoxWddm files to keep / strip / rewrite, and
the virtgpu escape ABI.

---

## 5. Progress log

### 2026-11-03

* **Phase 1 done.** Sources cloned to `upstream/`; keep/strip/rewrite map and ABI in
  `docs/PHASE1_DESIGN.md`; build toolchain (clang-cl + lld-link + NuGet WDK/SDK on Linux) in
  `toolchain/` and `build/`.
* **Deviation, on purpose:** phases 2 and 3 were re-ordered to get something testable sooner.
  Instead of forking VBoxWddm first, the proven display-only `viogpudo` driver was forked and the
  phase 3 submission path (contexts, blob resources, user mappings, fences, capsets, host-visible
  window) was built on top of it as a `\\.\VGpu3D` control device. The VBoxWddm port (full WDDM,
  native D3D) remains the plan for phases 2/7 and reuses this transport. Reasoning in
  `docs/PHASE1_DESIGN.md` section 1.
* **Delivered, untested on Windows:** `out/vgpu-guest.zip` (driver, INF, install scripts,
  `vgputest.exe`), `WIN11_3D.xml`, `docs/HOST-SETUP.md`. See `docs/STATUS.md`.
* **Next:** first run on the Windows machine, then Stage B (Mesa Venus winsys, Vulkan ICD).
