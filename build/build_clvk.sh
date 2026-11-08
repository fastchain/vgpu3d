#!/bin/bash
# OpenCL 3.0 for the guest: clvk (OpenCL on Vulkan, clspv compiler built in) cross-built with MinGW-w64.
#   build_clvk.sh  -> out/clvk/OpenCL.dll (clvk; registered as an OpenCL ICD by the INF)
# Two stages: (A) a native clspv build supplies the LLVM/clang tablegens and libclc (clspv bakes libclc
# into the library with a python script); (B) the Windows build of clvk + clspv + LLVM uses them.
# Sources: upstream/clvk (git clone --recursive https://github.com/kpet/clvk; then
# external/clspv/utils/fetch_sources.py --deps llvm).  Vulkan: Khronos headers + an import library
# generated from vulkan-1.dll (out/vulkan-mingw).
set -euo pipefail
R=/workspace/driver; C=$R/upstream/clvk; E=$C/external; H=$R/out/clspv-host; B=$R/out/clvk-build; P=$R/out/clvk
T=x86_64-w64-mingw32
if [ ! -x $H/bin/clspv ]; then
  cmake -S $E/clspv -B $H -G Ninja -DCMAKE_BUILD_TYPE=Release -DCLSPV_BUILD_TESTS=OFF \
    -DSPIRV_HEADERS_SOURCE_DIR=$E/SPIRV-Headers -DSPIRV_TOOLS_SOURCE_DIR=$E/SPIRV-Tools \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_USE_LINKER=lld >/dev/null
  ninja -C $H clspv llvm-tblgen clang-tblgen
fi
LIBCLC=$(dirname $(dirname $(ls $H/third_party/llvm/lib/clang/*/lib/spirv32-unknown-vulkan/libclc.bc)))
V=$R/out/vulkan-mingw
[ -f $V/libvulkan-1.dll.a ] || { echo "ERROR: $V/libvulkan-1.dll.a missing (gendef vulkan-1.dll + dlltool)" >&2; exit 1; }
if [ ! -f $B/build.ninja ]; then
  cmake -S $C -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=$T-gcc-posix -DCMAKE_CXX_COMPILER=$T-g++-posix -DCMAKE_RC_COMPILER=$T-windres \
    -DCLVK_VULKAN_IMPLEMENTATION=custom -DVulkan_INCLUDE_DIRS=$R/upstream/khronos/Vulkan-Headers/include \
    -DVulkan_LIBRARIES=$V/libvulkan-1.dll.a \
    -DCLVK_CLSPV_ONLINE_COMPILER=1 -DCLVK_ENABLE_SPIRV_IL=OFF -DCLVK_BUILD_TESTS=OFF \
    -DCLSPV_EXTERNAL_LIBCLC_DIR=$LIBCLC -DCLSPV_BUILD_TESTS=OFF -DENABLE_CLSPV_OPT=OFF \
    -DLLVM_NATIVE_TOOL_DIR=$H/third_party/llvm/bin -DLLVM_TABLEGEN=$H/third_party/llvm/bin/llvm-tblgen \
    -DCLANG_TABLEGEN=$H/third_party/llvm/bin/clang-tblgen -DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu \
    -DLLVM_ENABLE_THREADS=ON -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    "-DCMAKE_SHARED_LINKER_FLAGS=-static -static-libgcc -static-libstdc++" >/dev/null
fi
# clspv marks its API dllexport, which turns off MinGW's export-all, and exporting all of LLVM would
# exceed the 65535-export limit: export exactly the cl* entry points clvk defines (from api.cpp.obj).
ninja -C $B OpenCL-objects
DEF=$B/OpenCL.def
{ echo "LIBRARY OpenCL.dll"; echo "EXPORTS"
  $T-nm -g --defined-only $B/src/CMakeFiles/OpenCL-objects.dir/api.cpp.obj | awk '$2=="T" && $3 ~ /^cl[A-Z]/ {print "    " $3}' | sort -u
} > $DEF.new
cmp -s $DEF.new $DEF || { mv $DEF.new $DEF; cmake $B "-DCMAKE_SHARED_LINKER_FLAGS=-static -static-libgcc -static-libstdc++ $DEF" >/dev/null; }
rm -f $DEF.new
ninja -C $B OpenCL
mkdir -p $P
$T-strip -o $P/OpenCL.dll $B/src/libOpenCL.dll   # MinGW names it libOpenCL.dll
ls -la $P/OpenCL.dll
