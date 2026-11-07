#!/bin/bash
# Cross-build the WDDM render miniport viogpu3d.sys (osy/kvm-guest-drivers-windows `neptune` base) on Linux
# with clang-cl + lld-link.  Output: out/kmd3d/viogpu3d.sys + .pdb.   Usage: build/build_kmd3d.sh [release] [srcroot]
set -u
R=/workspace/driver
T=$R/toolchain
S=${2:-$R/upstream/triton/kmd}
O=$R/out/kmd3d
mkdir -p $O
CONFIG=${1:-debug}
DEFS="/DPOOL_NX_OPTIN=1 /DWINNT=1 /DNT_UP=1 /DVIOGPU_3D=1 /D_KERNEL_MODE=1 /D_AMD64_ /DAMD64"
if [ "$CONFIG" = release ]; then DEFS="$DEFS /DNDEBUG /DDBG=0"; OPT="/O2"; else DEFS="$DEFS /DDBG=1 /DDBG_VERBOSE=1"; OPT="/O1"; fi
python3 -I $R/build/casefix_overlay.py $R/build/incfix $S $R/src/include >/dev/null
INC="-imsvc $R/build/incfix -imsvc $R/build/stubs -imsvc $S/viogpu/common -imsvc $S/viogpu/shared -imsvc $S/viogpu/viogpu3d -imsvc $S/VirtIO -imsvc $S/VirtIO/windows -imsvc $S/VirtIO/linux -imsvc $S"
CL="$T/cl.sh /c /kernel $OPT /Z7 /GS- /Zc:wchar_t /Gy $DEFS $INC"
V=$S/viogpu
SRC="$(ls $S/VirtIO/*.c) $(ls $S/VirtIO/windows/*.c 2>/dev/null)"
SRC="$SRC $V/common/edid.cpp $V/common/viogpu_idr.cpp $V/common/viogpu_pci.cpp $V/common/viogpu_pnp_fixup.cpp $V/common/viogpu_queue.cpp $V/common/baseobj.cpp $V/common/bitops.cpp"
SRC="$SRC $V/viogpu3d/driver.cpp $V/viogpu3d/viogpu_command.cpp $V/viogpu3d/viogpu_device.cpp $V/viogpu3d/viogpu_allocation.cpp $V/viogpu3d/viogpu_adapter.cpp $V/viogpu3d/viogpu_vidpn.cpp $R/build/kshim.c"
rm -f $O/*.obj $O/*.log
compile() {
  f=$1; b=$(basename $f); case $f in *.cpp) M=/Tp;; *) M=/Tc;; esac
  $CL $M $f /Fo$O/$b.obj >$O/$b.log 2>&1 || echo "FAILED $b"
}
export -f compile; export CL O
printf '%s\n' $SRC | xargs -P 8 -I{} bash -c 'compile {}'
fail=0
for l in $O/*.log; do
  if grep -qE 'error|fatal' $l; then fail=1; echo "== $(basename $l .log)"; grep -E 'error|fatal' $l | head -12; fi
done
[ $fail = 1 ] && { echo "COMPILE FAILED"; exit 1; }
echo "compiled $(ls $O/*.obj | wc -l) objects"
lld-link /nologo /driver /subsystem:native,10.0 /tsaware:no /entry:DriverEntry /nodefaultlib /machine:x64 /debug /pdb:$O/viogpu3d.pdb \
  /section:INIT,erd /section:.text,erp /section:.rdata,rp /section:.data,rwp /section:.pdata,rp /merge:.retplne=.rdata /opt:ref /opt:icf /align:4096 /base:0x10000 /release /out:$O/viogpu3d.sys $O/*.obj \
  -libpath:$T/wdk/c/Lib/10.0.26100.0/km/x64 ntoskrnl.lib hal.lib wmilib.lib displib.lib wdmsec.lib bufferoverflowfastfailk.lib 2>&1 | head -40
ls -la $O/viogpu3d.sys $O/viogpu3d.pdb 2>/dev/null
