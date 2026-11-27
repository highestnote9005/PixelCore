/* Scanline renderer.  Included by gba.c (unity build). */

static uint32_t color_lut[32768];
static int color_lut_ready = 0;
static void init_color_lut(void) {
    if (color_lut_ready) return;
    for (int c = 0; c < 32768; c++) {
        uint32_t r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
        r = (r << 3) | (r >> 2); gg = (gg << 3) | (gg >> 2); b = (b << 3) | (b >> 2);
        color_lut[c] = 0xFF000000u | (b << 16) | (gg << 8) | r; /* RGBA byte order on little-endian */
    }
    color_lut_ready = 1;
}

#define TRANSP 0x8000

static inline uint16_t rdh(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void render_text_bg(GBA *g, int n, int y, uint16_t *line) {
    uint16_t cnt = IO(g, 0x008 + 2 * n);
    uint32_t cbase = ((cnt >> 2) & 3) * 0x4000, sbase = ((cnt >> 8) & 31) * 0x800;
    int is8 = (cnt >> 7) & 1, size = cnt >> 14;
    uint32_t hofs = IO(g, 0x010 + 4 * n) & 0x1FF, vofs = IO(g, 0x012 + 4 * n) & 0x1FF;
    uint32_t w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    uint32_t py = (y + vofs) & (h - 1);
    uint32_t ty = py >> 3, iy = py & 7;
    int mosaic = (cnt >> 6) & 1;
    uint32_t mh = (IO(g, 0x4C) & 15) + 1;
    for (int x = 0; x < GBA_W; x++) {
        int sx = mosaic ? (x / mh) * mh : x;
        uint32_t px = (sx + hofs) & (w - 1);
        uint32_t tx = px >> 3;
        uint32_t sb = sbase + (tx >= 32 ? 0x800 : 0) + (ty >= 32 ? (size == 3 ? 0x1000 : 0x800) : 0);
        uint32_t ea = sb + (((ty & 31) * 32) + (tx & 31)) * 2;
        if (ea >= 0x18000) continue;
        uint16_t e = rdh(g->vram + ea);
        uint32_t tile = e & 0x3FF, ix = px & 7, iyy = iy;
        if (e & 0x400) ix = 7 - ix;
        if (e & 0x800) iyy = 7 - iyy;
        uint32_t c;
        if (!is8) {
            uint32_t a = cbase + tile * 32 + iyy * 4 + (ix >> 1);
            if (a >= 0x18000) continue;
            uint8_t b = g->vram[a];
            c = (ix & 1) ? b >> 4 : b & 15;
            if (c) line[x] = rdh(g->pal + ((e >> 12) * 16 + c) * 2) & 0x7FFF;
        } else {
            uint32_t a = cbase + tile * 64 + iyy * 8 + ix;
            if (a >= 0x18000) continue;
            c = g->vram[a];
            if (c) line[x] = rdh(g->pal + c * 2) & 0x7FFF;
        }
    }
}

static void render_affine_bg(GBA *g, int n, int y, uint16_t *line) {
    (void)y;
    uint16_t cnt = IO(g, 0x008 + 2 * n);
    uint32_t cbase = ((cnt >> 2) & 3) * 0x4000, sbase = ((cnt >> 8) & 31) * 0x800;
    int wrap = (cnt >> 13) & 1, size = cnt >> 14;
    int32_t dim = 128 << size;
    int32_t pa = (int16_t)IO(g, 0x20 + (n - 2) * 0x10), pc = (int16_t)IO(g, 0x24 + (n - 2) * 0x10);
    int32_t rx = g->bgx[n - 2], ry = g->bgy[n - 2];
    for (int x = 0; x < GBA_W; x++) {
        int32_t u = (rx + pa * x) >> 8, v = (ry + pc * x) >> 8;
        if (wrap) { u &= dim - 1; v &= dim - 1; }
        else if (u < 0 || v < 0 || u >= dim || v >= dim) continue;
        uint32_t ta = sbase + (uint32_t)(v >> 3) * (uint32_t)(dim >> 3) + (uint32_t)(u >> 3);
        if (ta >= 0x18000) continue;
        uint32_t tile = g->vram[ta];
        uint32_t a = cbase + tile * 64 + (v & 7) * 8 + (u & 7);
        if (a >= 0x18000) continue;
        uint32_t c = g->vram[a];
        if (c) line[x] = rdh(g->pal + c * 2) & 0x7FFF;
    }
}

static void render_bitmap_bg(GBA *g, int mode, int y, uint16_t *line) {
    (void)y;
    int32_t pa = (int16_t)IO(g, 0x20), pc = (int16_t)IO(g, 0x24);
    int32_t rx = g->bgx[0], ry = g->bgy[0];
    uint32_t fr = ((IO(g, 0) >> 4) & 1) ? 0xA000 : 0;
    int32_t w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
    for (int x = 0; x < GBA_W; x++) {
        int32_t u = (rx + pa * x) >> 8, v = (ry + pc * x) >> 8;
        if (u < 0 || v < 0 || u >= w || v >= h) continue;
        if (mode == 3) {
            line[x] = rdh(g->vram + (v * 240 + u) * 2) & 0x7FFF;
        } else if (mode == 4) {
            uint32_t idx = g->vram[fr + v * 240 + u];
            if (idx) line[x] = rdh(g->pal + idx * 2) & 0x7FFF;
        } else {
            line[x] = rdh(g->vram + fr + (v * 160 + u) * 2) & 0x7FFF;
        }
    }
}

static void render_sprites(GBA *g, int y, uint16_t *oc, uint8_t *op, uint8_t *of) {
    static const uint8_t W[4][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {0, 0, 0, 0}};
    static const uint8_t H[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {0, 0, 0, 0}};
    uint16_t dispcnt = IO(g, 0);
    int mode = dispcnt & 7, map1d = (dispcnt >> 6) & 1;
    const uint8_t *pal = g->pal + 512;
    for (int i = 0; i < GBA_W; i++) { oc[i] = TRANSP; op[i] = 4; of[i] = 0; }
    for (int i = 0; i < 128; i++) {
        const uint8_t *o = g->oam + i * 8;
        uint16_t a0 = rdh(o), a1 = rdh(o + 2), a2 = rdh(o + 4);
        int affine = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1;
        if (!affine && dbl) continue;
        int omode = (a0 >> 10) & 3;
        if (omode == 3) continue;
        int shape = a0 >> 14, sz = a1 >> 14;
        int w = W[shape][sz], h = H[shape][sz];
        if (!w) continue;
        int bw = w, bh = h;
        if (affine && dbl) { bw *= 2; bh *= 2; }
        int ly = (y - (a0 & 0xFF)) & 0xFF;
        if (ly >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox & 0x100) ox -= 0x200;
        if (ox + bw <= 0 || ox >= GBA_W) continue;
        int is8 = (a0 >> 13) & 1, prio = (a2 >> 10) & 3, tile = a2 & 0x3FF, pb = a2 >> 12;
        if (mode >= 3 && tile < 512) continue;
        uint32_t stride = map1d ? (uint32_t)(w / 8) * (is8 ? 64 : 32) : 1024;
        uint32_t tstep = is8 ? 64 : 32;
        uint32_t base = 0x10000 + (uint32_t)tile * 32;
        int32_t pa = 0, pbm = 0, pc = 0, pd = 0;
        if (affine) {
            const uint8_t *pp = g->oam + ((a1 >> 9) & 31) * 32;
            pa = (int16_t)rdh(pp + 6); pbm = (int16_t)rdh(pp + 14);
            pc = (int16_t)rdh(pp + 22); pd = (int16_t)rdh(pp + 30);
        }
        for (int sx = 0; sx < bw; sx++) {
            int x = ox + sx;
            if (x < 0 || x >= GBA_W) continue;
            int tx, ty;
            if (affine) {
                int dx = sx - bw / 2, dy = ly - bh / 2;
                tx = ((pa * dx + pbm * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = sx; ty = ly;
                if (a1 & 0x1000) tx = w - 1 - tx;
                if (a1 & 0x2000) ty = h - 1 - ty;
            }
            uint32_t a = base + (uint32_t)(ty >> 3) * stride + (uint32_t)(tx >> 3) * tstep + (uint32_t)(ty & 7) * (is8 ? 8 : 4);
            uint32_t c;
            if (is8) { a += tx & 7; if (a >= 0x18000) continue; c = g->vram[a]; }
            else { a += (tx & 7) >> 1; if (a >= 0x18000) continue; uint8_t b = g->vram[a]; c = (tx & 1) ? b >> 4 : b & 15; }
            if (!c) continue;
            if (omode == 2) { of[x] |= 2; continue; }
            if ((oc[x] & TRANSP) || prio < op[x]) {
                oc[x] = rdh(pal + (is8 ? c : pb * 16 + c) * 2) & 0x7FFF;
                op[x] = (uint8_t)prio;
                if (omode == 1) of[x] |= 1; else of[x] &= (uint8_t)~1;
            }
        }
    }
}

static inline int in_range(int v, int a, int b, int limit) {
    if (b > limit) b = limit;
    if (a <= b) return v >= a && v < b;
    return v >= a || v < b;
}

static inline uint16_t blend_alpha(uint16_t c1, uint16_t c2, int eva, int evb) {
    int r = ((c1 & 31) * eva + (c2 & 31) * evb) >> 4;
    int gg = (((c1 >> 5) & 31) * eva + ((c2 >> 5) & 31) * evb) >> 4;
    int b = (((c1 >> 10) & 31) * eva + ((c2 >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (gg > 31) gg = 31; if (b > 31) b = 31;
    return (uint16_t)(r | (gg << 5) | (b << 10));
}
static inline uint16_t blend_bright(uint16_t c, int evy, int dark) {
    int r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
    if (dark) { r -= (r * evy) >> 4; gg -= (gg * evy) >> 4; b -= (b * evy) >> 4; }
    else { r += ((31 - r) * evy) >> 4; gg += ((31 - gg) * evy) >> 4; b += ((31 - b) * evy) >> 4; }
    return (uint16_t)(r | (gg << 5) | (b << 10));
}

static void render_line(GBA *g, int y) {
    uint32_t *out = &g->video[y * GBA_W];
    uint16_t dispcnt = IO(g, 0);
    if (dispcnt & 0x80) { for (int x = 0; x < GBA_W; x++) out[x] = 0xFFFFFFFFu; return; }

    uint16_t bg[4][GBA_W];
    uint16_t oc[GBA_W];
    uint8_t op[GBA_W], of[GBA_W];
    int mode = dispcnt & 7;
    int bgon[4] = {0, 0, 0, 0};
    for (int n = 0; n < 4; n++) if ((dispcnt >> (8 + n)) & 1) bgon[n] = 1;
    if (mode == 1) bgon[3] = 0;
    if (mode == 2) bgon[0] = bgon[1] = 0;
    if (mode >= 3) bgon[0] = bgon[1] = bgon[3] = 0;

    for (int n = 0; n < 4; n++) {
        if (!bgon[n]) continue;
        for (int x = 0; x < GBA_W; x++) bg[n][x] = TRANSP;
        if (mode >= 3) render_bitmap_bg(g, mode, y, bg[n]);
        else if (mode == 0 || (mode == 1 && n < 2)) render_text_bg(g, n, y, bg[n]);
        else render_affine_bg(g, n, y, bg[n]);
    }
    int objon = (dispcnt >> 12) & 1;
    if (objon || ((dispcnt >> 15) & 1)) render_sprites(g, y, oc, op, of);
    else for (int x = 0; x < GBA_W; x++) { oc[x] = TRANSP; op[x] = 4; of[x] = 0; }

    int win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, objw = (dispcnt >> 15) & 1;
    int anywin = win0 | win1 | objw;
    uint16_t w0h = IO(g, 0x40), w1h = IO(g, 0x42), w0v = IO(g, 0x44), w1v = IO(g, 0x46);
    uint16_t winin = IO(g, 0x48), winout = IO(g, 0x4A);
    int w0y = win0 && in_range(y, w0v >> 8, w0v & 0xFF, 160);
    int w1y = win1 && in_range(y, w1v >> 8, w1v & 0xFF, 160);
    uint16_t bldcnt = IO(g, 0x50), bldal = IO(g, 0x52);
    int eff = (bldcnt >> 6) & 3;
    int eva = bldal & 31, evb = (bldal >> 8) & 31, evy = IO(g, 0x54) & 31;
    if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (evy > 16) evy = 16;
    int bgprio[4];
    for (int n = 0; n < 4; n++) bgprio[n] = IO(g, 0x08 + 2 * n) & 3;
    uint16_t backdrop = rdh(g->pal) & 0x7FFF;

    for (int x = 0; x < GBA_W; x++) {
        uint8_t ctl = 0x3F;
        if (anywin) {
            if (w0y && in_range(x, w0h >> 8, w0h & 0xFF, 240)) ctl = winin & 0xFF;
            else if (w1y && in_range(x, w1h >> 8, w1h & 0xFF, 240)) ctl = (winin >> 8) & 0xFF;
            else if (objw && (of[x] & 2)) ctl = (winout >> 8) & 0xFF;
            else ctl = winout & 0xFF;
        }
        int top = 5, second = 5;
        uint16_t c1 = backdrop, c2 = backdrop;
        int found = 0;
        for (int p = 0; p < 4 && found < 2; p++) {
            if (objon && (ctl & 0x10) && !(oc[x] & TRANSP) && op[x] == p) {
                if (!found) { top = 4; c1 = oc[x]; } else { second = 4; c2 = oc[x]; }
                found++;
                if (found >= 2) break;
            }
            for (int n = 0; n < 4 && found < 2; n++) {
                if (bgon[n] && (ctl & (1 << n)) && bgprio[n] == p && !(bg[n][x] & TRANSP)) {
                    if (!found) { top = n; c1 = bg[n][x]; } else { second = n; c2 = bg[n][x]; }
                    found++;
                }
            }
        }
        uint16_t c = c1;
        if (ctl & 0x20) {
            int semi = (top == 4) && (of[x] & 1);
            if (semi && ((bldcnt >> (8 + second)) & 1)) c = blend_alpha(c1, c2, eva, evb);
            else if (eff == 1 && ((bldcnt >> top) & 1) && ((bldcnt >> (8 + second)) & 1)) c = blend_alpha(c1, c2, eva, evb);
            else if (eff == 2 && ((bldcnt >> top) & 1)) c = blend_bright(c1, evy, 0);
            else if (eff == 3 && ((bldcnt >> top) & 1)) c = blend_bright(c1, evy, 1);
        }
        out[x] = color_lut[c & 0x7FFF];
    }
}
