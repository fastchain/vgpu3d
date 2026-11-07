#!/bin/bash
# Build user-mode tools (freestanding, kernel32 only) -> out/tools/*.exe
set -u
R=/workspace/driver; T=$R/toolchain; O=$R/out/tools; mkdir -p $O
SDKLIB=$T/sdk/c/um/x64
build_exe() {  # name src
  $T/cl_um.sh /c /O1 /Z7 /GS- /Gs999999 /clang:-fno-builtin -imsvc $R/src/include /Tc $2 /Fo$O/$1.obj 2>&1 | grep -E 'error|fatal'
  lld-link /nologo /subsystem:console /entry:start /nodefaultlib /machine:x64 /debug /pdb:$O/$1.pdb /out:$O/$1.exe $O/$1.obj -libpath:$SDKLIB kernel32.Lib 2>&1 | head -20
}
build_exe vgputest $R/src/umd/vgputest/vgputest.c
ls -la $O/*.exe
