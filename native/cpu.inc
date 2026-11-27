/* ARM7TDMI interpreter + HLE BIOS.  Included by gba.c (unity build). */

#define FLAG_T 0x20u
#define FLAG_I 0x80u
#define FLAG_F 0x40u

static inline uint32_t ror32(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline int bank_of(uint32_t mode) {
    switch (mode) {
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 3;
    case 0x17: return 4;
    case 0x1B: return 5;
    default: return 0;
    }
}

static void set_mode(GBA *g, uint32_t nm) {
    uint32_t om = g->cpsr & 0x1F;
    int ob = bank_of(om), nb = bank_of(nm);
    if (ob != nb) {
        g->bk13[ob] = g->r[13];
        g->bk14[ob] = g->r[14];
        if (ob == 1) for (int i = 0; i < 5; i++) { g->fiq_hi[i] = g->r[8 + i]; g->r[8 + i] = g->usr_hi[i]; }
        if (nb == 1) for (int i = 0; i < 5; i++) { g->usr_hi[i] = g->r[8 + i]; g->r[8 + i] = g->fiq_hi[i]; }
        g->r[13] = g->bk13[nb];
        g->r[14] = g->bk14[nb];
    }
    g->cpsr = (g->cpsr & ~0x1Fu) | nm;
}

static inline uint32_t get_cpsr(GBA *g) {
    return (g->cpsr & 0x0FFFFFFFu) | (g->fN << 31) | (g->fZ << 30) | (g->fC << 29) | (g->fV << 28);
}

static void set_cpsr_all(GBA *g, uint32_t v) {
    set_mode(g, v & 0x1F);
    g->cpsr = (g->cpsr & ~0xE0u) | (v & 0xE0u);
    g->fN = v >> 31; g->fZ = (v >> 30) & 1; g->fC = (v >> 29) & 1; g->fV = (v >> 28) & 1;
}

static inline int has_spsr(GBA *g) {
    uint32_t m = g->cpsr & 0x1F;
    return m != 0x10 && m != 0x1F;
}

static uint32_t ureg_get(GBA *g, int i) {
    uint32_t m = g->cpsr & 0x1F;
    if (i < 8 || i == 15) return g->r[i];
    if (i < 13) return m == 0x11 ? g->usr_hi[i - 8] : g->r[i];
    if (bank_of(m) == 0) return g->r[i];
    return i == 13 ? g->bk13[0] : g->bk14[0];
}
static void ureg_set(GBA *g, int i, uint32_t v) {
    uint32_t m = g->cpsr & 0x1F;
    if (i < 8 || i == 15) { g->r[i] = v; return; }
    if (i < 13) { if (m == 0x11) g->usr_hi[i - 8] = v; else g->r[i] = v; return; }
    if (bank_of(m) == 0) { g->r[i] = v; return; }
    if (i == 13) g->bk13[0] = v; else g->bk14[0] = v;
}

static inline void pc_write_arm(GBA *g, uint32_t v) {
    g->r[15] = (v & ~3u) + 8;
    g->flushed = 1;
    g->cycle += 2;
}
static inline void pc_write_thumb(GBA *g, uint32_t v) {
    g->r[15] = (v & ~1u) + 4;
    g->flushed = 1;
    g->cycle += 2;
}
/* write PC after a mode-aware operation (e.g. SUBS pc / LDM ^) */
static inline void pc_write_cur(GBA *g, uint32_t v) {
    if (g->cpsr & FLAG_T) pc_write_thumb(g, v); else pc_write_arm(g, v);
}

static void take_irq(GBA *g) {
    uint32_t old = get_cpsr(g);
    uint32_t lr = (g->cpsr & FLAG_T) ? g->r[15] : g->r[15] - 4;
    set_mode(g, 0x12);
    g->spsr[2] = old;
    g->r[14] = lr;
    g->cpsr = (g->cpsr & ~FLAG_T) | FLAG_I;
    g->r[15] = 0x18 + 8;
    g->halted = 0;
    g->cycle += 3;
}

static inline int cond_pass(GBA *g, uint32_t c) {
    switch (c) {
    case 0: return g->fZ;
    case 1: return !g->fZ;
    case 2: return g->fC;
    case 3: return !g->fC;
    case 4: return g->fN;
    case 5: return !g->fN;
    case 6: return g->fV;
    case 7: return !g->fV;
    case 8: return g->fC && !g->fZ;
    case 9: return !g->fC || g->fZ;
    case 10: return g->fN == g->fV;
    case 11: return g->fN != g->fV;
    case 12: return !g->fZ && g->fN == g->fV;
    case 13: return g->fZ || g->fN != g->fV;
    case 14: return 1;
    default: return 0;
    }
}

/* ---------------- ALU helpers ---------------- */
static inline uint32_t do_add(GBA *g, uint32_t a, uint32_t b, uint32_t c, int s) {
    uint64_t r = (uint64_t)a + b + c;
    uint32_t res = (uint32_t)r;
    if (s) { g->fN = res >> 31; g->fZ = !res; g->fC = (uint32_t)(r >> 32); g->fV = ((a ^ res) & (b ^ res)) >> 31; }
    return res;
}
static inline uint32_t do_sub(GBA *g, uint32_t a, uint32_t b, uint32_t c, int s) {
    uint64_t r = (uint64_t)a + (uint32_t)~b + c;
    uint32_t res = (uint32_t)r;
    if (s) { g->fN = res >> 31; g->fZ = !res; g->fC = (uint32_t)(r >> 32); g->fV = ((a ^ b) & (a ^ res)) >> 31; }
    return res;
}
static inline void set_nz(GBA *g, uint32_t v) { g->fN = v >> 31; g->fZ = !v; }

static inline uint32_t shift_imm(GBA *g, uint32_t type, uint32_t v, uint32_t amt, uint32_t *c) {
    switch (type) {
    case 0: if (!amt) return v; *c = (v >> (32 - amt)) & 1; return v << amt;
    case 1: if (!amt) { *c = v >> 31; return 0; } *c = (v >> (amt - 1)) & 1; return v >> amt;
    case 2: if (!amt) { *c = v >> 31; return (uint32_t)((int32_t)v >> 31); } *c = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt);
    default:
        if (!amt) { uint32_t cc = g->fC; *c = v & 1; return (v >> 1) | (cc << 31); }
        *c = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
    }
}
static inline uint32_t shift_reg(GBA *g, uint32_t type, uint32_t v, uint32_t amt, uint32_t *c) {
    (void)g;
    if (!amt) return v;
    switch (type) {
    case 0:
        if (amt < 32) { *c = (v >> (32 - amt)) & 1; return v << amt; }
        if (amt == 32) { *c = v & 1; return 0; }
        *c = 0; return 0;
    case 1:
        if (amt < 32) { *c = (v >> (amt - 1)) & 1; return v >> amt; }
        if (amt == 32) { *c = v >> 31; return 0; }
        *c = 0; return 0;
    case 2:
        if (amt < 32) { *c = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *c = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default:
        amt &= 31;
        if (!amt) { *c = v >> 31; return v; }
        *c = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
    }
}

static inline int mul_cycles(uint32_t rs) {
    if ((rs & 0xFFFFFF00u) == 0 || (rs & 0xFFFFFF00u) == 0xFFFFFF00u) return 1;
    if ((rs & 0xFFFF0000u) == 0 || (rs & 0xFFFF0000u) == 0xFFFF0000u) return 2;
    if ((rs & 0xFF000000u) == 0 || (rs & 0xFF000000u) == 0xFF000000u) return 3;
    return 4;
}

/* ---------------- HLE BIOS ---------------- */
static int16_t sintab[256];
static int sintab_ready = 0;
static void init_sintab(void) {
    if (sintab_ready) return;
    for (int i = 0; i < 256; i++) sintab[i] = (int16_t)lrint(sin(i * 6.283185307179586 / 256.0) * 16384.0);
    sintab_ready = 1;
}

static int32_t bios_arctan(int32_t i) {
    int32_t a = -((i * i) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}
static int32_t bios_arctan2(int32_t x, int32_t y) {
    if (!y) return x >= 0 ? 0 : 0x8000;
    if (!x) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) { if (x >= y) return bios_arctan((y << 14) / x); }
        else if (-x >= y) return bios_arctan((y << 14) / x) + 0x8000;
        return 0x4000 - bios_arctan((x << 14) / y);
    } else {
        if (x <= 0) { if (-x > -y) return bios_arctan((y << 14) / x) + 0x8000; }
        else if (x >= -y) return bios_arctan((y << 14) / x) + 0x10000;
        return 0xC000 - bios_arctan((x << 14) / y);
    }
}

static void store_buf(GBA *g, uint32_t dst, const uint8_t *buf, uint32_t len, int half) {
    if (half) {
        for (uint32_t i = 0; i + 1 < len + 1; i += 2) {
            uint32_t v = buf[i] | ((i + 1 < len ? buf[i + 1] : 0) << 8);
            write16(g, dst + i, v);
        }
    } else {
        for (uint32_t i = 0; i < len; i++) write8(g, dst + i, buf[i]);
    }
}

static void bios_lz77(GBA *g, int half) {
    uint32_t src = g->r[0], dst = g->r[1];
    uint32_t hdr = read32(g, src);
    uint32_t len = hdr >> 8;
    src += 4;
    uint8_t *buf = (uint8_t *)malloc(len + 16);
    if (!buf) return;
    uint32_t n = 0;
    while (n < len) {
        uint8_t flags = (uint8_t)read8(g, src++);
        for (int i = 0; i < 8 && n < len; i++) {
            if (flags & (0x80 >> i)) {
                uint32_t b0 = read8(g, src++), b1 = read8(g, src++);
                uint32_t cnt = (b0 >> 4) + 3, disp = (((b0 & 0xF) << 8) | b1) + 1;
                for (uint32_t k = 0; k < cnt && n < len; k++) { buf[n] = n >= disp ? buf[n - disp] : 0; n++; }
            } else {
                buf[n++] = (uint8_t)read8(g, src++);
            }
        }
    }
    store_buf(g, dst, buf, len, half);
    free(buf);
}

static void bios_rl(GBA *g, int half) {
    uint32_t src = g->r[0], dst = g->r[1];
    uint32_t hdr = read32(g, src);
    uint32_t len = hdr >> 8;
    src += 4;
    uint8_t *buf = (uint8_t *)malloc(len + 16);
    if (!buf) return;
    uint32_t n = 0;
    while (n < len) {
        uint8_t f = (uint8_t)read8(g, src++);
        if (f & 0x80) {
            uint32_t cnt = (f & 0x7F) + 3;
            uint8_t d = (uint8_t)read8(g, src++);
            for (uint32_t k = 0; k < cnt && n < len; k++) buf[n++] = d;
        } else {
            uint32_t cnt = (f & 0x7F) + 1;
            for (uint32_t k = 0; k < cnt && n < len; k++) buf[n++] = (uint8_t)read8(g, src++);
        }
    }
    store_buf(g, dst, buf, len, half);
    free(buf);
}

static void bios_huff(GBA *g) {
    uint32_t src = g->r[0], dst = g->r[1];
    uint32_t hdr = read32(g, src);
    int bits = hdr & 0xF;
    uint32_t size = hdr >> 8;
    src += 4;
    uint32_t tree = src;
    uint32_t bs = tree + (read8(g, tree) + 1) * 2;
    uint32_t node = tree + 1, outw = 0, written = 0;
    int outbits = 0;
    while (written < size) {
        uint32_t w = read32(g, bs);
        bs += 4;
        for (int i = 31; i >= 0 && written < size; i--) {
            uint32_t nb = read8(g, node);
            uint32_t bit = (w >> i) & 1;
            uint32_t next = (node & ~1u) + (nb & 0x3F) * 2 + 2 + bit;
            int leaf = bit ? (nb & 0x40) : (nb & 0x80);
            if (leaf) {
                outw |= (read8(g, next) & ((1u << bits) - 1)) << outbits;
                outbits += bits;
                if (outbits >= 32) { write32(g, dst, outw); dst += 4; written += 4; outw = 0; outbits = 0; }
                node = tree + 1;
            } else {
                node = next;
            }
        }
    }
}

static void bios_bitunpack(GBA *g) {
    uint32_t src = g->r[0], dst = g->r[1], info = g->r[2];
    uint32_t len = read16(g, info);
    uint32_t sw = read8(g, info + 2), dw = read8(g, info + 3);
    uint32_t off = read32(g, info + 4);
    int zero = off >> 31;
    off &= 0x7FFFFFFF;
    uint32_t acc = 0;
    int ob = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t b = read8(g, src++);
        for (uint32_t p = 0; p < 8; p += sw) {
            uint32_t v = (b >> p) & ((1u << sw) - 1);
            if (v || zero) v += off;
            acc |= (v & ((dw >= 32) ? 0xFFFFFFFFu : ((1u << dw) - 1))) << ob;
            ob += dw;
            if (ob >= 32) { write32(g, dst, acc); dst += 4; acc = 0; ob = 0; }
        }
    }
}

static void bios_diff(GBA *g, int w16, int half) {
    uint32_t src = g->r[0], dst = g->r[1];
    uint32_t len = read32(g, src) >> 8;
    src += 4;
    if (w16) {
        uint16_t acc = 0;
        for (uint32_t i = 0; i < len; i += 2) { acc = (uint16_t)(acc + read16(g, src)); src += 2; write16(g, dst, acc); dst += 2; }
    } else {
        uint8_t acc = 0, prev = 0;
        for (uint32_t i = 0; i < len; i++) {
            acc = (uint8_t)(acc + read8(g, src++));
            if (half) { if (i & 1) write16(g, dst + i - 1, prev | (acc << 8)); else prev = acc; }
            else write8(g, dst + i, acc);
        }
    }
}

static void bios_cpuset(GBA *g, int fast) {
    uint32_t src = g->r[0], dst = g->r[1], c = g->r[2];
    uint32_t cnt = c & 0x1FFFFF;
    int fill = (c >> 24) & 1;
    if (fast) {
        cnt = (cnt + 7) & ~7u;
        src &= ~3u; dst &= ~3u;
        uint32_t v = fill ? read32(g, src) : 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (!fill) v = read32(g, src), src += 4;
            write32(g, dst, v);
            dst += 4;
        }
    } else if ((c >> 26) & 1) {
        src &= ~3u; dst &= ~3u;
        uint32_t v = fill ? read32(g, src) : 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (!fill) v = read32(g, src), src += 4;
            write32(g, dst, v);
            dst += 4;
        }
    } else {
        src &= ~1u; dst &= ~1u;
        uint32_t v = fill ? read16(g, src) : 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (!fill) v = read16(g, src), src += 2;
            write16(g, dst, v);
            dst += 2;
        }
    }
}

static void bios_bgaffine(GBA *g) {
    uint32_t src = g->r[0], dst = g->r[1], n = g->r[2];
    for (uint32_t i = 0; i < n; i++) {
        int32_t ox = (int32_t)read32(g, src), oy = (int32_t)read32(g, src + 4);
        int32_t cx = (int16_t)read16(g, src + 8), cy = (int16_t)read16(g, src + 10);
        int32_t sx = (int16_t)read16(g, src + 12), sy = (int16_t)read16(g, src + 14);
        uint32_t th = read16(g, src + 16) >> 8;
        int32_t sn = sintab[th & 255], cs = sintab[(th + 64) & 255];
        int32_t a = (sx * cs) >> 14, b = -((sx * sn) >> 14), c = (sy * sn) >> 14, d = (sy * cs) >> 14;
        int32_t rx = ox - (a * cx + b * cy), ry = oy - (c * cx + d * cy);
        write16(g, dst, (uint16_t)a); write16(g, dst + 2, (uint16_t)b);
        write16(g, dst + 4, (uint16_t)c); write16(g, dst + 6, (uint16_t)d);
        write32(g, dst + 8, (uint32_t)rx); write32(g, dst + 12, (uint32_t)ry);
        src += 20; dst += 16;
    }
}

static void bios_objaffine(GBA *g) {
    uint32_t src = g->r[0], dst = g->r[1], n = g->r[2], off = g->r[3];
    for (uint32_t i = 0; i < n; i++) {
        int32_t sx = (int16_t)read16(g, src), sy = (int16_t)read16(g, src + 2);
        uint32_t th = read16(g, src + 4) >> 8;
        int32_t sn = sintab[th & 255], cs = sintab[(th + 64) & 255];
        write16(g, dst, (uint16_t)((sx * cs) >> 14));
        write16(g, dst + off, (uint16_t)(-((sx * sn) >> 14)));
        write16(g, dst + off * 2, (uint16_t)((sy * sn) >> 14));
        write16(g, dst + off * 3, (uint16_t)((sy * cs) >> 14));
        src += 8; dst += off * 4;
    }
}

static void cpu_reset_state(GBA *g, uint32_t pc);

static void bios_swi(GBA *g, uint32_t num) {
    g->cycle += 10;
    switch (num) {
    case 0x00: cpu_reset_state(g, (read8(g, 0x03007FFA) ? 0x02000000u : 0x08000000u)); g->flushed = 1; break;
    case 0x01: {
        uint32_t f = g->r[0];
        if (f & 1) memset(g->ewram, 0, sizeof g->ewram);
        if (f & 2) memset(g->iwram, 0, 0x7E00);
        if (f & 4) memset(g->pal, 0, sizeof g->pal);
        if (f & 8) memset(g->vram, 0, sizeof g->vram);
        if (f & 16) memset(g->oam, 0, sizeof g->oam);
        IO(g, 0) = 0x80;
        if (f & 0x80) {
            for (uint32_t o = 0x02; o < 0x60; o += 2) IO(g, o) = 0;
            for (uint32_t o = 0xB0; o < 0xE0; o += 2) IO(g, o) = 0;
            for (int i = 0; i < 4; i++) { g->tm[i].enabled = 0; g->tm[i].reload = 0; IO(g, 0x100 + i * 4) = 0; IO(g, 0x102 + i * 4) = 0; }
            timers_recalc(g);
        }
        break;
    }
    case 0x02: case 0x03: g->halted = 1; break;
    case 0x04: case 0x05: {
        uint32_t discard = num == 5 ? 1 : g->r[0], mask = num == 5 ? 1 : g->r[1];
        uint16_t bif;
        memcpy(&bif, g->iwram + 0x7FF8, 2);
        if (!g->swi_resume) {
            IO(g, R_IME) = 1;
            update_irq(g);
            if (discard) bif &= (uint16_t)~mask;
        }
        if (bif & mask) {
            bif &= (uint16_t)~mask;
            g->swi_resume = 0;
        } else {
            g->swi_resume = 1;
            g->halted = 1;
            g->flushed = 1; /* re-execute this SWI after the interrupt */
        }
        memcpy(g->iwram + 0x7FF8, &bif, 2);
        break;
    }
    case 0x06: case 0x07: {
        int32_t n = (int32_t)g->r[num == 6 ? 0 : 1], d = (int32_t)g->r[num == 6 ? 1 : 0];
        int32_t q, m;
        if (d == 0) { q = n < 0 ? -1 : 1; m = n; }
        else if (d == -1) { q = (int32_t)(0u - (uint32_t)n); m = 0; }
        else { q = n / d; m = n % d; }
        g->r[0] = (uint32_t)q; g->r[1] = (uint32_t)m; g->r[3] = (uint32_t)(q < 0 ? -q : q);
        break;
    }
    case 0x08: {
        uint32_t x = g->r[0];
        uint32_t r = (uint32_t)sqrt((double)x);
        while ((uint64_t)r * r > x) r--;
        while ((uint64_t)(r + 1) * (r + 1) <= x) r++;
        g->r[0] = r;
        break;
    }
    case 0x09: g->r[0] = (uint32_t)bios_arctan((int32_t)g->r[0]) & 0xFFFF; break;
    case 0x0A: g->r[0] = (uint32_t)bios_arctan2((int32_t)g->r[0], (int32_t)g->r[1]) & 0xFFFF; break;
    case 0x0B: bios_cpuset(g, 0); break;
    case 0x0C: bios_cpuset(g, 1); break;
    case 0x0D: g->r[0] = 0xBAAE187F; break;
    case 0x0E: bios_bgaffine(g); break;
    case 0x0F: bios_objaffine(g); break;
    case 0x10: bios_bitunpack(g); break;
    case 0x11: bios_lz77(g, 0); break;
    case 0x12: bios_lz77(g, 1); break;
    case 0x13: bios_huff(g); break;
    case 0x14: bios_rl(g, 0); break;
    case 0x15: bios_rl(g, 1); break;
    case 0x16: bios_diff(g, 0, 0); break;
    case 0x17: bios_diff(g, 0, 1); break;
    case 0x18: bios_diff(g, 1, 0); break;
    case 0x1F: {
        double e = (180.0 - (int32_t)g->r[1] - (int32_t)g->r[2] / 256.0) / 12.0;
        double f = (double)read32(g, g->r[0] + 4);
        g->r[0] = (uint32_t)(f / pow(2.0, e));
        break;
    }
    default: break;
    }
}

/* ---------------- ARM ---------------- */
static void arm_sdt(GBA *g, uint32_t op) {
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t base = g->r[rn], off;
    if (op & (1u << 25)) {
        uint32_t c = g->fC;
        off = shift_imm(g, (op >> 5) & 3, g->r[op & 15], (op >> 7) & 31, &c);
    } else {
        off = op & 0xFFF;
    }
    int up = (op >> 23) & 1, pre = (op >> 24) & 1, wb = (op >> 21) & 1, byte = (op >> 22) & 1, load = (op >> 20) & 1;
    uint32_t nb = up ? base + off : base - off;
    uint32_t addr = pre ? nb : base;
    if (load) {
        uint32_t v;
        if (byte) {
            v = read8(g, addr);
            g->cycle += g->c16n[(addr >> 24) & 15];
        } else {
            v = ror32(read32(g, addr & ~3u), (addr & 3) * 8);
            g->cycle += g->c32n[(addr >> 24) & 15];
        }
        g->cycle += 1;
        if (!pre || wb) g->r[rn] = nb;
        if (rd == 15) pc_write_arm(g, v); else g->r[rd] = v;
    } else {
        uint32_t v = g->r[rd];
        if (rd == 15) v += 4;
        if (byte) { write8(g, addr, v); g->cycle += g->c16n[(addr >> 24) & 15]; }
        else { write32(g, addr & ~3u, v); g->cycle += g->c32n[(addr >> 24) & 15]; }
        if (!pre || wb) g->r[rn] = nb;
    }
}

static void arm_hdt(GBA *g, uint32_t op) {
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t base = g->r[rn];
    uint32_t off = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 0xF)) : g->r[op & 15];
    int up = (op >> 23) & 1, pre = (op >> 24) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
    uint32_t sh = (op >> 5) & 3;
    uint32_t nb = up ? base + off : base - off;
    uint32_t addr = pre ? nb : base;
    g->cycle += g->c16n[(addr >> 24) & 15];
    if (load) {
        uint32_t v;
        switch (sh) {
        case 1: v = ror32(read16(g, addr & ~1u), (addr & 1) * 8); break;
        case 2: v = (uint32_t)(int32_t)(int8_t)read8(g, addr); break;
        default:
            if (addr & 1) v = (uint32_t)(int32_t)(int8_t)read8(g, addr);
            else v = (uint32_t)(int32_t)(int16_t)read16(g, addr);
            break;
        }
        g->cycle += 1;
        if (!pre || wb) g->r[rn] = nb;
        if (rd == 15) pc_write_arm(g, v); else g->r[rd] = v;
    } else {
        uint32_t v = g->r[rd];
        if (rd == 15) v += 4;
        write16(g, addr & ~1u, v);
        if (!pre || wb) g->r[rn] = nb;
    }
}

static void arm_block(GBA *g, uint32_t op) {
    uint32_t rn = (op >> 16) & 15, list = op & 0xFFFF;
    int P = (op >> 24) & 1, U = (op >> 23) & 1, S = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
    uint32_t base = g->r[rn];
    int n = __builtin_popcount(list);
    if (list == 0) { list = 0x8000; n = 16; }
    uint32_t start, nb;
    if (U) { start = P ? base + 4 : base; nb = base + n * 4; }
    else { start = P ? base - n * 4 : base - n * 4 + 4; nb = base - n * 4; }
    int user = S && (!L || !(list & 0x8000));
    start &= ~3u;
    g->cycle += n + (L ? 1 : 0);
    if (L) {
        if (W) g->r[rn] = nb;
        uint32_t a = start;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            uint32_t v = read32(g, a);
            a += 4;
            if (i == 15) {
                if (S) set_cpsr_all(g, g->spsr[bank_of(g->cpsr & 0x1F)]);
                pc_write_cur(g, v);
            } else if (user) ureg_set(g, i, v);
            else g->r[i] = v;
        }
    } else {
        uint32_t a = start;
        int first = 1;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            uint32_t v = (i == 15) ? g->r[15] + 4 : (user ? ureg_get(g, i) : g->r[i]);
            write32(g, a, v);
            a += 4;
            if (first && W) g->r[rn] = nb;
            first = 0;
        }
    }
}

static void arm_dp(GBA *g, uint32_t op) {
    uint32_t opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t c = g->fC, op2, a = g->r[rn];
    if (op & (1u << 25)) {
        uint32_t rot = ((op >> 8) & 15) * 2;
        op2 = ror32(op & 0xFF, rot);
        if (rot) c = op2 >> 31;
    } else {
        uint32_t v = g->r[op & 15];
        if (op & 0x10) {
            g->cycle += 1;
            if ((op & 15) == 15) v += 4;
            if (rn == 15) a += 4;
            op2 = shift_reg(g, (op >> 5) & 3, v, g->r[(op >> 8) & 15] & 0xFF, &c);
        } else {
            op2 = shift_imm(g, (op >> 5) & 3, v, (op >> 7) & 31, &c);
        }
    }
    uint32_t res = 0;
    int logical = 0, write = 1;
    switch (opc) {
    case 0: res = a & op2; logical = 1; break;
    case 1: res = a ^ op2; logical = 1; break;
    case 2: res = do_sub(g, a, op2, 1, s); break;
    case 3: res = do_sub(g, op2, a, 1, s); break;
    case 4: res = do_add(g, a, op2, 0, s); break;
    case 5: res = do_add(g, a, op2, g->fC, s); break;
    case 6: res = do_sub(g, a, op2, g->fC, s); break;
    case 7: res = do_sub(g, op2, a, g->fC, s); break;
    case 8: res = a & op2; logical = 1; write = 0; s = 1; break;
    case 9: res = a ^ op2; logical = 1; write = 0; s = 1; break;
    case 10: do_sub(g, a, op2, 1, 1); write = 0; break;
    case 11: do_add(g, a, op2, 0, 1); write = 0; break;
    case 12: res = a | op2; logical = 1; break;
    case 13: res = op2; logical = 1; break;
    case 14: res = a & ~op2; logical = 1; break;
    default: res = ~op2; logical = 1; break;
    }
    if (logical && s && !(rd == 15 && write)) { set_nz(g, res); g->fC = c; }
    if (write) {
        if (rd == 15) {
            if (s) {
                if (has_spsr(g)) set_cpsr_all(g, g->spsr[bank_of(g->cpsr & 0x1F)]);
                pc_write_cur(g, res);
            } else {
                pc_write_arm(g, res);
            }
        } else {
            g->r[rd] = res;
        }
    }
}

static void arm_psr(GBA *g, uint32_t op) {
    if (op & (1u << 21)) { /* MSR */
        uint32_t v;
        if (op & (1u << 25)) v = ror32(op & 0xFF, ((op >> 8) & 15) * 2);
        else v = g->r[op & 15];
        uint32_t fm = (op >> 16) & 15, mask = 0;
        if (fm & 8) mask |= 0xFF000000u;
        if (fm & 4) mask |= 0x00FF0000u;
        if (fm & 2) mask |= 0x0000FF00u;
        if (fm & 1) mask |= 0x000000FFu;
        if (op & (1u << 22)) {
            if (has_spsr(g)) {
                int b = bank_of(g->cpsr & 0x1F);
                g->spsr[b] = (g->spsr[b] & ~mask) | (v & mask);
            }
        } else {
            if ((g->cpsr & 0x1F) == 0x10) mask &= 0xFF000000u;
            uint32_t cur = get_cpsr(g);
            uint32_t nv = (cur & ~mask) | (v & mask);
            nv = (nv & ~FLAG_T) | (cur & FLAG_T);
            set_cpsr_all(g, nv);
        }
    } else { /* MRS */
        uint32_t rd = (op >> 12) & 15;
        if (op & (1u << 22)) g->r[rd] = has_spsr(g) ? g->spsr[bank_of(g->cpsr & 0x1F)] : get_cpsr(g);
        else g->r[rd] = get_cpsr(g);
    }
}

static void arm_step(GBA *g) {
    uint32_t pc = g->r[15] - 8;
    uint32_t op = read32(g, pc);
    g->cycle += g->c32s[(pc >> 24) & 15];
    uint32_t cond = op >> 28;
    if (cond != 14 && !cond_pass(g, cond)) { g->r[15] += 4; return; }

    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x90) == 0x90) {
            if ((op & 0x0FC000F0) == 0x00000090) {
                uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
                uint32_t res = g->r[rm] * g->r[rs];
                if (op & (1u << 21)) res += g->r[rn];
                g->cycle += mul_cycles(g->r[rs]) + ((op >> 21) & 1);
                g->r[rd] = res;
                if (op & (1u << 20)) set_nz(g, res);
            } else if ((op & 0x0F8000F0) == 0x00800090) {
                uint32_t rdh = (op >> 16) & 15, rdl = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
                uint64_t res;
                if (op & (1u << 22)) res = (uint64_t)((int64_t)(int32_t)g->r[rm] * (int64_t)(int32_t)g->r[rs]);
                else res = (uint64_t)g->r[rm] * (uint64_t)g->r[rs];
                if (op & (1u << 21)) res += ((uint64_t)g->r[rdh] << 32) | g->r[rdl];
                g->cycle += mul_cycles(g->r[rs]) + 1 + ((op >> 21) & 1);
                g->r[rdl] = (uint32_t)res;
                g->r[rdh] = (uint32_t)(res >> 32);
                if (op & (1u << 20)) { g->fN = (uint32_t)(res >> 63); g->fZ = res == 0; }
            } else if ((op & 0x0FB00FF0) == 0x01000090) {
                uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
                uint32_t addr = g->r[rn], v;
                if (op & (1u << 22)) { v = read8(g, addr); write8(g, addr, g->r[rm]); }
                else { v = ror32(read32(g, addr & ~3u), (addr & 3) * 8); write32(g, addr & ~3u, g->r[rm]); }
                g->cycle += 3;
                g->r[rd] = v;
            } else if ((op & 0x60) != 0) {
                arm_hdt(g, op);
            }
            break;
        }
        /* fallthrough */
    case 1:
        if ((op & 0x0FFFFFF0) == 0x012FFF10) {
            uint32_t v = g->r[op & 15];
            if (v & 1) { g->cpsr |= FLAG_T; pc_write_thumb(g, v); }
            else pc_write_arm(g, v);
        } else if (((op >> 23) & 3) == 2 && !((op >> 20) & 1)) {
            arm_psr(g, op);
        } else {
            arm_dp(g, op);
        }
        break;
    case 2: case 3:
        if ((op >> 25) & 1 && (op & 0x10)) break; /* undefined */
        arm_sdt(g, op);
        break;
    case 4: arm_block(g, op); break;
    case 5: {
        int32_t off = ((int32_t)(op << 8)) >> 6;
        if (op & (1u << 24)) g->r[14] = g->r[15] - 4;
        pc_write_arm(g, g->r[15] + (uint32_t)off);
        break;
    }
    case 7:
        if (op & (1u << 24)) bios_swi(g, (op >> 16) & 0xFF);
        break;
    default: break; /* coprocessor: ignored */
    }
    if (g->flushed) g->flushed = 0; else g->r[15] += 4;
}

/* ---------------- Thumb ---------------- */
static void thumb_step(GBA *g) {
    uint32_t pc = g->r[15] - 4;
    uint32_t op = read16(g, pc);
    g->cycle += g->c16s[(pc >> 24) & 15];
    uint32_t rd, rs, rb, ro, v, a, c;

    switch (op >> 11) {
    case 0: case 1: case 2:
        c = g->fC;
        v = shift_imm(g, op >> 11, g->r[(op >> 3) & 7], (op >> 6) & 31, &c);
        g->r[op & 7] = v; set_nz(g, v); g->fC = c;
        break;
    case 3: {
        uint32_t b = (op & 0x400) ? (op >> 6) & 7 : g->r[(op >> 6) & 7];
        a = g->r[(op >> 3) & 7];
        g->r[op & 7] = (op & 0x200) ? do_sub(g, a, b, 1, 1) : do_add(g, a, b, 0, 1);
        break;
    }
    case 4: case 5: case 6: case 7: {
        rd = (op >> 8) & 7;
        uint32_t imm = op & 0xFF;
        switch ((op >> 11) & 3) {
        case 0: g->r[rd] = imm; set_nz(g, imm); break;
        case 1: do_sub(g, g->r[rd], imm, 1, 1); break;
        case 2: g->r[rd] = do_add(g, g->r[rd], imm, 0, 1); break;
        default: g->r[rd] = do_sub(g, g->r[rd], imm, 1, 1); break;
        }
        break;
    }
    case 8:
        if (op & 0x400) {
            rd = ((op >> 4) & 8) | (op & 7);
            rs = (op >> 3) & 15;
            uint32_t b = g->r[rs];
            switch ((op >> 8) & 3) {
            case 0: v = g->r[rd] + b; if (rd == 15) pc_write_thumb(g, v); else g->r[rd] = v; break;
            case 1: do_sub(g, g->r[rd], b, 1, 1); break;
            case 2: if (rd == 15) pc_write_thumb(g, b); else g->r[rd] = b; break;
            default:
                if (b & 1) pc_write_thumb(g, b);
                else { g->cpsr &= ~FLAG_T; pc_write_arm(g, b); }
                break;
            }
        } else {
            rd = op & 7; rs = (op >> 3) & 7;
            a = g->r[rd];
            uint32_t b = g->r[rs];
            c = g->fC;
            switch ((op >> 6) & 15) {
            case 0: v = a & b; set_nz(g, v); g->r[rd] = v; break;
            case 1: v = a ^ b; set_nz(g, v); g->r[rd] = v; break;
            case 2: v = shift_reg(g, 0, a, b & 0xFF, &c); g->fC = c; set_nz(g, v); g->r[rd] = v; g->cycle++; break;
            case 3: v = shift_reg(g, 1, a, b & 0xFF, &c); g->fC = c; set_nz(g, v); g->r[rd] = v; g->cycle++; break;
            case 4: v = shift_reg(g, 2, a, b & 0xFF, &c); g->fC = c; set_nz(g, v); g->r[rd] = v; g->cycle++; break;
            case 5: g->r[rd] = do_add(g, a, b, g->fC, 1); break;
            case 6: g->r[rd] = do_sub(g, a, b, g->fC, 1); break;
            case 7: v = shift_reg(g, 3, a, b & 0xFF, &c); g->fC = c; set_nz(g, v); g->r[rd] = v; g->cycle++; break;
            case 8: set_nz(g, a & b); break;
            case 9: g->r[rd] = do_sub(g, 0, b, 1, 1); break;
            case 10: do_sub(g, a, b, 1, 1); break;
            case 11: do_add(g, a, b, 0, 1); break;
            case 12: v = a | b; set_nz(g, v); g->r[rd] = v; break;
            case 13: v = a * b; set_nz(g, v); g->r[rd] = v; g->cycle += mul_cycles(a); break;
            case 14: v = a & ~b; set_nz(g, v); g->r[rd] = v; break;
            default: v = ~b; set_nz(g, v); g->r[rd] = v; break;
            }
        }
        break;
    case 9:
        rd = (op >> 8) & 7;
        a = (g->r[15] & ~3u) + ((op & 0xFF) << 2);
        g->r[rd] = read32(g, a);
        g->cycle += 1 + g->c32n[(a >> 24) & 15];
        break;
    case 10: case 11:
        rd = op & 7; rb = (op >> 3) & 7; ro = (op >> 6) & 7;
        a = g->r[rb] + g->r[ro];
        g->cycle += g->c16n[(a >> 24) & 15];
        switch ((op >> 9) & 7) {
        case 0: write32(g, a & ~3u, g->r[rd]); g->cycle += g->c32n[(a >> 24) & 15] - 1; break;
        case 1: write16(g, a & ~1u, g->r[rd]); break;
        case 2: write8(g, a, g->r[rd]); break;
        case 3: g->r[rd] = (uint32_t)(int32_t)(int8_t)read8(g, a); g->cycle++; break;
        case 4: g->r[rd] = ror32(read32(g, a & ~3u), (a & 3) * 8); g->cycle += g->c32n[(a >> 24) & 15]; break;
        case 5: g->r[rd] = ror32(read16(g, a & ~1u), (a & 1) * 8); g->cycle++; break;
        case 6: g->r[rd] = read8(g, a); g->cycle++; break;
        default:
            if (a & 1) g->r[rd] = (uint32_t)(int32_t)(int8_t)read8(g, a);
            else g->r[rd] = (uint32_t)(int32_t)(int16_t)read16(g, a);
            g->cycle++;
            break;
        }
        break;
    case 12: case 13: case 14: case 15:
        rd = op & 7; rb = (op >> 3) & 7;
        ro = (op >> 6) & 31;
        if (op & 0x1000) { /* byte */
            a = g->r[rb] + ro;
            g->cycle += g->c16n[(a >> 24) & 15];
            if (op & 0x800) { g->r[rd] = read8(g, a); g->cycle++; } else write8(g, a, g->r[rd]);
        } else {
            a = g->r[rb] + ro * 4;
            g->cycle += g->c32n[(a >> 24) & 15];
            if (op & 0x800) { g->r[rd] = ror32(read32(g, a & ~3u), (a & 3) * 8); g->cycle++; }
            else write32(g, a & ~3u, g->r[rd]);
        }
        break;
    case 16: case 17:
        rd = op & 7; rb = (op >> 3) & 7;
        a = g->r[rb] + ((op >> 6) & 31) * 2;
        g->cycle += g->c16n[(a >> 24) & 15];
        if (op & 0x800) { g->r[rd] = ror32(read16(g, a & ~1u), (a & 1) * 8); g->cycle++; }
        else write16(g, a & ~1u, g->r[rd]);
        break;
    case 18: case 19:
        rd = (op >> 8) & 7;
        a = g->r[13] + (op & 0xFF) * 4;
        g->cycle += g->c32n[(a >> 24) & 15];
        if (op & 0x800) { g->r[rd] = ror32(read32(g, a & ~3u), (a & 3) * 8); g->cycle++; }
        else write32(g, a & ~3u, g->r[rd]);
        break;
    case 20: case 21:
        rd = (op >> 8) & 7;
        g->r[rd] = ((op & 0x800) ? g->r[13] : (g->r[15] & ~3u)) + ((op & 0xFF) << 2);
        break;
    case 22: case 23:
        if ((op & 0xFF00) == 0xB000) {
            uint32_t off = (op & 0x7F) << 2;
            g->r[13] += (op & 0x80) ? (uint32_t)(-(int32_t)off) : off;
        } else if ((op & 0x0600) == 0x0400) {
            uint32_t list = op & 0xFF;
            int extra = (op >> 8) & 1, n = __builtin_popcount(list) + extra;
            g->cycle += n;
            if (op & 0x800) { /* POP */
                a = g->r[13];
                for (int i = 0; i < 8; i++) if (list & (1u << i)) { g->r[i] = read32(g, a); a += 4; }
                if (extra) { v = read32(g, a); a += 4; g->r[13] = a; pc_write_thumb(g, v); }
                else g->r[13] = a;
            } else { /* PUSH */
                a = g->r[13] - n * 4;
                g->r[13] = a;
                for (int i = 0; i < 8; i++) if (list & (1u << i)) { write32(g, a, g->r[i]); a += 4; }
                if (extra) write32(g, a, g->r[14]);
            }
        }
        break;
    case 24: case 25: {
        rb = (op >> 8) & 7;
        uint32_t list = op & 0xFF;
        uint32_t base = g->r[rb];
        int n = __builtin_popcount(list), empty = 0;
        if (!list) { list = 0x8000; n = 16; empty = 1; }
        g->cycle += n;
        a = base & ~3u;
        uint32_t nb = base + n * 4;
        if (op & 0x800) {
            g->r[rb] = nb;
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) continue;
                v = read32(g, a); a += 4;
                if (i == 15) pc_write_thumb(g, v); else g->r[i] = v;
            }
            (void)empty;
        } else {
            int first = 1;
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) continue;
                write32(g, a, i == 15 ? g->r[15] + 2 : g->r[i]); a += 4;
                if (first) g->r[rb] = nb;
                first = 0;
            }
        }
        break;
    }
    case 26: case 27: {
        uint32_t cnd = (op >> 8) & 15;
        if (cnd == 15) { bios_swi(g, op & 0xFF); break; }
        if (cnd == 14) break;
        if (cond_pass(g, cnd)) pc_write_thumb(g, g->r[15] + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2));
        break;
    }
    case 28:
        pc_write_thumb(g, g->r[15] + (uint32_t)(((int32_t)((op & 0x7FF) << 21)) >> 20));
        break;
    case 30:
        g->r[14] = g->r[15] + (uint32_t)(((int32_t)((op & 0x7FF) << 21)) >> 9);
        break;
    case 31:
        v = g->r[14] + ((op & 0x7FF) << 1);
        g->r[14] = (g->r[15] - 2) | 1;
        pc_write_thumb(g, v);
        break;
    default: break;
    }
    if (g->flushed) g->flushed = 0; else g->r[15] += 2;
}

/* ---------------- run loop ---------------- */
static void cpu_reset_state(GBA *g, uint32_t pc) {
    memset(g->r, 0, sizeof g->r);
    memset(g->spsr, 0, sizeof g->spsr);
    memset(g->bk13, 0, sizeof g->bk13);
    memset(g->bk14, 0, sizeof g->bk14);
    g->cpsr = 0x1F;
    g->fN = g->fZ = g->fC = g->fV = 0;
    g->bk13[2] = 0x03007FA0;
    g->bk13[3] = 0x03007FE0;
    g->r[13] = 0x03007F00;
    g->r[15] = pc + 8;
    g->halted = 0;
    g->flushed = 0;
    g->swi_resume = 0;
}

static void cpu_run(GBA *g, int64_t end) {
    while (g->cycle < end) {
        if (g->halted) {
            if (IO(g, R_IE) & IO(g, R_IF) & 0x3FFF) {
                g->halted = 0;
            } else {
                int64_t t = end;
                if (g->next_timer_event < t) t = g->next_timer_event;
                if (t <= g->cycle) t = g->cycle + 1;
                g->cycle = t;
                if (g->cycle >= g->next_timer_event) timers_process(g);
                continue;
            }
        }
        if (g->irq_line && !(g->cpsr & FLAG_I)) take_irq(g);
        if (g->cpsr & FLAG_T) thumb_step(g); else arm_step(g);
        if (g->cycle >= g->next_timer_event) timers_process(g);
    }
}
