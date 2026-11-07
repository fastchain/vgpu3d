#!/bin/bash
# Build DXVK (upstream/dxvk, v3.1.1 + vgpu patch) for embedding in our WDDM D3D11 UMD:
# dxvk_dxgi.dll + dxvk_d3d11.dll (renamed so they never collide with Windows' own d3d11/dxgi,
# dxvk_d3d11 imports dxvk_dxgi).  Output: out/dxvk/
# usage: build_dxvk.sh [32]   -- "32" builds the WOW64 pair dxvk32_dxgi.dll + dxvk32_d3d11.dll into out/dxvk32/
set -eu
R=/workspace/driver; S=$R/upstream/dxvk; B=$R/out/dxvk-build; P=$R/out/dxvk; XF=build-win64.txt; T=x86_64-w64-mingw32; N=dxvk
if [ "${1:-64}" = 32 ]; then B=$R/out/dxvk32-build; P=$R/out/dxvk32; XF=build-win32.txt; T=i686-w64-mingw32; N=dxvk32; fi
if [ ! -f $B/build.ninja ]; then
  meson setup $B $S --cross-file $S/$XF --buildtype=release -Dbuild_id=false \
    -Denable_d3d9=false -Denable_d3d8=false -Denable_d3d10=false
fi
ninja -C $B src/dxgi/${N}_dxgi.dll src/d3d11/${N}_d3d11.dll
mkdir -p $P
cp $B/src/dxgi/${N}_dxgi.dll $B/src/d3d11/${N}_d3d11.dll $P/
$T-strip $P/*.dll
ls -la $P
