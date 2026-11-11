#!/usr/bin/env python3
"""Inventory privileged instructions in extracted firmware PE executable sections.

Requires the research venv (capstone). This is a linear static inventory, not
reachability analysis: executable sections may contain embedded data. It cannot
prove an instruction is executed, or that AMDHV implements its complete semantics.
Usage: python audit_efi_features.py EXTRACTED_DIR > inventory.json
"""
import collections
import hashlib
import json
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64


def sections(data):
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError('not DOS/PE')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('invalid PE signature')
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    mode = {0x8664: CS_MODE_64, 0x14c: CS_MODE_32}.get(machine)
    if mode is None:
        raise ValueError(f'unsupported machine {machine:#x}')
    table = pe + 24 + optional_size
    for index in range(count):
        off = table + 40 * index
        name = data[off:off + 8].rstrip(b'\0').decode('ascii', 'replace')
        size, rva, raw_size, raw = struct.unpack_from('<IIII', data, off + 8)
        flags = struct.unpack_from('<I', data, off + 36)[0]
        if flags & 0x20000000:
            length = min(size or raw_size, raw_size)
            if raw + length > len(data):
                raise ValueError('truncated executable section')
            yield mode, name, rva, data[raw:raw + length]


def category(ins):
    m, op = ins.mnemonic, ins.op_str
    if m == 'mov' and any(x in op for x in ('cr0', 'cr2', 'cr3', 'cr4', 'cr8', 'dr0', 'dr1', 'dr2', 'dr3', 'dr4', 'dr5', 'dr6', 'dr7')):
        return 'control-debug'
    if m in ('rdmsr', 'wrmsr', 'rdtsc', 'rdtscp', 'cpuid', 'xgetbv', 'xsetbv'):
        return 'msr-clock-cpuid-xcr'
    if m in ('lgdt', 'lidt', 'sgdt', 'sidt', 'lldt', 'ltr', 'sldt', 'str', 'lmsw', 'smsw', 'clts'):
        return 'descriptor-mode'
    if m.startswith(('iret', 'pushf', 'popf')) or m in ('sti', 'cli', 'int', 'int3', 'into', 'hlt', 'pause', 'ud2'):
        return 'event-flags-halt'
    if m.startswith(('fxsave', 'fxrstor', 'xsave', 'xrstor')) or m in ('finit', 'fninit', 'ldmxcsr', 'stmxcsr'):
        return 'fp-state'
    if m in ('in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd') or m.startswith(('rep ins', 'rep outs')):
        return 'port-io'
    if m in ('invlpg', 'invpcid', 'invd', 'wbinvd', 'monitor', 'mwait', 'syscall', 'sysret', 'sysenter', 'sysexit', 'swapgs'):
        return 'paging-cache-system'
    if m in ('rdfsbase', 'rdgsbase', 'wrfsbase', 'wrgsbase', 'stac', 'clac'):
        return 'native-cr4-features'
    if m in ('rdpkru', 'wrpkru'):
        return 'protection-keys'
    if 'zmm' in op or any(f'k{i}' in op for i in range(8)):
        return 'avx512-state'
    return None


def main(root):
    modules = []
    totals = collections.Counter()
    for path in sorted(root.rglob('*.pe')):
        data = path.read_bytes()
        sites = []
        counts = collections.Counter()
        for mode, name, rva, code in sections(data):
            decoder = Cs(CS_ARCH_X86, mode)
            decoder.skipdata = True
            for ins in decoder.disasm(code, rva):
                group = category(ins)
                if group:
                    counts[group] += 1
                    sites.append(dict(rva=hex(ins.address), section=name,
                                      bits=64 if mode == CS_MODE_64 else 32,
                                      category=group, bytes=ins.bytes.hex(),
                                      instruction=f'{ins.mnemonic} {ins.op_str}'.rstrip()))
        totals.update(counts)
        ui = path.with_name('section2.ui')
        modules.append(dict(path=str(path.relative_to(root)),
                            name=ui.read_bytes().decode('utf-16le', 'replace').strip('\0') if ui.exists() else '',
                            sha256=hashlib.sha256(data).hexdigest(),
                            counts=dict(counts), sites=sites))
    if not modules:
        raise SystemExit('No PE modules found; extract VZG11.fd first')
    print(json.dumps(dict(scope='Static executable-section linear decode; includes possible embedded data. No reachability or runtime proof; TE/raw modules excluded.',
                          module_count=len(modules), totals=dict(totals), modules=modules), indent=2))


if __name__ == '__main__':
    main(Path(sys.argv[1]))
