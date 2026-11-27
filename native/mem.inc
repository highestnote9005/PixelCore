/* Memory bus, IO registers, DMA, timers, saves.  Included by gba.c (unity build). */

#define IO(g, off) ((g)->io[(off) >> 1])
#define R_IE 0x200
#define R_IF 0x202
#define R_WAITCNT 0x204
#define R_IME 0x208

/* ------------------------------------------------------------------ */
/* IRQ                                                                */
/* ------------------------------------------------------------------ */
static void update_irq(GBA *g) {
    g->irq_line = (IO(g, R_IME) & 1) && (IO(g, R_IE) & IO(g, R_IF) & 0x3FFF);
}
static void raise_irq(GBA *g, uint16_t bits) {
    IO(g, R_IF) |= bits;
    update_irq(g);
}

/* ------------------------------------------------------------------ */
/* Wait states (rough cycle model)                                    */
/* ------------------------------------------------------------------ */
static void update_waitstates(GBA *g) {
    static const uint8_t nt[4] = {4, 3, 2, 8};
    uint16_t w = IO(g, R_WAITCNT);
    int ws_n[3] = {nt[(w >> 2) & 3], nt[(w >> 5) & 3], nt[(w >> 8) & 3]};
    int ws_s[3] = {(w & 0x10) ? 1 : 2, (w & 0x80) ? 1 : 4, (w & 0x400) ? 1 : 8};
    int pf = (w >> 14) & 1;
    for (int r = 0; r < 16; r++) {
        int s16 = 1, n16 = 1, s32 = 1, n32 = 1;
        switch (r) {
        case 2: s16 = n16 = 3; s32 = n32 = 6; break;
        case 5: case 6: s32 = n32 = 2; break;
        case 8: case 9: case 10: case 11: case 12: case 13: {
            int k = (r - 8) >> 1;
            s16 = 1 + (pf ? 0 : ws_s[k]);
            n16 = 1 + ws_n[k];
            s32 = s16 * 2;
            n32 = n16 + 1 + ws_s[k];
            break;
        }
        case 14: case 15: s16 = n16 = s32 = n32 = 1 + nt[w & 3]; break;
        default: break;
        }
        g->c16s[r] = s16; g->c16n[r] = n16; g->c32s[r] = s32; g->c32n[r] = n32;
    }
}

/* ------------------------------------------------------------------ */
/* Timers                                                             */
/* ------------------------------------------------------------------ */
static uint32_t timer_value(GBA *g, int i) {
    GbaTimer *t = &g->tm[i];
    if (t->enabled && !t->cascade)
        return (t->counter + (uint32_t)((g->cycle - t->last) >> t->shift)) & 0xFFFF;
    return t->counter & 0xFFFF;
}

static void timers_recalc(GBA *g) {
    int64_t best = INT64_MAX / 2;
    for (int i = 0; i < 4; i++) {
        GbaTimer *t = &g->tm[i];
        if (!t->enabled || t->cascade) continue;
        int64_t when = t->last + ((int64_t)(0x10000 - t->counter) << t->shift);
        if (when < best) best = when;
    }
    g->next_timer_event = best;
}

static void timer_overflow(GBA *g, int i, uint32_t n) {
    GbaTimer *t = &g->tm[i];
    if (t->irq) raise_irq(g, (uint16_t)(1u << (3 + i)));
    if (i <= 1) {
        uint16_t sh = IO(g, 0x82);
        if (((sh >> 10) & 1) == (unsigned)i) for (uint32_t k = 0; k < n && k < 8; k++) fifo_timer(g, 0);
        if (((sh >> 14) & 1) == (unsigned)i) for (uint32_t k = 0; k < n && k < 8; k++) fifo_timer(g, 1);
    }
    if (i < 3) {
        GbaTimer *c = &g->tm[i + 1];
        if (c->enabled && c->cascade) {
            uint64_t v = (uint64_t)c->counter + n;
            if (v >= 0x10000) {
                uint32_t period = 0x10000 - c->reload;
                uint64_t over = v - 0x10000;
                uint32_t ov = 1 + (uint32_t)(over / period);
                c->counter = (uint32_t)(c->reload + over % period);
                timer_overflow(g, i + 1, ov);
            } else {
                c->counter = (uint32_t)v;
            }
        }
    }
}

static void timers_process(GBA *g) {
    for (int i = 0; i < 4; i++) {
        GbaTimer *t = &g->tm[i];
        if (!t->enabled || t->cascade) continue;
        int64_t ticks = (g->cycle - t->last) >> t->shift;
        if (ticks <= 0) continue;
        t->last += ticks << t->shift;
        uint64_t c = (uint64_t)t->counter + (uint64_t)ticks;
        if (c >= 0x10000) {
            uint32_t period = 0x10000 - t->reload;
            uint64_t over = c - 0x10000;
            uint32_t n = 1 + (uint32_t)(over / period);
            t->counter = (uint32_t)(t->reload + over % period);
            timer_overflow(g, i, n);
        } else {
            t->counter = (uint32_t)c;
        }
    }
    timers_recalc(g);
}

static void timer_write_ctrl(GBA *g, int i, uint16_t v) {
    static const uint8_t sh[4] = {0, 6, 8, 10};
    GbaTimer *t = &g->tm[i];
    if (t->enabled && !t->cascade) {
        t->counter = timer_value(g, i);
        t->last = g->cycle;
    }
    int was = t->enabled;
    t->ctrl = v;
    t->shift = sh[v & 3];
    t->cascade = (i > 0) && ((v >> 2) & 1);
    t->irq = (v >> 6) & 1;
    t->enabled = (v >> 7) & 1;
    if (!was && t->enabled) {
        t->counter = t->reload;
        t->last = g->cycle;
    }
    timers_recalc(g);
}

/* ------------------------------------------------------------------ */
/* Save memory: SRAM, Flash, EEPROM                                   */
/* ------------------------------------------------------------------ */
static void save_touch(GBA *g) { g->save_dirty = 1; }

static uint8_t flash_read(GBA *g, uint32_t a) {
    a &= 0xFFFF;
    if (g->fl_id) {
        if (g->save_type == SAVE_FLASH128) return a == 0 ? 0xC2 : (a == 1 ? 0x09 : 0);
        return a == 0 ? 0x32 : (a == 1 ? 0x1B : 0);
    }
    uint32_t idx = (g->save_type == SAVE_FLASH128 ? (uint32_t)g->fl_bank * 0x10000u : 0) + a;
    return g->save[idx];
}

static void flash_write(GBA *g, uint32_t a, uint8_t v) {
    a &= 0xFFFF;
    uint32_t bankoff = (g->save_type == SAVE_FLASH128 ? (uint32_t)g->fl_bank * 0x10000u : 0);
    if (g->fl_prog) {
        g->save[bankoff + a] &= v;
        g->fl_prog = 0;
        save_touch(g);
        return;
    }
    if (g->fl_bankcmd && a == 0) {
        g->fl_bank = v & 1;
        g->fl_bankcmd = 0;
        return;
    }
    switch (g->fl_state) {
    case 0:
        if (a == 0x5555 && v == 0xAA) g->fl_state = 1;
        else if (v == 0xF0) g->fl_id = 0;
        break;
    case 1:
        g->fl_state = (a == 0x2AAA && v == 0x55) ? 2 : 0;
        break;
    default:
        g->fl_state = 0;
        if (a == 0x5555) {
            switch (v) {
            case 0x90: g->fl_id = 1; break;
            case 0xF0: g->fl_id = 0; break;
            case 0x80: g->fl_erase = 1; break;
            case 0xA0: g->fl_prog = 1; break;
            case 0xB0: if (g->save_type == SAVE_FLASH128) g->fl_bankcmd = 1; break;
            case 0x10:
                if (g->fl_erase) {
                    memset(g->save, 0xFF, g->save_size);
                    g->fl_erase = 0;
                    save_touch(g);
                }
                break;
            default: break;
            }
        } else if (v == 0x30 && g->fl_erase) {
            memset(g->save + bankoff + (a & 0xF000), 0xFF, 0x1000);
            g->fl_erase = 0;
            save_touch(g);
        }
        break;
    }
}

static uint8_t backup_read8(GBA *g, uint32_t a) {
    switch (g->save_type) {
    case SAVE_SRAM: return g->save[a & 0x7FFF];
    case SAVE_FLASH64: case SAVE_FLASH128: return flash_read(g, a);
    default: return 0xFF;
    }
}
static void backup_write8(GBA *g, uint32_t a, uint8_t v) {
    switch (g->save_type) {
    case SAVE_SRAM:
        g->save[a & 0x7FFF] = v;
        save_touch(g);
        break;
    case SAVE_FLASH64: case SAVE_FLASH128: flash_write(g, a, v); break;
    default: break;
    }
}

static void ee_dma_hint(GBA *g, uint32_t n) {
    if (g->ee_size_known) return;
    if (n == 9 || n == 73) { g->ee_abits = 6; g->save_size = 512; g->ee_size_known = 1; }
    else if (n == 17 || n == 81) { g->ee_abits = 14; g->save_size = 8192; g->ee_size_known = 1; }
}

static void ee_write(GBA *g, uint32_t v) {
    if (g->ee_bits_n == 0) g->ee_mode = 0;
    if (g->ee_bits_n < sizeof g->ee_buf) g->ee_buf[g->ee_bits_n] = v & 1;
    g->ee_bits_n++;
    uint32_t n = g->ee_bits_n;
    if (n < 2) return;
    uint32_t ab = g->ee_abits ? g->ee_abits : 14;
    uint32_t cmd = (g->ee_buf[0] << 1) | g->ee_buf[1];
    if (cmd == 3) {
        if (n == 2 + ab + 1) {
            uint32_t addr = 0;
            for (uint32_t i = 0; i < ab; i++) addr = (addr << 1) | g->ee_buf[2 + i];
            addr = (addr * 8) % (g->save_size ? g->save_size : 8192);
            memcpy(g->ee_data, g->save + addr, 8);
            g->ee_read_pos = 0;
            g->ee_mode = 1;
            g->ee_bits_n = 0;
        }
    } else if (cmd == 2) {
        if (n == 2 + ab + 64 + 1) {
            uint32_t addr = 0;
            for (uint32_t i = 0; i < ab; i++) addr = (addr << 1) | g->ee_buf[2 + i];
            addr = (addr * 8) % (g->save_size ? g->save_size : 8192);
            for (int k = 0; k < 8; k++) {
                uint8_t b = 0;
                for (int j = 0; j < 8; j++) b = (uint8_t)((b << 1) | g->ee_buf[2 + ab + k * 8 + j]);
                g->save[addr + k] = b;
            }
            save_touch(g);
            g->ee_bits_n = 0;
            g->ee_mode = 0;
        }
    } else {
        g->ee_bits_n = 0;
    }
}

static uint32_t ee_read(GBA *g) {
    if (g->ee_mode == 1) {
        uint32_t p = g->ee_read_pos++;
        uint32_t bit = 0;
        if (p >= 4) {
            uint32_t k = p - 4;
            bit = (g->ee_data[(k >> 3) & 7] >> (7 - (k & 7))) & 1;
        }
        if (g->ee_read_pos >= 68) g->ee_mode = 0;
        return bit;
    }
    return 1;
}

static inline int is_eeprom_addr(GBA *g, uint32_t a) {
    return g->save_type == SAVE_EEPROM && (g->rom_size <= 0x1000000 || (a & 0x01FFFFFF) >= 0x01FFFF00);
}

/* ------------------------------------------------------------------ */
/* IO read                                                            */
/* ------------------------------------------------------------------ */
static uint32_t io_read16(GBA *g, uint32_t off) {
    off &= 0x3FE;
    if (off >= 0xB0 && off < 0xE0) {
        uint32_t rel = (off - 0xB0) % 12;
        if (rel == 10) return g->io[off >> 1];
        return 0;
    }
    switch (off) {
    case 0x006: return g->vcount;
    case 0x084: {
        uint32_t v = IO(g, 0x84) & 0x80;
        for (int i = 0; i < 4; i++) if (g->ch[i].on) v |= 1u << i;
        return v;
    }
    case 0x100: case 0x104: case 0x108: case 0x10C: return timer_value(g, (off - 0x100) >> 2);
    case 0x130: return (~g->keys) & 0x3FF;
    case 0x0A0: case 0x0A2: case 0x0A4: case 0x0A6: return 0;
    default: break;
    }
    if (off >= 0x90 && off < 0xA0) {
        int bank = !g->ch[2].w_bank;
        return g->wave_ram[bank][off - 0x90] | (g->wave_ram[bank][off - 0x90 + 1] << 8);
    }
    return g->io[off >> 1];
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */
static void dma_run(GBA *g, int ch) {
    uint32_t cbase = 0xB0 + 12 * ch + 10;
    uint16_t ctrl = IO(g, cbase);
    int timing = (ctrl >> 12) & 3;
    uint32_t src = g->dma_src[ch], dst = g->dma_dst[ch], n = g->dma_cnt[ch];
    int sc = (ctrl >> 7) & 3, dc = (ctrl >> 5) & 3;
    int w32 = (ctrl >> 10) & 1;
    if (timing == 3 && (ch == 1 || ch == 2)) { w32 = 1; n = 4; dc = 2; }
    int step = w32 ? 4 : 2;
    int sinc = (sc == 0 || sc == 3) ? step : (sc == 1 ? -step : 0);
    int dinc = (dc == 0 || dc == 3) ? step : (dc == 1 ? -step : 0);
    if (ch == 3 && (dst >> 24) == 0xD && g->save_type == SAVE_EEPROM) ee_dma_hint(g, n);
    if (ch == 3 && (src >> 24) == 0xD && g->save_type == SAVE_EEPROM) ee_dma_hint(g, 0);

    g->cycle += 2;
    if (w32) {
        src &= ~3u; dst &= ~3u;
        for (uint32_t i = 0; i < n; i++) {
            write32(g, dst, read32(g, src));
            g->cycle += g->c32s[(src >> 24) & 15] + g->c32s[(dst >> 24) & 15];
            src += sinc; dst += dinc;
        }
    } else {
        src &= ~1u; dst &= ~1u;
        for (uint32_t i = 0; i < n; i++) {
            write16(g, dst, read16(g, src));
            g->cycle += g->c16s[(src >> 24) & 15] + g->c16s[(dst >> 24) & 15];
            src += sinc; dst += dinc;
        }
    }
    g->dma_src[ch] = src;
    g->dma_dst[ch] = (dc == 3) ? g->dma_dst0[ch] : dst;
    if (((ctrl >> 9) & 1) && timing != 0) {
        uint32_t cl = IO(g, 0xB0 + 12 * ch + 8);
        g->dma_cnt[ch] = cl ? cl : (ch == 3 ? 0x10000 : 0x4000);
        if (dc == 3) g->dma_dst[ch] = g->dma_dst0[ch];
    } else {
        IO(g, cbase) &= (uint16_t)~0x8000;
    }
    if (ctrl & 0x4000) raise_irq(g, (uint16_t)(1u << (8 + ch)));
}

static void dma_trigger(GBA *g, int timing) {
    for (int ch = 0; ch < 4; ch++) {
        uint16_t ctrl = IO(g, 0xB0 + 12 * ch + 10);
        if ((ctrl & 0x8000) && ((ctrl >> 12) & 3) == timing) dma_run(g, ch);
    }
}

static void dma_fifo_request(GBA *g, int fifo) {
    uint32_t target = 0x040000A0u + fifo * 4;
    for (int ch = 1; ch <= 2; ch++) {
        uint16_t ctrl = IO(g, 0xB0 + 12 * ch + 10);
        if ((ctrl & 0x8000) && ((ctrl >> 12) & 3) == 3 && (g->dma_dst0[ch] & ~3u) == target) dma_run(g, ch);
    }
}

static void dma_write_ctrl(GBA *g, int ch, uint16_t v) {
    uint32_t b = 0xB0 + 12 * ch;
    uint16_t old = IO(g, b + 10);
    IO(g, b + 10) = v;
    if (!(old & 0x8000) && (v & 0x8000)) {
        uint32_t sad = IO(g, b) | ((uint32_t)IO(g, b + 2) << 16);
        uint32_t dad = IO(g, b + 4) | ((uint32_t)IO(g, b + 6) << 16);
        uint32_t cl = IO(g, b + 8);
        g->dma_src[ch] = sad & (ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
        g->dma_dst[ch] = dad & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
        g->dma_dst0[ch] = g->dma_dst[ch];
        g->dma_cnt[ch] = cl ? cl : (ch == 3 ? 0x10000 : 0x4000);
        if (((v >> 12) & 3) == 0) dma_run(g, ch);
    }
}

/* ------------------------------------------------------------------ */
/* IO write                                                           */
/* ------------------------------------------------------------------ */
static void io_write16(GBA *g, uint32_t off, uint16_t v) {
    off &= 0x3FE;
    switch (off) {
    case 0x004: IO(g, 0x004) = (uint16_t)((v & 0xFF38) | (IO(g, 0x004) & 7)); return;
    case 0x006: return;
    case 0x028: case 0x02A: case 0x02C: case 0x02E:
    case 0x038: case 0x03A: case 0x03C: case 0x03E: {
        g->io[off >> 1] = v;
        uint32_t base = off & ~2u;
        int32_t val = (int32_t)(g->io[base >> 1] | ((uint32_t)g->io[(base + 2) >> 1] << 16));
        val = (val << 4) >> 4;
        int n = (off >= 0x38) ? 1 : 0;
        if (base == 0x28 || base == 0x38) g->bgx[n] = val; else g->bgy[n] = val;
        return;
    }
    case 0x082: {
        if (v & 0x0800) { g->fifo_cnt[0] = 0; g->fifo_rd[0] = 0; g->fifo_cur[0] = 0; }
        if (v & 0x8000) { g->fifo_cnt[1] = 0; g->fifo_rd[1] = 0; g->fifo_cur[1] = 0; }
        apu_sync(g);
        IO(g, 0x82) = (uint16_t)(v & 0x770F);
        return;
    }
    case 0x0A0: case 0x0A2: fifo_push(g, 0, v & 0xFF); fifo_push(g, 0, v >> 8); return;
    case 0x0A4: case 0x0A6: fifo_push(g, 1, v & 0xFF); fifo_push(g, 1, v >> 8); return;
    case 0x100: case 0x104: case 0x108: case 0x10C: g->tm[(off - 0x100) >> 2].reload = v; IO(g, off) = v; return;
    case 0x102: case 0x106: case 0x10A: case 0x10E: timer_write_ctrl(g, (off - 0x102) >> 2, v); return;
    case 0x130: return;
    case 0x200: IO(g, R_IE) = v; update_irq(g); return;
    case 0x202: IO(g, R_IF) &= (uint16_t)~v; update_irq(g); return;
    case 0x204: IO(g, R_WAITCNT) = (uint16_t)(v & 0x7FFF); update_waitstates(g); return;
    case 0x208: IO(g, R_IME) = v & 1; update_irq(g); return;
    case 0x300:
        IO(g, 0x300) = v & 0xFF;
        if (v >> 8) g->halted = 1;
        return;
    default: break;
    }
    if (off >= 0xB0 && off < 0xE0) {
        uint32_t rel = (off - 0xB0) % 12;
        int ch = (off - 0xB0) / 12;
        if (rel == 10) { dma_write_ctrl(g, ch, v); return; }
        g->io[off >> 1] = v;
        return;
    }
    if (off >= 0x90 && off < 0xA0) {
        int bank = !g->ch[2].w_bank;
        apu_sync(g);
        g->wave_ram[bank][off - 0x90] = (uint8_t)v;
        g->wave_ram[bank][off - 0x90 + 1] = (uint8_t)(v >> 8);
        return;
    }
    g->io[off >> 1] = v;
    if (off >= 0x60 && off <= 0x88) apu_write(g, off, v);
}

/* ------------------------------------------------------------------ */
/* Bus reads                                                          */
/* ------------------------------------------------------------------ */
static inline uint32_t rom_rd16(GBA *g, uint32_t o) {
    o &= 0x1FFFFFEu;
    if (o + 1 < g->rom_size) { uint16_t v; memcpy(&v, g->rom + o, 2); return v; }
    return (o >> 1) & 0xFFFF;
}
static inline uint32_t rom_rd32(GBA *g, uint32_t o) {
    o &= 0x1FFFFFCu;
    if (o + 3 < g->rom_size) { uint32_t v; memcpy(&v, g->rom + o, 4); return v; }
    return rom_rd16(g, o) | (rom_rd16(g, o + 2) << 16);
}

static uint32_t read32(GBA *g, uint32_t a) {
    uint32_t v, o;
    switch (a >> 24) {
    case 0: if (a < 0x4000) { memcpy(&v, g->bios + (a & 0x3FFC), 4); return v; } return 0;
    case 2: memcpy(&v, g->ewram + (a & 0x3FFFC), 4); return v;
    case 3: memcpy(&v, g->iwram + (a & 0x7FFC), 4); return v;
    case 4: if ((a & 0xFFFFFF) >= 0x400) return 0; return io_read16(g, a & 0x3FC) | (io_read16(g, (a & 0x3FC) + 2) << 16);
    case 5: memcpy(&v, g->pal + (a & 0x3FC), 4); return v;
    case 6: o = a & 0x1FFFC; if (o >= 0x18000) o -= 0x8000; memcpy(&v, g->vram + o, 4); return v;
    case 7: memcpy(&v, g->oam + (a & 0x3FC), 4); return v;
    case 8: case 9: case 10: case 11: case 12: return rom_rd32(g, a);
    case 13:
        if (is_eeprom_addr(g, a)) return ee_read(g);
        return rom_rd32(g, a);
    case 14: case 15: v = backup_read8(g, a); return v * 0x01010101u;
    default: return 0;
    }
}

static uint32_t read16(GBA *g, uint32_t a) {
    uint32_t o;
    uint16_t v;
    switch (a >> 24) {
    case 0: if (a < 0x4000) { memcpy(&v, g->bios + (a & 0x3FFE), 2); return v; } return 0;
    case 2: memcpy(&v, g->ewram + (a & 0x3FFFE), 2); return v;
    case 3: memcpy(&v, g->iwram + (a & 0x7FFE), 2); return v;
    case 4: if ((a & 0xFFFFFF) >= 0x400) return 0; return io_read16(g, a & 0x3FE);
    case 5: memcpy(&v, g->pal + (a & 0x3FE), 2); return v;
    case 6: o = a & 0x1FFFE; if (o >= 0x18000) o -= 0x8000; memcpy(&v, g->vram + o, 2); return v;
    case 7: memcpy(&v, g->oam + (a & 0x3FE), 2); return v;
    case 8: case 9: case 10: case 11: case 12: return rom_rd16(g, a);
    case 13:
        if (is_eeprom_addr(g, a)) return ee_read(g);
        return rom_rd16(g, a);
    case 14: case 15: return backup_read8(g, a) * 0x0101u;
    default: return 0;
    }
}

static uint32_t read8(GBA *g, uint32_t a) {
    switch (a >> 24) {
    case 0: return a < 0x4000 ? g->bios[a & 0x3FFF] : 0;
    case 2: return g->ewram[a & 0x3FFFF];
    case 3: return g->iwram[a & 0x7FFF];
    case 4: {
        if ((a & 0xFFFFFF) >= 0x400) return 0;
        uint32_t o = a & 0x3FF;
        if (o == 0x301) return 0;
        return (io_read16(g, o & ~1u) >> ((o & 1) * 8)) & 0xFF;
    }
    case 5: return g->pal[a & 0x3FF];
    case 6: { uint32_t o = a & 0x1FFFF; if (o >= 0x18000) o -= 0x8000; return g->vram[o]; }
    case 7: return g->oam[a & 0x3FF];
    case 8: case 9: case 10: case 11: case 12: case 13:
        return (rom_rd16(g, a & ~1u) >> ((a & 1) * 8)) & 0xFF;
    case 14: case 15: return backup_read8(g, a);
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Bus writes                                                         */
/* ------------------------------------------------------------------ */
static void write32(GBA *g, uint32_t a, uint32_t v) {
    uint32_t o;
    switch (a >> 24) {
    case 2: memcpy(g->ewram + (a & 0x3FFFC), &v, 4); break;
    case 3: memcpy(g->iwram + (a & 0x7FFC), &v, 4); break;
    case 4:
        if ((a & 0xFFFFFF) < 0x400) {
            io_write16(g, a & 0x3FC, (uint16_t)v);
            io_write16(g, (a & 0x3FC) + 2, (uint16_t)(v >> 16));
        }
        break;
    case 5: memcpy(g->pal + (a & 0x3FC), &v, 4); break;
    case 6: o = a & 0x1FFFC; if (o >= 0x18000) o -= 0x8000; memcpy(g->vram + o, &v, 4); break;
    case 7: memcpy(g->oam + (a & 0x3FC), &v, 4); break;
    case 13: if (is_eeprom_addr(g, a)) ee_write(g, v); break;
    case 14: case 15: backup_write8(g, a, (uint8_t)(v >> ((a & 3) * 8))); break;
    default: break;
    }
}

static void write16(GBA *g, uint32_t a, uint32_t v) {
    uint32_t o;
    uint16_t h = (uint16_t)v;
    switch (a >> 24) {
    case 2: memcpy(g->ewram + (a & 0x3FFFE), &h, 2); break;
    case 3: memcpy(g->iwram + (a & 0x7FFE), &h, 2); break;
    case 4: if ((a & 0xFFFFFF) < 0x400) io_write16(g, a & 0x3FE, h); break;
    case 5: memcpy(g->pal + (a & 0x3FE), &h, 2); break;
    case 6: o = a & 0x1FFFE; if (o >= 0x18000) o -= 0x8000; memcpy(g->vram + o, &h, 2); break;
    case 7: memcpy(g->oam + (a & 0x3FE), &h, 2); break;
    case 13: if (is_eeprom_addr(g, a)) ee_write(g, v); break;
    case 14: case 15: backup_write8(g, a, (uint8_t)(v >> ((a & 1) * 8))); break;
    default: break;
    }
}

static void write8(GBA *g, uint32_t a, uint32_t v) {
    uint8_t b = (uint8_t)v;
    switch (a >> 24) {
    case 2: g->ewram[a & 0x3FFFF] = b; break;
    case 3: g->iwram[a & 0x7FFF] = b; break;
    case 4: {
        if ((a & 0xFFFFFF) >= 0x400) break;
        uint32_t o = a & 0x3FF;
        if (o == 0x301) { g->halted = 1; break; }
        if (o >= 0x90 && o < 0xA0) {
            apu_sync(g);
            g->wave_ram[!g->ch[2].w_bank][o - 0x90] = b;
            break;
        }
        if (o >= 0xA0 && o < 0xA8) { fifo_push(g, (o - 0xA0) >> 2, b); break; }
        if (o == 0x202 || o == 0x203) {
            io_write16(g, o & ~1u, (uint16_t)(b << ((o & 1) * 8)));
            break;
        }
        uint16_t cur = g->io[o >> 1];
        uint16_t nv = (o & 1) ? (uint16_t)((cur & 0x00FF) | (b << 8)) : (uint16_t)((cur & 0xFF00) | b);
        io_write16(g, o & ~1u, nv);
        break;
    }
    case 5: g->pal[a & 0x3FE] = b; g->pal[(a & 0x3FE) | 1] = b; break;
    case 6: {
        uint32_t o = a & 0x1FFFF;
        if (o >= 0x18000) o -= 0x8000;
        uint32_t lim = ((IO(g, 0) & 7) >= 3) ? 0x14000 : 0x10000;
        if (o < lim) { g->vram[o & ~1u] = b; g->vram[o | 1] = b; }
        break;
    }
    case 7: break;
    case 14: case 15: backup_write8(g, a, b); break;
    default: break;
    }
}
