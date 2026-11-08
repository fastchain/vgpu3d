#!/bin/bash
# Cross-build the Khronos OpenCL ICD loader (OpenCL.dll) for Windows with MinGW-w64.
# Windows ships no OpenCL.dll (GPU vendors install it); the loader finds ICDs through the display
# adapter's OpenCLDriverName[Wow] registry value (written by our INF) and HKLM\SOFTWARE\Khronos\OpenCL\Vendors.
#   build_opencl_loader.sh [32]  -> out/opencl/OpenCL.dll (64-bit) or out/opencl32/OpenCL.dll
# Needs cmake + ninja; sources in upstream/OpenCL-ICD-Loader and upstream/OpenCL-Headers (Khronos GitHub).
set -euo pipefail
R=/workspace/driver; T=x86_64-w64-mingw32; B=$R/out/opencl-loader-build; P=$R/out/opencl
if [ "${1:-64}" = 32 ]; then T=i686-w64-mingw32; B=$R/out/opencl-loader32-build; P=$R/out/opencl32; fi
[ -d $R/upstream/OpenCL-ICD-Loader ] || git clone -q https://github.com/KhronosGroup/OpenCL-ICD-Loader.git $R/upstream/OpenCL-ICD-Loader
[ -d $R/upstream/OpenCL-Headers ] || git clone -q https://github.com/KhronosGroup/OpenCL-Headers.git $R/upstream/OpenCL-Headers
cmake -S $R/upstream/OpenCL-ICD-Loader -B $B -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=$T-gcc-posix -DCMAKE_RC_COMPILER=$T-windres -DBUILD_TESTING=OFF \
  -DOPENCL_ICD_LOADER_HEADERS_DIR=$R/upstream/OpenCL-Headers -DCMAKE_SHARED_LINKER_FLAGS=-static >/dev/null
ninja -C $B OpenCL >/dev/null
mkdir -p $P
$T-strip -o $P/OpenCL.dll $B/OpenCL.dll
ls -la $P/OpenCL.dll
