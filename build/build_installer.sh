#!/bin/bash
# Build the Windows installer out/vgpu3d-setup-<version>.exe (NSIS) from the staged package.
#   build/build_installer.sh [--package]   (--package: run package_kmd3d.sh release first)
# Needs makensis (apt install nsis).  The version comes from the package INF's DriverVer.
set -euo pipefail
R=/workspace/driver; P=$R/out/package-kmd3d/vgpu3d
[ "${1:-}" = --package ] && $R/build/package_kmd3d.sh release
[ -f $P/vgpu3d.inf ] || { echo "ERROR: no package in $P; run build/package_kmd3d.sh first" >&2; exit 1; }
# refresh the scripts (same renames as package_kmd3d.sh) so script fixes don't need a full rebuild
cp $R/tools/windows/install.ps1 $R/tools/windows/uninstall3d.ps1 $P/
sed -i "s/'vgpu.sys'/'viogpu3d.sys'/; s/'vgpu.inf'/'vgpu3d.inf'/; s/'vgpu.cat'/'vgpu3d.cat'/" $P/install.ps1
V=$(sed -n 's/^DriverVer=[^,]*,\([0-9.]*\).*/\1/p' $P/vgpu3d.inf | tr -d '\r')
V=${V%.*}   # 1.0.YYDDD.N -> 1.0.YYDDD (VIProductVersion appends .0)
OUT=$R/out/vgpu3d-setup-$V.exe
SIZEKB=$(du -sk $P | cut -f1)
makensis -V2 -DVERSION=$V -DPKGDIR=$P -DOUTFILE=$OUT -DREADME=$R/README.md -DSIZEKB=$SIZEKB $R/src/installer/vgpu3d.nsi
ls -la $OUT
