// BSD 3-Clause License
//
// Copyright (c) 2021, Aaron Giles
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/*
 * The SSG of a Yamaha OPN chip (the YM2203's AY-3-8910 compatible half): three
 * tone channels, a noise generator and a 32-step envelope. The engine is
 * ymfm's ssg_engine (ymfm_ssg.cpp) in plain C; the I/O ports are left out.
 *
 * The engine steps at the SSG's clock / 8, which with the YM2203's default
 * prescaler is the chip's clock / 16 (ymfm's ssg_effective_clock is clock /
 * 2). ym_ssg_render() averages the steps each output sample covers.
 */
#ifndef YM_SSG_H
#define YM_SSG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t regs[16];
    uint32_t tone_count[3], tone_state[3];
    uint32_t envelope_count, envelope_state;
    uint32_t noise_count, noise_state;

    uint32_t tick_frac; /* Q16 steps carried between samples */
    uint32_t tick_step; /* Q16 steps per output sample */
    uint32_t last;      /* the last sample's average, before the gain */
    int32_t dc_x, dc_y; /* DC blocker */
} ym_ssg_t;

/* clock: the SSG's (the chip's clock * 2 / its SSG prescaler) */
void ym_ssg_set_clock(ym_ssg_t *s, uint32_t clock, uint32_t samplerate);
void ym_ssg_reset(ym_ssg_t *s);
void ym_ssg_write(ym_ssg_t *s, uint8_t reg, uint8_t value);

/* Adds n samples to out (saturating): the three channels, each at full
 * amplitude 16382 (MAME's), times gain_q15 / 32768, with the DC removed. */
void ym_ssg_render(ym_ssg_t *s, int16_t *out, int n, int gain_q15);

#ifdef __cplusplus
}
#endif

#endif
