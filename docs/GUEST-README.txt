vgpu - Windows guest test package
=================================

Contents
  vgpu.sys / vgpu.inf   display driver (virtio-gpu 2D scanout) + 3D control device \\.\VGpu3D
  vgputest.exe          smoke test for the control device
  install.ps1           signs (test cert), builds the catalog, installs
  uninstall.ps1         removes the package
  collect-logs.ps1      gathers diagnostics for bug reports
  tools\                Inf2Cat (from the Windows WDK package) used by install.ps1

Prerequisites (host side, once)
  * The VM must use a virtio-gpu GL device (virtio-vga-gl / virtio-gpu-gl-pci) with blob + venus,
    and SPICE with GL, see WIN11_3D.xml and docs/HOST-SETUP.md in the project.
  * Secure Boot must NOT be enforced in the guest (test signing refuses to enable otherwise).
    WIN11_3D.xml already uses OVMF with Secure Boot capable but keys not enrolled.

Steps (elevated PowerShell in the guest, in this folder)
  1. Set-ExecutionPolicy -Scope Process Bypass
  2. .\install.ps1          -> if it says "REBOOT", reboot and run it again.
  3. Device Manager > Display adapters: the virtio-gpu adapter should now use "vgpu VirtIO GPU".
     If it still uses the old driver: Update driver > Let me pick > Have Disk > vgpu.inf.
  4. .\vgputest.exe -v
  5. Send back the full output of vgputest.exe. If something crashed or failed, also run
     .\collect-logs.ps1 and send vgpu-diagnostics.txt (and the C:\Windows\Minidump files on a bugcheck).

To watch the driver's own messages: run Sysinternals DebugView as admin, Capture > Capture Kernel,
and filter on "vgpu3d:".

Rollback: .\uninstall.ps1 (or boot into safe mode and remove the vgpu package in Device Manager).
