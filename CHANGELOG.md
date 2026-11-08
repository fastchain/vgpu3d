# Changelog

## 1.0.26312 (2026-11-08)

First release. Installer: `vgpu3d-setup-1.0.26312.exe`, driver version 1.0.26312.23940.

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
- No hardware mouse cursor yet: the pointer is drawn into the desktop image and moves at the display
  pipeline's frame rate.
- Installing or updating the driver restarts the adapter; apps using the GPU restart.
- Light workloads are CPU-bound, and very high desktop resolutions cost CPU.
- Tested on an NVIDIA RTX 4090 host only.
- Superposition's D3D11 path renders darker, harder shadows than its OpenGL path. This was investigated
  (docs/STATUS.md) and is the app's own D3D11 lighting setup, not a driver bug.
