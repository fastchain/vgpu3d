#!/bin/bash
# Stage the WDDM package (M1 + M2 D3D11 UMD): out/vgpu3d-m1.zip
#   our viogpu3d.sys build + Venus ICD (D3DKMT backend, from Triton v0.5 for now) + our Zink OpenGL ICD
#   + resolution service (Triton v0.5 binaries), INF src/kmd3d-inf/vgpu3d.inf, signed in the guest by install.ps1.
set -euo pipefail
R=/workspace/driver; P=$R/out/package-kmd3d/vgpu3d; TV=$R/upstream/triton/v0.5/drivers/viogpu3d
# Never ship stale binaries: both builds must produce files newer than this marker
# (a missing toolchain once made every build fail quietly while packaging reused old outputs).
mkdir -p $R/out; touch $R/out/.package-start
$R/build/build_kmd3d.sh ${1:-debug} $R/src/kmd3d | tail -2
$R/build/build_umd.sh
$R/build/build_umd.sh 32
[ -f $R/out/triton-venus32/vulkan_virtio32.dll ] || $R/build/build_triton_venus.sh 32
for f in $R/out/kmd3d/viogpu3d.sys $R/out/umd3d/vgpu_d3d11.dll $R/out/umd3d/vgpu_d3d11_32.dll; do
  [ $f -nt $R/out/.package-start ] || { echo "ERROR: $f was not rebuilt" >&2; exit 1; }
done
rm -rf $R/out/package-kmd3d; mkdir -p $P/tools
cp $R/out/kmd3d/viogpu3d.sys $R/out/kmd3d/viogpu3d.pdb $P/
cp $R/out/triton-venus/vulkan_virtio.dll $P/   # our MinGW build of the Triton Venus D3DKMT backend (build_triton_venus.sh)
cp $TV/vgpusrv.exe $TV/viogpuap.exe $P/
# D3D10/11 UMD: Triton DDI layer + DXVK (build_umd.sh)
cp $R/out/umd3d/vgpu_d3d11.dll $R/out/umd3d/dxvk_d3d11.dll $R/out/umd3d/dxvk_dxgi.dll $R/out/umd3d/vkd3d_d3d12core.dll $P/
# WOW64 (32-bit) UMD, DXVK pair and Venus ICD (build_umd.sh 32, build_triton_venus.sh 32)
cp $R/out/umd3d/vgpu_d3d11_32.dll $R/out/umd3d/dxvk32_d3d11.dll $R/out/umd3d/dxvk32_dxgi.dll $R/out/umd3d/vkd3d32_d3d12core.dll $P/
cp $R/out/triton-venus32/vulkan_virtio32.dll $R/src/kmd3d-inf/virtio_icd32.json $P/
cp $R/out/gl-win/libgallium_wgl.dll $P/
[ -f $R/out/gl-win32/libgallium_wgl32.dll ] || $R/build/build_gl.sh 32
cp $R/out/gl-win32/libgallium_wgl32.dll $P/
# manifest-relative path: the loader cannot resolve Triton's bare "vulkan_virtio.dll" from the DriverStore (LoadLibrary error 87)
cp $R/src/kmd3d-inf/virtio_icd.json $P/
cp $R/src/kmd3d-inf/vgpu3d.inf $P/
sed -i "s#^DriverVer=.*#DriverVer=$(date -u -d yesterday +%m/%d/%Y),1.0.$(date -u +%y%j).$(( $(date -u +%s) % 86400 / 2 ))#" $P/vgpu3d.inf
cp $R/toolchain/wdk/c/bin/10.0.26100.0/x86/Inf2Cat.exe $R/toolchain/wdk/c/bin/10.0.26100.0/x86/Microsoft.UniversalStore.HardwareWorkflow.*.dll $R/toolchain/wdk/c/bin/10.0.26100.0/x86/WindowsProtectedFiles.xml $P/tools/
cp $R/tools/windows/install.ps1 $P/install.ps1
sed -i "s/'vgpu.sys'/'viogpu3d.sys'/; s/'vgpu.inf'/'vgpu3d.inf'/; s/'vgpu.cat'/'vgpu3d.cat'/" $P/install.ps1
( cd $P/.. && python3 -I -c "
import zipfile, os
z = zipfile.ZipFile('$R/out/vgpu3d-m1.zip', 'w', zipfile.ZIP_DEFLATED)
for d, _, fs in os.walk('vgpu3d'):
    for f in fs: z.write(os.path.join(d, f))
z.close()" )
ls -la $R/out/vgpu3d-m1.zip
