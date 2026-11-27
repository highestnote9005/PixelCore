/* PocketGBA core: main file (unity build of cpu/mem/ppu/apu). */
#include "gba.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* forward declarations */
static uint32_t read32(GBA *g, uint32_t a);
static uint32_t read16(GBA *g, uint32_t a);
static uint32_t read8(GBA *g, uint32_t a);
static void write32(GBA *g, uint32_t a, uint32_t v);
static void write16(GBA *g, uint32_t a, uint32_t v);
static void write8(GBA *g, uint32_t a, uint32_t v);
static void apu_sync(GBA *g);
static void apu_write(GBA *g, uint32_t off, uint16_t v);
static void fifo_push(GBA *g, int ch, uint8_t v);
static void fifo_timer(GBA *g, int ch);
static void dma_fifo_request(GBA *g, int fifo);

#include "mem.inc"
#include "apu.inc"
#include "cpu.inc"
#include "ppu.inc"

#define CYC_LINE 1232
#define CYC_VIS 960

static void install_mini_bios(GBA *g) {
    static const uint32_t code[] = {
        0xE92D500F, /* stmfd sp!,{r0-r3,r12,lr} */
        0xE3A00301, /* mov r0,#0x04000000 */
        0xE28FE000, /* add lr,pc,#0 */
        0xE510F004, /* ldr pc,[r0,#-4] */
        0xE8BD500F, /* ldmfd sp!,{r0-r3,r12,lr} */
        0xE25EF004  /* subs pc,lr,#4 */
    };
    memset(g->bios, 0, sizeof g->bios);
    uint32_t b = 0xEA000042; /* b 0x128 (from 0x18) */
    memcpy(g->bios + 0x18, &b, 4);
    uint32_t rst = 0xEAFFFFFE; /* b . */
    memcpy(g->bios, &rst, 4);
    memcpy(g->bios + 0x128, code, sizeof code);
}

GBA *gba_new(void) {
    init_sintab();
    init_color_lut();
    GBA *g = (GBA *)calloc(1, sizeof(GBA));
    if (g) install_mini_bios(g);
    return g;
}

void gba_free(GBA *g) {
    if (!g) return;
    free((void *)g->rom);
    free(g);
}

static int find_str(const uint8_t *d, size_t n, const char *s) {
    size_t l = strlen(s);
    if (n < l) return 0;
    for (size_t i = 0; i + l <= n; i += 4)
        if (d[i] == (uint8_t)s[0] && memcmp(d + i, s, l) == 0) return 1;
    return 0;
}

static void detect_save(GBA *g, const uint8_t *d, size_t n) {
    g->save_type = SAVE_SRAM;
    g->save_size = 0x8000;
    if (find_str(d, n, "EEPROM_V")) { g->save_type = SAVE_EEPROM; g->save_size = 0x2000; }
    else if (find_str(d, n, "FLASH1M_V")) { g->save_type = SAVE_FLASH128; g->save_size = 0x20000; }
    else if (find_str(d, n, "FLASH512_V") || find_str(d, n, "FLASH_V")) { g->save_type = SAVE_FLASH64; g->save_size = 0x10000; }
    else if (find_str(d, n, "SRAM_V") || find_str(d, n, "SRAM_F_V")) { g->save_type = SAVE_SRAM; g->save_size = 0x8000; }
    memset(g->save, 0xFF, sizeof g->save);
    g->ee_size_known = 0;
    g->ee_abits = 0;
}

int gba_load_rom(GBA *g, const uint8_t *data, size_t size) {
    if (!g || !data || size < 0xC0 || size > 0x2000000) return -1;
    uint8_t *copy = (uint8_t *)malloc(size + 8);
    if (!copy) return -2;
    memcpy(copy, data, size);
    memset(copy + size, 0, 8);
    free((void *)g->rom);
    g->rom = copy;
    g->rom_size = (uint32_t)size;
    detect_save(g, copy, size);
    gba_reset(g);
    return 0;
}

void gba_reset(GBA *g) {
    uint8_t keep_save[sizeof g->save];
    memcpy(keep_save, g->save, sizeof keep_save);
    const uint8_t *rom = g->rom;
    uint32_t rs = g->rom_size, st = g->save_type, ss = g->save_size;
    uint8_t known = g->ee_size_known;
    uint32_t eab = g->ee_abits;

    memset(g->r, 0, sizeof g->r);
    memset(g->ewram, 0, sizeof g->ewram);
    memset(g->iwram, 0, sizeof g->iwram);
    memset(g->pal, 0, sizeof g->pal);
    memset(g->vram, 0, sizeof g->vram);
    memset(g->oam, 0, sizeof g->oam);
    memset(g->io, 0, sizeof g->io);
    memset(g->tm, 0, sizeof g->tm);
    memset(g->ch, 0, sizeof g->ch);
    memset(g->wave_ram, 0, sizeof g->wave_ram);
    memset(g->fifo, 0, sizeof g->fifo);
    memset(g->fifo_rd, 0, sizeof g->fifo_rd);
    memset(g->fifo_cnt, 0, sizeof g->fifo_cnt);
    memset(g->fifo_cur, 0, sizeof g->fifo_cur);
    memset(g->video, 0, sizeof g->video);
    g->fl_state = g->fl_id = g->fl_bank = g->fl_erase = g->fl_prog = g->fl_bankcmd = 0;
    g->ee_bits_n = g->ee_mode = g->ee_read_pos = 0;
    g->cycle = g->line_start = g->audio_cycle = 0;
    g->a_w = g->a_r = 0;
    g->fs_count = 0; g->fs_step = 0;
    g->hp_x[0] = g->hp_x[1] = g->hp_y[0] = g->hp_y[1] = 0;
    g->vcount = 0;
    g->bgx[0] = g->bgx[1] = g->bgy[0] = g->bgy[1] = 0;
    g->next_timer_event = INT64_MAX / 2;
    g->irq_line = 0;
    g->keys = 0;
    g->rom = rom; g->rom_size = rs; g->save_type = (uint8_t)st; g->save_size = ss;
    g->ee_size_known = known; g->ee_abits = eab;
    memcpy(g->save, keep_save, sizeof keep_save);
    IO(g, 0x130) = 0x3FF;
    IO(g, 0x20) = IO(g, 0x26) = IO(g, 0x30) = IO(g, 0x36) = 0x100;
    IO(g, 0x22) = IO(g, 0x24) = IO(g, 0x32) = IO(g, 0x34) = 0;
    update_waitstates(g);
    cpu_reset_state(g, 0x08000000);
}

void gba_set_keys(GBA *g, uint32_t pressed) { g->keys = (uint16_t)(pressed & 0x3FF); }

static void line_start_events(GBA *g) {
    uint32_t v = g->vcount;
    uint16_t ds = IO(g, 0x04);
    uint16_t lyc = ds >> 8;
    ds &= (uint16_t)~7;
    if (v >= 160 && v <= 226) ds |= 1;
    if (v == lyc) { ds |= 4; if (ds & 0x20) raise_irq(g, 4); }
    IO(g, 0x04) = ds;
    if (v == 160) {
        if (ds & 0x08) raise_irq(g, 1);
        dma_trigger(g, 1);
    }
}

void gba_run_frame(GBA *g) {
    if (!g->rom) return;
    for (int line = 0; line < 228; line++) {
        g->vcount = (uint32_t)line;
        if (line == 0) {
            for (int n = 0; n < 2; n++) {
                uint32_t b = n ? 0x38 : 0x28;
                int32_t x = (int32_t)(IO(g, b) | ((uint32_t)IO(g, b + 2) << 16));
                int32_t y = (int32_t)(IO(g, b + 4) | ((uint32_t)IO(g, b + 6) << 16));
                g->bgx[n] = (x << 4) >> 4;
                g->bgy[n] = (y << 4) >> 4;
            }
        }
        line_start_events(g);
        int64_t ls = g->line_start;
        cpu_run(g, ls + CYC_VIS);
        if (line < 160) {
            render_line(g, line);
            for (int n = 0; n < 2; n++) {
                g->bgx[n] += (int16_t)IO(g, n ? 0x32 : 0x22); /* PB */
                g->bgy[n] += (int16_t)IO(g, n ? 0x36 : 0x26); /* PD */
            }
        }
        IO(g, 0x04) |= 2;
        if (IO(g, 0x04) & 0x10) raise_irq(g, 2);
        if (line < 160) dma_trigger(g, 2);
        cpu_run(g, ls + CYC_LINE);
        IO(g, 0x04) &= (uint16_t)~2;
        g->line_start = ls + CYC_LINE;
    }
    apu_sync(g);
    /* keep counters bounded */
    if (g->cycle > (int64_t)1 << 50) {
        /* unreachable in practice (13 years of emulation) */
    }
}

const uint32_t *gba_video(GBA *g) { return g->video; }

int gba_audio_pop(GBA *g, int16_t *out, int max_frames) {
    int avail = (int)(g->a_w - g->a_r);
    int n = avail < max_frames ? avail : max_frames;
    for (int i = 0; i < n; i++) {
        uint32_t p = (g->a_r + (uint32_t)i) & (GBA_AUDIO_CAP - 1);
        out[i * 2] = g->abuf[p * 2];
        out[i * 2 + 1] = g->abuf[p * 2 + 1];
    }
    g->a_r += (uint32_t)n;
    return n;
}

void gba_audio_clear(GBA *g) { g->a_r = g->a_w; }

const uint8_t *gba_save_data(GBA *g, size_t *size) {
    if (size) *size = g->save_size;
    return g->save;
}

int gba_save_load(GBA *g, const uint8_t *data, size_t size) {
    if (size > sizeof g->save) size = sizeof g->save;
    memcpy(g->save, data, size);
    if (g->save_type == SAVE_EEPROM && size <= 512) { g->save_size = 512; g->ee_abits = 6; g->ee_size_known = 1; }
    else if (g->save_type == SAVE_EEPROM) { g->save_size = 8192; g->ee_abits = 14; g->ee_size_known = 1; }
    return 0;
}

int gba_save_take_dirty(GBA *g) {
    int d = g->save_dirty;
    g->save_dirty = 0;
    return d;
}

size_t gba_state_size(void) { return sizeof(GBA); }

void gba_state_save(GBA *g, uint8_t *out) { memcpy(out, g, sizeof(GBA)); }

int gba_state_load(GBA *g, const uint8_t *in, size_t size) {
    if (size != sizeof(GBA)) return -1;
    const uint8_t *rom = g->rom;
    uint32_t rs = g->rom_size;
    memcpy(g, in, sizeof(GBA));
    g->rom = rom;
    g->rom_size = rs;
    g->a_r = g->a_w;
    return 0;
}

uint32_t gba_save_size(GBA *g) { return g->save_size; }
