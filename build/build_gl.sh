#!/bin/bash
# Cross-build Mesa's WGL OpenGL driver for Windows (MinGW-w64): Zink (OpenGL on Vulkan -> Venus) + softpipe.
# Output: out/gl-win/{opengl32.dll (app-local drop-in), libgallium_wgl.dll (ICD / driver), gltest.exe}
set -eu
R=/workspace/driver; M=$R/mesa; B=$R/out/mesa-gl-build; P=$R/out/gl-win
if [ ! -f $B/build.ninja ]; then
  meson setup $B $M --cross-file $R/build/mesa/mingw64.ini --buildtype=debugoptimized \
    -Dgallium-drivers=zink,softpipe -Dvulkan-drivers= -Dplatforms=windows -Dllvm=disabled -Dopengl=true \
    -Dgles1=disabled -Dgles2=enabled -Degl=disabled -Dglx=disabled -Dzstd=disabled -Dbuild-tests=false \
    -Dvideo-codecs= -Dspirv-tools=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvulkan-layers= \
    -Dtools= -Ddefault_library=static -Db_ndebug=false -Dc_link_args=-static -Dcpp_link_args=-static
fi
ninja -C $B src/gallium/targets/wgl/libgallium_wgl.dll src/gallium/targets/libgl-gdi/opengl32.dll
mkdir -p $P
x86_64-w64-mingw32-strip -o $P/libgallium_wgl.dll $B/src/gallium/targets/wgl/libgallium_wgl.dll
x86_64-w64-mingw32-strip -o $P/opengl32.dll $B/src/gallium/targets/libgl-gdi/opengl32.dll
x86_64-w64-mingw32-gcc-posix -O2 -Wall -o $P/gltest.exe $R/src/umd/gltest/gltest.c -static -lopengl32 -lgdi32
ls -la $P
