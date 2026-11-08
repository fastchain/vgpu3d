#!/bin/bash
# Cross-build Mesa's WGL OpenGL driver for Windows (MinGW-w64): Zink (OpenGL on Vulkan -> Venus) + softpipe.
# Output: out/gl-win/{opengl32.dll (app-local drop-in), libgallium_wgl.dll (ICD / driver), gltest.exe}
# usage: build_gl.sh [32]   -- "32" builds the WOW64 ICD out/gl-win32/libgallium_wgl32.dll (+ gltest32.exe)
# VGPU_RELEASE=1: release build (NDEBUG, no asserts/NIR validation) in out/mesa-gl[32]-build-rel -> out/gl-win[32]-rel
set -eu
R=/workspace/driver; M=$R/mesa; B=$R/out/mesa-gl-build; P=$R/out/gl-win; XF=$R/build/mesa/mingw64.ini
T=x86_64-w64-mingw32; SFX=
if [ "${1:-64}" = 32 ]; then
  B=$R/out/mesa-gl32-build; P=$R/out/gl-win32; XF=$R/build/mesa/mingw32.ini; T=i686-w64-mingw32; SFX=32
fi
BT=debugoptimized; NDBG=false
if [ "${VGPU_RELEASE:-0}" = 1 ]; then B=$B-rel; P=$P-rel; BT=release; NDBG=true; fi
if [ ! -f $B/build.ninja ]; then
  meson setup $B $M --cross-file $XF --buildtype=$BT \
    -Dgallium-drivers=zink,softpipe -Dvulkan-drivers= -Dplatforms=windows -Dllvm=disabled -Dopengl=true \
    -Dgles1=disabled -Dgles2=enabled -Degl=disabled -Dglx=disabled -Dzstd=disabled -Dbuild-tests=false \
    -Dvideo-codecs= -Dspirv-tools=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvulkan-layers= \
    -Dtools= -Ddefault_library=static -Db_ndebug=$NDBG -Dc_link_args=-static -Dcpp_link_args=-static
fi
ninja -C $B src/gallium/targets/wgl/libgallium_wgl.dll src/gallium/targets/libgl-gdi/opengl32.dll
mkdir -p $P
$T-strip -o $P/libgallium_wgl$SFX.dll $B/src/gallium/targets/wgl/libgallium_wgl.dll
$T-strip -o $P/opengl32.dll $B/src/gallium/targets/libgl-gdi/opengl32.dll
$T-gcc-posix -O2 -Wall -o $P/gltest$SFX.exe $R/src/umd/gltest/gltest.c -static -lopengl32 -lgdi32
ls -la $P
