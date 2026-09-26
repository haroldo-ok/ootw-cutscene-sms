#!/usr/bin/env python3
"""Repack the OOTW SMS ROM for the fast player:
  * tile dictionary stored PRE-CONVERTED to VDP 4-bitplane format (32 B/tile,
    tile did at ROM offset 0x8000 + did*32) -> upload is 32 x OUTI
  * every AWV stream remapped to slots 1..447 (VRAM 0x0020..0x37FF) so tiles
    never overlap the name table at 0x3800 (the 512-slot allocation corrupted
    the tilemap); frames that need >447 distinct tiles are reduced by
    nearest-tile substitution using real pixel distance
  * layout: [code 32K][planar tiles][awv streams][awa streams], 4 MiB
Usage: python3 tools/awrepack.py rom/ootw-sms.sms rom/ootw-new.sms rom/src/fmv_data.h
"""
import re
import struct
import sys

import numpy as np

N_SLOTS = 448          # slots 1..447 usable (0 = blank / font)
ROM_SIZE = 4 * 1024 * 1024
TILES_AT = 0x8000


def parse_hdr(path):
    h = open(path).read()
    awv = [int(x) for x in re.search(r'ROM_AWV\[.*?\{(.*?)\}', h, re.S).group(1).replace('UL', '').split(',')]
    awa = [int(x) for x in re.search(r'ROM_AWA\[.*?\{(.*?)\}', h, re.S).group(1).replace('UL', '').split(',')]
    base = int(re.search(r'ROM_TILEOFF_BASE (\d+)UL', h).group(1))
    ntiles = int(re.search(r'ROM_NTILES (\d+)', h).group(1))
    return awv, awa, base, ntiles


def decompact(rom, off):
    """Port of the C decoder: returns 64 pixels (row-major)."""
    form = rom[off]
    pix = [0] * 64
    if form < 0x10:
        pix = [form] * 64
    elif form == 0x10:
        b0 = rom[off + 1]
        bb = rom[off + 2:off + 10]
        for i in range(64):
            pix[i] = (b0 >> 4) if (bb[i >> 3] >> (i & 7)) & 1 else (b0 & 15)
    elif form == 0x20:
        b0, b1 = rom[off + 1], rom[off + 2]
        bb = rom[off + 3:off + 19]
        cols = (b0 & 15, b0 >> 4, b1 & 15, b1 >> 4)
        for i in range(64):
            pix[i] = cols[(bb[i >> 2] >> ((i & 3) << 1)) & 3]
    else:
        d = rom[off + 1:off + 33]
        for i in range(32):
            pix[i << 1] = d[i] >> 4
            pix[(i << 1) + 1] = d[i] & 15
    return pix


def planar(pix):
    out = bytearray(32)
    for j in range(8):
        for k in range(4):
            bits = 0
            for x in range(8):
                if (pix[j * 8 + x] >> k) & 1:
                    bits |= 0x80 >> x
            out[j * 4 + k] = bits
    return bytes(out)


def parse_awv(rom, base):
    assert rom[base:base + 4] == b'AWV0'
    nf = struct.unpack_from('<H', rom, base + 4)[0]
    p = base + 8
    frames = []
    while True:
        fl = rom[p]; p += 1
        if fl == 0xFF:
            break
        dur = struct.unpack_from('<H', rom, p)[0]; p += 2
        pal = None
        if fl & 1:
            pal = rom[p:p + 16]; p += 16
        nup = struct.unpack_from('<H', rom, p)[0]; p += 2
        ups = [struct.unpack_from('<HH', rom, p + 4 * i) for i in range(nup)]; p += 4 * nup
        nc = struct.unpack_from('<H', rom, p)[0]; p += 2
        cells = [struct.unpack_from('<HH', rom, p + 4 * i) for i in range(nc)]; p += 4 * nc
        frames.append([fl, dur, pal, ups, cells])
    assert len(frames) == nf
    return frames, p


def awa_len(rom, base):
    p = base + 2
    while True:
        p += 2                      # delta
        nwr = rom[p]; p += 1
        if nwr == 0xFF:
            return p - base
        p += 2 * nwr


def remap_scene(frames, tiles_np, stats):
    """Re-run slot allocation with 447 slots. Returns new frames."""
    slot_did = [None] * N_SLOTS      # new VRAM state
    stamp = [0] * N_SLOTS
    cell_slot = [0] * 768            # new tilemap state (slot per cell)
    cell_did = [None] * 768          # new tilemap content
    old_slot_did = [None] * 512      # decode of the original stream
    old_cell_did = [None] * 768
    out = []
    t = 0
    for fl, dur, pal, ups, cells in frames:
        t += 1
        for did, sl in ups:
            old_slot_did[sl] = did
        for c, sl in cells:
            old_cell_did[c] = old_slot_did[sl]
        want = list(old_cell_did)        # intended screen (did per cell)
        needed = {}
        for c, d in enumerate(want):
            if d is not None:
                needed.setdefault(d, []).append(c)
        # ---- reduce to 447 distinct tiles by nearest-tile substitution ----
        if len(needed) > N_SLOTS - 1:
            excess = len(needed) - (N_SLOTS - 1)
            stats['frames_reduced'] += 1
            stats['tiles_merged'] += excess
            # drop tiles with the fewest cells first
            order = sorted(needed, key=lambda d: (len(needed[d]), d))
            drop = order[:excess]
            keep = order[excess:]
            keep_np = tiles_np[keep]                      # (K,64)
            for d in drop:
                dist = np.count_nonzero(keep_np != tiles_np[d], axis=1)
                best = keep[int(np.argmin(dist))]
                for c in needed[d]:
                    want[c] = best
                    needed[best].append(c)
                del needed[d]
        # ---- allocate slots ----
        have = {d: s for s, d in enumerate(slot_did) if d is not None and s}
        new_ups = []
        missing = [d for d in needed if d not in have]
        if missing:
            onscreen = set(cell_did[c] for c in range(768))
            free = [s for s in range(1, N_SLOTS) if slot_did[s] is None]
            cold = [s for s in range(1, N_SLOTS)
                    if slot_did[s] is not None and slot_did[s] not in needed
                    and slot_did[s] not in onscreen]
            cold.sort(key=lambda s: stamp[s])
            warm = [s for s in range(1, N_SLOTS)
                    if slot_did[s] is not None and slot_did[s] not in needed
                    and slot_did[s] in onscreen]
            warm.sort(key=lambda s: stamp[s])
            pool = free + cold + warm
            assert len(pool) >= len(missing), 'allocator out of slots'
            for d, s in zip(missing, pool):
                slot_did[s] = d
                have[d] = s
                new_ups.append((d, s))
        for d in needed:
            stamp[have[d]] = t
        # ---- cells: rewrite every cell whose slot changed ----
        new_cells = []
        for c in range(768):
            d = want[c]
            s = 0 if d is None else have[d]
            if s != cell_slot[c] or (cell_did[c] != d and d is not None):
                new_cells.append((c, s))
            cell_slot[c] = s
            cell_did[c] = d
        out.append([fl, dur, pal, new_ups, new_cells])
    return out


def ser_awv(frames):
    b = bytearray(b'AWV0' + struct.pack('<HH', len(frames), 0))
    for fl, dur, pal, ups, cells in frames:
        b += bytes([fl]) + struct.pack('<H', dur)
        if fl & 1:
            assert pal is not None and len(pal) == 16
            b += bytes(pal)
        b += struct.pack('<H', len(ups))
        for d, s in ups:
            b += struct.pack('<HH', d, s)
        b += struct.pack('<H', len(cells))
        for c, s in cells:
            b += struct.pack('<HH', c, s)
    b += b'\xff'
    return bytes(b)


def main():
    src, dst, hdr = sys.argv[1:4]
    rom = open(src, 'rb').read()
    awv, awa, base, ntiles = parse_hdr(hdr)
    print(f'tiles={ntiles} tileoff_base={base}')
    # ---- tiles ----
    pix_all = np.zeros((ntiles, 64), dtype=np.uint8)
    tiles = bytearray()
    for did in range(ntiles):
        o = base + 3 * did
        toff = rom[o] | (rom[o + 1] << 8) | (rom[o + 2] << 16)
        pix = decompact(rom, toff)
        pix_all[did] = pix
        tiles += planar(pix)
    print(f'planar tile bank = {len(tiles)} bytes')
    # ---- streams ----
    stats = {'frames_reduced': 0, 'tiles_merged': 0}
    new_awv = []
    for i, b in enumerate(awv):
        frames, end = parse_awv(rom, b)
        nf = remap_scene(frames, pix_all, stats)
        blob = ser_awv(nf)
        # sanity: no slot >= 448, and re-parse ok
        chk, _ = parse_awv(blob, 0)
        assert len(chk) == len(frames)
        mx = max([s for f in chk for _, s in f[3]] + [s for f in chk for _, s in f[4]])
        assert mx < N_SLOTS, mx
        nup = sum(len(f[3]) for f in chk); nc = sum(len(f[4]) for f in chk)
        onup = sum(len(f[3]) for f in frames); onc = sum(len(f[4]) for f in frames)
        print(f'scene {i:2d}: frames={len(chk):4d} ups {onup}->{nup} cells {onc}->{nc} bytes {end-b}->{len(blob)}')
        new_awv.append(blob)
    print('reduction stats:', stats)
    new_awa = [rom[b:b + awa_len(rom, b)] for b in awa]
    # ---- layout ----
    out = bytearray(rom[:TILES_AT])
    out += tiles
    offs_v = []
    for blob in new_awv:
        if len(out) & 1:
            out += b'\0'
        offs_v.append(len(out))
        out += blob
    offs_a = []
    for blob in new_awa:
        if len(out) & 1:
            out += b'\0'
        offs_a.append(len(out))
        out += blob
    used = len(out)
    assert used <= ROM_SIZE, used
    out += b'\0' * (ROM_SIZE - used)
    open(dst, 'wb').write(out)
    print(f'ROM written: {dst} used={used} ({used/1048576:.2f} MiB)')
    # ---- header ----
    def b3(v):
        return '{ 0x%02X, 0x%02X, 0x%02X }' % (v & 255, (v >> 8) & 255, (v >> 16) & 255)
    h = ['/* generated by tools/awrepack.py - ROM offsets of FMV data (planar tiles) */',
         '#ifndef FMV_DATA_H', '#define FMV_DATA_H',
         f'#define FMV_NSCENES {len(awv)}',
         f'#define ROM_TILES_PLANAR 1',
         f'#define ROM_TILES_AT {TILES_AT}UL',
         f'#define ROM_NTILES {ntiles}',
         f'#define FMV_NSLOTS {N_SLOTS}',
         'static const unsigned long ROM_AWV[FMV_NSCENES] = {',
         ',\n'.join(f'  {v}UL' for v in offs_v), '};',
         'static const unsigned long ROM_AWA[FMV_NSCENES] = {',
         ',\n'.join(f'  {v}UL' for v in offs_a), '};',
         'const unsigned char ROM_AWV_B[FMV_NSCENES][3] = {',
         ',\n'.join('  ' + b3(v) for v in offs_v), '};',
         'const unsigned char ROM_AWA_B[FMV_NSCENES][3] = {',
         ',\n'.join('  ' + b3(v) for v in offs_a), '};',
         '#endif', '']
    open(hdr + '.new', 'w').write('\n'.join(h))
    print(f'header written: {hdr}.new')


if __name__ == '__main__':
    main()
