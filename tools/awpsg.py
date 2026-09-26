#!/usr/bin/env python3
"""awpsg.py - OOTW RT_MUSIC -> Sega Master System PSG register streams.

Input : work/game_files (res 7 intro, res 138 ending), work/capture/<scene>/events.jsonl
Output: build/fmv/<scene>.awm

Module layout (RT_MUSIC):
  u16 delay@0 (row time = delay*60000/7159092 ms), 15 x (u16 resNum, u16 volume) @2,
  u8 numOrder@0x3F, orderTable[numOrder]@0x40 (pattern indices),
  patterns @0xC0: each 1024 B = 64 rows x 4 ch x (u16 note_1, u16 note_2)
  note_1: period (0x37..0xFFF), 0xFFFD = sync, 0xFFFE = stop
  note_2: sample<<12 (1..15, 0 = none) | effect<<8 (5 vol+, 6 vol-) | param

PSG reg writes: tone ch0/1/2 lo/hi = 0x80|2n / 0x80|2n+1, vol chn = 0x90|n (0 loud .. 15 mute)
tone N = round(period/16), vol = 15 - min(15, m*16/64)
Channels: paula 0->PSG0, 1->PSG1, 2->PSG2, 3->PSG2 (ch3 wins ties)

.awm stream:
  u16 start_tick60; then frames: u16 delta_ticks60, u8 nwr, nwr x (u8 reg, u8 val); end u8 0xFF
"""
import os, sys, struct, json

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from aw_data import AwData

OUT = os.path.join(HERE, '..', 'build', 'fmv')
GAME = os.path.join(HERE, '..', 'work', 'game_files')
CAP = os.path.join(HERE, '..', 'work', 'capture')

MUSIC = [('intro', 7, 15700), ('ending', 138, 15000)]  # delay override from script

TONE_LO = [0x80, 0x82, 0x84]
TONE_HI = [0x81, 0x83, 0x85]
VOL_REG = [0x90, 0x91, 0x92]


def load_module(d, res):
    b = d.load(res)
    delay = struct.unpack('>H', b[:2])[0]
    ins = []
    for i in range(15):
        rn, vol = struct.unpack('>HH', b[2 + i * 4:6 + i * 4])
        ins.append((rn, vol))
    num_order = b[0x3F]
    orders = list(b[0x40:0x40 + num_order])
    data = b[0xC0:]
    npat = len(data) // 1024
    return delay, ins, orders, data, npat


def period_to_n(p):
    n = (p + 8) // 16
    return max(1, min(1023, n))


def m_to_vol(m):
    return 15 - min(15, max(0, m) * 16 // 64)


def encode_music(scene, res, delay_ov, d):
    delay, ins, orders, data, npat = load_module(d, res)
    if delay_ov: delay = delay_ov
    row_ms = delay * 60000 / 7159092.0

    # start tick from capture events (res event with this res number)
    start70 = 0
    evpath = os.path.join(CAP, scene, 'events.jsonl')
    if os.path.exists(evpath):
        for line in open(evpath):
            e = json.loads(line)
            if e.get('type') == 'res' and e.get('res') == res:
                start70 = e.get('t', 0)
                break
    start60 = (start70 * 60) // 70

    # walk rows -> register writes at absolute ms
    writes = []   # (tick60, reg, val)
    ms = 0.0
    t60_acc = 0.0
    for oi, pat_idx in enumerate(orders):
        base = (pat_idx % npat) * 1024
        for row in range(64):
            t60_acc = (oi * 64 + row) * row_ms * 0.06
            tick = start60 + int(round(t60_acc))
            row_writes = {}
            for ch in range(4):
                p = base + row * 16 + ch * 4
                n1, n2 = struct.unpack('>HH', data[p:p + 4])
                sample = (n2 >> 12) & 0xF
                effect = (n2 >> 8) & 0xF
                param = n2 & 0xFF
                if n1 == 0xFFFD:
                    continue
                psg_ch = 2 if ch == 3 else ch
                if n1 == 0xFFFE:
                    row_writes[VOL_REG[psg_ch]] = 15
                    continue
                if sample != 0 and n1 != 0:
                    rn, svol = ins[sample - 1]
                    if rn == 0:
                        continue
                    m = svol
                    if effect == 5:
                        m = min(0x3F, m + param)
                    elif effect == 6:
                        m = max(0, m - param)
                    n = period_to_n(n1)
                    row_writes[TONE_LO[psg_ch]] = n & 0x0F
                    row_writes[TONE_HI[psg_ch]] = (n >> 4) & 0x3F
                    row_writes[VOL_REG[psg_ch]] = m_to_vol(m)
            # ch3 wins over ch2: apply in order ch2 then ch3 already done via dict key collision
            for reg, val in row_writes.items():
                writes.append((tick, reg, val))

    # coalesce same-tick, sort, emit deltas
    writes.sort(key=lambda w: (w[0], w[1]))
    out = bytearray()
    out += struct.pack('<H', start60)
    cur = start60
    i = 0
    nframes = 0
    while i < len(writes):
        t = writes[i][0]
        batch = []
        while i < len(writes) and writes[i][0] == t:
            batch.append((writes[i][1], writes[i][2]))
            i += 1
        delta = t - cur
        cur = t
        out += struct.pack('<HB', delta, len(batch))
        for reg, val in batch:
            out += bytes([reg, val])
        nframes += 1
    out += bytes([0xFF])
    open(os.path.join(OUT, scene + '.awm'), 'wb').write(bytes(out))
    print(f'{scene}: row_ms={row_ms:.1f} orders={len(orders)} writes={len(writes)} '
          f'frames={nframes} bytes={len(out)} start={start60}')
    return len(out)


def main():
    os.makedirs(OUT, exist_ok=True)
    d = AwData(GAME)
    tot = 0
    for scene, res, delay_ov in MUSIC:
        tot += encode_music(scene, res, delay_ov, d)
    print('music total bytes:', tot)


if __name__ == '__main__':
    main()
