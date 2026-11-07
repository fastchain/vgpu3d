#!/bin/bash
# Cross-build vgpu.sys (display + virtio-gpu 3D control device) on Linux with clang-cl + lld-link.
# Output: out/kmd/vgpu.sys + vgpu.pdb.   Usage: build/build_kmd.sh [release]
set -u
R=/workspace/driver
T=$R/toolchain
S=$R/src/kmd
O=$R/out/kmd
mkdir -p $O
CONFIG=${1:-debug}
DEFS="/DPOOL_NX_OPTIN=1 /DWINNT=1 /DNT_UP=1 /DVIOGPU_DOD=1 /D_KERNEL_MODE=1"
if [ "$CONFIG" = release ]; then DEFS="$DEFS /DNDEBUG /DDBG=0"; OPT="/O2"; else DEFS="$DEFS /DDBG=1"; OPT="/O1"; fi
python3 -I $R/build/casefix_overlay.py $R/build/incfix $S $R/src/include >/dev/null
INC="-imsvc $R/build/incfix -imsvc $R/build/stubs -imsvc $R/src/include -imsvc $S/viogpu/common -imsvc $S/viogpu/shared -imsvc $S/viogpu/viogpudo -imsvc $S/VirtIO -imsvc $S/VirtIO/windows -imsvc $S/VirtIO/linux"
CL="$T/cl.sh /c /kernel $OPT /Z7 /GS- /Zc:wchar_t /Gy $DEFS $INC"

SRC="$S/VirtIO/VirtIOPCICommon.c $S/VirtIO/VirtIOPCILegacy.c $S/VirtIO/VirtIOPCIModern.c $S/VirtIO/VirtIORing.c $S/VirtIO/VirtIORing-Packed.c"
SRC="$SRC $(ls $S/VirtIO/windows/*.c) $(ls $S/viogpu/common/*.cpp) $S/viogpu/viogpudo/driver.cpp $S/viogpu/viogpudo/viogpudo.cpp $S/viogpu/viogpudo/vgpu3d.cpp $R/build/kshim.c"

rm -f $O/*.obj $O/*.log
compile() {
  f=$1; b=$(basename $f); case $f in *.cpp) M=/Tp;; *) M=/Tc;; esac
  $CL $M $f /Fo$O/$b.obj >$O/$b.log 2>&1 || echo "FAILED $b"
}
export -f compile; export CL O
printf '%s\n' $SRC | xargs -P 8 -I{} bash -c 'compile {}'
# summarize diagnostics
fail=0
for l in $O/*.log; do
  if grep -qE 'error|fatal' $l; then fail=1; echo "== $(basename $l .log)"; grep -E 'error|fatal' $l | head -12; fi
done
[ $fail = 1 ] && { echo "COMPILE FAILED"; exit 1; }
echo "compiled $(ls $O/*.obj | wc -l) objects"
lld-link /nologo /driver /subsystem:native,10.0 /tsaware:no /entry:DriverEntry /nodefaultlib /machine:x64 /debug /pdb:$O/vgpu.pdb \
  /section:INIT,erd /section:.text,erp /section:.rdata,rp /section:.data,rwp /section:.pdata,rp /merge:.retplne=.rdata /opt:ref /opt:icf /align:4096 /base:0x10000 /release /out:$O/vgpu.sys $O/*.obj \
  -libpath:$T/wdk/c/Lib/10.0.26100.0/km/x64 ntoskrnl.lib hal.lib wmilib.lib displib.lib wdmsec.lib bufferoverflowfastfailk.lib 2>&1 | head -40
ls -la $O/vgpu.sys $O/vgpu.pdb 2>/dev/null
