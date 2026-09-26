#!/usr/bin/env python3
"""awverify.py - exact-replay verifier for awenc streams.

Replays each .awv (slot sim + nametable + palette + tiles) and compares
reconstructed 256x192 indexed frames against the encoder's quantized source.
"""
import os, sys, struct

sys.path.insert(0, os.path.dirname(__file__))
from awenc import squish_frame, quantize_palette, decompact_tile, frame_cells, CAP, OUT, SCENES, \
    W, H, DW, DH, TW, TH, NX, NY, NC, TRIM_MARGIN

def load_tiles():
    tb = open(os.path.join(OUT, 'tiles.bin'), 'rb').read()
    ob = open(os.path.join(OUT, 'tile_off.bin'), 'rb').read()
    n = len(ob) // 3 - 1
    tiles = []
    for i in range(n):
        o = ob[3 * i] | (ob[3 * i + 1] << 8) | (ob[3 * i + 2] << 16)
        t, _ = decompact_tile(tb, o)
        tiles.append(bytes(t))
    return tiles

def verify(name, tiles):
    raw = open(os.path.join(OUT, name + '.awv'), 'rb').read()
    magic, nframes, _ = struct.unpack_from('<IHH', raw, 0)
    assert magic == 0x30565741
    p = 8
    slot_mem = [None] * 512
    nt = [0] * NC
    pal = [0] * 16
    rec = []       # (indexed_frame, dur)
    state = bytearray(4 * 4)  # dummy
    def compose():
        return [slot_mem[nt[c]] or bytes(64) for c in range(NC)]
    while True:
        flags = raw[p]; p += 1
        if flags == 0xFF:
            break
        dur = struct.unpack_from('<H', raw, p)[0]; p += 2
        if flags & 1:
            pal = list(raw[p:p + 16]); p += 16
        nup = struct.unpack_from('<H', raw, p)[0]; p += 2
        for _ in range(nup):
            did, s = struct.unpack_from('<HH', raw, p); p += 4
            slot_mem[s] = tiles[did]
        ncell = struct.unpack_from('<H', raw, p)[0]; p += 2
        for _ in range(ncell):
            c, s = struct.unpack_from('<HH', raw, p); p += 4
            nt[c] = s
        rec.append((compose(), dur))

    # rebuild expected stream from source (same logic as encoder)
    d = os.path.join(CAP, name)
    frames = open(os.path.join(d, 'frames.idx'), 'rb').read()
    pals = open(os.path.join(d, 'palettes.bin'), 'rb').read()
    n = len(frames) // (W * H)
    sq = [squish_frame(frames[i * W * H:(i + 1) * W * H]) for i in range(n)]
    palb = [bytes(quantize_palette(pals[i * 48:(i + 1) * 48])) for i in range(n)]
    last = n - 1
    while last > 0 and sq[last] == sq[last - 1]:
        last -= 1
    nkeep = min(n, last + TRIM_MARGIN)
    runs = []
    for i in range(nkeep):
        if runs and sq[i] == sq[runs[-1][0]] and palb[i] == palb[runs[-1][0]]:
            runs[-1][1] += 1
        else:
            runs.append([i, 1])
    assert len(rec) == len(runs), f'{name}: {len(rec)} != {len(runs)}'
    for (cells_now, dur), (fi, cnt) in zip(rec, runs):
        expected = [bytes(t) for t in frame_cells(sq[fi])]
        for c in range(NC):
            assert cells_now[c] == expected[c], f'{name}: cell {c} mismatch at frame {fi}'
        assert dur == max(1, (cnt * 60 + 35) // 70)
    print(f'{name}: OK ({len(rec)} runs replayed exactly)')

def main():
    tiles = load_tiles()
    print('tiles:', len(tiles))
    for name in SCENES:
        verify(name, tiles)
    print('ALL STREAMS VERIFY')

if __name__ == '__main__':
    main()
