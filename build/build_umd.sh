#!/bin/bash
# Build our WDDM D3D10/11 UMD: Triton's DDI layer (D3D10/11 files only, upstream/triton/mesa
# src/virtio/neptune/triton) + src/umd3d/vgpu_triton_backend.c (DXVK device instead of Neptune),
# MinGW-w64.  Output: out/umd3d/vgpu_d3d11.dll (+ DXVK dlls from build_dxvk.sh)
set -eu
R=/workspace/driver; M=$R/upstream/triton/mesa; N=$M/src/virtio/neptune; O=$R/out/umd3d-build; P=$R/out/umd3d
[ -f $R/out/dxvk/dxvk_d3d11.dll ] || $R/build/build_dxvk.sh
mkdir -p $O $P
CC=x86_64-w64-mingw32-gcc-posix
CFLAGS="-O2 -g -std=gnu11 -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -DVGPU_DXVK_INPROC -I$R/out/winddk -include vgpu_mingw_ddk.h
        -I$N/triton -I$N -I$M/src -I$M/include -I$M/src/util -I$M/src/virtio -Wno-unused-function"
objs=()
for f in tritonDDI tritonResource tritonView tritonState tritonShader tritonDxbc tritonQuery \
         tritonDxgi tritonPresent tritonKm tritonHostCaps; do
  $CC $CFLAGS -c -o $O/$f.o $N/triton/$f.c
  objs+=($O/$f.o)
done
$CC $CFLAGS -c -o $O/vgpu_triton_backend.o $R/src/umd3d/vgpu_triton_backend.c
objs+=($O/vgpu_triton_backend.o)
printf 'LIBRARY vgpu_d3d11.dll\nEXPORTS\n    OpenAdapter10_2\n' > $O/vgpu_d3d11.def
# d3d10umddi.h defines file-scope const UINTs in every TU (upstream MSVC: /FORCE:MULTIPLE); identical values
$CC -shared -o $O/vgpu_d3d11.dll "${objs[@]}" $O/vgpu_d3d11.def -Wl,--allow-multiple-definition -static -ldxguid -luuid -lgdi32 -luser32 -lole32
x86_64-w64-mingw32-strip -o $P/vgpu_d3d11.dll $O/vgpu_d3d11.dll
cp $R/out/dxvk/dxvk_d3d11.dll $R/out/dxvk/dxvk_dxgi.dll $P/
ls -la $P
