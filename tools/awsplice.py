#!/usr/bin/env python3
"""Code-only ROM update: splice a fresh code.sms (32KB) into the existing
4MB stitched ROM without touching the FMV data region at/above 0x8000.
Usage: python3 tools/awsplice.py code.sms out.sms
"""
import struct
import sys

CODE_LEN = 0x8000  # banks 0-1; FMV data starts at 0x8000


def checksum(rom: bytearray) -> int:
    return sum(rom[0:0x7FF0]) & 0xFFFF


def fix_header(rom: bytearray) -> None:
    rom[0x7FFA] = checksum(rom) & 0xFF
    rom[0x7FFB] = (checksum(rom) >> 8) & 0xFF


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    code = bytearray(open(sys.argv[1], 'rb').read())
    if len(code) > CODE_LEN:
        sys.exit(f'code.sms too big: {len(code)} > {CODE_LEN}')
    code.extend(b'\x00' * (CODE_LEN - len(code)))
    rom = bytearray(open(sys.argv[2], 'rb').read())
    if len(rom) != 4 * 1024 * 1024:
        sys.exit(f'existing ROM not 4MB: {len(rom)}')
    rom[0:CODE_LEN] = code
    fix_header(rom)
    open(sys.argv[2], 'wb').write(rom)
    print(f'{sys.argv[2]}: code respliced, checksum 0x{checksum(rom):04x}')


if __name__ == '__main__':
    main()
