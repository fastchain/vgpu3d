Direct3D on the vgpu (host GPU) driver - DXVK
=============================================

Windows' own Direct3D needs a WDDM render driver, which vgpu does not have yet. DXVK
implements Direct3D 8/9/10/11 on top of Vulkan, and Vulkan reaches the host GPU through
the vgpu Venus driver.

Use it per application: copy the DLLs the application uses next to its .exe

    64-bit app:  C:\Program Files\vgpu\dxvk\x64\{d3d11.dll, dxgi.dll, d3d10core.dll, d3d9.dll, d3d8.dll}
    32-bit app:  C:\Program Files\vgpu\dxvk\x32\...

Most D3D11 applications need d3d11.dll + dxgi.dll.  Remove the copies to go back to
Windows' Direct3D (Microsoft Basic Render Driver, CPU only).

Logs: DXVK writes <app>_d3d11.log / <app>_dxgi.log next to the exe (DXVK_LOG_LEVEL=info).
DXVK reports an AMD vendor id for NVIDIA GPUs by default (its normal game-compat behaviour).

Known limits (first version): presentation copies each frame through the CPU (fine for
CAD / desktop apps, not for high-fps games); vkWaitForFences round trip ~10 ms on QEMU's
fence polling. No Direct3D 12 yet (would be vkd3d-proton, untested).
