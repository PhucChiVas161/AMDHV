#!/usr/bin/env python3
"""Inventory x86-64 HV calls and immediate startup MSRs from a dyld_info disassembly.
This is static evidence; it does not claim any call succeeds on AMDHV.
"""
import collections
import json
import pathlib
import re
import sys

lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
calls = collections.Counter()
startup = []
for i, line in enumerate(lines):
    call = re.search(r'^(0x[0-9A-Fa-f]+)\s+callq\s+(_hv_\w+)', line)
    if not call:
        continue
    calls[call[2]] += 1
    if call[2] != '_hv_vcpu_enable_native_msr':
        continue
    # The current binary passes an immediate MSR in ESI within the preceding 3 instructions.
    candidates = [re.search(r'movl\s+\$(0x[0-9a-f]+),\s*%esi', x) for x in lines[max(0, i-3):i]]
    candidates = [m for m in candidates if m]
    if len(candidates) != 1:
        raise SystemExit(f'Unresolved startup MSR at {call[1]}; inspect instructions')
    startup.append({'call_address': call[1], 'msr': candidates[0][1]})
if not startup:
    raise SystemExit('No native startup MSRs found; incorrect architecture/input?')
print(json.dumps({'source': sys.argv[1], 'architecture': 'x86_64',
    'scope': 'Static call sites only; not runtime success or exhaustive indirect calls',
    'startup_msrs': startup, 'hv_call_sites': dict(sorted(calls.items()))}, indent=2))
