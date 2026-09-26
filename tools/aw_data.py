"""Another World / Out Of This World DOS data access.

Parses memlist.bin + bankXX (ByteKiller-packed), per rawgl (cyxx/rawgl).
"""
import os
import struct

# Resource types (Resource::ResType)
RT_SOUND = 0
RT_MUSIC = 1
RT_BITMAP = 2
RT_PALETTE = 3
RT_BYTECODE = 4
RT_SHAPE = 5
RT_BANK = 6

TYPE_NAMES = {
    RT_SOUND: "sound", RT_MUSIC: "music", RT_BITMAP: "bitmap",
    RT_PALETTE: "palette", RT_BYTECODE: "bytecode", RT_SHAPE: "shape",
    RT_BANK: "bank",
}

# Game parts (code sections)
MEMLIST_PARTS = [
    [0x14, 0x15, 0x16, 0x00],  # 16000 - protection screens
    [0x17, 0x18, 0x19, 0x00],  # 16001 - introduction
    [0x1A, 0x1B, 0x1C, 0x11],  # 16002 - water
    [0x1D, 0x1E, 0x1F, 0x11],  # 16003 - jail
    [0x20, 0x21, 0x22, 0x11],  # 16004 - cite
    [0x23, 0x24, 0x25, 0x00],  # 16005 - arene
    [0x26, 0x27, 0x28, 0x11],  # 16006 - luxe
    [0x29, 0x2A, 0x2B, 0x11],  # 16007 - final
    [0x7D, 0x7E, 0x7F, 0x00],  # 16008 - password screen
    [0x7D, 0x7E, 0x7F, 0x00],  # 16009 - password screen
]


class MemEntry:
    __slots__ = ("status", "type", "rank", "bank", "pos", "packed", "unpacked", "num")

    def __repr__(self):
        return (f"MemEntry(#{self.num} {TYPE_NAMES.get(self.type, self.type)} "
                f"bank={self.bank:02x} pos={self.pos:#x} packed={self.packed} "
                f"unpacked={self.unpacked})")


def bytekiller_unpack(src):
    """Port of rawgl bytekiller_unpack. src: packed bytes; returns unpacked."""
    crc, size = struct.unpack(">II", src[-8:])
    dst = bytearray(size)
    # unpack fills from the end backwards
    pos = size - 1                     # uc.dst starts at dst+size-1
    state = {"p": len(src) - 12}       # first bits word at srcSize-12
    bits = struct.unpack(">I", src[state["p"]:state["p"] + 4])[0]
    state["p"] -= 4
    crc ^= bits
    remain = size

    def next_bit():
        nonlocal bits, crc
        carry = bits & 1
        bits >>= 1
        if bits == 0:
            q = state["p"]
            bits = struct.unpack(">I", src[q:q + 4])[0]
            state["p"] = q - 4
            crc ^= bits
            carry = bits & 1
            bits = (1 << 31) | (bits >> 1)
        return carry

    def get_bits(count):
        v = 0
        for _ in range(count):
            v = (v << 1) | next_bit()
        return v

    def copy_literal(bits_count, ln):
        nonlocal pos, remain
        count = get_bits(bits_count) + ln + 1
        remain -= count
        if remain < 0:
            count += remain
            remain = 0
        for _ in range(count):
            dst[pos] = get_bits(8)
            pos -= 1

    def copy_reference(bits_count, count):
        nonlocal pos, remain
        remain -= count
        if remain < 0:
            count += remain
            remain = 0
        offset = get_bits(bits_count)
        for _ in range(count):
            dst[pos] = dst[pos + offset]
            pos -= 1

    while remain > 0:
        if not next_bit():
            if not next_bit():
                copy_literal(3, 0)
            else:
                copy_reference(8, 2)
        else:
            code = get_bits(2)
            if code == 3:
                copy_literal(8, 8)
            elif code == 2:
                copy_reference(12, get_bits(8) + 1)
            elif code == 1:
                copy_reference(10, 4)
            else:
                copy_reference(9, 3)
    assert remain == 0
    assert crc == 0, f"crc mismatch {crc:#x}"
    return bytes(dst)


class AwData:
    def __init__(self, data_dir):
        self.dir = data_dir
        raw = open(os.path.join(data_dir, "memlist.bin"), "rb").read()
        self.entries = []
        for i in range(len(raw) // 20):
            e = MemEntry()
            e.num = i
            (e.status, e.type, _bptr, e.rank, e.bank, e.pos,
             e.packed, e.unpacked) = struct.unpack(">BBIBBIII", raw[i * 20:(i + 1) * 20])
            self.entries.append(e)
            if e.status == 0xFF:
                break
        self._cache = {}
        self._banks = {}

    def bank(self, num):
        if num not in self._banks:
            self._banks[num] = open(os.path.join(self.dir, f"bank{num:02x}"), "rb").read()
        return self._banks[num]

    def load(self, num):
        """Return unpacked bytes of memlist entry `num`."""
        if num in self._cache:
            return self._cache[num]
        e = self.entries[num]
        if e.bank == 0 or e.unpacked == 0:
            self._cache[num] = b""
            return b""
        blob = self.bank(e.bank)[e.pos:e.pos + e.packed]
        if e.packed != e.unpacked:
            blob = bytekiller_unpack(blob)
        assert len(blob) == e.unpacked, (num, len(blob), e.unpacked)
        self._cache[num] = blob
        return blob


def parse_palette_vga(seg, num):
    """readPaletteAmiga: DOS VGA palettes are 12-bit RGB big-endian words."""
    pal = []
    p = num * 16 * 2
    for i in range(16):
        color = struct.unpack(">H", seg[p:p + 2])[0]
        p += 2
        r = (color >> 8) & 0xF
        g = (color >> 4) & 0xF
        b = color & 0xF
        pal.append((r * 17, g * 17, b * 17))
    return pal
