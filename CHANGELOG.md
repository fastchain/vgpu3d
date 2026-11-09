# Changelog

## 1.0.26313.13703 (2026-11-09)

Installer: `vgpu3d-setup-1.0.26313.13703.exe`. Ideas adopted from the Yttrium project (re-implemented, no code
copied; see README credits).

### Fixed
- **Driver updates and adapter restarts no longer crash apps or the desktop.** Installing a driver update,
  disabling/enabling the adapter or a GPU-timeout recovery used to crash DWM, Explorer, the Start menu and every
  running D3D11/D3D12/Vulkan app. Apps now get the regular "device removed / device lost" error (and can
  recreate their device or exit), DWM keeps running, and new apps start normally right away.
- **Tearing in Vulkan and OpenGL windows** (e.g. GravityMark-Vulkan, FurMark, Zink apps): they ended with a GDI
  blit into the window, which DWM could show half-written (1–5 % of frames). They now present through a
  flip-model DXGI swap chain: 0 mixed frames in tests, about 4 % lower GravityMark score. IMMEDIATE and MAILBOX
  present modes are now offered. `VGPU_WSI_DXGI=0` (environment) or `HKLM\SOFTWARE\vgpu` `WsiDxgi`=0 (DWORD)
  restores the old path.
- Smaller fixes: a deadlock when opening a shared resource failed, an undersized WSI blit buffer when the host
  rounds memory requirements up, async present keeping pointers to the app's damage rectangles.

### Docs
- Turn off Fast Startup / hibernation in the guest (stale GPU state after a "shutdown" that is really a
  hibernate). README lists every bundled library and source with its authors and licence.

### Known limitations
- Older windowed D3D apps with blt-model swap chains running uncapped: 1–4 % mixed frames remain, more under
  heavy host load (unchanged since 1.0.26313.7767; clean with vsync). 3DMark's SystemInfo still shows the GPU as "not available".

## 1.0.26313.7767 (2026-11-09)

Installer: `vgpu3d-setup-1.0.26313.7767.exe`. From this release on, releases are named by the full driver version.

### Fixed
- **Tearing during fast motion.** Each frame is copied into the guest framebuffer and the host copies it out when it
  processes the transfer, which could be after the next frame had started overwriting the same memory: the host
  then showed a frame that was part old, part new. In a GravityMark run up to ~3% of presents hit this. The driver
  now waits for the host to finish the previous frame before overwriting it (on average well under 1 ms).
- **Tearing in older windowed D3D apps** (legacy blt-model swap chains) running uncapped: their frame was copied
  into DWM's window surface while DWM could be reading it on the host GPU (~17 % of frames mixed). The copy now
  waits while DWM composes; ~1–2 % remain. Cost: such apps' *uncapped* frame rate drops (test window ~400 →
  ~120 fps); apps with vsync or capped at the refresh rate are unaffected. Vulkan windows (GDI blit) still tear
  occasionally.
- **3DMark Steel Nomad (D3D12)** no longer fails with `DXGI_ERROR_DEVICE_REMOVED`. Shareable textures in formats
  the host GPU cannot lay out linearly (e.g. block-compressed) made the host reject the image
  (`VK_ERROR_FORMAT_NOT_SUPPORTED`) and vkd3d-proton removed the device; such images are now created optimal and
  not shareable. Steel Nomad DX12 demo: **2295 points (22.96 fps)** on an RTX 4090 host.

### Added
- **Real video memory size.** Windows, DXGI, dxdiag and Task Manager report the host GPU's memory (24 GB on an
  RTX 4090) as dedicated video memory instead of the ~4 GB host-visible window, so apps budget correctly and
  3DMark stops warning about VRAM. The installer measures it through Vulkan (`tools\vgpuvram.exe`) and stores it
  as `VideoMemoryMB` in the adapter's registry key; `install.ps1 -VramOnly` refreshes it after a host GPU change.

### Known limitations
- As in 1.0.26312. 3DMark's SystemInfo still shows the GPU as "not available": it identifies cards by PCI ID, and
  the adapter is a virtio-gpu device.

## 1.0.26312 (2026-11-08)

First release. Installer: `vgpu3d-setup-1.0.26312.exe`, driver version 1.0.26312.24550.

### APIs
- **Vulkan 1.4**: Mesa Venus ICD over the WDDM driver, 64- and 32-bit.
- **Direct3D 10/11** (feature level 11_1): Triton DDI layer with in-process DXVK, 64- and 32-bit.
- **Direct3D 12**: vkd3d-proton through the same DDI layer, 64- and 32-bit.
- **Direct3D 9**: Windows' D3D9On12 on our D3D12 driver.
- **OpenGL 4.6**: Mesa Zink, 64- and 32-bit.
- **OpenCL 3.0** (64-bit): clvk, registered as the adapter's OpenCL driver. Buffers live in device-local
  memory (clpeak: ~600–690 GB/s, 62–68 TFLOPS fp32 on an RTX 4090). The compute-unit count comes from the
  host GPU's name.
- **DirectML** works on our D3D12.

### Display
- GPU-composited desktop (DWM) with modes up to 5120×2160 and dirty-rectangle framebuffer presents.
- Exclusive fullscreen works (Heaven, Superposition, GravityMark in every API).
- Hardware mouse cursor: the pointer goes through the virtio-gpu cursor queue, so the viewer (SPICE /
  virt-manager) draws it on the host side and it moves as smoothly as on the host. Registry `HWCursor=0` in the
  adapter key turns it off.

### Packaging
- NSIS installer: enables test signing if needed, creates and trusts a local signing certificate, signs and
  installs the driver. It resumes after the reboot and supports silent mode (`/S`), upgrades, downgrades and
  uninstall.
- Release build: NDEBUG kernel driver with error-only tracing (`DebugLevel=2`) and NDEBUG Venus/Zink. DXVK
  writes no log files unless `DXVK_LOG_LEVEL` is set.

### Known limitations
- Test-signed: Secure Boot must be off (not enforced).
- OpenCL is 64-bit only. Never install Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack (its
  Dozen driver deadlocks with our D3D12).
- Installing or updating the driver restarts the adapter; apps using the GPU restart.
- Light workloads are CPU-bound, and very high desktop resolutions cost CPU.
- Tested on an NVIDIA RTX 4090 host only.
- Superposition's D3D11 path renders darker, harder shadows than its OpenGL path. This was investigated
  (docs/STATUS.md) and is the app's own D3D11 lighting setup, not a driver bug.
