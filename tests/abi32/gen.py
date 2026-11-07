#!/usr/bin/env python3
"""Generate a C file that records sizeof/offsetof of every struct in wddm_hw.h (the UMD<->KMD
WDDM ABI) as a const table, so 32- and 64-bit MinGW builds can be compared from their objects."""
import re, sys
h = open(sys.argv[1]).read()
out = ['#include <windows.h>', '#include <stddef.h>', '#include "wddm_hw.h"',
       'const unsigned long long vgpu_abi_table[] = {']
names = []
# typedef struct _X { ... } X;   (one level of nesting is enough for this header)
for m in re.finditer(r'typedef\s+(?:struct|union)\s+\w*\s*\{(.*?)\}\s*(\w+)\s*;', h, re.S):
    body, name = m.group(1), m.group(2)
    out.append(f'  sizeof({name}),')
    names.append(f'{name} size')
    depth = 0
    for line in body.splitlines():
        depth += line.count('{') - line.count('}')
        fm = re.match(r'\s*[A-Za-z_][\w\s\*]*?\b(\w+)\s*(\[[^\]]*\])?\s*;', line)
        if depth == 0 and fm and '(' not in line and not line.strip().startswith(('union', 'struct', '}')):
            out.append(f'  offsetof({name}, {fm.group(1)}),')
            names.append(f'{name}.{fm.group(1)}')
out.append('};')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
open(sys.argv[3], 'w').write('\n'.join(names) + '\n')
