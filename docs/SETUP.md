# vgpu3d: host, QEMU and VM setup

This is how to set up a Linux host, QEMU/libvirt and a Windows 11 VM so the current `vgpu3d` driver
package gives the guest hardware 3D on the host GPU. It covers the package from 2026-11-07, the
4K + 32-bit build: `vgpu3d-m2-wow64-*.zip`, built by `build/package_kmd3d.sh` as `out/vgpu3d-m1.zip`.

A complete working libvirt domain is in [`examples/WIN11_3D.xml`](examples/WIN11_3D.xml).
Section 4 explains every part of it that matters.

---

## 1. What you get

The guest sees one display adapter, **"vgpu VirtIO GPU 3D (WDDM)"**. Windows apps use it by default,
with no per-app setup.

| API | 64-bit apps | 32-bit apps | How |
|---|---|---|---|
| Vulkan 1.4 | yes | yes | Mesa Venus ICD → host Vulkan driver |
| Direct3D 10 / 11 (FL 11_1) | yes | yes | Triton DDI layer + DXVK → Venus |
| OpenGL 4.6 | yes | yes | Mesa Zink (GL on Vulkan) → Venus |
| Direct3D 12 | yes | yes | Triton D3D12 DDI layer + vkd3d-proton → Venus (new, see §8) |
| Direct3D 9 | yes | yes | Windows' D3D9On12 on our D3D12 driver (automatic) |
| Desktop (DWM) | GPU composited | – | D3D11 on the host GPU |

The guest reports the host GPU by name, for example `Virtio-GPU Venus (NVIDIA GeForce RTX 4090)`.
Display modes go up to 5120×2160 (any mode whose 32 bpp framebuffer fits 64 MiB, at most 5120×2880).

How it works: the guest kernel driver (`viogpu3d.sys`, WDDM 2 / GpuMmu) talks to QEMU's
`virtio-vga-gl` device. Venus forwards Vulkan commands through shared rings to
`virgl_render_server` on the host, which runs them on the host GPU's Vulkan driver.

---

## 2. Host requirements

### 2.1 Linux, QEMU, libvirt, virglrenderer

| Component | Needed | Tested with |
|---|---|---|
| Linux kernel | any recent; KVM | 6.18 |
| QEMU | ≥ 9.2, with `virtio-vga-gl` and the `blob`, `hostmem`, `venus` properties | 11.1.2 |
| libvirt | ≥ 9 (for `<graphics type="egl-headless">` and `qemu:commandline`) | 12.8.0 |
| virglrenderer | ≥ 1.0 built **with Venus** (`-Dvenus=true`), so `virgl_render_server` exists | distro build |

Check:
```sh
qemu-system-x86_64 -device virtio-vga-gl,help | grep -E 'blob|hostmem|venus'   # all three must exist
ldd "$(command -v qemu-system-x86_64)" | grep virglrenderer                      # linked
ls /usr/libexec/virgl_render_server /usr/lib/*/virgl_render_server 2>/dev/null  # Venus render server
```

### 2.2 Host GPU and Vulkan driver

Any GPU whose **host** Vulkan driver supports Venus works. Venus needs:
- Vulkan 1.1+ (1.3 recommended: DXVK needs 1.3-class features for Direct3D 11);
- `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`;
- timeline semaphores.

```sh
vulkaninfo --summary
vulkaninfo | grep -E 'VK_EXT_external_memory_dma_buf|VK_EXT_image_drm_format_modifier|VK_KHR_external_memory_fd'
```

| Host GPU | Status |
|---|---|
| NVIDIA, proprietary driver | **Tested** (RTX 4090). Works with the egl-headless display in section 4.5. SPICE GL and GPU scanout show black. |
| AMD (RADV), Intel (ANV) | Not tested here. These are Venus's main targets and are expected to work. |
| No Vulkan / very old GPU | Does not work |

### 2.3 Required host settings

1. **Turn off QEMU's seccomp sandbox** in `/etc/libvirt/qemu.conf`:
   ```
   seccomp_sandbox = 0
   ```
   Then `sudo systemctl restart libvirtd`. Without this, QEMU runs with `-sandbox …,spawn=deny`, which
   blocks virglrenderer from starting `virgl_render_server`. The QEMU log then says
   `failed to initialize venus renderer`, and the guest gets no Vulkan (capset 4 missing).
   With the setting, libvirt passes `-sandbox off`.
2. **Render-node access.** The user QEMU runs as (`libvirt-qemu` or `qemu`) needs the host GPU's render node:
   ```sh
   ls -l /dev/dri/renderD*                    # pick the node of the GPU to use
   sudo usermod -aG render libvirt-qemu       # Debian/Ubuntu; "qemu" on Fedora/Arch
   ```
   AppArmor/SELinux must not block `/dev/dri`. If the host has several GPUs, use
   `ls -l /dev/dri/by-path/` to find the right `renderD*`.
3. **Shared guest memory.** This is done in the domain XML (section 4.2); nothing to do on the host.

---

## 3. Guest requirements

- **Windows 11 x64**, a WDDM 2.x build. Tested on Windows 11 Pro build 26300 (26H2); 22H2 also boots.
- **Test signing.** The package is test-signed, so test signing must be on, and that needs Secure Boot
  *not enforced*. The example XML gives UEFI with Secure Boot capable but no keys enrolled (section 4.1),
  so `bcdedit /set testsigning on` works and Windows 11 setup is still satisfied. The desktop then shows
  a "Test Mode" watermark.
- About 200 MB of disk for the package.

---

## 4. The libvirt domain, explained

Use [`examples/WIN11_3D.xml`](examples/WIN11_3D.xml) as the template. Change the name, UUID, disk paths
and MAC for your machine. The parts that matter for the driver are below.

### 4.1 Firmware: UEFI, Secure Boot capable, keys not enrolled
```xml
<os firmware="efi">
  <type arch="x86_64" machine="q35">hvm</type>
  <firmware>
    <feature enabled="no"  name="enrolled-keys"/>
    <feature enabled="yes" name="secure-boot"/>
  </firmware>
  <loader secure="yes"/>
</os>
<features> … <smm state="on"/> </features>
```
Windows 11 setup sees a Secure Boot capable machine, but nothing is enforced, so test signing can be
enabled. When the driver is production signed, switch `enrolled-keys` to `yes`.

### 4.2 Shared memory (required for blob resources)
```xml
<memoryBacking>
  <source type="memfd"/>
  <access mode="shared"/>
</memoryBacking>
```
Blob resources let the host map guest pages directly. Without shared memory, Vulkan memory mapping fails.

### 4.3 CPU and RAM
```xml
<memory unit="GiB">8</memory>
<vcpu placement="static">12</vcpu>
<cpu mode="host-passthrough" check="none" migratable="on">
  <topology sockets="1" dies="1" cores="12" threads="1"/>
</cpu>
```
Keep `sockets="1"`: Windows 11 Home/Pro licenses at most 2 sockets. The example also enables the
Hyper-V enlightenments, a large performance gain for Windows guests. The translation stack is
CPU-heavy (section 9), so give the VM several cores.

### 4.4 GPU device: virtio-vga-gl with blob, hostmem and Venus
```xml
<video>
  <model type="virtio" heads="1" primary="yes">
    <acceleration accel3d="yes"/>
  </model>
</video>
…
<qemu:commandline>
  <qemu:arg value="-global"/><qemu:arg value="virtio-vga-gl.blob=true"/>
  <qemu:arg value="-global"/><qemu:arg value="virtio-vga-gl.hostmem=4G"/>
  <qemu:arg value="-global"/><qemu:arg value="virtio-vga-gl.venus=true"/>
</qemu:commandline>
```
The root element needs `xmlns:qemu="http://libvirt.org/schemas/domain/qemu/1.0"`. With
`accel3d="yes"`, libvirt picks `virtio-vga-gl` for the primary adapter. The globals switch on:

| Property | Why |
|---|---|
| `blob=true` | blob resources, needed by Venus and by the driver's buffer sharing |
| `hostmem=4G` | host-visible memory window (PCI BAR) for mapped Vulkan memory. 4G is the tested size; less limits large mapped allocations |
| `venus=true` | Vulkan capset (4). Without it the guest has no Vulkan, Direct3D or OpenGL |

libvirt marks the domain "tainted: custom-argv" because of `qemu:commandline`. That's expected.

### 4.5 Display: SPICE + egl-headless (the tested setup)
```xml
<graphics type="spice" autoport="yes">
  <listen type="address" address="127.0.0.1"/>
</graphics>
<graphics type="egl-headless">
  <gl rendernode="/dev/dri/renderD128"/>
</graphics>
```
- SPICE **must be the first** `<graphics>` element. It carries the display to virt-manager or `remote-viewer`.
- `egl-headless` gives QEMU's virglrenderer a GL context on the chosen render node. SPICE itself has
  **no** `<gl>`, so the viewer receives finished frames and needs no GPU of its own. This also works
  over a network or SSH tunnel.
- Set `rendernode` to the host GPU that should run the guest.

**SPICE with `gl=on`:** an alternative is SPICE `<gl enable="yes" rendernode="…"/>` with
`<listen type="none"/>` and no egl-headless. On the NVIDIA proprietary driver it showed **only black,
from the first firmware frame**, even with virt-manager on the host's own screen: the viewer cannot
import QEMU's GPU buffers. It also requires the viewer on the host itself. A prepared variant is
`/workspace/WIN11/WIN11_3D.spicegl.xml` (not in this repo); don't use it on NVIDIA.

How the desktop reaches the screen today: the driver copies each displayed frame into a 2D
framebuffer (CPU path). This works on every host, but costs CPU at high resolutions (section 9).

### 4.6 Other devices in the example
- Disk on SATA (`ide-hd`). virtio-blk/scsi also work once their drivers are installed.
- `e1000e` NIC. Pin the MAC (`<mac address="…"/>`) so redefining the domain keeps the guest's IP.
- TPM 2.0 (`<tpm model="tpm-crb">` with the emulator backend): a Windows 11 requirement.
- SPICE agent channel, USB tablet/keyboard, virtio-balloon, virtio-rng.

### 4.7 Define and start
```sh
sudo virsh define WIN11_3D.xml
sudo virsh start WIN11_3D
```
Then open it with virt-manager. Check the QEMU log for virgl/Venus errors:
```sh
sudo tail -f /var/log/libvirt/qemu/WIN11_3D.log
```

---

## 5. Installing the driver in the guest

### 5.1 Installer (recommended)

`build/build_installer.sh` produces `out/vgpu3d-setup-<version>.exe` (NSIS, about 20 MB).

1. Copy it into the guest. Save your work: the adapter restarts and GPU-using apps may close.
2. Run it (UAC prompt). Setup checks for 64-bit Windows 11 and a virtio-gpu adapter (PCI 1AF4:1050),
   unpacks the package to `C:\Program Files\vgpu3d` and runs the bundled `install.ps1` (below).
3. If test signing was off, setup turns it on and asks for a reboot; a `RunOnce` entry finishes the
   installation in a console window at the next logon. Secure Boot must not be enforced.
4. If the finish page asks for a reboot (the adapter shows Code 14, typically when switching from Basic
   Display), reboot.

Silent: `vgpu3d-setup-<version>.exe /S`. Exit code 0 = installed, 3010 = reboot needed, 1 = failed.
Uninstall from *Settings → Apps* (or `"C:\Program Files\vgpu3d\uninstall.exe" /S`): removes every
`vgpu3d.inf` package, the `CN=vgpu test signing` certificate and the files. Test signing stays on
(`bcdedit /set testsigning off`).

### 5.2 Manual install from the zip

1. **Test signing, one time.** In an elevated PowerShell: `bcdedit /set testsigning on`, then reboot.
   (`install.ps1` checks this and enables it for you if Secure Boot allows; it then exits with code 3.)
2. Copy the package zip into the guest and unpack it, for example to `C:\vgpu\m1`.
3. Run the installer from an elevated PowerShell:
   ```powershell
   cd C:\vgpu\m1\vgpu3d
   powershell -ExecutionPolicy Bypass -File .\install.ps1
   ```
   It does the following:
   - creates a self-signed code-signing certificate and trusts it (Root + TrustedPublisher);
   - signs `viogpu3d.sys`;
   - builds `vgpu3d.cat` with the bundled `Inf2Cat.exe` and signs it;
   - resets the kernel driver's boot-loop guard (`VgpuBootGuard`);
   - installs the package with `pnputil /add-driver … /install`.
4. The screen flickers while the adapter restarts. Device Manager → Display adapters should show
   **vgpu VirtIO GPU 3D (WDDM)** with status OK. Usually no reboot is needed.

**Updating:** run the new installer (or `install.ps1` from the new package). A driver update restarts the
adapter. Every app that holds a GPU device crashes at that moment, including Explorer, the Start menu and
DWM; Windows restarts them on its own. **Close your work first** (known issue, section 8).

**Removing:** uninstall from Apps, or run `uninstall3d.ps1 [-RemoveCert]` from the package (same as
`tools/windows/vgpu3d-rollback.ps1`). The adapter falls back to Microsoft Basic Display Adapter.

### What gets installed
All files go to the driver's DriverStore folder (`C:\Windows\System32\DriverStore\FileRepository\vgpu3d.inf_amd64_*`):

| File | Role |
|---|---|
| `viogpu3d.sys` | WDDM kernel driver |
| `vgpu_d3d11.dll`, `dxvk_d3d11.dll`, `dxvk_dxgi.dll` | 64-bit D3D10/11 user-mode driver (Triton DDI + DXVK) |
| `vulkan_virtio.dll`, `virtio_icd.json` | 64-bit Vulkan ICD (Venus) |
| `libgallium_wgl.dll` | 64-bit OpenGL ICD (Zink) |
| `vgpu_d3d11_32.dll`, `dxvk32_d3d11.dll`, `dxvk32_dxgi.dll` | 32-bit (WOW64) D3D10/11 |
| `vulkan_virtio32.dll`, `virtio_icd32.json` | 32-bit Vulkan ICD |
| `libgallium_wgl32.dll` | 32-bit OpenGL ICD |
| `vgpusrv.exe`, `viogpuap.exe` | resolution helper service (Triton) |

The INF registers them through `UserModeDriverName[Wow]`, `VulkanDriverName[Wow]` and
`OpenGLDriverName[Wow]`, so the D3D runtime, the Vulkan loader and `opengl32.dll` find them in every
process. No app-local DLLs are needed.

---

## 6. Checking that it works

| Check | Expected |
|---|---|
| Device Manager | "vgpu VirtIO GPU 3D (WDDM)", OK |
| `vulkaninfo --summary` (Vulkan SDK/runtime) | GPU0 `Virtio-GPU Venus (<host GPU>)` |
| `dxdiag` → Display | the vgpu adapter, Direct3D acceleration enabled, feature levels up to 11_1 |
| Any OpenGL app or `gltest.exe` | renderer `zink Vulkan 1.4(Virtio-GPU Venus (<host GPU>))` |
| Repo test apps (`out/tests64`, `out/tests32`) | `d3d11test`, `vkcompute`, `sharetest` print `RESULT: all checks passed` / `sharing works` |
| Host log | no `vrend`/`vkr` errors. `vkr: Warning: vkAllocateDescriptorSets failed` lines are harmless (DXVK growing its descriptor pools) |

**Changing resolution:** Settings → Display, or the SPICE window size (custom mode). Modes larger than
64 MiB at 32 bpp, or larger than 5120×2880, are hidden on purpose.

**Applications:** apps pick the GPU on their own. For Autodesk Fusion, set Preferences → Graphics driver
= DirectX 11 (or "Auto"). Remove any old per-app `opengl32.dll`/`libgallium_wgl.dll` from Fusion's
webdeploy folder; they're no longer needed.

---

## 7. Performance (RTX 4090 host, 2026-11-07)

| Benchmark | Result |
|---|---|
| Fusion 2.5M-triangle orbit, window ~2048×670 | ~60 fps (DirectX 11) |
| Fusion maximized, 2560×1080 / 3840×2160 / 5120×2160 | 66–69 / ~31 / ~17 fps |
| Unigine Heaven 4.0 (32-bit), 1080p High, DX11 / OpenGL | 48.4 / 95.7 fps |
| Unigine Valley 1.0 (32-bit), 1080p High, DX11 / OpenGL | 52.0 / 76.8 fps |
| GravityMark 1.89, 1080p, Vulkan / DX11 | 9463 (56.6 fps) / 5379 (32.2 fps) |
| FurMark 2.10 1080p preset, OpenGL | 6360 (106 fps) |

---

## 8. Known limitations

- **Direct3D 9 runs through Windows' D3D9On12** (D3D9 on our D3D12 driver), chosen automatically because the
  driver has no native D3D9 UMD.
- **Direct3D 12 is new.** Each ExecuteCommandLists waits for its GPU work before returning; a kernel-side gate
  stalled behind itself (GravityMark D3D12 133 -> 17 fps), so the CPU wait stays. Tested: a clear/readback/present test, GravityMark
  D3D12, PassMark DirectX 12.
- **No OpenCL.** GPU-compute tests that need it (PassMark GPU Compute, Geekbench) don't run. Do **not** install
  Microsoft's "OpenCL, OpenGL and Vulkan Compatibility Pack": its Dozen Vulkan driver calls D3D12, our D3D12
  driver calls Vulkan, and every Vulkan app (and DWM after a reboot) deadlocks.
- **Exclusive fullscreen in Unigine (D3D11)**: Heaven hangs (its staging Map never completes) and Superposition
  makes the host reject a submission (`vkQueueSubmit2 resulted in CS error`). GravityMark works in exclusive
  fullscreen in all four APIs, and so does blttest (32/64-bit). Windowed and borderless Unigine work.
- **Superposition (D3D11)** shows black patches on lit geometry in some scenes; OpenGL renders the
  same scenes correctly.
- **Driver updates crash GPU-using apps**, including Explorer and DWM, while the adapter restarts.
  Windows restarts the shell; save your work before updating.
- **The desktop goes through a CPU framebuffer copy** (section 4.5), and so do old-style (blt-model)
  D3D windows. Large resolutions cost CPU.
- Video memory is reported as 256 MB to apps; that's an estimate, not a limit.

---

## 9. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Guest has no Vulkan; QEMU log: `failed to initialize venus renderer` | `seccomp_sandbox = 0` missing (2.3), or virglrenderer built without Venus |
| Adapter shows Code 39 after a reinstall | the boot-loop guard tripped (`VgpuBootGuard` ≠ 0 under `HKLM\SYSTEM\CurrentControlSet\Services\VioGpu3D`); re-run the installer, which resets it |
| Adapter shows Code 43, or falls back to Basic Display | test signing off, package not signed (re-run `install.ps1`), or `blob`/`venus` globals missing |
| Black viewer from the first frame | SPICE `gl=on` on NVIDIA: use the egl-headless setup (4.5) |
| QEMU log: `vrend_renderer_transfer_iov … Illegal resource` | a resource without guest backing (old driver builds); update the driver |
| High CPU load, GPU mostly idle (host `nvtop`) | expected with this design: D3D/GL → Vulkan translation in the guest, Venus decode on one host thread per app, and the CPU display copy. Give the VM more cores; lower resolution helps most |
| Explorer/Start menu crash once | happened during a driver update (8); Windows restarts it |
| Specific app renders black/white | check `C:\Users\<you>\<App>\log` and the host log. GL apps: see dual-source blending (8) |

**Logs:**
- Host QEMU/virgl: `/var/log/libvirt/qemu/<domain>.log`.
- Guest kernel and UMD debug output: DbgView (Sysinternals) with "Capture Kernel". The KMD log level is
  `HKLM\SYSTEM\CurrentControlSet\Services\VioGpu3D\Parameters\DebugLevel`: 2 = errors, 3 = warnings,
  4 = information (entry/exit), 5 = verbose (floods DbgView and can fill RAM). Release packages install 2
  and compile everything above errors out, so higher values only take effect with a debug build
  (`build/package_kmd3d.sh` without `release`, which installs 4).
- DXVK (D3D10/11) writes no log unless `DXVK_LOG_LEVEL` is set (`info`, `warn`, ...); the files
  `<app>_d3d11.log` / `<app>_dxgi.log` then appear next to the app, or under `DXVK_LOG_PATH`.
- `VGPU_STATS_EVERY=<n>` (environment variable) makes the D3D11 UMD log present/copy timings every n frames.
