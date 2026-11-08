#!/bin/bash
# Build guest test programs that have no other build script: cltest (OpenCL) and dmltest (DirectML).
# Output: out/tests64/{cltest,dmltest}.exe.  DirectML.h comes from the Windows SDK in toolchain/
# (copied alone into out/dml-include so the rest of the SDK does not shadow MinGW's headers).
set -euo pipefail
R=/workspace/driver; T=x86_64-w64-mingw32; O=$R/out/tests64
mkdir -p $O $R/out/dml-include
cp $R/toolchain/sdk/c/Include/10.0.26100.0/um/DirectML.h $R/out/dml-include/
$T-gcc-posix -O2 -Wall -o $O/cltest.exe $R/src/umd/cltest/cltest.c -static
$T-g++-posix -O2 -Wall -Wno-unknown-pragmas -I$R/out/dml-include -o $O/dmltest.exe $R/src/umd/dmltest/dmltest.cpp \
  -static -ld3d12 -ldxgi -luuid
ls -la $O/cltest.exe $O/dmltest.exe
