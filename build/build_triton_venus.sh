#!/bin/bash
# Cross-build the Venus Vulkan ICD with the D3DKMT (WDDM) backend from osy/virtio-win-mesa "neptune"
# (upstream/triton/mesa) using MinGW-w64.  Output: out/triton-venus/vulkan_virtio.dll (+ .debug.dll)
# usage: build_triton_venus.sh [32]   -- "32" builds the WOW64 ICD out/triton-venus32/vulkan_virtio32.dll
# MinGW lacks the WDK DDI headers: they are staged from our NuGet WDK into out/winddk (Mesa's
# include/winddk convention) plus vgpu_mingw_ddk.h for missing SAL annotations.
# Source fixes for MinGW in the tree: pid_t guard (vn_common.h), "static inline" (wddm_hw.h).
set -eu
R=/workspace/driver; M=$R/upstream/triton/mesa; B=$R/out/triton-venus-build; P=$R/out/triton-venus; W=$R/out/winddk
XF=$R/build/mesa/mingw64.ini; T=x86_64-w64-mingw32; DLL=vulkan_virtio
if [ "${1:-64}" = 32 ]; then
  B=$R/out/triton-venus32-build; P=$R/out/triton-venus32; XF=$R/build/mesa/mingw32.ini; T=i686-w64-mingw32; DLL=vulkan_virtio32
fi
I=$R/toolchain/wdk/c/Include/10.0.26100.0
mkdir -p $W
for h in d3d10TokenizedProgramFormat.hpp d3d11TokenizedProgramFormat.hpp d3d12TokenizedProgramFormat.hpp \
         d3d10umddi.h d3dkmddi.h d3dkmdt.h d3dumddi.h dxgiddi.h dxmini.h; do
  [ -f $W/$h ] || cp "$(find $I -iname $h | head -1)" $W/$h
done
for h in d3dkmthk.h d3dukmdt.h; do [ -f $W/$h ] || cp $R/toolchain/sdk/c/Include/10.0.26100.0/shared/$h $W/; done
[ -f $W/vgpu_mingw_ddk.h ] || printf '/* vgpu: SAL annotations used by the WDK DDI headers that MinGW lacks. Force-included. */\n#pragma once\n#include <sal.h>\n#ifndef _Maybenull_\n#define _Maybenull_\n#endif\n' > $W/vgpu_mingw_ddk.h
# assert() -> vgpu_mesa_assert (vn_renderer_virtgpu_win32.c): log + abort instead of msvcrt's
# MessageBox, which hangs DWM invisibly
CARGS="-I$W -include vgpu_mingw_ddk.h -D_assert=vgpu_mesa_assert -D_wassert=vgpu_mesa_wassert"
if [ -f $B/build.ninja ] && ! grep -q vgpu_mesa_assert $B/meson-info/intro-buildoptions.json 2>/dev/null; then
  meson configure $B "-Dc_args=$CARGS" >/dev/null
fi
if [ ! -f $B/build.ninja ]; then
  meson setup $B $M --cross-file $XF --buildtype=debugoptimized \
    -Dvulkan-drivers=virtio -Dgallium-drivers= -Dplatforms=windows -Dllvm=disabled -Dopengl=false \
    -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dzstd=disabled -Dbuild-tests=false \
    -Dvideo-codecs= -Dspirv-tools=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvulkan-layers= \
    -Dtools= -Ddefault_library=static -Db_ndebug=false -Dc_link_args=-static -Dcpp_link_args=-static \
    "-Dc_args=$CARGS" "-Dcpp_args=-I$W"
fi
ninja -C $B src/virtio/vulkan/vulkan_virtio.dll
mkdir -p $P
$T-strip -o $P/$DLL.dll $B/src/virtio/vulkan/vulkan_virtio.dll
cp $B/src/virtio/vulkan/vulkan_virtio.dll $P/$DLL.debug.dll
ls -la $P
