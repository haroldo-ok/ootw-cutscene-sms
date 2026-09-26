#!/usr/bin/env python3
"""awsfx.py - OOTW RT_SOUND sfx -> PSG writes, merged with music into scene .awa.

RT_SOUND layout: [u16 lenW BE][u16 loopW BE][4 pad][PCM s8 lenW*2][loop s8 loopW*2]
Events (work/capture/<s>/events.jsonl):
  sfx: {t, res, freq(0..39 period idx), vol(0..63, 0=stop), ch(0..3 Paula)}
  rate_hz = 7159092 / (PERIOD[freq]*2)
Channel map: ch0->PSG0, ch1->PSG1, ch2->PSG2, ch3->PSG noise (reg6 rate, vol3)
Looped wavetable (loopW*2 <= 96 B): tone at rate/(loop bytes) Hz until stopped.
Recording (large/zero loop): tone-vs-noise by autocorrelation; volume envelope every 2 ticks.

Merged stream (scene .awa):
  u16 start0 (0); frames: u16 delta_ticks60, u8 nwr, nwr x (u8 reg, u8 val); end u8 0xFF
Music writes (.awm from awpsg.py) are merged in; sfx applied after (wins same-tick).
"""
import os, sys, struct, json, math

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from aw_data import AwData

OUT = os.path.join(HERE, '..', 'build', 'fmv')
GAME = os.path.join(HERE, '..', 'work', 'game_files')
CAP = os.path.join(HERE, '..', 'work', 'capture')

SCENES = ['titles', 'intro', 'open16002', 'mut16002', 'open16003', 'cistern16003',
          'escape16003', 'open16004', 'exit16004', 'open16005', 'exit16005',
          'open16006', 'gate16006', 'ending', 'password']

PERIOD = [
    1076, 1016, 960, 906, 856, 808, 762, 720, 678, 640,
    604, 570, 538, 508, 480, 453, 428, 404, 381, 360,
    339, 320, 302, 285, 269, 254, 240, 226, 214, 202,
    190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
]

TONE_LO = [0x80, 0x82, 0x84]
TONE_HI = [0x81, 0x83, 0x85]
VOL_REG = [0x90, 0x91, 0x92]
NOISE_REG = 0xE0      # player OUTs (0xE0|val) for noise control
NOISE_VOL = 0x93

_res_cache = {}
_info_cache = {}


def snd_info(d, res):
    if res in _info_cache:
        return _info_cache[res]
    b = d.load(res)
    if len(b) < 4:
        info = dict(lenB=0, loopB=0, is_loop_tone=False, periodic=False, lag=0, env=[0])
        _info_cache[res] = info
        return info
    lenW, loopW = struct.unpack('>HH', b[:4])
    pcm = list(struct.unpack(f'{lenW * 2}b', b[8:8 + lenW * 2])) if lenW else []
    loop = b[8 + lenW * 2:8 + lenW * 2 + loopW * 2]
    loopB = loopW * 2
    # classify
    is_loop_tone = 0 < loopB <= 96
    periodic = False
    lag_best = 0
    if len(pcm) > 256:
        # autocorrelation on a 512-sample window
        w = pcm[64:576]
        m = sum(w) / len(w)
        w = [x - m for x in w]
        e0 = sum(x * x for x in w) or 1
        best, bestlag = 0.0, 0
        for lag in range(8, min(200, len(w) // 2)):
            s = sum(w[i] * w[i + lag] for i in range(0, len(w) - lag))
            r = s / e0
            if r > best:
                best, bestlag = r, lag
        periodic = best > 0.6
        lag_best = bestlag
    env = []
    if pcm:
        step = max(1, len(pcm) // 48)
        for i in range(0, len(pcm), step):
            chunk = pcm[i:i + step]
            env.append(max(abs(x) for x in chunk) if chunk else 0)
    info = dict(lenB=len(pcm), loopB=loopB, is_loop_tone=is_loop_tone,
                periodic=periodic, lag=lag_best, env=env)
    _info_cache[res] = info
    return info


def hz_to_n(hz):
    n = int(round(3579545.0 / (16.0 * hz)))
    return max(1, min(1023, n))


def m_to_vol(m):
    return 15 - min(15, max(0, m) * 16 // 64)


def sfx_writes(t60, res, freq_idx, vol, ch, info, out):
    """Emit register writes for one sfx event into out list [(tick, reg, val, prio)]."""
    rate = 7159092.0 / (PERIOD[freq_idx % 40] * 2.0)
    psg_ch = 2 if ch == 3 else ch
    noise = (ch == 3) or (not info['is_loop_tone'] and not info['periodic'])
    if vol == 0 or info['lenB'] == 0:
        # stop
        if ch == 3:
            out.append((t60, NOISE_VOL, 15, 1))
        else:
            out.append((t60, VOL_REG[psg_ch], 15, 1))
        return
    m0 = vol
    if info['is_loop_tone']:
        loopB = info['loopB']
        hz = rate / loopB
        n = hz_to_n(max(20.0, min(8000.0, hz)))
        out.append((t60, TONE_LO[psg_ch], n & 0x0F, 1))
        out.append((t60, TONE_HI[psg_ch], (n >> 4) & 0x3F, 1))
        out.append((t60, VOL_REG[psg_ch], m_to_vol(m0), 1))
        return
    # recording: envelope
    dur_ms = info['lenB'] / rate * 1000.0
    env = info['env'] or [127]
    nseg = max(1, int(dur_ms / 32.0 + 0.5))
    if noise:
        out.append((t60, NOISE_REG, 6, 1))  # noise: rate 2, white feedback (latch 0xE0|6)
    else:
        n = hz_to_n(max(40.0, min(8000.0, rate / max(1, info['lag']))))
        out.append((t60, TONE_LO[psg_ch], n & 0x0F, 1))
        out.append((t60, TONE_HI[psg_ch], (n >> 4) & 0x3F, 1))
    vreg = NOISE_VOL if noise else VOL_REG[psg_ch]
    for k in range(nseg + 1):
        e = env[min(len(env) - 1, k * len(env) // max(1, nseg))]
        m = m0 * e // 127
        t = t60 + (k * dur_ms / 1000.0 * 60.0)
        out.append((int(round(t)), vreg, m_to_vol(m), 1))
    out.append((int(round(t60 + dur_ms / 1000.0 * 60.0)) + 1, vreg, 15, 1))


def merge_scene(scene, d):
    writes = []
    # music
    awm = os.path.join(OUT, scene + '.awm')
    if os.path.exists(awm):
        raw = open(awm, 'rb').read()
        p = 2
        cur = struct.unpack_from('<H', raw, 0)[0]
        while raw[p] != 0xFF:
            delta, nwr = struct.unpack_from('<HB', raw, p)
            p += 3
            cur += delta
            for _ in range(nwr):
                reg, val = raw[p], raw[p + 1]
                p += 2
                writes.append((cur, reg, val, 0))
    # sfx
    evs = [json.loads(l) for l in open(os.path.join(CAP, scene, 'events.jsonl'))]
    for e in evs:
        if e.get('type') == 'sfx':
            t60 = e['t'] * 60 // 70
            info = snd_info(d, e['res'])
            sfx_writes(t60, e['res'], e['freq'], e['vol'], e['ch'], info, writes)
        elif e.get('type') == 'sfxstop':
            t60 = e['t'] * 60 // 70
            ch = e['ch']
            writes.append((t60, NOISE_VOL if ch == 3 else VOL_REG[2 if ch == 3 else ch], 15, 1))
    writes.sort(key=lambda w: (w[0], w[3]))
    out = bytearray()
    out += struct.pack('<H', 0)
    cur = 0
    i = 0
    nframes = 0
    while i < len(writes):
        t = writes[i][0]
        batch = {}
        while i < len(writes) and writes[i][0] == t:
            batch[writes[i][1]] = writes[i][2]
            i += 1
        delta = t - cur
        cur = t
        batch = sorted(batch.items())
        out += struct.pack('<HB', delta, len(batch))
        for reg, val in batch:
            out += bytes([reg, val])
        nframes += 1
    out += struct.pack('<HB', 0, 0xFF)   # end: nwr==0xFF marker (Z80 reader)
    open(os.path.join(OUT, scene + '.awa'), 'wb').write(bytes(out))
    return len(out), nframes


def main():
    d = AwData(GAME)
    # pre-analyze all used resources
    used = set()
    for scene in SCENES:
        for line in open(os.path.join(CAP, scene, 'events.jsonl')):
            e = json.loads(line)
            if e.get('type') == 'sfx':
                used.add(e['res'])
    for res in sorted(used):
        info = snd_info(d, res)
        kind = 'tone-loop' if info['is_loop_tone'] else ('rec-periodic' if info['periodic'] else 'rec-noise')
        print(f'  snd {res}: {kind} lenB={info["lenB"]} loopB={info["loopB"]}')
    tot = 0
    for scene in SCENES:
        nb, nf = merge_scene(scene, d)
        tot += nb
        print(f'{scene}: audio bytes={nb} frames={nf}')
    print('audio total:', tot)


if __name__ == '__main__':
    main()
