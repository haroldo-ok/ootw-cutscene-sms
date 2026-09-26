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
#include "font.h"

SMS_EMBED_SEGA_ROM_HEADER(20261, 1);

__sfr __at (0x7F) PSGPort;

#define FONT0        0      /* font glyphs occupy tiles 0..95 (glyph ' ' = tile 0 = blank) */
#define NCELLS       768
#define B_UP         6
#define B_CELL       60

/* ---- 24-bit ROM readers ------------------------------------------------ */
static unsigned char curbank = 0xFF;

static void sel_bank(unsigned long addr) {
    unsigned char b = (unsigned char)(addr >> 14);
    if (b != curbank) {
        curbank = b;
        SMS_mapROMBank(b);
    }
}

static unsigned char rom_rd(unsigned long *p) {
    unsigned char v;
    sel_bank(*p);
    v = *(volatile unsigned char *)(0x8000 + ((unsigned int)(*p & 0x3FFF)));
    *p += 1;
    return v;
}

static unsigned int rom_rd16(unsigned long *p) {
    unsigned int lo = rom_rd(p);
    return lo | ((unsigned int)rom_rd(p) << 8);
}

static unsigned long rom_rd24(unsigned long *p) {
    unsigned long a = rom_rd(p);
    a |= (unsigned long)rom_rd(p) << 8;
    a |= (unsigned long)rom_rd(p) << 16;
    return a;
}

/* ---- tile decompaction (compact forms -> 32-byte VRAM tile) -------------- */
static unsigned char tbuf[32];
static unsigned char bb[16];

static void decompact(unsigned long *tp) {
    unsigned char form = rom_rd(tp);
    unsigned char i, j, b0, b1, v;
    if (form < 0x10) {                       /* form 0: solid colour */
        v = (unsigned char)(form | (form << 4));
        for (i = 0; i < 32; i++) tbuf[i] = v;
    } else if (form == 0x10) {               /* form 1: 2 colours, 1bpp x 64 */
        b0 = rom_rd(tp);                     /* low nibble c0, high nibble c1 */
        for (i = 0; i < 8; i++) bb[i] = rom_rd(tp);
        for (j = 0; j < 32; j++) {
            unsigned char i0 = j << 1;
            unsigned char p0 = (bb[i0 >> 3] >> (i0 & 7)) & 1;
            unsigned char p1 = (bb[(i0 + 1) >> 3] >> ((i0 + 1) & 7)) & 1;
            unsigned char c0 = b0 & 15, c1 = b0 >> 4;
            tbuf[j] = (unsigned char)(((p0 ? c1 : c0) << 4) | (p1 ? c1 : c0));
        }
    } else if (form == 0x20) {               /* form 2: 4 colours, 2bpp x 64 */
        b0 = rom_rd(tp);                     /* c1:c0 */
        b1 = rom_rd(tp);                     /* c3:c2 */
        for (i = 0; i < 16; i++) bb[i] = rom_rd(tp);
        for (j = 0; j < 32; j++) {
            unsigned char i0 = j << 1;
            unsigned char p0 = (bb[i0 >> 2] >> ((i0 & 3) << 1)) & 3;
            unsigned char p1 = (bb[(i0 + 1) >> 2] >> (((i0 + 1) & 3) << 1)) & 3;
            unsigned char c0 = (p0 == 0) ? (unsigned char)(b0 & 15) : (p0 == 1) ? (unsigned char)(b0 >> 4) : (p0 == 2) ? (unsigned char)(b1 & 15) : (unsigned char)(b1 >> 4);
            unsigned char c1 = (p1 == 0) ? (unsigned char)(b0 & 15) : (p1 == 1) ? (unsigned char)(b0 >> 4) : (p1 == 2) ? (unsigned char)(b1 & 15) : (unsigned char)(b1 >> 4);
            tbuf[j] = (unsigned char)((c0 << 4) | c1);
        }
    } else {                                 /* form 3: raw 32 bytes (VRAM form) */
        for (i = 0; i < 32; i++) tbuf[i] = rom_rd(tp);
    }
}

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
        if (c >= 32 && c < 128) SMS_setTile(c - 32); else SMS_setTile(0);
    }
}

static void draw_menu(unsigned char sel) {
    unsigned char i;
    char line[24];
    SMS_displayOff();
    /* palette: 0 dark blue bg, 15 white text */
    palbuf[0] = 0x20;
    for (i = 1; i < 16; i++) palbuf[i] = 0;
    palbuf[15] = 0x3F;
    SMS_loadBGPalette(palbuf);
    SMS_load1bppTiles(ootw_font, FONT0, sizeof(ootw_font), 0, 15);
    /* clear screen */
    SMS_setNextTileatLoc(0);
    for (i = 0; i < 28 * 32; i++) SMS_setTile(0);
    draw_str(7, 1, "OUT OF THIS WORLD");
    draw_str(8, 2, "CUTSCENE VIEWER");
    draw_str(7, 3, "----------------");
    for (i = 0; i < FMV_NSCENES; i++) {
        unsigned char p = 0;
        unsigned char num = i + 1;
        line[p++] = (i == sel) ? '>' : ' ';
        line[p++] = ' ';
        if (num >= 10) line[p++] = (char)('0' + num / 10);
        else line[p++] = ' ';
        line[p++] = (char)('0' + num % 10);
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
    SMS_displayOn();
}

/* ---- scene player ------------------------------------------------------- */
static unsigned long vpos, apos, tpos;
static unsigned int  dur_left, nup_left, ncell_left;
static unsigned char vphase;      /* 0 hdr 1 uploads 2 cells 3 wait */
static unsigned char scene_done;
static unsigned int  a_wait, a_wr_left;
static unsigned char a_done;

static void scene_init(unsigned char n) {
    unsigned int i;
    SMS_displayOff();
    psg_silence();
    /* blank tile 0 + nametable all 0 + black palette */
    for (i = 0; i < 32; i++) tbuf[i] = 0;
    SMS_loadTiles(tbuf, 0, 32);
    SMS_setNextTileatLoc(0);
    for (i = 0; i < 28 * 32; i++) SMS_setTile(0);
    for (i = 0; i < 16; i++) palbuf[i] = 0;
    SMS_loadBGPalette(palbuf);
    vpos = ROM_AWV[n];
    apos = ROM_AWA[n];
    apos += 2;                    /* skip start field */
    a_wait = 0; a_wr_left = 0; a_done = 0;
    vphase = 0; scene_done = 0;
    SMS_displayOn();
}

static void service_audio(void) {
    unsigned char i, n, reg, val;
    if (a_done) return;
    if (a_wait) { a_wait--; return; }
    if (a_wr_left) {
        n = (unsigned char)a_wr_left;
        for (i = 0; i < n; i++) {
            reg = rom_rd(&apos);
            val = rom_rd(&apos);
            psg_write(reg, val);
        }
        a_wr_left = 0;
        return;
    }
    {
        unsigned char dlo = rom_rd(&apos);
        unsigned char dhi = rom_rd(&apos);
        unsigned char nwr = rom_rd(&apos);
        if (nwr == 0xFF) { a_done = 1; return; }
        a_wr_left = nwr;
        a_wait = ((unsigned int)dhi << 8) | dlo;
    }
}

static void service_video(void) {
    unsigned char i;
    if (vphase == 0) {
        unsigned char flags = rom_rd(&vpos);
        if (flags == 0xFF) { scene_done = 1; return; }
        dur_left = rom_rd16(&vpos);
        if (flags & 1) {
            for (i = 0; i < 16; i++) palbuf[i] = xlat_cram(rom_rd(&vpos));
            SMS_loadBGPalette(palbuf);
        }
        nup_left = rom_rd16(&vpos);
        vphase = 1;
    }
    if (vphase == 1) {
        unsigned char budget = B_UP;
        while (nup_left && budget) {
            unsigned int did = rom_rd16(&vpos);
            unsigned int slot = rom_rd16(&vpos);
            tpos = ROM_TILEOFF_BASE;
            tpos += (unsigned long)did * 3UL;
            tpos = rom_rd24(&tpos);
            decompact(&tpos);
            SMS_loadTiles(tbuf, slot, 32);
            nup_left--;
            budget--;
        }
        if (!nup_left) { ncell_left = rom_rd16(&vpos); vphase = 2; }
    }
    if (vphase == 2) {
        unsigned char budget = B_CELL;
        while (ncell_left && budget) {
            unsigned int cell = rom_rd16(&vpos);
            unsigned int slot = rom_rd16(&vpos);
            SMS_setNextTileatLoc(cell);
            SMS_setTile(slot);
            ncell_left--;
            budget--;
        }
        if (!ncell_left) vphase = 3;
    }
    if (vphase == 3) {
        if (dur_left) dur_left--;
        if (!dur_left) vphase = 0;
    }
}

static void play_scene(unsigned char n) {
    scene_init(n);
    while (!scene_done) {
        unsigned int keys;
        SMS_waitForVBlank();
        service_audio();
        service_video();
        keys = SMS_getKeysPressed();
        if (keys & PORT_A_KEY_2) break;
    }
    psg_silence();
    /* quick fade to black */
    {
        unsigned char i;
        for (i = 0; i < 16; i++) palbuf[i] = 0;
        SMS_loadBGPalette(palbuf);
    }
}

void main(void) {
    unsigned char sel = 0;
    unsigned int keys;

    SMS_init();
    SMS_displayOff();
    SMS_useFirstHalfTilesforSprites(0);
    SMS_setBGScrollX(0);
    SMS_setBGScrollY(0);
    SMS_displayOn();

    draw_menu(sel);
    for (;;) {
        SMS_waitForVBlank();
        keys = SMS_getKeysPressed();
        if (keys & PORT_A_KEY_UP) {
            sel = (sel == 0) ? (FMV_NSCENES - 1) : (sel - 1);
            draw_menu(sel);
        } else if (keys & PORT_A_KEY_DOWN) {
            sel = (sel + 1) % FMV_NSCENES;
            draw_menu(sel);
        } else if (keys & PORT_A_KEY_1) {
            play_scene(sel);
            draw_menu(sel);
        }
    }
}
