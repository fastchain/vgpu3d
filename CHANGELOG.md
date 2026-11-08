# Changelog

## 1.0.26312.27465 (2026-11-08)

Installer: `vgpu3d-setup-1.0.26312.27465.exe`. From this release on, releases are named by the full driver version.

### Fixed
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
