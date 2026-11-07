#!/bin/bash
# Build everything and stage the Windows guest package: out/package/vgpu-guest/ + out/vgpu-guest.zip
set -eu
R=/workspace/driver; T=$R/toolchain; P=$R/out/package/vgpu-guest
CONFIG=${1:-debug}
$R/build/build_kmd.sh $CONFIG | tail -4
$R/build/build_tools.sh | tail -2
rm -rf $R/out/package; mkdir -p $P/tools
cp $R/out/kmd/vgpu.sys $R/out/kmd/vgpu.pdb $R/src/kmd/inf/vgpu.inf $P/
# unique DriverVer per package so pnputil always treats it as an update
sed -i "s#^DriverVer=.*#DriverVer=$(date -u -d yesterday +%m/%d/%Y),1.0.$(date -u +%y%j).$(( $(date -u +%s) % 86400 / 2 ))#" $P/vgpu.inf
cp $R/out/tools/vgputest.exe $R/out/tools/vgputest.pdb $P/
cp $R/tools/windows/*.ps1 $P/
B=$T/wdk/c/bin/10.0.26100.0/x86
cp $B/Inf2Cat.exe $B/Microsoft.UniversalStore.HardwareWorkflow.*.dll $B/WindowsProtectedFiles.xml $P/tools/
cp $R/docs/GUEST-README.txt $P/README.txt
( cd $P/.. && python3 -I - <<'PY'
import zipfile, os
z = zipfile.ZipFile('/workspace/driver/out/vgpu-guest.zip', 'w', zipfile.ZIP_DEFLATED)
for d, _, fs in os.walk('vgpu-guest'):
    for f in fs:
        p = os.path.join(d, f); z.write(p, p)
z.close()
PY
)
( cd $P && sha256sum vgpu.sys vgpu.inf vgputest.exe > SHA256SUMS.txt )
ls -la $R/out/vgpu-guest.zip; cat $P/SHA256SUMS.txt
