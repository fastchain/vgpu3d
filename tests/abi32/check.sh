#!/bin/bash
# Compare the WDDM UMD<->KMD ABI (wddm_hw.h) between i686 and x86_64 MinGW builds. Every typedef'd
# struct/union in the header is instantiated with -g, and the DWARF member layout (names, offsets,
# sizes, nested types included) of both builds must be identical, or a 32-bit UMD would talk
# garbage to the 64-bit KMD.
set -euo pipefail
R=/workspace/driver; H=$R/upstream/triton/mesa/src/virtio/virtio-gpu/wddm_hw.h; T=${TMPDIR:-/tmp}/abi32; mkdir -p $T
DD=$(command -v llvm-dwarfdump || command -v llvm-dwarfdump-19)
names=$(grep -oE '^\}\s*[A-Z_][A-Z0-9_]*' $H | sed -E 's/^\}\s*//' | sort -u)
{ echo '#include <windows.h>'; echo '#include "wddm_hw.h"'
  for n in $names; do echo "$n vgpu_abi_$n;"; done; } > $T/abi.c
for a in i686 x86_64; do
  $a-w64-mingw32-gcc-posix -c -g -O0 -w -I$(dirname $H) -I$R/out/winddk -include vgpu_mingw_ddk.h -o $T/$a.o $T/abi.c
  # keep only the layout-relevant attributes of the vgpu types, in DIE order
  $DD --debug-info $T/$a.o | awk '
    /DW_TAG_/ { tag=$0; sub(/.*DW_TAG_/, "", tag) }
    /DW_AT_name/ { nm=$0; sub(/.*DW_AT_name[^"]*"/, "", nm); sub(/".*/, "", nm) }
    /DW_AT_byte_size|DW_AT_data_member_location|DW_AT_bit_size|DW_AT_data_bit_offset/ {
      v=$0; sub(/.*\(/, "", v); sub(/\).*/, "", v); a=$0; sub(/.*DW_AT_/, "", a); sub(/[ \t].*/, "", a)
      print tag, nm, a, v }' | grep -E "VIOGPU|_VIOGPU|member" > $T/$a.txt
done
echo "types checked: $(echo $names | wc -w); layout lines: $(wc -l < $T/x86_64.txt)"
if ! diff -u $T/i686.txt $T/x86_64.txt > $T/diff.txt; then echo "MISMATCH:"; head -40 $T/diff.txt; exit 1; fi
echo "wddm_hw.h ABI identical for i686 and x86_64"
