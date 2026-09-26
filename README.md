# Out of This World — Sega Master System Cutscene Viewer

A port of the cutscenes of *Out of This World* (*Another World*, Éric Chahi, 1991)
to the Sega Master System, delivered as a 4 MB ROM with a menu from which any of
the game's 15 cutscenes can be played as full-motion video with its music.

Gameplay is intentionally **not** ported (yet): this is a cutscene viewer.

## What you get

| Item | Description |
|---|---|
| `rom/ootw-sms.sms` | 4 MiB Sega Master System ROM (Sega mapper, 256 × 16 KiB pages) |
| Menu | 15 cutscenes; ↑/↓ to choose, button 1 to play, button 2 to stop |
| Video | Real FMV converted from the DOS game data — one shared tile dictionary (57,446 tiles) + per-frame tile uploads, tilemap deltas and palettes at 60 Hz |
| Audio | The cutscene music converted from the original sound-tracker modules to SN76489 PSG register streams |

Cutscenes: TITLES, INTRO, CITY 1 OPENING, MUTATION, CITY 2 OPENING, CISTERN DROP,
ESCAPE FALL, CITY 3 OPENING, SEA DEPARTURE, CITY 4 OPENING, BEAM DEPARTURE,
CITY 5 OPENING, FOREST GATE, ENDING, PASSWORD.

## How it works

### Data pipeline (PC side, Python)

1. **Extraction** – `tools/dump_resources.py` / `tools/aw_data.py` unpack the DOS
   `memlist.bin` + `bank*` files (Bytekiller compression), using
   [rawgl](https://github.com/cyxx/rawgl) as the format reference.
2. **Capture** – `tools/awcap/` runs the original bytecode/vector engine off-line
   and records every cutscene as a sequence of 256×192, 16-colour frames with
   exact timings and the palette changes.
3. **Encoding** – `tools/awenc.py` converts the frame sequences into the `AWV`
   stream format: frames are cut into 8×8 tiles, deduplicated into one dictionary
   shared by *all* scenes, and each frame stores only the tiles that need to be
   (re)uploaded to VRAM plus the tilemap cells that changed. Frame durations are
   stored in 60 Hz ticks.
4. **Music** – `tools/awpsg.py` / `tools/awsfx.py` translate the tracker modules
   (period tables, volumes, order lists) into timed PSG register writes (`AWA`).
5. **Repack** – `tools/awrepack.py` lays out the final ROM: 32 KiB of code, the
   tile dictionary **pre-converted to VDP 4-bitplane format** (32 bytes/tile at
   `0x8000 + id*32`, so a tile upload is 32 `OUTI`s), then the video and audio
   streams. It also constrains every scene to VRAM slots 1..447 so tiles never
   overlap the name table at `0x3800`; frames that need more distinct tiles are
   reduced by nearest-tile substitution using real pixel distance.
6. **Splice** – `tools/awsplice.py` drops a freshly compiled 32 KiB code image
   into the 4 MB ROM and fixes the Sega header checksum.

### Player (SMS side, SDCC C + Z80 assembly)

`rom/src/main.c` implements the menu and the streaming player:

- frame sync on the VDP **V-counter wrap** (port `0x7E`) — status-register bit 7
  is "IRQ pending" on many emulators and cannot be used for pacing with `di`;
- a per-frame work budget (`TICK_UNITS`) that ships tile uploads and tilemap
  cells at a rate that fits one frame, with hot loops in Z80 assembly
  (`aw_tile`, `aw_ups`, `aw_cells`; VRAM writes spaced ≥ 28 cycles so they are
  safe during active display);
- time spent shipping a frame's data is deducted from that frame's hold, so
  normal scenes play in real time (TITLES: 249 ticks vs 248 scheduled);
- PSG audio streams are serviced every tick;
- 24-bit ROM pointers with bank switching through the Sega mapper.

## Building

Requirements: `sdcc` (4.x, Z80), `python3` (+ `numpy` for `awrepack.py`), `gcc`.
`toolchain/` carries the SMSlib/PSGlib headers and libraries plus `crt0_sms.rel`.

```bash
cd rom
T=../toolchain
sdcc -mz80 --sdcccall 0 -I$T/include -c src/main.c -o main.rel
sdcc -o code.ihx -mz80 --sdcccall 0 --no-std-crt0 --data-loc 0xC000 \
     $T/lib/crt0_sms.rel main.rel $T/lib/SMSlib.lib
ihx2sms code.ihx code.sms                       # "SEGA header found, checksum updated"
python3 ../tools/awsplice.py code.sms ootw-sms.sms
```

After every build make sure no runtime division/modulo helper crept in
(`grep -c 'call.*__mod\|call.*__div' main.asm` must print `0`) — SDCC's
helpers are not safe in this player.

To regenerate the data layout from an existing ROM (e.g. after changing the
slot budget): `python3 tools/awrepack.py <old.sms> <new.sms> rom/src/fmv_data.h`
then copy `rom/src/fmv_data.h.new` over `fmv_data.h` and rebuild the code.

## Testing

### End-to-end test with PicoDrive

`tools/e2e/pd_e2e.c` loads PicoDrive's libretro core as a shared library, boots
the ROM, drives the joypad, screenshots each step and reads the player's RAM
probes through the linker map (`rom/code.noi`). It asserts that the menu
renders, the cursor moves, **confirming a selection starts the video**, the
video progresses and completes (`g_endflag == 0x77`), the menu is restored, and
the stop button aborts back to the menu.

```bash
git clone --recursive https://github.com/notaz/PicoDrive && make -C PicoDrive -f Makefile.libretro
gcc -O2 -o tools/e2e/pd_e2e tools/e2e/pd_e2e.c -ldl -lz \
    -I PicoDrive/platform/libretro/libretro-common/include
E2E_SCENE=0 ./tools/e2e/pd_e2e PicoDrive/picodrive_libretro.so rom/ootw-sms.sms rom/code.noi out/
```

`E2E_SCENE=n` selects which cutscene scenario A plays. Controller mapping in
PicoDrive: RetroPad **B** = SMS button 1 (confirm), RetroPad **A** = SMS button
2 (stop).

### Scripted regression

`rom/tests/playtest.txt` is a script for `smstest` (from the
[sega-master-system-skill-for-claude](https://github.com/haroldo-ok/sega-master-system-skill-for-claude)
toolkit) covering boot, menu navigation and scene start/stop.

## Repository layout

```
rom/src/main.c        player + menu (C with inline Z80 asm)
rom/src/fmv_data.h    generated ROM offsets of the FMV data
rom/src/font.h        menu font
rom/tests/            smstest script
rom/ootw-sms.sms      the 4 MB ROM
tools/                extraction, capture, encoders, repack, splice, verify
tools/e2e/pd_e2e.c    PicoDrive end-to-end test
toolchain/            SMSlib/PSGlib headers, libraries, crt0
```

## Known limitations

- Video frames that introduce several hundred new tiles at once cannot be
  shipped in 1/60 s on a 3.58 MHz Z80; the densest moments (e.g. MUTATION,
  CITY 5 OPENING) run about 1.4–1.7× slower than scheduled. Everything else
  plays in real time.
- Colours are the SMS's 64-colour RGB222 approximation of the original palettes.
- Frames that needed more than 447 distinct tiles use nearest-tile substitutes
  (about 10 % of the tiles in ~200 frames of the two densest scenes).

## Credits

*Another World* / *Out of This World* © Éric Chahi / Delphine Software.
Format reference: [rawgl](https://github.com/cyxx/rawgl). SMS methodology and
tooling: [sega-master-system-skill-for-claude](https://github.com/haroldo-ok/sega-master-system-skill-for-claude),
[devkitSMS](https://github.com/sverx/devkitSMS). Emulator for E2E:
[PicoDrive](https://github.com/notaz/PicoDrive).
