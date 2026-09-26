/* ---------------------------------------------------------------------------
 * Out of This World - Sega Master System cutscene viewer
 *
 * Plays 15 FMV cutscenes converted from the DOS game data (awenc streams).
 * Shared tile dictionary (compact forms decompacted on upload), per-frame
 * 16-colour palettes, nametable deltas, 60 Hz durations, PSG audio streams.
 *
 * VRAM budget per VBlank: 6 tile uploads + 60 nametable cells.
 * ------------------------------------------------------------------------- */
#include "SMSlib.h"
#include "fmv_data.h"

extern const unsigned char ROM_AWV_B[FMV_NSCENES][3];
extern const unsigned char ROM_AWA_B[FMV_NSCENES][3];

#include "font.h"

volatile unsigned char g_mile;
volatile unsigned char g_autoplay;
volatile unsigned int g_vblcalls;
volatile unsigned int g_vblspin;
volatile unsigned int g_keyval;
volatile unsigned int g_whybreak;
volatile unsigned int g_rawjoy;
volatile unsigned int g_pal1;
volatile unsigned int g_pal15;
volatile unsigned int g_uploads;
volatile unsigned int g_cells;
volatile unsigned int g_ticks;
volatile unsigned int g_firstdid;
volatile unsigned int g_firstcell;
volatile unsigned int g_firstcellslot;
volatile unsigned int g_endflag;
volatile unsigned int g_dbg1;
volatile unsigned int g_dbg2;
volatile unsigned int g_dbg3;
volatile unsigned char g_rbytes[8];
volatile unsigned char g_rnb;
volatile unsigned char g_rbank;
volatile unsigned int g_vflags;
volatile unsigned int g_vdur;
volatile unsigned int g_vnup;
volatile unsigned int g_vncell;
volatile unsigned int g_vpos2;
volatile unsigned char g_vstage;

SMS_EMBED_SEGA_ROM_HEADER(20261, 1);

__sfr __at (0x7F) PSGPort;
__sfr __at (0xDC) JOYPAD1;

static unsigned char joy_held(void) {
    unsigned char raw = JOYPAD1;    /* active low */
    return (unsigned char)(~raw);   /* active high, bits 0-5 */
}
__sfr __at (0xBE) VDPDATA_;
__sfr __at (0xBF) VDPCTRL_;

#define FONT0        0      /* font glyphs occupy tiles 0..95 (glyph ' ' = tile 0 = blank) */
#define NCELLS       768
#define TICK_UNITS   360
#define UNIT_TILE    8


/* ---- direct VDP port writes (avoid SMSlib ABI crossings in hot path) ---- */
__sfr __at (0xBE) VDPDATA;
__sfr __at (0xBF) VDPCTRL;

__sfr __at (0x7E) VDPVCTR;

/* Frame sync via the V-counter wrap: portable across emulators and
 * hardware.  (Status bit7 is "IRQ pending" on many emulators - with `di`
 * it sticks high forever and a status poll would spin to its guard.) */
static void wait_vbl(void) {
    unsigned char last, c;
    unsigned int guard = 0;
    unsigned char stuck = 0;
    g_vblcalls++;
    last = VDPVCTR;
    for (;;) {
        c = VDPVCTR;
        if (c < last) return;              /* counter wrapped: new frame */
        if (c == last) {
            if (++stuck == 200) { g_vblspin = 2; return; }  /* dead port */
        } else { stuck = 0; last = c; }
        if (++guard == 30000) { g_vblspin = 1; return; }
    }
}

static void vdp_addr(unsigned int a) {
    VDPCTRL = (unsigned char)(a & 0xFF);
    VDPCTRL = (unsigned char)(0x40 | ((a >> 8) & 0x3F));
}
static void vdp_loc(unsigned int loc) {
    vdp_addr(0x7800u + (loc << 1));
}
static void vdp_tile(unsigned int t) {
    VDPDATA = (unsigned char)(t & 0xFF);
    VDPDATA = (unsigned char)((t >> 8) & 0x01);
}
static void vdp_bulk(unsigned char *src, unsigned int dest, unsigned int size) {
    unsigned int i;
    vdp_addr(dest);
    for (i = 0; i < size; i++) VDPDATA = src[i];
}
static void vdp_tiles(unsigned char *src, unsigned int tf, unsigned int size) {
    vdp_bulk(src, (unsigned int)(tf * 32u), size);
}
static unsigned char vdp_r1 = 0x40;
static void vdp_disp(unsigned char on) {
    vdp_r1 = (unsigned char)(on ? 0x40 : 0x00);
    VDPCTRL = vdp_r1;
    VDPCTRL = 0x81;
}
static void vdp_pal16(unsigned char *p) {
    unsigned char i;
    VDPCTRL = 0x00;
    VDPCTRL = 0xC0;
    for (i = 0; i < 16; i++) VDPDATA = p[i];
}

/* ---- 24-bit ROM readers (pure byte ops: no long math, no helpers) ---- */
static unsigned char curbank = 0xFF;

static void sel_bank(const unsigned char *p) {
    /* volatile: SDCC 4.5 miscompiles the fused compare-then-store form */
    volatile unsigned char b;
    volatile unsigned char want;
    want = (unsigned char)((unsigned char)(p[2] << 2) | (unsigned char)(p[1] >> 6));
    b = want;
    if (b == curbank) return;
    b = want;
    curbank = b;
    SMS_mapROMBank(b);
}

static unsigned char rom_rd(unsigned char *p) {
    unsigned char v;
    unsigned int page;
    sel_bank(p);
    page = (unsigned int)(((unsigned int)(p[1] & 0x3F) << 8) | p[0]);
    v = *(volatile unsigned char *)(0x8000u + page);
    if (++p[0] == 0) { if (++p[1] == 0) ++p[2]; }
    return v;
}

static unsigned int rom_rd16(unsigned char *p) {
    unsigned int lo = rom_rd(p);
    return lo | ((unsigned int)rom_rd(p) << 8);
}

static void rom_rd24(unsigned char *p) {
    unsigned char a = rom_rd(p);
    unsigned char b = rom_rd(p);
    unsigned char c = rom_rd(p);
    p[0] = a; p[1] = b; p[2] = c;
}
#if 0
    unsigned long a = rom_rd(p);
    a |= (unsigned long)rom_rd(p) << 8;
    a |= (unsigned long)rom_rd(p) << 16;
    return a;
}
#endif

/* ---- tile decompaction (compact forms -> 32-byte VRAM tile) -------------- */
static unsigned char tbuf[32];
static unsigned char bb[16];

static unsigned char pix[64];


/* ---- palette translate xxRRGGBB -> CRAM 00BBGGRR ------------------------ */
static unsigned char palbuf[16];

static unsigned char xlat_cram(unsigned char p) {
    return (unsigned char)(((p & 3) << 4) | (p & 0x0C) | ((p >> 4) & 3));
}

/* ---- PSG register stream writes ---------------------------------------- */
static void psg_write(unsigned char reg, unsigned char val) {
    switch (reg) {
    case 0x80: PSGPort = (unsigned char)(0x80 | (val & 15)); break;
    case 0x82: PSGPort = (unsigned char)(0xA0 | (val & 15)); break;
    case 0x84: PSGPort = (unsigned char)(0xC0 | (val & 15)); break;
    case 0x81: case 0x83: case 0x85: PSGPort = (unsigned char)(val & 63); break;
    case 0x90: PSGPort = (unsigned char)(0x90 | (val & 15)); break;
    case 0x91: PSGPort = (unsigned char)(0xB0 | (val & 15)); break;
    case 0x92: PSGPort = (unsigned char)(0xD0 | (val & 15)); break;
    case 0x93: PSGPort = (unsigned char)(0xF0 | (val & 15)); break;
    case 0xE0: PSGPort = (unsigned char)(0xE0 | (val & 7)); break;
    }
}

static void psg_silence(void) {
    PSGPort = 0x9F; PSGPort = 0xBF; PSGPort = 0xDF; PSGPort = 0xFF;
}

/* ---- menu --------------------------------------------------------------- */
static const char * const SCENE_NAMES[FMV_NSCENES] = {
    "TITLES", "INTRO", "CITY 1 OPENING", "MUTATION", "CITY 2 OPENING",
    "CISTERN DROP", "ESCAPE FALL", "CITY 3 OPENING", "SEA DEPARTURE",
    "CITY 4 OPENING", "BEAM DEPARTURE", "CITY 5 OPENING", "FOREST GATE",
    "ENDING", "PASSWORD"
};

static void draw_str(unsigned char x, unsigned char y, const char *s) {
    SMS_setNextTileatXY(x, y);
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 32 && c < 128) vdp_tile(c - 32); else vdp_tile(0);
    }
}

static void menu_cursor(unsigned char ow, unsigned char nw) {
    vdp_loc((unsigned int)((5u + (unsigned int)ow) * 32u + 3u));
    vdp_tile(0);
    vdp_loc((unsigned int)((5u + (unsigned int)nw) * 32u + 3u));
    vdp_tile(30);
}
static unsigned char ldrawn = 255;
static void draw_menu_impl(unsigned char sel) {
    unsigned int i;
    char line[24];
    g_dbg1 = sel;
    if (ldrawn == sel) return;
    ldrawn = sel;
    g_dbg3 = 0xBB;
    g_mile = 10;
    vdp_disp(0);
    /* palette: 0 dark blue bg, 15 white text */
    palbuf[0] = 0x20;
    for (i = 1; i < 16; i++) palbuf[i] = 0;
    palbuf[15] = 0x3F;
    vdp_pal16(palbuf);
    g_mile = 11;
    load_font();
    g_mile = 12;
    /* clear screen */
    vdp_loc(0);
    for (i = 0; i < 28 * 32; i++) vdp_tile(0);
    draw_str(7, 1, "OUT OF THIS WORLD");
    draw_str(8, 2, "CUTSCENE VIEWER");
    draw_str(7, 3, "----------------");
    for (i = 0; i < FMV_NSCENES; i++) {
        unsigned char p = 0;
        unsigned char num = i + 1;
        line[p++] = (i == sel) ? '>' : ' ';
        line[p++] = ' ';
        if (num >= 10) {
            line[p++] = '1';
            line[p++] = (char)('0' + (num - 10));   /* scenes <= 19: no div/mod */
        } else {
            line[p++] = ' ';
            line[p++] = (char)('0' + num);
        }
        line[p++] = ' ';
        {
            const char *n = SCENE_NAMES[i];
            while (*n) line[p++] = *n++;
        }
        line[p] = 0;
        draw_str(3, 5 + i, line);
    }
    draw_str(2, 22, "UP/DOWN: SELECT  1: PLAY");
    draw_str(2, 23, "2: STOP AND RETURN TO MENU");
    g_mile = 13;
    vdp_disp(1);
    g_mile = 14;
}

/* ---- scene player ------------------------------------------------------- */
static unsigned char vpos[3], apos[3];
static unsigned char data_ticks;
static void p3_add16(unsigned char *p, unsigned int x) {
    unsigned int t = (unsigned int)p[0] + (x & 0xFF);
    p[0] = (unsigned char)t;
    t = (unsigned int)p[1] + (x >> 8) + (t >> 8);
    p[1] = (unsigned char)t;
    p[2] = (unsigned char)(p[2] + (unsigned char)(t >> 8));
}
static void p3_set(unsigned char *p, const unsigned char *src) {
    p[0] = src[0]; p[1] = src[1]; p[2] = src[2];
}
static unsigned int  dur_left, nup_left, ncell_left;
static unsigned char vphase;      /* 0 hdr 1 uploads 2 cells 3 wait */
static unsigned char scene_done;
static unsigned int  a_wait, a_wr_left;
static unsigned char a_done;

static void load_font(void) {
    unsigned char g, r, bits;
    for (g = 0; g < 96; g++) {
        for (r = 0; r < 8; r++) {
            bits = ootw_font[g * 8 + r];
            tbuf[r * 4 + 0] = bits;
            tbuf[r * 4 + 1] = bits;
            tbuf[r * 4 + 2] = bits;
            tbuf[r * 4 + 3] = bits;
        }
        vdp_bulk(tbuf, (unsigned int)((FONT0 + g) * 32u), 32);
    }
}

static void scene_init(unsigned char n) {
    unsigned int i;
    g_mile = 3; vdp_disp(0);
    psg_silence();
    /* blank tile 0 + nametable all 0 + black palette */
    for (i = 0; i < 32; i++) tbuf[i] = 0;
    vdp_tiles(tbuf, 0, 32);
    vdp_loc(0);
    for (i = 0; i < 28 * 32; i++) vdp_tile(0);
    for (i = 0; i < 16; i++) palbuf[i] = 0;
    vdp_pal16(palbuf);
    vpos[0] = ROM_AWV_B[n][0]; vpos[1] = ROM_AWV_B[n][1]; vpos[2] = ROM_AWV_B[n][2]; p3_add16(vpos, 8);  /* +8 skips AWV0 magic */
    data_ticks = 0;
    apos[0] = ROM_AWA_B[n][0];
    apos[1] = ROM_AWA_B[n][1];
    apos[2] = ROM_AWA_B[n][2];
    p3_add16(apos, 2);                        /* skip start field */
    g_dbg1 = (unsigned int)(((unsigned int)(vpos[1] & 0x3F) << 8) | vpos[0]);
    g_dbg2 = vpos[2];
    a_wait = 0; a_wr_left = 0; a_done = 0;
    vphase = 0; scene_done = 0;
    g_uploads = 0; g_cells = 0; g_ticks = 0; g_firstdid = 0xFFFF; g_rnb = 0; g_rbank = 0xAA; g_vstage = 0;
    g_firstcell = 0xFFFF; g_firstcellslot = 0xFFFF; g_endflag = 0;
    vdp_disp(1);
}

static void service_audio(void) {
    unsigned char i, n, reg, val;
    if (a_done) return;
    if (a_wait) { a_wait--; return; }
    if (a_wr_left) {
        n = (unsigned char)a_wr_left;
        for (i = 0; i < n; i++) {
            reg = rom_rd(apos);
            val = rom_rd(apos);
            psg_write(reg, val);
        }
        a_wr_left = 0;
        return;
    }
    {
        unsigned char dlo = rom_rd(apos);
        unsigned char dhi = rom_rd(apos);
        unsigned char nwr = rom_rd(apos);
        if (nwr == 0xFF) { a_done = 1; return; }
        a_wr_left = nwr;
        a_wait = ((unsigned int)dhi << 8) | dlo;
    }
}

static unsigned char awbank, awdone;

/* Upload one planar tile from ROM (did) to VRAM slot.  Maps the tile bank
 * itself and restores the mapper to curbank afterwards.  Byte spacing is
 * 28 cycles: safe for VRAM writes during active display. */
static void aw_tile(unsigned int did, unsigned int slot) __naked {
    did; slot;
    __asm
    push ix
    ld ix,#0
    add ix,sp
    ld e,4 (ix)
    ld d,5 (ix)              ; DE = did
    ld l,6 (ix)
    ld h,7 (ix)              ; HL = slot
    add hl,hl
    add hl,hl
    add hl,hl
    add hl,hl
    add hl,hl                ; HL = slot*32
    ld a,l
    out (0xBF),a
    ld a,h
    or #0x40
    out (0xBF),a             ; VRAM write address
    ld a,d
    srl a
    add a,#2
    ld (0xFFFF),a            ; tile bank = 2 + (did>>9)
    ld a,d
    and #1
    ld h,a
    ld l,e                   ; HL = did & 511
    add hl,hl
    add hl,hl
    add hl,hl
    add hl,hl
    add hl,hl
    set 7,h                  ; HL = 0x8000 + (did&511)*32
    ld c,#0xBE
    .rept 32
    outi
    nop
    nop
    nop
    .endm
    ld a,(_curbank)
    ld (0xFFFF),a            ; back to the stream bank
    pop ix
    ret
    __endasm;
}

/* Fetch the stream pointer vpos -> mapper + HL.  Shared prologue. */

/* Store HL/awbank back into vpos, return awdone in L. */

/* Process up to n upload records (did,slot) straight from the mapped
 * stream.  Stops early (returns count done) if the next record would
 * straddle a 16K page; the caller then handles one record the slow way. */
static unsigned char aw_ups(unsigned char n) __naked {
    n;
    __asm
    push ix
    ld ix,#0
    add ix,sp
    ld a,(_vpos+2)
    add a,a
    add a,a
    ld c,a
    ld a,(_vpos+1)
    ld d,a
    rlca
    rlca
    and #3
    or c
    ld (_curbank),a
    ld (_awbank),a
    ld (0xFFFF),a
    ld a,d
    and #0x3F
    or #0x80
    ld h,a
    ld a,(_vpos+0)
    ld l,a
    xor a
    ld (_awdone),a
    1$:
    ld a,4 (ix)
    or a
    jp z,9$
    ld a,h
    cp #0xBF
    jr nz,2$
    ld a,l
    cp #0xFD
    jp nc,9$
    2$:
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    ld c,(hl)
    inc hl
    ld b,(hl)
    inc hl
    push hl
    push bc
    push de
    call _aw_tile
    pop de
    pop bc
    pop hl
    ld a,h
    cp #0xC0
    jr nz,3$
    ld h,#0x80
    ld a,(_awbank)
    inc a
    ld (_awbank),a
    ld (_curbank),a
    ld (0xFFFF),a
    3$:
    dec 4 (ix)
    ld a,(_awdone)
    inc a
    ld (_awdone),a
    jp 1$
    9$:
    ld a,l
    ld (_vpos+0),a
    ld a,(_awbank)
    ld b,a
    rrca
    rrca
    and #0xC0
    ld c,a
    ld a,h
    and #0x3F
    or c
    ld (_vpos+1),a
    ld a,b
    srl a
    srl a
    ld (_vpos+2),a
    ld a,(_awdone)
    ld l,a
    pop ix
    ret
    __endasm;
}

/* Process up to n cell records (cell,slot) -> nametable. */
static unsigned char aw_cells(unsigned char n) __naked {
    n;
    __asm
    push ix
    ld ix,#0
    add ix,sp
    ld a,(_vpos+2)
    add a,a
    add a,a
    ld c,a
    ld a,(_vpos+1)
    ld d,a
    rlca
    rlca
    and #3
    or c
    ld (_curbank),a
    ld (_awbank),a
    ld (0xFFFF),a
    ld a,d
    and #0x3F
    or #0x80
    ld h,a
    ld a,(_vpos+0)
    ld l,a
    xor a
    ld (_awdone),a
    1$:
    ld a,4 (ix)
    or a
    jp z,9$
    ld a,h
    cp #0xBF
    jr nz,2$
    ld a,l
    cp #0xFD
    jp nc,9$
    2$:
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl                   ; DE = cell
    ex de,hl
    add hl,hl                ; HL = cell*2
    ld a,l
    out (0xBF),a
    ld a,h
    or #0x78
    out (0xBF),a             ; nametable 0x3800 | write
    ex de,hl
    ld a,(hl)
    inc hl
    out (0xBE),a             ; slot lo
    ld a,(hl)
    inc hl
    and #1
    nop
    nop
    nop
    nop
    out (0xBE),a             ; slot hi (>=28 cycles after previous write)
    ld a,h
    cp #0xC0
    jr nz,3$
    ld h,#0x80
    ld a,(_awbank)
    inc a
    ld (_awbank),a
    ld (_curbank),a
    ld (0xFFFF),a
    3$:
    dec 4 (ix)
    ld a,(_awdone)
    inc a
    ld (_awdone),a
    jp 1$
    9$:
    ld a,l
    ld (_vpos+0),a
    ld a,(_awbank)
    ld b,a
    rrca
    rrca
    and #0xC0
    ld c,a
    ld a,h
    and #0x3F
    or c
    ld (_vpos+1),a
    ld a,b
    srl a
    srl a
    ld (_vpos+2),a
    ld a,(_awdone)
    ld l,a
    pop ix
    ret
    __endasm;
}

static void service_video(void) {
    unsigned char i;
    unsigned int units = TICK_UNITS;
    if (vphase != 3) data_ticks++;
    if (vphase == 0) {
        unsigned char flags = rom_rd(vpos);
        if (g_vstage == 0) { g_vstage = 1; g_vflags = flags; g_vpos2 = vpos[0]; }
        if (flags == 0xFF) { scene_done = 1; g_endflag = 0x77; return; }
        dur_left = rom_rd16(vpos);
        if (flags & 1) {
            for (i = 0; i < 16; i++) palbuf[i] = xlat_cram(rom_rd(vpos));
            vdp_pal16(palbuf);
            g_pal1 = palbuf[1];
            g_pal15 = palbuf[15];
        }
        nup_left = rom_rd16(vpos);
        if (g_vstage == 1) { g_vstage = 2; g_vdur = dur_left; g_vnup = nup_left; }
        vphase = 1;
    }
    if (vphase == 1) {
        while (nup_left && units >= UNIT_TILE) {
            volatile unsigned int want = units >> 3;
            unsigned char done;
            if (want > nup_left) want = nup_left;
            if (want > 200) want = 200;
            done = aw_ups((unsigned char)want);
            if (!done) {                      /* record straddles a page */
                unsigned int did = rom_rd16(vpos);
                unsigned int slot = rom_rd16(vpos);
                aw_tile(did, slot);
                done = 1;
            }
            g_uploads += done;
            nup_left -= done;
            units -= (unsigned int)done << 3;
        }
        if (!nup_left) {
            ncell_left = rom_rd16(vpos);
            if (g_vstage == 2) { g_vstage = 3; g_vncell = ncell_left; }
            vphase = 2;
        }
    }
    if (vphase == 2) {
        while (ncell_left && units) {
            volatile unsigned int want = units;
            unsigned char done;
            if (want > ncell_left) want = ncell_left;
            if (want > 250) want = 250;
            done = aw_cells((unsigned char)want);
            if (!done) {
                unsigned int cell = rom_rd16(vpos);
                unsigned int slot = rom_rd16(vpos);
                vdp_loc(cell);
                vdp_tile(slot);
                done = 1;
            }
            g_cells += done;
            ncell_left -= done;
            units -= done;
        }
        if (!ncell_left) {
            /* catch up: the ticks spent shipping data count against the hold */
            volatile unsigned int d = dur_left;
            volatile unsigned char k = data_ticks;
            if (d > k) d -= k; else d = 1;
            dur_left = d;
            data_ticks = 0;
            vphase = 3;
        }
    }
}

static void service_dur(void) {
    if (vphase == 3) {
        if (dur_left) dur_left--;
        if (!dur_left) vphase = 0;
    }
}

static void play_scene(unsigned char n) {
    scene_init(n);
    while (!scene_done) {
        unsigned int keys;
        wait_vbl();
        g_ticks++;
        service_audio();
        service_dur();
        if (vphase != 3 && !scene_done) service_video();
        keys = joy_held();
        g_keyval = keys;
        if (keys & PORT_A_KEY_2) { g_whybreak = 0x22; break; }
        if (scene_done) g_whybreak = 0x77;
    }
    if (!g_whybreak) g_whybreak = 0xEE;
    psg_silence();
    /* quick fade to black */
    {
        unsigned char i;
        for (i = 0; i < 16; i++) palbuf[i] = 0;
        vdp_pal16(palbuf);
    }
}

void main(void) {
    unsigned char sel = 0;
    __asm di __endasm;
    unsigned int keys;

    g_mile = 1; SMS_init(); g_mile = 2;
    vdp_disp(0);
    SMS_useFirstHalfTilesforSprites(0);
    SMS_setBGScrollX(0);
    SMS_setBGScrollY(0);
    vdp_disp(1);

    static unsigned char last_held = 0;
    unsigned char held, newly;
    unsigned char cool = 0;
    g_mile = 4; draw_menu_impl(sel); g_mile = 5;
    for (;;) {
        wait_vbl();
        g_mile = 6;
        held = (unsigned char)joy_held();
        g_rawjoy = JOYPAD1;
        g_dbg3 = held;
        newly = (unsigned char)(held & (unsigned char)~last_held);
        last_held = held;
        if (cool) cool--;
        if (g_autoplay) {
            unsigned char nn = (unsigned char)(g_autoplay - 1);
            g_autoplay = 0;
            cool = 30;
            play_scene(nn);
            ldrawn = 255;
            draw_menu_impl(sel);
        } else if ((held & PORT_A_KEY_UP) && !cool) {
            g_dbg2 = 0xAA;
            { unsigned char ow = sel;
              sel = (sel == 0) ? (FMV_NSCENES - 1) : (sel - 1);
              menu_cursor(ow, sel); }
            cool = 12;
        } else if ((held & PORT_A_KEY_DOWN) && !cool) {
            g_dbg2 = 0xDD;
            { unsigned char ow = sel;
              sel = (unsigned char)(sel + 1);
              if (sel >= FMV_NSCENES) sel = 0;
              menu_cursor(ow, sel); }
            cool = 12;
        } else if ((held & PORT_A_KEY_1) && !cool) {
            cool = 30;
            play_scene(sel);
            ldrawn = 255;
            draw_menu_impl(sel);
        }
    }
}
