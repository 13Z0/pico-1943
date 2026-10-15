/*
 * 1943 core internals: the machine state shared by the core's files.
 *
 * One machine runs at a time; its state is a single struct, `m`, for the main
 * CPU side and the video, and `snd` (c1943_sound.c) for the sound board, which
 * may run on another core.
 */
#ifndef C1943_INTERNAL_H
#define C1943_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "c1943.h"
#include "c1943_port.h"
#include "z80.h"

/* The board's timing: 384 x 262 at 6 MHz. */
#define C1943_LINES 262
#define C1943_MAIN_PER_LINE 384
#define C1943_MAIN_PER_FRAME (C1943_LINES * C1943_MAIN_PER_LINE) /* 100608 */
#define C1943_SOUND_PER_LINE 192
#define C1943_SOUND_PER_FRAME (C1943_LINES * C1943_SOUND_PER_LINE) /* 50304 */

/* The video registers and RAM latched at vblank, which the renderer draws. */
typedef struct
{
    /* word aligned: copied a word at a time (latch_video) */
    uint8_t vram[0x800] __attribute__((aligned(4))); /* d000-d3ff characters, d400-d7ff their attributes */
    uint8_t spr[128 * 4] __attribute__((aligned(4))); /* the four used bytes of each sprite of the DMA buffer */
    uint16_t bg_scrollx, bg2_scrollx;
    uint8_t bg_scrolly;
    uint8_t layers; /* d806: b4 bg, b5 bg2, b6 sprites */
    uint8_t chars_on; /* c804 b7 */
} c1943_video_t;

typedef struct
{
    int set; /* c1943_set_id_t, or -1 */
    c1943_fmt_t fmt;
    int samplerate;

    /* the data, see c1943_part_t */
    const uint8_t *rom_main, *rom_banks, *tilemap, *proms;
    const uint16_t *chars;
    const uint32_t *bg, *bg2, *sprites;

    z80_t cpu;
    uint8_t ram[0x1000] __attribute__((aligned(4)));       /* e000-efff */
    uint8_t spriteram[0x1000] __attribute__((aligned(4))); /* f000-ffff */
    uint8_t vram[0x800] __attribute__((aligned(4)));       /* d000-d7ff */
    uint8_t spritebuf[128 * 4] __attribute__((aligned(4))); /* the DMA copy the sprite hardware draws from */
    uint8_t scroll[5];          /* d800-d804 */
    uint8_t layers, control, bank, prot_cmd;
    uint8_t sound_reset;
    uint8_t sched_ok; /* the main CPU's scheduler is where main_idle() expects it */
    uint32_t inputs;
    int line;            /* the scanline being run */
    int32_t slice_start; /* the main CPU's icount at the start of the slice */

    c1943_event_t events[C1943_MAX_EVENTS];
    int nevents;

    c1943_video_t video;

    /* colours in the canvas format: per colour code, per pen */
    uint16_t pal_chars[32][4], pal_bg[16][16], pal_bg2[16][16], pal_spr[16][16];
    uint16_t bg_trans[16];   /* bit pen: the front layer is transparent there */
    uint16_t bg_pens[512];   /* bit pen: the front layer's tile uses it */
    uint16_t spr_behind[16]; /* bit pen: the sprite pixel goes behind the front layer */

    c1943_stats_t stats;
    uint32_t fast_bytes;
} c1943_t;

extern c1943_t m;

/* c1943_video.c: the colours from the PROMs, and what the renderer needs
 * from the data (m.bg) */
void c1943_video_init(const uint8_t *proms);

/* c1943_sound.c */
void c1943_sound_start(const uint8_t *rom_sound);

/* --- Profiling ---------------------------------------------------------------
 * In C1943_FRAME_STATS builds: CPU cycles summed per part of the frame into
 * c1943_prof[]: 0 the main CPU, 1 the sound board (2 and 3 included), 2 FM,
 * 3 SSG, 4 drawing. The board provides c1943_cycles() and prints the sums. */
#if C1943_FRAME_STATS
#define C1943_NPROF 8
extern uint32_t c1943_prof[C1943_NPROF];
uint32_t c1943_cycles(void);
#define C1943_PROF_START(t) const uint32_t t = c1943_cycles()
#define C1943_PROF_ADD(i, t) (c1943_prof[i] += c1943_cycles() - (t))
#else
#define C1943_PROF_START(t) ((void)0)
#define C1943_PROF_ADD(i, t) ((void)0)
#endif

#endif
