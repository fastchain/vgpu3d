#!/bin/bash
# Cross-build the Mesa Venus Vulkan ICD for Windows (MinGW-w64) with the vgpu backend.
# Output: out/venus-win/{libvulkan_virtio.dll, venus_icd.json, vkcompute.exe, vkcube.exe, d3d11test.exe, vgpulat.exe}
# Mesa tree: /workspace/driver/mesa (git worktree of upstream/mesa, branch vgpu-win,
# changes also in patches/mesa-venus-windows-vgpu.patch).
set -eu
R=/workspace/driver; M=$R/mesa; B=$R/out/mesa-build; P=$R/out/venus-win
cmp -s $R/src/include/vgpu_abi.h $M/src/virtio/vulkan/vgpu_abi.h || { echo "vgpu_abi.h out of sync: cp src/include/vgpu_abi.h mesa/src/virtio/vulkan/"; exit 1; }
if [ ! -f $B/build.ninja ]; then
  meson setup $B $M --cross-file $R/build/mesa/mingw64.ini --buildtype=debugoptimized \
    -Dvulkan-drivers=virtio -Dgallium-drivers= -Dplatforms=windows -Dllvm=disabled -Dopengl=false \
    -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dzstd=disabled -Dbuild-tests=false \
    -Dvideo-codecs= -Dspirv-tools=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvulkan-layers= \
    -Dtools= -Ddefault_library=static -Db_ndebug=false -Dc_link_args=-static -Dcpp_link_args=-static
fi
ninja -C $B src/virtio/vulkan/libvulkan_virtio.dll
mkdir -p $P
x86_64-w64-mingw32-strip -o $P/libvulkan_virtio.dll $B/src/virtio/vulkan/libvulkan_virtio.dll
cp $B/src/virtio/vulkan/libvulkan_virtio.dll $P/libvulkan_virtio.debug.dll
rm -f $P/libwinpthread-1.dll   # winpthread is linked statically (-static)
python3 -I -c "
import json; j=json.load(open('$B/src/virtio/vulkan/virtio_icd.x86_64.json'))
j['ICD']['library_path']='.\\\\libvulkan_virtio.dll'; json.dump(j,open('$P/venus_icd.json','w'),indent=2)"
CC=x86_64-w64-mingw32-gcc-posix
(cd $R/src/umd/vkcompute && glslangValidator -V --vn fill_spv -o fill_spv.h fill.comp >/dev/null && $CC -O2 -Wall -I$M/include -o $P/vkcompute.exe vkcompute.c -static)
$CC -O2 -Wall -I$R/src/include -o $P/vgpulat.exe $R/src/umd/vgpulat/vgpulat.c -static
K=$R/upstream/khronos
[ -d $K/Vulkan-Tools ] && $CC -O2 -DVK_USE_PLATFORM_WIN32_KHR -DWIN32_LEAN_AND_MEAN -I$K/Vulkan-Tools/cube -I$K/Vulkan-Headers/include \
  -o $P/vkcube.exe $K/Vulkan-Tools/cube/cube.c -static -mwindows -lgdi32
$CC -O2 -Wall -o $P/d3d11test.exe $R/src/umd/d3d11test/d3d11test.c -static -ldxguid -luuid
ls -la $P
