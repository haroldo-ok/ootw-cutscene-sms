/*
 * pd_e2e - end-to-end test of ootw-sms.sms through the PicoDrive libretro core.
 *
 * Boots the ROM in a real emulator, drives the pad, and asserts that the
 * cutscene viewer: 1) shows the menu, 2) navigates with up/down, 3) STARTS
 * the selected cutscene video on confirm (SMS button 1 = RetroPad B),
 * 4) keeps running (no freeze), 5) returns to the menu at scene end / abort.
 *
 * In-core instrumentation: reads the game's own probe globals (g_ticks,
 * g_mile, ...) from RETRO_MEMORY_SYSTEM_RAM using symbol offsets from code.noi.
 *
 * Usage: pd_e2e <picodrive_libretro.so> <rom.sms> <code.noi> <outdir>
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include "libretro.h"

static void *core;
static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static uint8_t *fb;               /* XRGB8888 or RGB565 copy of last frame */
static unsigned fb_w, fb_h;
static size_t fb_pitch;
static enum retro_pixel_format fb_fmt = RETRO_PIXEL_FORMAT_XRGB8888;
static uint32_t joy[2];           /* bitmask of RETRO_DEVICE_ID_JOYPAD_* */

static char outdir[512];
static int failures;

/* ---- tiny PNG writer (RGB888) ---- */
static void put_u32be(FILE *f, uint32_t v) {
    fputc((v >> 24) & 255, f); fputc((v >> 16) & 255, f);
    fputc((v >> 8) & 255, f); fputc(v & 255, f);
}
static void chunk(FILE *f, const char *tag, const void *d, uint32_t n) {
    put_u32be(f, n); fwrite(tag, 1, 4, f); fwrite(d, 1, n, f);
    uint32_t c = crc32(0, (const Bytef *)tag, 4);
    c = crc32(c, (const Bytef *)d, n);
    put_u32be(f, c);
}
static void save_png(const char *name) {
    char path[640]; snprintf(path, sizeof path, "%s/%s", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL cannot write %s\n", path); failures++; return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {0, 0, (uint8_t)(fb_w >> 8), (uint8_t)fb_w,
                        0, 0, (uint8_t)(fb_h >> 8), (uint8_t)fb_h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)fb_h * (fb_w * 3 + 1);
    uint8_t *raw = malloc(raw_n), *p = raw;
    for (unsigned y = 0; y < fb_h; y++) {
        *p++ = 0;
        const uint8_t *row = fb + y * fb_pitch;
        for (unsigned x = 0; x < fb_w; x++) {
            uint8_t r, g, b;
            if (fb_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                const uint8_t *px = row + x * 4;
                r = px[2]; g = px[1]; b = px[0];   /* little-endian XRGB */
            } else if (fb_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                const uint8_t *px = row + x * 2;
                uint16_t v = (uint16_t)(px[0] | (px[1] << 8));
                r = (uint8_t)(((v >> 11) & 31) * 255 / 31);
                g = (uint8_t)(((v >> 5) & 63) * 255 / 63);
                b = (uint8_t)((v & 31) * 255 / 31);
            } else { r = g = b = 0; }
            *p++ = r; *p++ = g; *p++ = b;
        }
    }
    uLongf z_n = compressBound(raw_n);
    uint8_t *z = malloc(z_n);
    compress2(z, &z_n, raw, raw_n, 6);
    chunk(f, "IDAT", z, (uint32_t)z_n);
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z); fclose(f);
    printf("snap: %s\n", path);
}

/* ---- frame analysis ---- */
static long px_nonblack(void) {
    long n = 0;
    for (unsigned y = 0; y < fb_h; y++) {
        const uint8_t *row = fb + y * fb_pitch;
        for (unsigned x = 0; x < fb_w; x++) {
            uint8_t r, g, b;
            if (fb_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                const uint8_t *px = row + x * 4;
                r = px[2]; g = px[1]; b = px[0];
            } else if (fb_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                const uint8_t *px = row + x * 2;
                uint16_t v = (uint16_t)(px[0] | (px[1] << 8));
                r = (uint8_t)((v >> 11) & 31); g = (uint8_t)((v >> 5) & 63); b = (uint8_t)(v & 31);
            } else continue;
            if (r | g | b) n++;
        }
    }
    return n;
}
/* menu bg is the dark blue (0,0,0xAA) or (0,0,85) in 565-ish scales */
static long px_blue(void) {
    long n = 0;
    for (unsigned y = 0; y < fb_h; y++) {
        const uint8_t *row = fb + y * fb_pitch;
        for (unsigned x = 0; x < fb_w; x++) {
            uint8_t r, g, b;
            if (fb_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                const uint8_t *px = row + x * 4;
                r = px[2]; g = px[1]; b = px[0];
            } else if (fb_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                const uint8_t *px = row + x * 2;
                uint16_t v = (uint16_t)(px[0] | (px[1] << 8));
                r = (uint8_t)((v >> 11) & 31); g = (uint8_t)((v >> 5) & 63); b = (uint8_t)(v & 31);
            } else continue;
            if (b > 20 && b > r + 20 && b > g + 20) n++;
        }
    }
    return n;
}
static uint64_t frame_hash(void) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned y = 0; y < fb_h; y++) {
        const uint8_t *row = fb + y * fb_pitch;
        for (size_t i = 0; i < (size_t)fb_w * 3; i++) {
            h ^= row[i % fb_pitch];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

/* ---- libretro callbacks ---- */
static bool env_cb_fn(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        static const char *d = "/tmp/pd_e2e_sys";
        *(const char **)data = d; return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565 ||
            f == RETRO_PIXEL_FORMAT_0RGB1555) { fb_fmt = f; return true; }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        return false;                      /* core must fall back to defaults */
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}
static void video_cb_fn(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;                     /* dupe frame */
    if (!fb || fb_w != w || fb_h != h) {
        fb_w = w; fb_h = h;
        fb = realloc(fb, (size_t)pitch * h + 8);
    }
    fb_pitch = pitch;
    memcpy(fb, data, (size_t)pitch * h);
}
static void audio_cb_fn(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb_fn(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll_cb_fn(void) {}
static int16_t input_state_cb_fn(unsigned port, unsigned device, unsigned idx, unsigned id) {
    (void)idx;
    if (device == RETRO_DEVICE_JOYPAD && port < 2) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)joy[port];
        return (joy[port] >> id) & 1;
    }
    return 0;
}

/* ---- in-core RAM probes ---- */
static uint8_t *sysram;
static size_t sysram_size;
typedef struct { char name[64]; unsigned addr; } Sym;
static Sym syms[4096];
static int nsyms;
static void load_noi(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && nsyms < 4096) {
        char nm[64]; unsigned a;
        if (sscanf(line, "DEF %63s %x", nm, &a) == 2 ||
            sscanf(line, "def %63s %x", nm, &a) == 2) {
            snprintf(syms[nsyms].name, 64, "%s", nm);
            syms[nsyms].addr = a;
            nsyms++;
        }
    }
    fclose(f);
    printf("noi: %d symbols\n", nsyms);
}
static const Sym *find_sym(const char *n) {
    for (int i = 0; i < nsyms; i++)
        if (!strcmp(syms[i].name, n)) return &syms[i];
    return NULL;
}
static unsigned probe(const char *n) {
    if (!sysram) return 0xEEEE;
    const Sym *s = find_sym(n);
    if (!s) return 0xEEEE;
    unsigned off = s->addr - 0xC000;
    if (off + 1 >= sysram_size) return 0xEEEE;
    return sysram[off] | (sysram[off + 1] << 8);
}
static void print_probes(const char *tag) {
    printf("probe[%s]: ticks=%u mile=%u endflag=0x%02x whybreak=0x%02x uploads=%u cells=%u dbg1=%u dbg2=0x%02x\n",
           tag, probe("_g_ticks"), probe("_g_mile"),
           probe("_g_endflag") & 0xFF, probe("_g_whybreak") & 0xFF,
           probe("_g_uploads"), probe("_g_cells"),
           probe("_g_dbg1"), probe("_g_dbg2") & 0xFF);
}

/* ---- helpers ---- */
static void (*f_retro_run)(void);
static void run_n(int n) {
    for (int i = 0; i < n; i++) f_retro_run();
}
static void press(uint32_t mask, int hold_n, int gap_n) {
    joy[0] = mask;
    run_n(hold_n);
    joy[0] = 0;
    if (gap_n) run_n(gap_n);
}
static void check(const char *what, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: pd_e2e <core.so> <rom.sms> <code.noi> <outdir>\n");
        return 2;
    }
    snprintf(outdir, sizeof outdir, "%s", argv[4]);
    load_noi(argv[3]);

    core = dlopen(argv[1], RTLD_NOW);
    if (!core) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
#define BIND(var, sym) *(void **)(&var) = dlsym(core, sym); \
    if (!(var)) { fprintf(stderr, "missing %s\n", sym); return 2; }
    void (*f_set_env)(retro_environment_t);
    void (*f_set_video)(retro_video_refresh_t);
    void (*f_set_audio)(retro_audio_sample_t);
    void (*f_set_audbatch)(retro_audio_sample_batch_t);
    void (*f_set_poll)(retro_input_poll_t);
    void (*f_set_input)(retro_input_state_t);
    void (*f_init)(void);
    void (*f_deinit)(void);
    bool (*f_load)(const struct retro_game_info *);
    void (*f_unload)(void);
    void (*f_get_av)(struct retro_system_av_info *);
    void *(*f_get_mem)(unsigned);
    size_t (*f_get_mem_size)(unsigned);
    size_t (*f_serialize_size)(void);
    BIND(f_set_env, "retro_set_environment");
    BIND(f_set_video, "retro_set_video_refresh");
    BIND(f_set_audio, "retro_set_audio_sample");
    BIND(f_set_audbatch, "retro_set_audio_sample_batch");
    BIND(f_set_poll, "retro_set_input_poll");
    BIND(f_set_input, "retro_set_input_state");
    BIND(f_init, "retro_init");
    BIND(f_deinit, "retro_deinit");
    BIND(f_load, "retro_load_game");
    BIND(f_unload, "retro_unload_game");
    BIND(f_get_av, "retro_get_system_av_info");
    BIND(f_get_mem, "retro_get_memory_data");
    BIND(f_get_mem_size, "retro_get_memory_size");
    BIND(f_serialize_size, "retro_serialize_size");
    BIND(f_retro_run, "retro_run");
#undef BIND
    env_cb = env_cb_fn; video_cb = video_cb_fn; audio_cb = audio_cb_fn;
    audio_batch_cb = audio_batch_cb_fn;
    input_poll_cb = input_poll_cb_fn; input_state_cb = input_state_cb_fn;

    f_set_env(env_cb_fn);
    f_set_video(video_cb_fn);
    f_set_audio(audio_cb_fn);
    f_set_audbatch(audio_batch_cb_fn);
    f_set_poll(input_poll_cb_fn);
    f_set_input(input_state_cb_fn);
    f_init();

    struct retro_game_info game = {0};
    game.path = argv[2];
    if (!f_load(&game)) { fprintf(stderr, "retro_load_game failed\n"); return 2; }

    struct retro_system_av_info av;
    f_get_av(&av);
    printf("av: %ux%u @ %.2f fps\n", av.geometry.base_width, av.geometry.base_height,
           av.timing.fps);

    sysram = f_get_mem(RETRO_MEMORY_SYSTEM_RAM);
    sysram_size = f_get_mem_size(RETRO_MEMORY_SYSTEM_RAM);
    printf("system ram: %p (%zu bytes)\n", (void *)sysram, sysram_size);

    /* ---------------- scenario A: menu + navigation + TITLES ---------------- */
    printf("\n== A: boot/menu ==\n");
    run_n(120);
    save_png("e2e_a0_menu.png");
    print_probes("menu");
    long blue = px_blue();
    printf("  blue px = %ld\n", blue);
    check("menu shows blue background", blue > 20000);
    uint64_t h_menu = frame_hash();

    printf("\n== A: navigate down x1 ==\n");
    press(1u << RETRO_DEVICE_ID_JOYPAD_DOWN, 6, 30);
    save_png("e2e_a1_down.png");
    check("cursor moved (frame changed)", frame_hash() != h_menu);
    press(1u << RETRO_DEVICE_ID_JOYPAD_UP, 6, 20);   /* back to item 0 */
    {   /* E2E_SCENE=n selects which cutscene scenario A plays (default 0) */
        const char *es = getenv("E2E_SCENE");
        int n = es ? atoi(es) : 0;
        for (int k = 0; k < n; k++) press(1u << RETRO_DEVICE_ID_JOYPAD_DOWN, 6, 20);
        printf("  scene index = %d\n", n);
    }

    printf("\n== A: confirm (RetroPad B = SMS button 1) -> TITLES ==\n");
    joy[0] = 1u << RETRO_DEVICE_ID_JOYPAD_B;
    run_n(5);
    joy[0] = 0;

    int nonblack_seen = 0, distinct = 0, ticks_alive = 0;
    uint64_t prev_hash = 0;
    unsigned t0 = probe("_g_ticks");
    for (int s = 0; s < 120; s++) {        /* 120 * 15 = 1800 frames = 30s */
        run_n(15);
        long nb = px_nonblack();
        if (nb > 200) nonblack_seen = 1;
        uint64_t h = frame_hash();
        if (prev_hash && h != prev_hash) distinct++;
        prev_hash = h;
        unsigned t = probe("_g_ticks");
        if (t != 0xEEEE && t > t0) ticks_alive++;
        if (s % 4 == 0) {
            char nm[64]; snprintf(nm, sizeof nm, "e2e_a2_scene_%03d.png", s);
            save_png(nm);
            print_probes("scene");
        }
        /* scene over? back at menu = blue bg and stable ticks */
        if (px_blue() > 20000 && probe("_g_endflag") == 0x77) {
            printf("  scene completed at sample %d\n", s);
            break;
        }
    }
    check("video content appeared (non-black frames)", nonblack_seen);
    check("video progressed (frames changed)", distinct >= 3);
    check("game loop alive during scene (g_ticks advanced)", ticks_alive > 3 && probe("_g_ticks") > 20);
    print_probes("scene_end");

    printf("\n== A: menu restored ==\n");
    save_png("e2e_a3_back.png");
    check("menu visible again after scene", px_blue() > 20000);

    /* ---------------- scenario B: button matrix for the STOP button -------- */
    printf("\n== B: find the STOP button ==\n");
    run_n(4);
    joy[0] = 1u << RETRO_DEVICE_ID_JOYPAD_B;    /* confirm scene 0 if at menu */
    run_n(5);
    joy[0] = 0;
    run_n(60);
    {
        static const int cands[8] = {
            RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
            RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
            RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_UP };
        static const char *cname[8] = { "A", "Y", "X", "START", "L", "R", "SELECT", "UP" };
        int found = -1;
        for (int ci = 0; ci < 8 && found < 0; ci++) {
            joy[0] = 1u << cands[ci];
            run_n(30);
            joy[0] = 0;
            run_n(4);
            if ((probe("_g_whybreak") & 0xFF) != 0) { found = ci; printf("  stop button = %s (whybreak=0x%02x)\n", cname[ci], probe("_g_whybreak") & 0xFF); }
        }
        check("a stop button exists", found >= 0);
        if (found >= 0) {
            run_n(30);
            save_png("e2e_b1_abort.png");
            check("stop returns to menu", px_blue() > 20000);
        }
    }

    printf("\n== RESULT: %s (%d failure(s)) ==\n", failures ? "FAIL" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
