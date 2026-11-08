#!/bin/bash
# Build vkd3d-proton (upstream/vkd3d-proton, v3.1) for embedding in our WDDM D3D12 UMD:
# d3d12core.dll renamed vkd3d_d3d12core.dll so it never collides with Windows' own D3D12Core.
# The UMD loads it in-process and creates the D3D12 device Triton's D3D12 DDI layer drives.
# Output: out/vkd3d/   usage: build_vkd3d.sh [32]   -- "32" builds out/vkd3d32/vkd3d32_d3d12core.dll
set -eu
R=/workspace/driver; S=$R/upstream/vkd3d-proton; B=$R/out/vkd3d-build; P=$R/out/vkd3d; XF=build-win64.txt; T=x86_64-w64-mingw32; N=vkd3d
if [ "${1:-64}" = 32 ]; then B=$R/out/vkd3d32-build; P=$R/out/vkd3d32; XF=build-win32.txt; T=i686-w64-mingw32; N=vkd3d32; fi
if [ ! -f $B/build.ninja ]; then
  meson setup $B $S --cross-file $S/$XF --buildtype=release -Denable_tests=false -Denable_extras=false
fi
ninja -C $B
mkdir -p $P
cp $B/libs/d3d12core/d3d12core.dll $P/${N}_d3d12core.dll
$T-strip $P/*.dll
ls -la $P
