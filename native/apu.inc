/* Audio: 4 PSG channels + 2 DirectSound FIFOs, output at 32768 Hz stereo.  Included by gba.c. */

#define APU_STEP 512 /* cycles per output sample */

static const uint8_t duty_tab[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 0}};

static void fifo_push(GBA *g, int ch, uint8_t v) {
    if (g->fifo_cnt[ch] < 32) {
        g->fifo[ch][(g->fifo_rd[ch] + g->fifo_cnt[ch]) & 31] = v;
        g->fifo_cnt[ch]++;
    }
}

static void fifo_timer(GBA *g, int ch) {
    apu_sync(g);
    if (g->fifo_cnt[ch] > 0) {
        g->fifo_cur[ch] = (int8_t)g->fifo[ch][g->fifo_rd[ch]];
        g->fifo_rd[ch] = (g->fifo_rd[ch] + 1) & 31;
        g->fifo_cnt[ch]--;
    }
    if (g->fifo_cnt[ch] <= 16) dma_fifo_request(g, ch);
}

static uint32_t sweep_calc(GBA *g, GbaChan *c) {
    uint32_t d = c->sw_shadow >> c->sw_shift;
    uint32_t nf = c->sw_dir ? c->sw_shadow - d : c->sw_shadow + d;
    if (nf > 2047 && !c->sw_dir) c->on = 0;
    (void)g;
    return nf;
}

static uint32_t chan_period(int i, GbaChan *c) {
    switch (i) {
    case 0: case 1: return (2048 - c->freq) * 16;
    case 2: return (2048 - c->freq) * 8;
    default: {
        uint32_t r = c->n_div;
        return (r ? 64u * r : 32u) << c->n_shift;
    }
    }
}

static void chan_trigger(GBA *g, int i) {
    GbaChan *c = &g->ch[i];
    if (!c->dac) return;
    c->on = 1;
    if (!c->len) c->len = (i == 2) ? 256 : 64;
    c->timer = (int32_t)chan_period(i, c);
    c->env_vol = c->vol_init;
    c->env_timer = c->env_period ? c->env_period : 8;
    if (i == 0) {
        c->sw_shadow = c->freq;
        c->sw_timer = c->sw_period ? c->sw_period : 8;
        c->sw_en = (c->sw_period || c->sw_shift) ? 1 : 0;
        if (c->sw_shift) sweep_calc(g, c);
    }
    if (i == 2) c->duty_pos = 0;
    if (i == 3) c->lfsr = c->n_width ? 0x7F : 0x7FFF;
}

static void apu_write(GBA *g, uint32_t off, uint16_t v) {
    apu_sync(g);
    switch (off) {
    case 0x60: { GbaChan *c = &g->ch[0]; c->sw_period = (v >> 4) & 7; c->sw_dir = (v >> 3) & 1; c->sw_shift = v & 7; break; }
    case 0x62: case 0x68: {
        GbaChan *c = &g->ch[off == 0x62 ? 0 : 1];
        c->duty = (v >> 6) & 3; c->len = 64 - (v & 63);
        c->vol_init = v >> 12; c->env_dir = (v >> 11) & 1; c->env_period = (v >> 8) & 7;
        c->dac = (v & 0xF800) != 0;
        if (!c->dac) c->on = 0;
        break;
    }
    case 0x64: case 0x6C: {
        int i = off == 0x64 ? 0 : 1;
        GbaChan *c = &g->ch[i];
        c->freq = v & 0x7FF; c->len_en = (v >> 14) & 1;
        if (v & 0x8000) chan_trigger(g, i);
        break;
    }
    case 0x70: {
        GbaChan *c = &g->ch[2];
        c->w_dim = (v >> 5) & 1; c->w_bank = (v >> 6) & 1; c->dac = (v >> 7) & 1;
        if (!c->dac) c->on = 0;
        break;
    }
    case 0x72: { GbaChan *c = &g->ch[2]; c->len = 256 - (v & 255); c->w_vol = (v >> 13) & 3; c->w_force75 = (v >> 15) & 1; break; }
    case 0x74: {
        GbaChan *c = &g->ch[2];
        c->freq = v & 0x7FF; c->len_en = (v >> 14) & 1;
        if (v & 0x8000) chan_trigger(g, 2);
        break;
    }
    case 0x78: {
        GbaChan *c = &g->ch[3];
        c->len = 64 - (v & 63);
        c->vol_init = v >> 12; c->env_dir = (v >> 11) & 1; c->env_period = (v >> 8) & 7;
        c->dac = (v & 0xF800) != 0;
        if (!c->dac) c->on = 0;
        break;
    }
    case 0x7C: {
        GbaChan *c = &g->ch[3];
        c->n_div = v & 7; c->n_width = (v >> 3) & 1; c->n_shift = (v >> 4) & 15; c->len_en = (v >> 14) & 1;
        if (v & 0x8000) chan_trigger(g, 3);
        break;
    }
    case 0x84:
        if (!(v & 0x80)) for (int i = 0; i < 4; i++) g->ch[i].on = 0;
        break;
    default: break;
    }
}

static void frame_seq(GBA *g) {
    int s = g->fs_step;
    if ((s & 1) == 0) { /* length, 256 Hz */
        for (int i = 0; i < 4; i++) {
            GbaChan *c = &g->ch[i];
            if (c->len_en && c->len) { if (--c->len == 0) c->on = 0; }
        }
    }
    if (s == 2 || s == 6) { /* sweep, 128 Hz */
        GbaChan *c = &g->ch[0];
        if (c->sw_en && c->sw_timer && --c->sw_timer == 0) {
            c->sw_timer = c->sw_period ? c->sw_period : 8;
            if (c->sw_period) {
                uint32_t nf = sweep_calc(g, c);
                if (nf <= 2047 && c->sw_shift) {
                    c->sw_shadow = (uint16_t)nf;
                    c->freq = (uint16_t)nf;
                    sweep_calc(g, c);
                }
            }
        }
    }
    if (s == 7) { /* envelope, 64 Hz */
        for (int i = 0; i < 4; i++) {
            if (i == 2) continue;
            GbaChan *c = &g->ch[i];
            if (c->env_period && c->env_timer && --c->env_timer == 0) {
                c->env_timer = c->env_period;
                if (c->env_dir) { if (c->env_vol < 15) c->env_vol++; }
                else if (c->env_vol > 0) c->env_vol--;
            }
        }
    }
    g->fs_step = (uint8_t)((s + 1) & 7);
}

static void apu_sample(GBA *g) {
    /* advance PSG channels */
    for (int i = 0; i < 2; i++) {
        GbaChan *c = &g->ch[i];
        if (!c->on) continue;
        c->timer -= APU_STEP;
        while (c->timer <= 0) { c->timer += (int32_t)chan_period(i, c); c->duty_pos = (c->duty_pos + 1) & 7; }
    }
    {
        GbaChan *c = &g->ch[2];
        if (c->on) {
            c->timer -= APU_STEP;
            uint32_t per = chan_period(2, c);
            while (c->timer <= 0) { c->timer += (int32_t)per; c->duty_pos = (c->duty_pos + 1) & (c->w_dim ? 63 : 31); }
        }
        c = &g->ch[3];
        if (c->on) {
            c->timer -= APU_STEP;
            uint32_t per = chan_period(3, c);
            while (c->timer <= 0) {
                c->timer += (int32_t)per;
                uint16_t x = (uint16_t)((c->lfsr & 1) ^ ((c->lfsr >> 1) & 1));
                c->lfsr = (uint16_t)((c->lfsr >> 1) | (x << (c->n_width ? 6 : 14)));
            }
        }
    }
    if (++g->fs_count >= 64) { g->fs_count = 0; frame_seq(g); }

    uint16_t scl = IO(g, 0x80), sch = IO(g, 0x82);
    int master = (IO(g, 0x84) >> 7) & 1;
    int psgL = 0, psgR = 0;
    for (int i = 0; i < 4; i++) {
        GbaChan *c = &g->ch[i];
        if (!c->on) continue;
        int o = 0;
        if (i < 2) o = duty_tab[c->duty][c->duty_pos] ? c->env_vol : 0;
        else if (i == 2) {
            int pos = c->duty_pos, bank = c->w_dim ? (pos >> 5) : c->w_bank, idx = pos & 31;
            uint8_t b = g->wave_ram[bank][idx >> 1];
            int s = (idx & 1) ? (b & 15) : (b >> 4);
            if (c->w_force75) s = s * 3 / 4;
            else switch (c->w_vol) { case 0: s = 0; break; case 2: s >>= 1; break; case 3: s >>= 2; break; default: break; }
            o = s;
        } else {
            o = (c->lfsr & 1) ? 0 : c->env_vol;
        }
        if ((scl >> (8 + i)) & 1) psgR += o;
        if ((scl >> (12 + i)) & 1) psgL += o;
    }
    static const int psgmul[4] = {1, 2, 4, 4};
    int pm = psgmul[sch & 3];
    int L = psgL * (((scl >> 4) & 7) + 1) * pm / 4;
    int R = psgR * ((scl & 7) + 1) * pm / 4;
    for (int f = 0; f < 2; f++) {
        int sh = f ? 12 : 8;
        int vol = (sch >> (f ? 3 : 2)) & 1;
        int s = g->fifo_cur[f] * (vol ? 4 : 2);
        if ((sch >> sh) & 1) R += s;
        if ((sch >> (sh + 1)) & 1) L += s;
    }
    if (!master) { L = R = 0; }
    if (L > 511) L = 511; if (L < -512) L = -512;
    if (R > 511) R = 511; if (R < -512) R = -512;
    /* DC-blocking high-pass (AC coupling) */
    int32_t in[2] = {L * 56, R * 56};
    for (int k = 0; k < 2; k++) {
        g->hp_y[k] = in[k] - g->hp_x[k] + g->hp_y[k] - (g->hp_y[k] >> 10);
        g->hp_x[k] = in[k];
    }
    int32_t oL = g->hp_y[0], oR = g->hp_y[1];
    if (oL > 32767) oL = 32767; if (oL < -32768) oL = -32768;
    if (oR > 32767) oR = 32767; if (oR < -32768) oR = -32768;
    uint32_t w = g->a_w & (GBA_AUDIO_CAP - 1);
    g->abuf[w * 2] = (int16_t)oL;
    g->abuf[w * 2 + 1] = (int16_t)oR;
    g->a_w++;
    if (g->a_w - g->a_r > GBA_AUDIO_CAP) g->a_r = g->a_w - GBA_AUDIO_CAP;
}

static void apu_sync(GBA *g) {
    while (g->audio_cycle + APU_STEP <= g->cycle) {
        g->audio_cycle += APU_STEP;
        apu_sample(g);
    }
}
