#!/bin/bash
# Build DXVK (upstream/dxvk, v3.1.1 + vgpu patch) for embedding in our WDDM D3D11 UMD:
# dxvk_dxgi.dll + dxvk_d3d11.dll (renamed so they never collide with Windows' own d3d11/dxgi,
# dxvk_d3d11 imports dxvk_dxgi).  Output: out/dxvk/
set -eu
R=/workspace/driver; S=$R/upstream/dxvk; B=$R/out/dxvk-build; P=$R/out/dxvk
if [ ! -f $B/build.ninja ]; then
  meson setup $B $S --cross-file $S/build-win64.txt --buildtype=release -Dbuild_id=false \
    -Denable_d3d9=false -Denable_d3d8=false -Denable_d3d10=false
fi
ninja -C $B src/dxgi/dxvk_dxgi.dll src/d3d11/dxvk_d3d11.dll
mkdir -p $P
cp $B/src/dxgi/dxvk_dxgi.dll $B/src/d3d11/dxvk_d3d11.dll $P/
x86_64-w64-mingw32-strip $P/*.dll
ls -la $P
