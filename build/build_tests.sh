#!/bin/bash
# Build guest test programs that have no other build script: cltest (OpenCL), dmltest (DirectML) and
# blttest (D3D11 present paths, 64- and 32-bit), vkprops (Vulkan features/formats DXVK relies on).  Output: out/tests64/{cltest,dmltest,blttest_x86_64,blttest_i686}.exe.  DirectML.h comes from the Windows SDK in toolchain/
# (copied alone into out/dml-include so the rest of the SDK does not shadow MinGW's headers).
set -euo pipefail
R=/workspace/driver; T=x86_64-w64-mingw32; O=$R/out/tests64
mkdir -p $O $R/out/dml-include
cp $R/toolchain/sdk/c/Include/10.0.26100.0/um/DirectML.h $R/out/dml-include/
$T-gcc-posix -O2 -Wall -o $O/cltest.exe $R/src/umd/cltest/cltest.c -static
$T-g++-posix -O2 -Wall -Wno-unknown-pragmas -I$R/out/dml-include -o $O/dmltest.exe $R/src/umd/dmltest/dmltest.cpp \
  -static -ld3d12 -ldxgi -luuid
$T-gcc-posix -O2 -Wall -I$R/upstream/khronos/Vulkan-Headers/include -o $O/vkprops.exe $R/src/umd/vkprops/vkprops.c \
  $R/out/vulkan-mingw/libvulkan-1.dll.a
$T-gcc-posix -O2 -Wall -I$R/upstream/khronos/Vulkan-Headers/include -o $O/vgpuvram.exe $R/src/umd/vgpuvram/vgpuvram.c \
  $R/out/vulkan-mingw/libvulkan-1.dll.a
$T-gcc-posix -O2 -Wall -I$R/upstream/khronos/Vulkan-Headers/include -o $O/tearcheckvk.exe $R/src/umd/tearcheck/tearcheckvk.c \
  $R/out/vulkan-mingw/libvulkan-1.dll.a -lgdi32
$T-gcc-posix -O2 -Wall -I$R/upstream/khronos/Vulkan-Headers/include -o $O/rttest.exe $R/src/umd/rttest/rttest.c \
  $R/out/vulkan-mingw/libvulkan-1.dll.a
for t in x86_64 i686; do
  $t-w64-mingw32-gcc-posix -O2 -Wall -o $O/dxrtest_$t.exe $R/src/umd/dxrtest/dxrtest.c -static -ld3d12 -luuid
done
$T-g++-posix -O2 -Wall -o $O/tearcheck12.exe $R/src/umd/tearcheck/tearcheck12.cpp -static -ld3d12 -ldxgi -lgdi32 -luuid
$T-gcc-posix -O2 -Wall -o $O/tearcheck.exe $R/src/umd/tearcheck/tearcheck.c -static -ld3d11 -ldxgi -lgdi32 -luuid
$T-gcc-posix -O2 -Wall -o $O/readbacktest.exe $R/src/umd/readbacktest/readbacktest.c -static -ld3d11 -ldxgi -luuid
for T2 in x86_64 i686; do
  $T2-w64-mingw32-gcc-posix -O2 -Wall -mwindows -o $O/blttest_$T2.exe $R/src/umd/blttest/blttest.c -static -ld3d11 -ldxgi -luuid
done
ls -la $O/cltest.exe $O/dmltest.exe $O/vkprops.exe $O/vgpuvram.exe $O/readbacktest.exe $O/blttest_*.exe
