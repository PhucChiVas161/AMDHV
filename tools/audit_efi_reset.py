#!/usr/bin/env python3
"""Disassemble verified early x86 EFI blocks. Requires capstone; no guest execution.
Block boundaries/modes are manually verified for VZG11 on macOS build25G332.
This is static evidence, not a complete CFG or proof of successful boot.
"""
from pathlib import Path
import hashlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_16, CS_MODE_32
rom = Path('/System/Library/Frameworks/Virtualization.framework/Versions/A/Resources/VZG11.fd')
data = rom.read_bytes()
print(f'Source: {rom}\nSHA256: {hashlib.sha256(data).hexdigest()}')
for offset, length, mode in [(0xffff0,16,16),(0xfffa8,19,16),(0xfff43,31,16),
                            (0xfff62,24,32),(0xfffbb,15,32),(0xffd30,169,32),
                            (0xffe48,45,32),(0xffe75,51,32),(0xffeaa,153,32)]:
    print(f'\nBlock offset={offset:x} mode={mode}')
    decoder = Cs(CS_ARCH_X86, CS_MODE_16 if mode == 16 else CS_MODE_32)
    for ins in decoder.disasm(data[offset:offset+length], 0xfff00000+offset):
        print(f'{ins.address:08x} {ins.bytes.hex():24s} {ins.mnemonic} {ins.op_str}')
