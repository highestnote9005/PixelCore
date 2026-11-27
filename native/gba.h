/*
 * PocketGBA core - a compact Game Boy Advance emulator core in portable C.
 * ARM7TDMI interpreter, scanline PPU, DMA, timers, PSG + DirectSound audio,
 * SRAM / Flash / EEPROM saves, HLE BIOS.
 */
#ifndef POCKETGBA_H
#define POCKETGBA_H

#include <stdint.h>
#include <stddef.h>

#define GBA_W 240
#define GBA_H 160
#define GBA_AUDIO_RATE 32768
#define GBA_AUDIO_CAP 16384 /* stereo frames, power of two */

enum {
    GBA_KEY_A = 1, GBA_KEY_B = 2, GBA_KEY_SELECT = 4, GBA_KEY_START = 8,
    GBA_KEY_RIGHT = 16, GBA_KEY_LEFT = 32, GBA_KEY_UP = 64, GBA_KEY_DOWN = 128,
    GBA_KEY_R = 256, GBA_KEY_L = 512
};

enum { SAVE_NONE = 0, SAVE_SRAM, SAVE_FLASH64, SAVE_FLASH128, SAVE_EEPROM };

typedef struct {
    uint16_t reload, ctrl;
    uint32_t counter;
    int64_t last;
    uint8_t enabled, cascade, irq, shift;
} GbaTimer;

typedef struct {
    uint8_t on, dac;
    uint8_t duty, duty_pos;
    uint8_t vol_init, env_dir, env_period, env_vol, env_timer;
    uint8_t len_en;
    uint16_t len;
    uint16_t freq;
    int32_t timer;
    uint8_t sw_period, sw_dir, sw_shift, sw_timer, sw_en;
    uint16_t sw_shadow;
    uint8_t w_dim, w_bank, w_vol, w_force75;
    uint16_t lfsr;
    uint8_t n_width, n_shift, n_div;
} GbaChan;

typedef struct GBA {
    /* ---- CPU ---- */
    uint32_t r[16];             /* r[15] holds (address of executing insn + 8 ARM / + 4 Thumb) */
    uint32_t cpsr;              /* mode, I, F, T (flags kept separately) */
    uint32_t fN, fZ, fC, fV;
    uint32_t spsr[6];
    uint32_t bk13[6], bk14[6];
    uint32_t usr_hi[5], fiq_hi[5];
    uint8_t halted, flushed, irq_line, swi_resume;
    int64_t cycle;
    int64_t line_start;

    /* ---- memory ---- */
    uint8_t bios[0x4000];
    uint8_t ewram[0x40000];
    uint8_t iwram[0x8000];
    uint8_t pal[0x400];
    uint8_t vram[0x18000];
    uint8_t oam[0x400];
    uint16_t io[0x200];
    const uint8_t *rom;
    uint32_t rom_size;
    uint8_t c16s[16], c16n[16], c32s[16], c32n[16]; /* access cost tables */

    /* ---- save ---- */
    uint8_t save[0x20000];
    uint8_t save_type;
    uint32_t save_size;
    uint8_t save_dirty;
    uint8_t fl_state, fl_id, fl_bank, fl_erase, fl_prog, fl_bankcmd;
    uint8_t ee_size_known;
    uint32_t ee_bits_n, ee_addr, ee_read_pos, ee_mode, ee_abits;
    uint8_t ee_buf[96];
    uint8_t ee_data[8];

    /* ---- timers / dma / irq ---- */
    GbaTimer tm[4];
    int64_t next_timer_event;
    uint32_t dma_src[4], dma_dst[4], dma_dst0[4], dma_cnt[4];

    /* ---- video ---- */
    uint32_t vcount;
    int32_t bgx[2], bgy[2];
    uint32_t video[GBA_W * GBA_H];

    /* ---- audio ---- */
    GbaChan ch[4];
    uint8_t wave_ram[2][16];
    uint8_t fifo[2][32];
    uint8_t fifo_rd[2], fifo_cnt[2];
    int8_t fifo_cur[2];
    int64_t audio_cycle;
    int32_t fs_count;
    uint8_t fs_step;
    int32_t hp_x[2], hp_y[2];
    uint32_t a_w, a_r;
    int16_t abuf[GBA_AUDIO_CAP * 2];

    /* ---- input ---- */
    uint16_t keys;
} GBA;

GBA *gba_new(void);
void gba_free(GBA *g);
/* data must stay alive while the core uses it (the core copies it, so it does not). returns 0 on success */
int gba_load_rom(GBA *g, const uint8_t *data, size_t size);
void gba_reset(GBA *g);
void gba_set_keys(GBA *g, uint32_t pressed_mask);
void gba_run_frame(GBA *g);
const uint32_t *gba_video(GBA *g);
/* pops up to max_frames stereo frames (interleaved L,R int16). returns frames written */
int gba_audio_pop(GBA *g, int16_t *out, int max_frames);
void gba_audio_clear(GBA *g);

const uint8_t *gba_save_data(GBA *g, size_t *size);
int gba_save_load(GBA *g, const uint8_t *data, size_t size);
int gba_save_take_dirty(GBA *g);
uint32_t gba_save_size(GBA *g);

size_t gba_state_size(void);
void gba_state_save(GBA *g, uint8_t *out);
int gba_state_load(GBA *g, const uint8_t *in, size_t size);

#endif
