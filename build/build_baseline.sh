#!/bin/bash
# Cross-compile upstream viogpudo (+VirtIO lib) on Linux with clang-cl + WDK NuGet headers.
# Proves the toolchain on real code; baseline for the vgpu driver.
set -u
T=/workspace/driver/toolchain
U=/workspace/driver/upstream/virtio-win
O=/workspace/driver/out/baseline; mkdir -p $O
CL="$T/cl.sh /c /kernel /O1 /GS- /Zc:wchar_t /DPOOL_NX_OPTIN=1 /DNDEBUG /DWINNT=1 /DNT_UP=1"
INC="-imsvc /workspace/driver/build/stubs -imsvc $U/viogpu/common -imsvc $U/viogpu/shared -imsvc $U/VirtIO -imsvc $U/viogpu/viogpudo -imsvc $U/VirtIO/windows -imsvc $U/VirtIO/linux -imsvc $U"
fail=0
$T/cl.sh /c /kernel /Tc /workspace/driver/build/kshim.c /Fo$O/kshim.obj 2>&1 | grep -E 'error' 
for f in $U/VirtIO/VirtIOPCICommon.c $U/VirtIO/VirtIOPCILegacy.c $U/VirtIO/VirtIOPCIModern.c $U/VirtIO/VirtIORing.c $U/VirtIO/VirtIORing-Packed.c $(ls $U/VirtIO/windows/*.c 2>/dev/null) \
         $U/viogpu/common/*.cpp $U/viogpu/viogpudo/driver.cpp $U/viogpu/viogpudo/viogpudo.cpp; do
  case $f in *.cpp) M=/Tp;; *) M=/Tc;; esac
  out=$($CL /DVIOGPU_DOD=1 /DDBG=1 $INC $M $f /Fo$O/$(basename $f).obj 2>&1 | grep -E 'error|fatal' | head -5); [ -n "$out" ] && { echo "$out"; fail=1; }
done
ls $O | wc -l; echo fail=$fail
[ $fail = 0 ] && lld-link /nologo /driver /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 /out:$O/viogpudo.sys $O/*.obj \
  -libpath:$T/wdk/c/Lib/10.0.26100.0/km/x64 ntoskrnl.lib hal.lib wmilib.lib displib.lib bufferoverflowfastfailk.lib 2>&1 | head -30
ls -la $O/*.sys 2>/dev/null
