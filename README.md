# vgpu: Windows guest GPU driver for QEMU/KVM virtio-gpu (3D)

Goal: Vulkan/OpenGL/Direct3D acceleration for Windows 11 guests on a KVM host, using
virglrenderer (virgl + Venus). Plan: `PLAN.md`. Current state: `docs/STATUS.md`.

## Layout

| Path | Contents |
|---|---|
| `PLAN.md` | Phased plan and progress log |
| `docs/` | `STATUS.md`, `PHASE1_DESIGN.md`, `HOST-SETUP.md`, `GUEST-README.txt` |
| `src/include/vgpu_abi.h` | User/kernel ABI of the `\\.\VGpu3D` control device |
| `src/kmd/` | Kernel driver: virtio-win `VirtIO` library + forked `viogpu` + `vgpu3d.cpp`; `inf/vgpu.inf` |
| `src/umd/vgputest/` | Smoke test run inside the Windows guest |
| `build/` | Cross-build scripts (Linux, clang-cl + lld-link): `build_kmd.sh`, `build_tools.sh`, `package.sh` |
| `toolchain/` | Downloaded WDK/SDK headers and libraries (NuGet), compile wrappers (not source) |
| `upstream/` | Pristine shallow clones of VirtualBox, virtio-win, Mesa (reference only) |
| `tests/` | Host-side checks (`build/run_tests.sh`) |
| `tools/windows/` | PowerShell install/uninstall/diagnostics scripts shipped in the guest package |
| `out/` | Build products; `out/vgpu-guest.zip` is what goes to the Windows machine |

## Build

```sh
build/run_tests.sh     # ABI layout checks
build/package.sh       # driver + tools + guest zip (about 20 s)
```

Needs `clang`, `lld`, `llvm` from apt, and the WDK/SDK packages already unpacked in `toolchain/`
(see `docs/PHASE1_DESIGN.md` section 5 for how they were obtained).

## Licensing note

`src/kmd` derives from virtio-win (BSD-3-Clause). Anything copied from VirtualBox
(GPLv3) in later stages makes the combined kernel driver GPLv3; keep that code in its own tree.
