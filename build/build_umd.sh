#!/bin/bash
# Build our WDDM D3D10/11 UMD: Triton's DDI layer (D3D10/11 files only, upstream/triton/mesa
# src/virtio/neptune/triton) + src/umd3d/vgpu_triton_backend.c (DXVK device instead of Neptune),
# MinGW-w64.  Output: out/umd3d/vgpu_d3d11.dll (+ DXVK dlls from build_dxvk.sh)
# usage: build_umd.sh [32]   -- "32" builds the WOW64 UMD out/umd3d/vgpu_d3d11_32.dll (+ dxvk32 pair)
set -eu
R=/workspace/driver; M=$R/upstream/triton/mesa; N=$M/src/virtio/neptune; O=$R/out/umd3d-build; P=$R/out/umd3d
T=x86_64-w64-mingw32; DLL=vgpu_d3d11; DX=dxvk; EXP=OpenAdapter10_2
if [ "${1:-64}" = 32 ]; then
  # i386: the DDI entry point is __stdcall (OpenAdapter10_2@4); export it undecorated
  T=i686-w64-mingw32; DLL=vgpu_d3d11_32; DX=dxvk32; O=$R/out/umd3d32-build; EXP="OpenAdapter10_2=OpenAdapter10_2@4"
fi
[ -f $R/out/$DX/${DX}_d3d11.dll ] || $R/build/build_dxvk.sh ${1:-64}
mkdir -p $O $P
CC=$T-gcc-posix
CFLAGS="-O2 -g -std=gnu11 -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -DVGPU_DXVK_INPROC -I$R/out/winddk -include vgpu_mingw_ddk.h -include stddef.h
        -I$N/triton -I$N -I$M/src -I$M/include -I$M/src/util -I$M/src/virtio -Wno-unused-function"
objs=()
for f in tritonDDI tritonResource tritonView tritonState tritonShader tritonDxbc tritonQuery \
         tritonDxgi tritonPresent tritonKm tritonHostCaps; do
  $CC $CFLAGS -c -o $O/$f.o $N/triton/$f.c
  objs+=($O/$f.o)
done
$CC $CFLAGS -c -o $O/vgpu_triton_backend.o $R/src/umd3d/vgpu_triton_backend.c
objs+=($O/vgpu_triton_backend.o)
printf 'LIBRARY %s.dll\nEXPORTS\n    %s\n' $DLL "$EXP" > $O/vgpu_d3d11.def
# d3d10umddi.h defines file-scope const UINTs in every TU (upstream MSVC: /FORCE:MULTIPLE); identical values
$CC -shared -o $O/$DLL.dll "${objs[@]}" $O/vgpu_d3d11.def -Wl,--allow-multiple-definition -static -ldxguid -luuid -lgdi32 -luser32 -lole32
$T-strip -o $P/$DLL.dll $O/$DLL.dll
cp $R/out/$DX/${DX}_d3d11.dll $R/out/$DX/${DX}_dxgi.dll $P/
ls -la $P
