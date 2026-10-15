// license:GPL-2.0+
// copyright-holders:Jarek Burczynski,Tatsuyuki Satoh
/*
 * Yamaha YM2203 (OPN): three FM channels of four operators, and an SSG.
 *
 * The FM part is MAME's fm.cpp (MAME 0.229, the last before ymfm), its YM2203
 * path only, in plain C: no timers (1943 wires neither the IRQ nor reads the
 * status), no LFO (the YM2203 has none), no CSM. The SSG part is ym_ssg.c
 * (ymfm's), clocked as fm.cpp's prescaler says (clock * 2 / 4 by default).
 *
 * FM is rendered at its own rate (fm_rate), which need not be the chip's
 * clock / 72: fm.cpp scales the phase, envelope and detune steps to it.
 */
#ifndef YM2203_H
#define YM2203_H

#include <stdint.h>

#include "ym_ssg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const int32_t *DT; /* detune: the chip's dt_tab row */
    uint8_t KSR;       /* key scale rate: 3 - KS */
    uint32_t ar, d1r, d2r, rr;
    uint8_t ksr; /* key scale rate: kcode >> KSR */
    uint32_t mul;

    uint32_t phase;
    int32_t Incr;

    uint8_t state;
    uint32_t tl;
    int32_t volume;
    uint32_t sl;
    uint32_t vol_out;

    uint8_t eg_sh_ar, eg_sel_ar, eg_sh_d1r, eg_sel_d1r, eg_sh_d2r, eg_sel_d2r, eg_sh_rr, eg_sel_rr;
    uint8_t ssg, ssgn;
    uint32_t key;
} ym2203_slot_t;

typedef struct
{
    ym2203_slot_t SLOT[4];
    uint8_t ALGO, FB;
    int32_t op1_out[2];
    int32_t *connect1, *connect3, *connect2, *connect4;
    int32_t *mem_connect;
    int32_t mem_value;
    uint32_t fc;
    uint8_t kcode;
    uint32_t block_fnum;
} ym2203_ch_t;

typedef struct
{
    uint32_t clock, fm_rate, ssg_rate;
    double freqbase;
    uint32_t fn_k;           /* fn_table[i] = (i * fn_k) >> 16, see fm.cpp's OPNSetPres */
    uint32_t fn_max;
    int32_t dt_tab[8][32];

    uint8_t address, mode, prescaler_sel, fn_h;
    struct
    {
        uint32_t fc[3];
        uint8_t fn_h;
        uint8_t kcode[3];
        uint32_t block_fnum[3];
    } SL3;

    uint32_t eg_cnt, eg_timer, eg_timer_add, eg_timer_overflow;
    int32_t m2, c1, c2, mem;
    int32_t out_fm[3];

    ym2203_ch_t CH[3];
    ym_ssg_t ssg;
} ym2203_t;

/* clock of the chip; fm_rate and ssg_rate are the rates the two halves are
 * rendered at */
void ym2203_init(ym2203_t *c, uint32_t clock, uint32_t fm_rate, uint32_t ssg_rate);
void ym2203_reset(ym2203_t *c);

/* a0 = 0: the address port, 1: the data port */
void ym2203_write(ym2203_t *c, int a0, uint8_t v);

/* Adds n samples of FM, times gain_q8 / 256, to acc. */
void ym2203_render_fm(ym2203_t *c, int32_t *acc, int n, int gain_q8);

/* Adds n samples of SSG to out (saturating): the three channels, each at
 * MAME's full amplitude 16382, times gain_q15 / 32768. */
static inline void ym2203_render_ssg(ym2203_t *c, int16_t *out, int n, int gain_q15)
{
    ym_ssg_render(&c->ssg, out, n, gain_q15);
}

#ifdef __cplusplus
}
#endif

#endif
