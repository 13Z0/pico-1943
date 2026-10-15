// BSD 3-Clause License, Copyright (c) 2021, Aaron Giles; see ym_ssg.h.
/*
 * The SSG of the YM2203: ymfm's ssg_engine (ymfm_ssg.cpp) in plain C. See
 * ym_ssg.h.
 */
#include "ym_ssg.h"

#include <string.h>

#include "c1943_port.h"

/* volume to amplitude table, taken from MAME's implementation but biased so
 * that 0 == 0 (ymfm). In SRAM, as everything the sound board reads all the
 * time: on core1, a read from flash waits for the XIP cache that core0 shares
 * (c1943_port.h). */
static int16_t C1943_RAM_DATA s_amplitudes[32] = {
    0,    32,   78,   141,  178,  222,  262,  306,  369,  441,   509,   585,   701,   836,   965,   1112,
    1334, 1595, 1853, 2146, 2576, 3081, 3576, 4135, 5000, 6006, 7023, 8155, 9963, 11976, 14132, 16382,
};

#define BIT(v, b) (((v) >> (b)) & 1)

void ym_ssg_set_clock(ym_ssg_t *s, uint32_t clock, uint32_t samplerate)
{
    s->tick_step = (uint32_t)((((uint64_t)clock / 8) << 16) / samplerate);
}

void ym_ssg_reset(ym_ssg_t *s)
{
    const uint32_t step = s->tick_step;
    memset(s, 0, sizeof(*s));
    s->tick_step = step;
    s->noise_state = 1;
}

void ym_ssg_write(ym_ssg_t *s, uint8_t reg, uint8_t value)
{
    reg &= 0x0f;
    s->regs[reg] = value;
    /* writes to the envelope shape register reset the state */
    if (reg == 0x0d)
        s->envelope_state = 0;
}

static inline uint32_t tone_period(const ym_ssg_t *s, int ch)
{
    return s->regs[2 * ch] | ((s->regs[2 * ch + 1] & 0x0f) << 8);
}

/* ymfm's ssg_engine::clock(): one step at the SSG's clock / 8 */
static inline void C1943_HOT(step)(ym_ssg_t *s, const uint32_t tp[3], uint32_t np, uint32_t ep)
{
    /* tone period units are clock/16 but since we run at clock/8 that works
     * out for us to toggle the state (50% duty cycle) at twice the programmed
     * period */
    for (int ch = 0; ch < 3; ch++)
    {
        s->tone_count[ch]++;
        if (s->tone_count[ch] >= tp[ch])
        {
            s->tone_state[ch] ^= 1;
            s->tone_count[ch] = 0;
        }
    }
    /* noise period units are clock/16 but since we run at clock/8, our counter
     * needs a right shift prior to compare; a period of 0 should produce an
     * identical result to a period of 1 */
    s->noise_count++;
    if ((s->noise_count >> 1) >= np && s->noise_count != 1)
    {
        s->noise_state ^= (BIT(s->noise_state, 0) ^ BIT(s->noise_state, 3)) << 17;
        s->noise_state >>= 1;
        s->noise_count = 0;
    }
    /* envelope period units are clock/8 (the manual's clock/256 is for all 32
     * steps) */
    s->envelope_count++;
    if (s->envelope_count >= ep)
    {
        s->envelope_state++;
        s->envelope_count = 0;
    }
}

/* ymfm's ssg_engine::output(): the three channels' amplitudes, summed */
static inline uint32_t C1943_HOT(output)(ym_ssg_t *s)
{
    const uint8_t r13 = s->regs[0x0d];
    const uint32_t hold = BIT(r13, 0), alternate = BIT(r13, 1), attack0 = BIT(r13, 2), cont = BIT(r13, 3);
    uint32_t envelope_volume;
    if ((hold | (cont ^ 1)) && s->envelope_state >= 32)
    {
        s->envelope_state = 32;
        envelope_volume = ((attack0 ^ alternate) & cont) ? 31 : 0;
    }
    else
    {
        uint32_t attack = attack0;
        if (alternate)
            attack ^= BIT(s->envelope_state, 5);
        envelope_volume = (s->envelope_state & 31) ^ (attack ? 0 : 31);
    }

    const uint8_t mix = s->regs[0x07];
    uint32_t sum = 0;
    for (int ch = 0; ch < 3; ch++)
    {
        /* noise depends on the noise state, which is the LSB of noise_state;
         * tone on the current tone state */
        const uint32_t noise_on = BIT(mix, 3 + ch) | (s->noise_state & 1);
        const uint32_t tone_on = BIT(mix, ch) | s->tone_state[ch];
        uint32_t volume;
        if ((noise_on & tone_on) == 0)
            volume = 0;
        else if (BIT(s->regs[0x08 + ch], 4))
            volume = envelope_volume;
        else
        {
            /* amplitude 15 maps to envelope 31 */
            volume = (s->regs[0x08 + ch] & 0x0f) * 2;
            if (volume != 0)
                volume |= 1;
        }
        sum += (uint32_t)s_amplitudes[volume];
    }
    return sum;
}

/* The steps until step() changes anything the output depends on: a tone
 * toggles, the noise shifts or the envelope moves (at least 1). The counters
 * fire when, after their increment, they reach these thresholds. */
/* What step() changes that output() looks at, per the registers: the tone of
 * an audible channel whose tone is enabled; the noise, when an audible channel
 * has it enabled; the envelope, when a channel is in envelope mode. A channel
 * is audible in envelope mode or at a fixed volume above 0. Everything else
 * steps without being heard, and is caught up only now and then (catch_up). */
typedef struct
{
    uint32_t t[3], tn, te; /* the counters fire when they reach these */
    uint8_t tone;          /* bit ch: the channel's tone is heard */
    uint8_t noise, env;    /* the noise, the envelope is heard */
} ssg_use_t;

static inline void C1943_HOT(ssg_use)(const ym_ssg_t *s, const uint32_t tp[3], uint32_t np, uint32_t ep,
                                      ssg_use_t *u)
{
    const uint8_t mix = s->regs[0x07];
    u->tone = 0;
    u->noise = u->env = 0;
    for (int ch = 0; ch < 3; ch++)
    {
        u->t[ch] = tp[ch] ? tp[ch] : 1;
        const uint8_t vol = s->regs[0x08 + ch];
        const int env = BIT(vol, 4), audible = env || (vol & 0x0f);
        if (!audible)
            continue;
        if (env)
            u->env = 1;
        if (!BIT(mix, ch))
            u->tone |= (uint8_t)(1 << ch);
        if (!BIT(mix, 3 + ch))
            u->noise = 1;
    }
    u->tn = np ? 2 * np : 2; /* noise fires at (count >> 1) >= np, count != 1 */
    u->te = ep ? ep : 1;
}

/* The steps until step() changes something that is heard (at least 1). */
static inline uint32_t C1943_HOT(steps_quiet)(const ym_ssg_t *s, const ssg_use_t *u)
{
    uint32_t q = UINT32_MAX;
    for (int ch = 0; ch < 3; ch++)
    {
        if (!(u->tone & (1 << ch)))
            continue;
        const uint32_t left = u->t[ch] > s->tone_count[ch] ? u->t[ch] - s->tone_count[ch] : 1;
        if (left < q)
            q = left;
    }
    if (u->noise)
    {
        const uint32_t left = u->tn > s->noise_count ? u->tn - s->noise_count : 1;
        if (left < q)
            q = left;
    }
    if (u->env)
    {
        const uint32_t left = u->te > s->envelope_count ? u->te - s->envelope_count : 1;
        if (left < q)
            q = left;
    }
    return q;
}

/* n steps of a counter that, after its increment, fires when it reaches t
 * and starts again from 0; returns how often it fired */
static inline uint32_t C1943_HOT(advance)(uint32_t *count, uint32_t t, uint32_t n)
{
    if (!n)
        return 0;
    uint32_t c = *count, fired = 0;
    if (c >= t)
    {
        /* the period was made shorter than the count: the next step fires */
        fired = 1;
        c = 0;
        n--;
    }
    c += n;
    fired += c / t;
    *count = c % t;
    return fired;
}

/* The steps the counters that are not heard owe: taken at once. */
static void C1943_HOT(catch_up)(ym_ssg_t *s, const ssg_use_t *u, uint32_t n)
{
    if (!n)
        return;
    for (int ch = 0; ch < 3; ch++)
        if (!(u->tone & (1 << ch)))
            s->tone_state[ch] ^= advance(&s->tone_count[ch], u->t[ch], n) & 1;
    if (!u->noise)
        for (uint32_t k = advance(&s->noise_count, u->tn, n); k; k--)
        {
            s->noise_state ^= (BIT(s->noise_state, 0) ^ BIT(s->noise_state, 3)) << 17;
            s->noise_state >>= 1;
        }
    if (!u->env)
        s->envelope_state += advance(&s->envelope_count, u->te, n);
}

void C1943_HOT(ym_ssg_render)(ym_ssg_t *s, int16_t *out, int n, int gain_q15)
{
    const uint32_t tp[3] = {tone_period(s, 0), tone_period(s, 1), tone_period(s, 2)};
    const uint32_t np = s->regs[0x06] & 0x1f;
    const uint32_t ep = s->regs[0x0b] | (s->regs[0x0c] << 8);
    ssg_use_t u;
    ssg_use(s, tp, np, ep, &u);

    /* While nothing that is heard changes, the steps only count: they are
     * added at once, and the output is the one of the last step. "quiet" is
     * the number of steps that can be taken that way; the first one after
     * them is a step() that changes something. The counters nobody hears are
     * caught up before such a step, and at the end. */
    uint32_t quiet = steps_quiet(s, &u) - 1;
    uint32_t owed = 0; /* steps the counters that are not heard owe */
    uint32_t now = output(s);
    for (int i = 0; i < n; i++)
    {
        s->tick_frac += s->tick_step;
        const uint32_t ticks = s->tick_frac >> 16;
        s->tick_frac &= 0xffff;
        uint32_t acc;
        if (ticks <= quiet)
        {
            for (int ch = 0; ch < 3; ch++)
                if (u.tone & (1 << ch))
                    s->tone_count[ch] += ticks;
            if (u.noise)
                s->noise_count += ticks;
            if (u.env)
                s->envelope_count += ticks;
            owed += ticks;
            quiet -= ticks;
            acc = ticks * now;
        }
        else
        {
            catch_up(s, &u, owed);
            owed = 0;
            acc = 0;
            for (uint32_t t = 0; t < ticks; t++)
            {
                step(s, tp, np, ep);
                now = output(s);
                acc += now;
            }
            quiet = steps_quiet(s, &u) - 1;
        }
        if (ticks)
            s->last = acc / ticks; /* 0 .. 3 * 16382 */
        const int32_t x = ((int32_t)s->last * gain_q15) >> 15;

        /* DC blocker, y = x - x1 + (1 - 1/256) * y1, y in Q8 so that it decays
         * all the way to zero: the chip's output is unipolar */
        s->dc_y += (x - s->dc_x) * 256 - (s->dc_y >> 8);
        s->dc_x = x;
        const int32_t y = s->dc_y >> 8;

        int32_t v = out[i] + y;
        if (v > 32767)
            v = 32767;
        else if (v < -32768)
            v = -32768;
        out[i] = (int16_t)v;
    }
    catch_up(s, &u, owed);
}
