#!/bin/bash
# Build probe driver variants: p0 = vgpu link flags, others drop one suspect each.
set -eu
R=/workspace/driver; T=$R/toolchain; O=$R/out/probe; mkdir -p $O
python3 -I $R/build/casefix_overlay.py $R/build/incfix $R/src/kmd $R/src/include >/dev/null
$T/cl.sh /c /kernel /O1 /Z7 /GS- /Zc:wchar_t /Gy /DPOOL_NX_OPTIN=1 /D_KERNEL_MODE=1 -imsvc $R/build/incfix -imsvc $R/build/stubs /Tc $R/tests/probe/probe.c /Fo$O/probe.obj
L="-libpath:$T/wdk/c/Lib/10.0.26100.0/km/x64 ntoskrnl.lib hal.lib"
SEC="/section:INIT,erd /section:.text,erp /section:.rdata,rp /section:.data,rwp /section:.pdata,rp /merge:.retplne=.rdata"
link() { n=$1; shift; lld-link /nologo /driver /subsystem:native,10.0 /entry:DriverEntry /nodefaultlib /machine:x64 /release "$@" /out:$O/$n.sys $O/probe.obj $L; }
link p0 $SEC /opt:ref /opt:icf /align:4096 /base:0x10000 bufferoverflowfastfailk.lib $R/out/kmd/kshim.c.obj
link p1 $SEC /opt:ref /opt:icf /align:4096 bufferoverflowfastfailk.lib $R/out/kmd/kshim.c.obj
link p2 $SEC /opt:ref /opt:icf /align:4096 /base:0x10000
link p3 $SEC /opt:ref /opt:icf /base:0x140000000
link p4 /opt:ref /opt:icf /base:0x140000000
ls -la $O/*.sys
