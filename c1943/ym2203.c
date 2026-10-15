// license:GPL-2.0+
// copyright-holders:Jarek Burczynski,Tatsuyuki Satoh
/*
 * Yamaha YM2203 (OPN), the FM part: MAME's fm.cpp as of MAME 0.229 (version
 * 1.4.2, with the Genesis Plus GX envelope fixes by Eke-Eke), its YM2203 path
 * only, ported to plain C. See ym2203.h for what is left out.
 *
 * Differences from fm.cpp, none of them audible:
 *   - tl_tab and sin_tab are int16/uint16 (every value fits), and shared by
 *     the chips;
 *   - fn_table, 4096 uint32 per chip in fm.cpp, is computed as needed from a
 *     16.16 factor: only register writes read it;
 *   - the LFO, the timers, CSM and the status register are gone.
 */
#include "ym2203.h"

#include <math.h>
#include <string.h>

#include "c1943_port.h"

#define FREQ_SH 16 /* 16.16 fixed point (frequency calculations) */
#define EG_SH 16   /* 16.16 fixed point (envelope generator timing) */
#define FREQ_MASK ((1 << FREQ_SH) - 1)

#define ENV_BITS 10
#define ENV_LEN (1 << ENV_BITS)
#define ENV_STEP (128.0 / ENV_LEN)

#define MAX_ATT_INDEX (ENV_LEN - 1) /* 1023 */
#define MIN_ATT_INDEX (0)

#define EG_ATT 4
#define EG_DEC 3
#define EG_SUS 2
#define EG_REL 1
#define EG_OFF 0

#define SIN_BITS 10
#define SIN_LEN (1 << SIN_BITS)
#define SIN_MASK (SIN_LEN - 1)

#define TL_RES_LEN (256)
#define TL_TAB_LEN (13 * 2 * TL_RES_LEN)
#define ENV_QUIET (TL_TAB_LEN >> 3)

#define MAXOUT (+32767)
#define MINOUT (-32768)

/* The tables are in SRAM (C1943_RAM_DATA): they are read for every sample, on
 * core1 on HSTX boards, where a read from flash waits for the XIP cache that
 * core0 shares (c1943_port.h). */
static int16_t tl_tab[TL_TAB_LEN];
static uint16_t sin_tab[SIN_LEN];
static int tables_ready;

/* sustain level table (3dB per step) */
#define SC(db) (uint32_t)(db * (4.0 / ENV_STEP))
static uint32_t C1943_RAM_DATA sl_table[16] = {SC(0), SC(1), SC(2),  SC(3),  SC(4),  SC(5),  SC(6),  SC(7),
                                      SC(8), SC(9), SC(10), SC(11), SC(12), SC(13), SC(14), SC(31)};
#undef SC

#define RATE_STEPS (8)
static uint8_t C1943_RAM_DATA eg_inc[19 * RATE_STEPS] = {
    /*cycle:0 1  2 3  4 5  6 7*/
    /* 0 */ 0,  1,  0,  1,  0,  1,  0,  1,  /* rates 00..11 0 (increment by 0 or 1) */
    /* 1 */ 0,  1,  0,  1,  1,  1,  0,  1,  /* rates 00..11 1 */
    /* 2 */ 0,  1,  1,  1,  0,  1,  1,  1,  /* rates 00..11 2 */
    /* 3 */ 0,  1,  1,  1,  1,  1,  1,  1,  /* rates 00..11 3 */
    /* 4 */ 1,  1,  1,  1,  1,  1,  1,  1,  /* rate 12 0 (increment by 1) */
    /* 5 */ 1,  1,  1,  2,  1,  1,  1,  2,  /* rate 12 1 */
    /* 6 */ 1,  2,  1,  2,  1,  2,  1,  2,  /* rate 12 2 */
    /* 7 */ 1,  2,  2,  2,  1,  2,  2,  2,  /* rate 12 3 */
    /* 8 */ 2,  2,  2,  2,  2,  2,  2,  2,  /* rate 13 0 (increment by 2) */
    /* 9 */ 2,  2,  2,  4,  2,  2,  2,  4,  /* rate 13 1 */
    /*10 */ 2,  4,  2,  4,  2,  4,  2,  4,  /* rate 13 2 */
    /*11 */ 2,  4,  4,  4,  2,  4,  4,  4,  /* rate 13 3 */
    /*12 */ 4,  4,  4,  4,  4,  4,  4,  4,  /* rate 14 0 (increment by 4) */
    /*13 */ 4,  4,  4,  8,  4,  4,  4,  8,  /* rate 14 1 */
    /*14 */ 4,  8,  4,  8,  4,  8,  4,  8,  /* rate 14 2 */
    /*15 */ 4,  8,  8,  8,  4,  8,  8,  8,  /* rate 14 3 */
    /*16 */ 8,  8,  8,  8,  8,  8,  8,  8,  /* rates 15 0, 15 1, 15 2, 15 3 (increment by 8) */
    /*17 */ 16, 16, 16, 16, 16, 16, 16, 16, /* rates 15 2, 15 3 for attack */
    /*18 */ 0,  0,  0,  0,  0,  0,  0,  0,  /* infinity rates for attack and decay(s) */
};

#define O(a) (a * RATE_STEPS)
static uint8_t C1943_RAM_DATA eg_rate_select[32 + 64 + 32] = {
    /* 32 infinite time rates */
    O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
    O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
    /* rates 00-11 */
    O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1),
    O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3), O(0), O(1), O(2), O(3),
    /* rate 12 */
    O(4), O(5), O(6), O(7),
    /* rate 13 */
    O(8), O(9), O(10), O(11),
    /* rate 14 */
    O(12), O(13), O(14), O(15),
    /* rate 15 */
    O(16), O(16), O(16), O(16),
    /* 32 dummy rates (same as 15 3) */
    O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16),
    O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16)};
#undef O

static uint8_t C1943_RAM_DATA eg_rate_shift[32 + 64 + 32] = {
    /* 32 infinite time rates */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* rates 00-11 */
    11, 11, 11, 11, 10, 10, 10, 10, 9, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3,
    3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0,
    /* rate 12 */
    0, 0, 0, 0,
    /* rate 13 */
    0, 0, 0, 0,
    /* rate 14 */
    0, 0, 0, 0,
    /* rate 15 */
    0, 0, 0, 0,
    /* 32 dummy rates (same as 15 3) */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static uint8_t C1943_RAM_DATA dt_tab[4 * 32] = {
    /* this is YM2151 and YM2612 phase increment data (in 10.10 fixed point format)*/
    /* FD=0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* FD=1 */
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8,
    /* FD=2 */
    1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16,
    /* FD=3 */
    2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22};

/* OPN key frequency number -> key code follow table */
static uint8_t C1943_RAM_DATA opn_fktable[16] = {0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3};

#define OPN_CHAN(N) (N & 3)
#define OPN_SLOT(N) ((N >> 2) & 3)

#define SLOT1 0
#define SLOT2 2
#define SLOT3 1
#define SLOT4 3

static void init_tables(void)
{
    if (tables_ready)
        return;
    for (int x = 0; x < TL_RES_LEN; x++)
    {
        double m = (1 << 16) / pow(2, (x + 1) * (ENV_STEP / 4.0) / 8.0);
        m = floor(m);
        int n = (int)m; /* 16 bits here */
        n >>= 4;        /* 12 bits here */
        if (n & 1)      /* round to nearest */
            n = (n >> 1) + 1;
        else
            n = n >> 1;
        n <<= 2; /* 13 bits here (as in real chip) */
        tl_tab[x * 2 + 0] = (int16_t)n;
        tl_tab[x * 2 + 1] = (int16_t)-n;
        for (int i = 1; i < 13; i++)
        {
            tl_tab[x * 2 + 0 + i * 2 * TL_RES_LEN] = (int16_t)(n >> i);
            tl_tab[x * 2 + 1 + i * 2 * TL_RES_LEN] = (int16_t)-(n >> i);
        }
    }
    for (int i = 0; i < SIN_LEN; i++)
    {
        /* non-standard sinus */
        double m = sin(((i * 2) + 1) * M_PI / SIN_LEN); /* checked against the real chip */
        double o;
        if (m > 0.0)
            o = 8 * log(1.0 / m) / log(2.0); /* convert to 'decibels' */
        else
            o = 8 * log(-1.0 / m) / log(2.0);
        o = o / (ENV_STEP / 4);
        int n = (int)(2.0 * o);
        if (n & 1) /* round to nearest */
            n = (n >> 1) + 1;
        else
            n = n >> 1;
        sin_tab[i] = (uint16_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }
    tables_ready = 1;
}

static inline void FM_KEYON(ym2203_ch_t *CH, int s)
{
    ym2203_slot_t *SLOT = &CH->SLOT[s];
    if (!SLOT->key)
    {
        SLOT->key = 1;
        SLOT->phase = 0; /* restart Phase Generator */
        SLOT->ssgn = (SLOT->ssg & 0x04) >> 1;
        SLOT->state = EG_ATT;
    }
}

static inline void FM_KEYOFF(ym2203_ch_t *CH, int s)
{
    ym2203_slot_t *SLOT = &CH->SLOT[s];
    if (SLOT->key)
    {
        SLOT->key = 0;
        if (SLOT->state > EG_REL)
            SLOT->state = EG_REL; /* phase -> Release */
    }
}

static void setup_connection(ym2203_t *OPN, ym2203_ch_t *CH, int ch)
{
    int32_t *carrier = &OPN->out_fm[ch];
    int32_t **om1 = &CH->connect1;
    int32_t **om2 = &CH->connect3;
    int32_t **oc1 = &CH->connect2;
    int32_t **memc = &CH->mem_connect;

    switch (CH->ALGO)
    {
    case 0:
        /* M1---C1---MEM---M2---C2---OUT */
        *om1 = &OPN->c1;
        *oc1 = &OPN->mem;
        *om2 = &OPN->c2;
        *memc = &OPN->m2;
        break;
    case 1:
        /* M1------+-MEM---M2---C2---OUT */
        /*      C1-+                     */
        *om1 = &OPN->mem;
        *oc1 = &OPN->mem;
        *om2 = &OPN->c2;
        *memc = &OPN->m2;
        break;
    case 2:
        /* M1-----------------+-C2---OUT */
        /*      C1---MEM---M2-+          */
        *om1 = &OPN->c2;
        *oc1 = &OPN->mem;
        *om2 = &OPN->c2;
        *memc = &OPN->m2;
        break;
    case 3:
        /* M1---C1---MEM------+-C2---OUT */
        /*                 M2-+          */
        *om1 = &OPN->c1;
        *oc1 = &OPN->mem;
        *om2 = &OPN->c2;
        *memc = &OPN->c2;
        break;
    case 4:
        /* M1---C1-+-OUT */
        /* M2---C2-+     */
        /* MEM: not used */
        *om1 = &OPN->c1;
        *oc1 = carrier;
        *om2 = &OPN->c2;
        *memc = &OPN->mem; /* store it anywhere where it will not be used */
        break;
    case 5:
        /*    +----C1----+     */
        /* M1-+-MEM---M2-+-OUT */
        /*    +----C2----+     */
        *om1 = NULL; /* special mark */
        *oc1 = carrier;
        *om2 = carrier;
        *memc = &OPN->m2;
        break;
    case 6:
        /* M1---C1-+     */
        /*      M2-+-OUT */
        /*      C2-+     */
        /* MEM: not used */
        *om1 = &OPN->c1;
        *oc1 = carrier;
        *om2 = carrier;
        *memc = &OPN->mem;
        break;
    case 7:
        /* M1-+     */
        /* C1-+-OUT */
        /* M2-+     */
        /* C2-+     */
        /* MEM: not used*/
        *om1 = carrier;
        *oc1 = carrier;
        *om2 = carrier;
        *memc = &OPN->mem;
        break;
    }
    CH->connect4 = carrier;
}

static inline void set_det_mul(ym2203_t *OPN, ym2203_ch_t *CH, ym2203_slot_t *SLOT, int v)
{
    SLOT->mul = (v & 0x0f) ? (v & 0x0f) * 2 : 1;
    SLOT->DT = OPN->dt_tab[(v >> 4) & 7];
    CH->SLOT[SLOT1].Incr = -1;
}

static inline void set_tl(ym2203_slot_t *SLOT, int v)
{
    SLOT->tl = (v & 0x7f) << (ENV_BITS - 7); /* 7bit TL */
}

static inline void set_ar_ksr(ym2203_ch_t *CH, ym2203_slot_t *SLOT, int v)
{
    uint8_t old_KSR = SLOT->KSR;
    SLOT->ar = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;
    SLOT->KSR = 3 - (v >> 6);
    if (SLOT->KSR != old_KSR)
        CH->SLOT[SLOT1].Incr = -1;

    /* refresh Attack rate */
    if ((SLOT->ar + SLOT->ksr) < 32 + 62)
    {
        SLOT->eg_sh_ar = eg_rate_shift[SLOT->ar + SLOT->ksr];
        SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
    }
    else
    {
        SLOT->eg_sh_ar = 0;
        SLOT->eg_sel_ar = 17 * RATE_STEPS;
    }
}

static inline void set_dr(ym2203_slot_t *SLOT, int v)
{
    SLOT->d1r = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;
    SLOT->eg_sh_d1r = eg_rate_shift[SLOT->d1r + SLOT->ksr];
    SLOT->eg_sel_d1r = eg_rate_select[SLOT->d1r + SLOT->ksr];
}

static inline void set_sr(ym2203_slot_t *SLOT, int v)
{
    SLOT->d2r = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;
    SLOT->eg_sh_d2r = eg_rate_shift[SLOT->d2r + SLOT->ksr];
    SLOT->eg_sel_d2r = eg_rate_select[SLOT->d2r + SLOT->ksr];
}

static inline void set_sl_rr(ym2203_slot_t *SLOT, int v)
{
    SLOT->sl = sl_table[v >> 4];
    SLOT->rr = 34 + ((v & 0x0f) << 2);
    SLOT->eg_sh_rr = eg_rate_shift[SLOT->rr + SLOT->ksr];
    SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
}

static inline int32_t op_calc(uint32_t phase, unsigned int env, int32_t pm)
{
    uint32_t p = (env << 3) + sin_tab[(((int32_t)((phase & ~FREQ_MASK) + ((uint32_t)pm << 15))) >> FREQ_SH) & SIN_MASK];
    if (p >= TL_TAB_LEN)
        return 0;
    return tl_tab[p];
}

static inline int32_t op_calc1(uint32_t phase, unsigned int env, int32_t pm)
{
    uint32_t p = (env << 3) + sin_tab[(((int32_t)((phase & ~FREQ_MASK) + (uint32_t)pm)) >> FREQ_SH) & SIN_MASK];
    if (p >= TL_TAB_LEN)
        return 0;
    return tl_tab[p];
}

static void C1943_HOT(advance_eg_channel)(ym2203_t *OPN, ym2203_slot_t *SLOT)
{
    for (int i = 0; i < 4; i++, SLOT++)
    {
        unsigned int swap_flag = 0; /* SSG-EG swap flag */

        switch (SLOT->state)
        {
        case EG_ATT: /* attack phase */
            if (!(OPN->eg_cnt & ((1 << SLOT->eg_sh_ar) - 1)))
            {
                SLOT->volume +=
                    (~SLOT->volume * (eg_inc[SLOT->eg_sel_ar + ((OPN->eg_cnt >> SLOT->eg_sh_ar) & 7)])) >> 4;
                if (SLOT->volume <= MIN_ATT_INDEX)
                {
                    SLOT->volume = MIN_ATT_INDEX;
                    SLOT->state = EG_DEC;
                }
            }
            break;

        case EG_DEC: /* decay phase */
            if (!(OPN->eg_cnt & ((1 << SLOT->eg_sh_d1r) - 1)))
            {
                const int inc = eg_inc[SLOT->eg_sel_d1r + ((OPN->eg_cnt >> SLOT->eg_sh_d1r) & 7)];
                SLOT->volume += (SLOT->ssg & 0x08) ? 4 * inc : inc; /* SSG EG type envelope */
                if (SLOT->volume >= (int32_t)(SLOT->sl))
                    SLOT->state = EG_SUS;
            }
            break;

        case EG_SUS: /* sustain phase */
            if (SLOT->ssg & 0x08) /* SSG EG type envelope selected */
            {
                if (!(OPN->eg_cnt & ((1 << SLOT->eg_sh_d2r) - 1)))
                {
                    SLOT->volume += 4 * eg_inc[SLOT->eg_sel_d2r + ((OPN->eg_cnt >> SLOT->eg_sh_d2r) & 7)];
                    if (SLOT->volume >= ENV_QUIET)
                    {
                        SLOT->volume = MAX_ATT_INDEX;
                        if (SLOT->ssg & 0x01) /* bit 0 = hold */
                        {
                            if (!(SLOT->ssgn & 1)) /* have we swapped once ??? */
                                swap_flag = (SLOT->ssg & 0x02) | 1; /* bit 1 = alternate */
                        }
                        else
                        {
                            /* same as KEY-ON operation: restart the phase generator */
                            SLOT->phase = 0;
                            SLOT->volume = 511;
                            SLOT->state = EG_ATT;
                            swap_flag = (SLOT->ssg & 0x02); /* bit 1 = alternate */
                        }
                    }
                }
            }
            else
            {
                if (!(OPN->eg_cnt & ((1 << SLOT->eg_sh_d2r) - 1)))
                {
                    SLOT->volume += eg_inc[SLOT->eg_sel_d2r + ((OPN->eg_cnt >> SLOT->eg_sh_d2r) & 7)];
                    if (SLOT->volume >= MAX_ATT_INDEX)
                        SLOT->volume = MAX_ATT_INDEX; /* do not change SLOT->state (verified on real chip) */
                }
            }
            break;

        case EG_REL: /* release phase */
            if (!(OPN->eg_cnt & ((1 << SLOT->eg_sh_rr) - 1)))
            {
                /* SSG-EG affects Release phase also (Nemesis) */
                SLOT->volume += eg_inc[SLOT->eg_sel_rr + ((OPN->eg_cnt >> SLOT->eg_sh_rr) & 7)];
                if (SLOT->volume >= MAX_ATT_INDEX)
                {
                    SLOT->volume = MAX_ATT_INDEX;
                    SLOT->state = EG_OFF;
                }
            }
            break;
        }

        unsigned int out = (uint32_t)SLOT->volume;
        /* negate output (changes come from alternate bit, init comes from attack bit) */
        if ((SLOT->ssg & 0x08) && (SLOT->ssgn & 2) && (SLOT->state > EG_REL))
            out ^= MAX_ATT_INDEX;
        /* stored before ssgn changes below */
        SLOT->vol_out = out + SLOT->tl;
        SLOT->ssgn ^= swap_flag;
    }
}

static inline void C1943_HOT(chan_calc)(ym2203_t *OPN, ym2203_ch_t *CH)
{
    unsigned int eg_out;

    OPN->m2 = OPN->c1 = OPN->c2 = OPN->mem = 0;
    *CH->mem_connect = CH->mem_value; /* restore delayed sample (MEM) value to m2 or c2 */

    eg_out = CH->SLOT[SLOT1].vol_out;
    {
        int32_t out = CH->op1_out[0] + CH->op1_out[1];
        CH->op1_out[0] = CH->op1_out[1];

        if (!CH->connect1)
            OPN->mem = OPN->c1 = OPN->c2 = CH->op1_out[0]; /* algorithm 5 */
        else
            *CH->connect1 += CH->op1_out[0]; /* other algorithms */

        CH->op1_out[1] = 0;
        if (eg_out < ENV_QUIET) /* SLOT 1 */
        {
            if (!CH->FB)
                out = 0;
            CH->op1_out[1] = op_calc1(CH->SLOT[SLOT1].phase, eg_out, (int32_t)((uint32_t)out << CH->FB));
        }
    }

    eg_out = CH->SLOT[SLOT3].vol_out;
    if (eg_out < ENV_QUIET) /* SLOT 3 */
        *CH->connect3 += op_calc(CH->SLOT[SLOT3].phase, eg_out, OPN->m2);

    eg_out = CH->SLOT[SLOT2].vol_out;
    if (eg_out < ENV_QUIET) /* SLOT 2 */
        *CH->connect2 += op_calc(CH->SLOT[SLOT2].phase, eg_out, OPN->c1);

    eg_out = CH->SLOT[SLOT4].vol_out;
    if (eg_out < ENV_QUIET) /* SLOT 4 */
        *CH->connect4 += op_calc(CH->SLOT[SLOT4].phase, eg_out, OPN->c2);

    /* store current MEM */
    CH->mem_value = OPN->mem;

    /* update phase counters AFTER output calculations (no LFO on the YM2203) */
    CH->SLOT[SLOT1].phase += CH->SLOT[SLOT1].Incr;
    CH->SLOT[SLOT2].phase += CH->SLOT[SLOT2].Incr;
    CH->SLOT[SLOT3].phase += CH->SLOT[SLOT3].Incr;
    CH->SLOT[SLOT4].phase += CH->SLOT[SLOT4].Incr;
}

/* update phase increment and envelope generator */
static inline void refresh_fc_eg_slot(ym2203_t *OPN, ym2203_slot_t *SLOT, int fc, int kc)
{
    int ksr = kc >> SLOT->KSR;

    fc += SLOT->DT[kc];
    /* detects frequency overflow (credits to Nemesis) */
    if (fc < 0)
        fc += OPN->fn_max;
    /* (frequency) phase increment counter */
    SLOT->Incr = (fc * SLOT->mul) >> 1;

    if (SLOT->ksr != ksr)
    {
        SLOT->ksr = ksr;
        /* calculate envelope generator rates */
        if ((SLOT->ar + SLOT->ksr) < 32 + 62)
        {
            SLOT->eg_sh_ar = eg_rate_shift[SLOT->ar + SLOT->ksr];
            SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
        }
        else
        {
            SLOT->eg_sh_ar = 0;
            SLOT->eg_sel_ar = 17 * RATE_STEPS;
        }
        SLOT->eg_sh_d1r = eg_rate_shift[SLOT->d1r + SLOT->ksr];
        SLOT->eg_sh_d2r = eg_rate_shift[SLOT->d2r + SLOT->ksr];
        SLOT->eg_sh_rr = eg_rate_shift[SLOT->rr + SLOT->ksr];
        SLOT->eg_sel_d1r = eg_rate_select[SLOT->d1r + SLOT->ksr];
        SLOT->eg_sel_d2r = eg_rate_select[SLOT->d2r + SLOT->ksr];
        SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
    }
}

static void C1943_HOT(refresh_fc_eg_chan)(ym2203_t *OPN, ym2203_ch_t *CH)
{
    if (CH->SLOT[SLOT1].Incr == -1)
    {
        int fc = CH->fc;
        int kc = CH->kcode;
        refresh_fc_eg_slot(OPN, &CH->SLOT[SLOT1], fc, kc);
        refresh_fc_eg_slot(OPN, &CH->SLOT[SLOT2], fc, kc);
        refresh_fc_eg_slot(OPN, &CH->SLOT[SLOT3], fc, kc);
        refresh_fc_eg_slot(OPN, &CH->SLOT[SLOT4], fc, kc);
    }
}

/* fm.cpp's fn_table[i]: (uint32_t)(i * 32 * freqbase * (1 << (FREQ_SH - 10))) */
static inline uint32_t fn_table(const ym2203_t *OPN, uint32_t i)
{
    return (uint32_t)(((uint64_t)i * OPN->fn_k) >> 16);
}

/* prescaler set (and make time tables) */
static void OPNSetPres(ym2203_t *OPN, int pres, int SSGpres)
{
    OPN->freqbase = ((double)OPN->clock / OPN->fm_rate) / pres;
    OPN->eg_timer_add = (uint32_t)((1 << EG_SH) * OPN->freqbase);
    OPN->eg_timer_overflow = 3 * (1 << EG_SH);

    /* the SSG part */
    ym_ssg_set_clock(&OPN->ssg, OPN->clock * 2 / SSGpres, OPN->ssg_rate);

    /* DeTune table */
    for (int d = 0; d <= 3; d++)
    {
        for (int i = 0; i <= 31; i++)
        {
            double rate = ((double)dt_tab[d * 32 + i]) * SIN_LEN * OPN->freqbase * (1 << FREQ_SH) / ((double)(1 << 20));
            OPN->dt_tab[d][i] = (int32_t)rate;
            OPN->dt_tab[d + 4][i] = -OPN->dt_tab[d][i];
        }
    }
    OPN->fn_k = (uint32_t)(32.0 * OPN->freqbase * (1 << (FREQ_SH - 10)) * 65536.0);
    /* maximal frequency is required for Phase overflow calculation, register size is 17 bits (Nemesis) */
    OPN->fn_max = (uint32_t)((double)0x20000 * OPN->freqbase * (1 << (FREQ_SH - 10)));
}

/*
  prescaler circuit (best guess to verified chip behaviour)
  reg.2d : sel2 = in21 (select sel2)
  reg.2e : sel1 = in11 (select sel1)
  reg.2f : sel1 = in10 , sel2 = in20 (clear selector)
  reset  : sel1 = in11 , sel2 = in21 (clear both)
*/
static void OPNPrescaler_w(ym2203_t *OPN, int addr)
{
    static const int opn_pres[4] = {2 * 12, 2 * 12, 6 * 12, 3 * 12};
    static const int ssg_pres[4] = {1, 1, 4, 2};

    switch (addr)
    {
    case 0: /* when reset */
        OPN->prescaler_sel = 2;
        break;
    case 0x2d: /* divider sel : select 1/1 for 1/3line */
        OPN->prescaler_sel |= 0x02;
        break;
    case 0x2e: /* divider sel , select 1/3line for output */
        OPN->prescaler_sel |= 0x01;
        break;
    case 0x2f: /* divider sel , clear both selector to 1/2,1/2 */
        OPN->prescaler_sel = 0;
        break;
    }
    const int sel = OPN->prescaler_sel & 3;
    OPNSetPres(OPN, opn_pres[sel], ssg_pres[sel]);
}

/* write a OPN mode register 0x20-0x2f */
static void OPNWriteMode(ym2203_t *OPN, int r, int v)
{
    switch (r)
    {
    case 0x27: /* mode, timer control: only the channel 3 mode matters here */
        OPN->mode = (uint8_t)v;
        break;
    case 0x28: /* key on / off */
    {
        const uint8_t c = v & 0x03;
        if (c == 3)
            break;
        ym2203_ch_t *CH = &OPN->CH[c];
        if (v & 0x10)
            FM_KEYON(CH, SLOT1);
        else
            FM_KEYOFF(CH, SLOT1);
        if (v & 0x20)
            FM_KEYON(CH, SLOT2);
        else
            FM_KEYOFF(CH, SLOT2);
        if (v & 0x40)
            FM_KEYON(CH, SLOT3);
        else
            FM_KEYOFF(CH, SLOT3);
        if (v & 0x80)
            FM_KEYON(CH, SLOT4);
        else
            FM_KEYOFF(CH, SLOT4);
        break;
    }
    }
}

/* write a OPN register (0x30-0xff) */
static void OPNWriteReg(ym2203_t *OPN, int r, int v)
{
    const uint8_t c = OPN_CHAN(r);
    if (c == 3)
        return; /* 0xX3,0xX7,0xXB,0xXF */
    ym2203_ch_t *CH = &OPN->CH[c];
    ym2203_slot_t *SLOT = &(CH->SLOT[OPN_SLOT(r)]);

    switch (r & 0xf0)
    {
    case 0x30: /* DET , MUL */
        set_det_mul(OPN, CH, SLOT, v);
        break;
    case 0x40: /* TL */
        set_tl(SLOT, v);
        break;
    case 0x50: /* KS, AR */
        set_ar_ksr(CH, SLOT, v);
        break;
    case 0x60: /* DR (no AM on the YM2203) */
        set_dr(SLOT, v);
        break;
    case 0x70: /* SR */
        set_sr(SLOT, v);
        break;
    case 0x80: /* SL, RR */
        set_sl_rr(SLOT, v);
        break;
    case 0x90: /* SSG-EG */
        SLOT->ssg = v & 0x0f;
        SLOT->ssgn = (v & 0x04) >> 1; /* bit 1 in ssgn = attack */
        break;
    case 0xa0:
        switch (OPN_SLOT(r))
        {
        case 0: /* 0xa0-0xa2 : FNUM1 */
        {
            uint32_t fn = (((uint32_t)((OPN->fn_h) & 7)) << 8) + v;
            uint8_t blk = OPN->fn_h >> 3;
            CH->kcode = (blk << 2) | opn_fktable[fn >> 7];
            CH->fc = fn_table(OPN, fn * 2) >> (7 - blk);
            CH->block_fnum = (blk << 11) | fn;
            CH->SLOT[SLOT1].Incr = -1;
            break;
        }
        case 1: /* 0xa4-0xa6 : FNUM2,BLK */
            OPN->fn_h = v & 0x3f;
            break;
        case 2: /* 0xa8-0xaa : 3CH FNUM1 */
        {
            uint32_t fn = (((uint32_t)(OPN->SL3.fn_h & 7)) << 8) + v;
            uint8_t blk = OPN->SL3.fn_h >> 3;
            OPN->SL3.kcode[c] = (blk << 2) | opn_fktable[fn >> 7];
            OPN->SL3.fc[c] = fn_table(OPN, fn * 2) >> (7 - blk);
            OPN->SL3.block_fnum[c] = (blk << 11) | fn;
            OPN->CH[2].SLOT[SLOT1].Incr = -1;
            break;
        }
        case 3: /* 0xac-0xae : 3CH FNUM2,BLK */
            OPN->SL3.fn_h = v & 0x3f;
            break;
        }
        break;
    case 0xb0:
        if (OPN_SLOT(r) == 0) /* 0xb0-0xb2 : FB,ALGO */
        {
            int feedback = (v >> 3) & 7;
            CH->ALGO = v & 7;
            CH->FB = feedback ? feedback + 6 : 0;
            setup_connection(OPN, CH, c);
        }
        break;
    }
}

static void reset_channels(ym2203_t *OPN)
{
    OPN->mode = 0; /* normal mode */
    for (int c = 0; c < 3; c++)
    {
        OPN->CH[c].fc = 0;
        for (int s = 0; s < 4; s++)
        {
            OPN->CH[c].SLOT[s].ssg = 0;
            OPN->CH[c].SLOT[s].ssgn = 0;
            OPN->CH[c].SLOT[s].state = EG_OFF;
            OPN->CH[c].SLOT[s].volume = MAX_ATT_INDEX;
            OPN->CH[c].SLOT[s].vol_out = MAX_ATT_INDEX;
        }
    }
}

void ym2203_init(ym2203_t *c, uint32_t clock, uint32_t fm_rate, uint32_t ssg_rate)
{
    init_tables();
    memset(c, 0, sizeof(*c));
    c->clock = clock;
    c->fm_rate = fm_rate;
    c->ssg_rate = ssg_rate;
    for (int ch = 0; ch < 3; ch++)
        for (int s = 0; s < 4; s++)
            c->CH[ch].SLOT[s].DT = c->dt_tab[0];
    ym2203_reset(c);
}

void ym2203_reset(ym2203_t *c)
{
    ym_ssg_reset(&c->ssg);
    OPNPrescaler_w(c, 0);
    c->eg_timer = 0;
    c->eg_cnt = 0;
    reset_channels(c);
    /* reset operator parameters */
    for (int i = 0xb2; i >= 0x30; i--)
        OPNWriteReg(c, i, 0);
    for (int i = 0x26; i >= 0x20; i--)
        OPNWriteMode(c, i, 0);
}

void C1943_HOT(ym2203_write)(ym2203_t *c, int a0, uint8_t v)
{
    if (!(a0 & 1))
    {
        /* address port */
        c->address = v;
        /* prescaler select : 2d,2e,2f */
        if (v >= 0x2d && v <= 0x2f)
            OPNPrescaler_w(c, v);
        return;
    }
    /* data port */
    const int addr = c->address;
    switch (addr & 0xf0)
    {
    case 0x00: /* 0x00-0x0f : SSG section */
        ym_ssg_write(&c->ssg, (uint8_t)addr, v);
        break;
    case 0x20: /* 0x20-0x2f : Mode section */
        OPNWriteMode(c, addr, v);
        break;
    default: /* 0x30-0xff : OPN section */
        OPNWriteReg(c, addr, v);
        break;
    }
}

void C1943_HOT(ym2203_render_fm)(ym2203_t *OPN, int32_t *acc, int n, int gain_q8)
{
    ym2203_ch_t *cch0 = &OPN->CH[0], *cch1 = &OPN->CH[1], *cch2 = &OPN->CH[2];

    /* refresh PG and EG */
    refresh_fc_eg_chan(OPN, cch0);
    refresh_fc_eg_chan(OPN, cch1);
    if (OPN->mode & 0xc0)
    {
        /* 3SLOT MODE */
        if (cch2->SLOT[SLOT1].Incr == -1)
        {
            refresh_fc_eg_slot(OPN, &cch2->SLOT[SLOT1], OPN->SL3.fc[1], OPN->SL3.kcode[1]);
            refresh_fc_eg_slot(OPN, &cch2->SLOT[SLOT2], OPN->SL3.fc[2], OPN->SL3.kcode[2]);
            refresh_fc_eg_slot(OPN, &cch2->SLOT[SLOT3], OPN->SL3.fc[0], OPN->SL3.kcode[0]);
            refresh_fc_eg_slot(OPN, &cch2->SLOT[SLOT4], cch2->fc, cch2->kcode);
        }
    }
    else
    {
        refresh_fc_eg_chan(OPN, cch2);
    }

    for (int i = 0; i < n; i++)
    {
        OPN->out_fm[0] = OPN->out_fm[1] = OPN->out_fm[2] = 0;

        /* advance envelope generator */
        OPN->eg_timer += OPN->eg_timer_add;
        while (OPN->eg_timer >= OPN->eg_timer_overflow)
        {
            OPN->eg_timer -= OPN->eg_timer_overflow;
            OPN->eg_cnt++;
            advance_eg_channel(OPN, &cch0->SLOT[SLOT1]);
            advance_eg_channel(OPN, &cch1->SLOT[SLOT1]);
            advance_eg_channel(OPN, &cch2->SLOT[SLOT1]);
        }

        chan_calc(OPN, cch0);
        chan_calc(OPN, cch1);
        chan_calc(OPN, cch2);

        int32_t lt = OPN->out_fm[0] + OPN->out_fm[1] + OPN->out_fm[2];
        if (lt > MAXOUT)
            lt = MAXOUT;
        else if (lt < MINOUT)
            lt = MINOUT;
        acc[i] += (lt * gain_q8) >> 8;
    }
}
