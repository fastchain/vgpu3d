#!/bin/bash
# Stage the Venus/DXVK guest package: out/vgpu-venus.zip
set -eu
R=/workspace/driver; P=$R/out/package-venus/vgpu-venus
$R/build/build_venus.sh | tail -2
rm -rf $R/out/package-venus; mkdir -p $P/venus $P/vulkanrt $P/dxvk $P/tools
V=$R/out/venus-win
cp $V/libvulkan_virtio.dll $V/venus_icd.json $P/venus/
cp $V/vkcompute.exe $V/vkcube.exe $V/d3d11test.exe $V/vgpulat.exe $P/tools/
RT=$R/upstream/vulkanrt/x/VulkanRT-X64-1.4.328.1-Components
cp -r $RT/x64 $RT/x86 $RT/VulkanRT-License.txt $P/vulkanrt/
rm -f $P/vulkanrt/*/*.pdb
DX=$R/upstream/dxvk-dl/dxvk-3.1.1
cp -r $DX/x64 $DX/x32 $P/dxvk/
cp $R/docs/DXVK-README.txt $P/dxvk/README.txt
cp $R/tools/windows/install-venus.ps1 $R/tools/windows/uninstall-venus.ps1 $P/
( cd $P && find . -type f ! -name SHA256SUMS.txt -exec sha256sum {} + | sort -k2 > SHA256SUMS.txt )
( cd $R/out/package-venus && python3 -I -c "
import zipfile, os
z = zipfile.ZipFile('$R/out/vgpu-venus.zip', 'w', zipfile.ZIP_DEFLATED)
for d, _, fs in os.walk('vgpu-venus'):
    for f in fs: z.write(os.path.join(d, f))
z.close()" )
ls -la $R/out/vgpu-venus.zip
