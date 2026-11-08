# CLAUDE.md: notes for AI agents working in this repo

Windows 11 guest GPU driver for QEMU/KVM virtio-gpu: a WDDM kernel driver plus Venus (Vulkan),
Triton DDI + DXVK (D3D10/11) and Zink (OpenGL) user-mode drivers, 64- and 32-bit.
Start with `README.md`, then `docs/SETUP.md`, then `docs/STATUS.md` (newest sections at the end).

## Ground rules

- **Plan first.** Show a numbered plan and get explicit approval before plan-level changes or new
  components. Small fixes inside an approved step don't need a new plan.
- **Commit only when asked.** Never push. Third-party trees get local branches only.
- **Mesa** (`mesa/`, `upstream/triton/mesa/`): follow its AGENTS.md. Never open MRs or comments
  upstream. Commits you write need the trailer `Generated-by: LLM`. Don't touch the directories
  that AGENTS.md lists as AI-restricted.
- **No personal data in the repo:** host IPs, SSH users/ports/key names, MACs, UUIDs, home paths,
  emails, credentials. Use placeholders or environment variables (e.g. `VGPU_SSH`). Before
  committing, check with `git grep`.
- Keep `patches/*.patch` in sync with the third-party branches:
  ```sh
  git -C mesa diff 7d52dbb HEAD > patches/mesa-venus-windows-vgpu.patch
  git -C upstream/dxvk diff v3.1.1 HEAD > patches/dxvk-vgpu.patch
  git -C upstream/triton/mesa diff $(git -C upstream/triton/mesa merge-base HEAD neptune) HEAD > patches/triton-mesa-vgpu-m1.patch
  git -C upstream/vkd3d-proton diff v3.1 HEAD > patches/vkd3d-proton-vgpu.patch
  ```

## Building

- `build/package_kmd3d.sh` builds the kernel driver and both UMDs and produces `out/vgpu3d-m1.zip`.
  It **fails if a binary wasn't rebuilt**, so never work around that check.
- Third-party builds take an optional `32` argument for the WOW64 variant:
  `build_triton_venus.sh`, `build_dxvk.sh`, `build_vkd3d.sh`, `build_umd.sh`, `build_gl.sh`.
- After a container or session restart, apt packages may be gone. Check
  `which x86_64-w64-mingw32-gcc-posix i686-w64-mingw32-gcc-posix clang-cl-19 meson ninja`.
  After reinstalling the toolchain, **delete the meson build dirs (`out/*-build`)**: a partial
  rebuild mixing old and new objects produced a DLL that failed to load (error 1114).
- Builds pipe through `grep`/`tail`, so check that output files really have new timestamps.

## The UMD ↔ KMD ABI

- Shared structs live in `upstream/triton/mesa/src/virtio/virtio-gpu/wddm_hw.h`, mirrored in
  `src/kmd3d/viogpu/shared/viogpum.h`. Keep the two identical.
- The ABI must stay pointer-size neutral (`VIOGPU_UM_PTR`/`VIOGPU_UM_HANDLE` are 64-bit).
  After changing it, run `tests/abi32/check.sh`.
- UMD-created allocations need a `VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX` lookup cookie. Without
  one, `DxgkDdiOpenAllocation` can resolve a stale KMT handle to another process's allocation.
- Under GpuMmu, a TYPE_3D allocation only gets host backing once it has a GPU VA
  (`pfnMapGpuVirtualAddressCb`). vrend's "Illegal resource" in `transfer_iov` also means
  "no guest backing".

## Testing in the Windows VM

- Reach the guest over SSH. The connection details are environment-specific (`VGPU_SSH`), so
  ask the user and don't hardcode them. The guest shell is PowerShell 5.1. Use `scp -O` (legacy
  protocol) if SFTP is broken. For multi-line scripts, write a file, copy it over, and run it
  with `powershell -File`.
- GUI and GPU tests must run in the interactive session (session 0 sees no display adapter).
  Use the scheduled task `vgpu-run`: write `C:\vgpu\job.ps1`, start the task, wait for
  `--- done` in `C:\vgpu\job.txt`.
- Installing or reinstalling the driver restarts the adapter, and **every app holding a GPU
  device crashes** (Explorer, DWM, Fusion). Close the user's apps first, or say so.
- Logs:
  - Guest: DbgView writes `C:\vgpu\dbg.log`. It grows without limit, so rotate it.
  - Host QEMU/virgl: `/var/log/libvirt/qemu/<domain>.log`. If it is mounted into the
    container, read it with `sudo -n`.
- Screenshots (`C:\vgpu\shot.ps1`) are the only proof a frame rendered: a white or black window
  can still produce benchmark scores. Check a mid-run screenshot for every benchmark.

## Benchmark hygiene

- Before measuring, list the guest's environment variables:
  `^(VN_|MESA|ZINK|DXVK|GALLIUM|VGPU)`. Leftover `VN_PERF`/`VN_DEBUG` debug settings once made
  every Venus call synchronous and skewed all results.
- Compare runs only at the **same desktop resolution**: the CPU display path scales with desktop size.
- Benchmarks such as GravityMark cancel on any input. Ask the user not to touch the VM window.
- Runners:
  - `tools/windows/unigine-run.ps1` (Heaven/Valley, Phoronix automation define)
  - `tools/windows/bench3.ps1` (FurMark 2, GravityMark)
  - `tools/fusion-bench/bench.sh` (Autodesk Fusion via MCP)
- Known issues to keep in mind:
  - DX9 comes from Windows' D3D9On12 on our D3D12; OpenCL is not available (never install the MS
    Compatibility Pack: Dozen <-> our D3D12 deadlocks Vulkan). DX12 is vkd3d-proton with a CPU-waited queue gate.
  - Exclusive-fullscreen Heaven D3D11 hangs (host `CS error`); Superposition D3D11 shows black patches.
  - Fullscreen tests need the app in the foreground: run `C:\vgpu\fg.ps1 -Name <process>` beside it.
