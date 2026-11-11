#!/usr/bin/env python3
"""Static DxeIpl CR0.WP evidence, not a runtime memory dump or boot proof.
Requires capstone and an extraction made by uefi-firmware-parser -e -o DIR VZG11.fd.
Usage: python audit_efi_paging.py DIR
"""
from pathlib import Path
import hashlib
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
root = Path(sys.argv[1])
module = root / 'volume-0/file-86d70125-baa3-4296-a62f-602bebbb9081/section1.pe'
data = module.read_bytes()
expected = bytes.fromhex('0f20c0480d000001000f22c0')
if data[0x1d23:0x1d2f] != expected:
    raise SystemExit('Unexpected DxeIpl bytes; re-audit this firmware version')
rom = Path('/System/Library/Frameworks/Virtualization.framework/Versions/A/Resources/VZG11.fd')
print('ROM SHA256:', hashlib.sha256(rom.read_bytes()).hexdigest())
print('DxeIpl PE SHA256:', hashlib.sha256(data).hexdigest())
print('Static file offset=RVA for this PE text section; observed stop RIP=1fff0d2c.')
print('Inferred load base 1ffef000 aligns MOV CR0 at RVA1d2c with that stop.')
print('This inference is not a captured runtime instruction stream.')
for ins in Cs(CS_ARCH_X86, CS_MODE_64).disasm(data[0x1d23:0x1d2f], 0x1d23):
    print(f'RVA {ins.address:04x} {ins.bytes.hex():24s} {ins.mnemonic} {ins.op_str}')
