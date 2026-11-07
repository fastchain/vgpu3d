# Host setup for virtio-gpu 3D guests (KVM / libvirt / virt-manager)

The guest driver can only be as capable as the host's virtio-gpu device. This lists what the
host must provide and how to check it. Do these checks **before** blaming the guest driver.

## 1. QEMU must have the GL virtio-gpu devices with blob + Venus

```sh
qemu-system-x86_64 -device help 2>&1 | grep -E 'virtio-(vga|gpu)-gl'
qemu-system-x86_64 -device virtio-vga-gl,help 2>&1 | grep -E 'blob|hostmem|venus|context_init'
```

Expected: `virtio-vga-gl` and `virtio-gpu-gl-pci` exist, and the property list shows `blob`,
`hostmem` and `venus` (QEMU 9.2 or newer; Debian 13 ships 10.x). Missing `venus` means only
OpenGL (virgl) works, which is still enough to test the control device.

## 2. virglrenderer with Venus

```sh
ldd $(which qemu-system-x86_64) | grep -i virgl      # libvirglrenderer must be linked
dpkg -l | grep -E 'virglrenderer|libvirglrenderer'    # want 1.0.1+ (Venus needs 1.0+)
```

## 3. A usable host GPU stack

Venus forwards Vulkan to the **host** Vulkan driver, so the host needs a working Vulkan ICD with
dma-buf / external-memory support:

```sh
vulkaninfo --summary
vulkaninfo | grep -E 'VK_EXT_external_memory_dma_buf|VK_EXT_image_drm_format_modifier|VK_KHR_external_memory_fd'
```

Caveat for NVIDIA proprietary drivers (this project's host has an RTX 4090): Venus support on
the proprietary driver is the least mature combination. If `VK_EXT_external_memory_dma_buf` or
`VK_EXT_image_drm_format_modifier` is missing, the guest driver and `vgputest.exe` still work, but
Vulkan inside the guest will fail at the Mesa layer. The OpenGL (virgl) path needs an EGL+GBM
capable driver (NVIDIA 515+ qualifies).

## 4. Render node access for the QEMU process

```sh
ls -l /dev/dri/renderD*
id libvirt-qemu 2>/dev/null || id qemu          # the user libvirt runs QEMU as
```

That user must be in the `render` group (or the node's ACL must allow it), and AppArmor/SELinux
must not block `/dev/dri`. On Debian: `sudo adduser libvirt-qemu render`, then restart libvirtd and
the VM. If you run multiple GPUs, set `<gl enable="yes" rendernode="/dev/dri/renderD129"/>` in the
SPICE `<graphics>` element to choose the one QEMU should use.

## 5. Shared guest memory

Blob resources let the host access guest pages directly, so guest RAM must be shared:
`WIN11_3D.xml` already contains
`<memoryBacking><source type="memfd"/><access mode="shared"/></memoryBacking>`.

## 6. Using the VM definition

```sh
sudo virsh define WIN11_3D.xml
sudo virsh start WIN11_3D
```

virt-manager's display must be SPICE with OpenGL enabled (the XML sets this; the
`Display Spice` pane should show "Listen type: None" and "OpenGL: enabled"). If the window stays
black, test with `virsh domdisplay` and a local `remote-viewer`.

## 7. Quick sanity checks once the guest runs

In the guest, with the stock virtio-win or vgpu display driver installed:

| What you see | Meaning |
|---|---|
| `vgputest.exe`: "open \\.\VGpu3D" fails, error 2 | vgpu.sys is not the bound driver, or the host did not offer VIRGL |
| `3D_FEATURES = 0` | device is virtio-gpu without GL (plain `virtio-vga`) |
| `RESOURCE_BLOB = 0` | `blob=true` property not applied (check the `-global` lines in the XML) |
| `NUM_CAPSETS = 0` | virglrenderer failed to initialise; read `/var/log/libvirt/qemu/WIN11_3D.log` |
| capset id 4 missing | no Venus; QEMU/virglrenderer too old or Vulkan unusable on the host |
| `HOST_VISIBLE = 0` with blob on | `hostmem` property missing or the BAR was not assigned |
