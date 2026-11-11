#!/usr/bin/env python3
"""Regression tests for KC address translation and fail-closed validation."""
import struct
import unittest
import gen_kernsyms as g


def macho(cmds, filetype=2):
    return struct.pack('<IiiIIIII', 0xFEEDFACF, 0x01000007, 3, filetype,
                       len(cmds), sum(map(len, cmds)), 0, 0) + b''.join(cmds)


def segment(va, offset, filesize=128):
    return struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT', va, 4096,
                       offset, filesize, 5, 5, 0, 0)


def fixture(uuid=b'A' * 16):
    kernel = macho([struct.pack('<II16s', 0x1B, 24, b'A' * 16),
                    segment(0x1000, 128)])
    # KC uses absolute file offsets, even inside its embedded Mach-O.
    embedded = macho([struct.pack('<II16s', 0x1B, 24, uuid),
                      segment(0x9000, 512, 120)])
    entry = struct.pack('<IIQQII', 0x80000035, 56, 0x9000, 128, 32, 0)
    entry += b'com.apple.kernel\0'.ljust(24, b'\0')
    collection = macho([entry], 0xC).ljust(128, b'\0') + embedded
    collection = collection.ljust(512, b'\0') + bytes(range(120))
    return kernel, collection


class GeneratorTests(unittest.TestCase):
    def test_linked_bytes_use_absolute_file_offsets(self):
        kernel, collection = fixture()
        original, linked, segs = g.linked_text(kernel, collection)
        va = g.translate_text(0x1014, 24, original, linked)
        self.assertEqual(va, 0x9014)
        self.assertEqual(g.read_va(collection, segs, va, 24), bytes(range(20, 44)))

    def test_uuid_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'UUIDs differ'):
            g.linked_text(*fixture(b'B' * 16))

    def test_symbol_outside_text_or_in_removed_padding_is_rejected(self):
        original, linked, _ = g.linked_text(*fixture())
        for address in (0xFFF, 0x1070, 0x2000):
            with self.subTest(address=address), self.assertRaises(ValueError):
                g.translate_text(address, 24, original, linked)

    def test_missing_kernel_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing'):
            g.kernel_entry(macho([], 0xC))

    def test_truncated_segment_data_is_rejected(self):
        _, collection = fixture()
        segs = g.segments(collection, 128)
        with self.assertRaisesRegex(ValueError, 'past file'):
            g.read_va(collection[:520], segs, 0x9000, 24)

    def test_invalid_command_size_is_rejected(self):
        bad = macho([struct.pack('<II', 0x19, 0)])
        with self.assertRaisesRegex(ValueError, 'command size'):
            list(g.commands(bad))


if __name__ == '__main__':
    unittest.main()
