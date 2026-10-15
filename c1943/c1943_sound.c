/*
 * 1943: the sound board, after MAME's capcom/1943.cpp (license BSD-3-Clause,
 * copyright-holders Paul Leaman).
 *
 * Sound CPU (Z80, 3 MHz):
 *   0000-7fff  ROM
 *   c000-c7ff  RAM
 *   c800  (r)  the latch the main CPU writes
 *   d800       the i8751's port (unused by the MCU program: reads 0)
 *   e000-e001  YM2203 #1 (address, data); e002-e003 YM2203 #2
 * Interrupts (IRQ, RST 38h, held until taken) come four times a frame, at
 * lines 32, 96, 160 and 224; the program updates the chips once per interrupt.
 *
 * The main CPU only ever reaches this board through the latch and the reset
 * line (c804 bit 5), so the board runs a frame at a time after the main CPU,
 * from that frame's writes, timestamped in main CPU cycles (two per sound CPU
 * cycle). That is exact: whatever the sound CPU reads from the latch at a
 * given cycle is what the main CPU had last written by then. It lets the
 * board run on the same core after each frame, or on core1 a frame behind.
 *
 * The chips are rendered up to the current cycle before each register write,
 * so every write lands at its own sample. MAME's mix: each SSG channel at
 * 0.15, the FM at 0.10, both chips added.
 */
#include "c1943_internal.h"

#include "ym2203.h"

#define YM_CLOCK 1500000

/* The FM is made at half the output rate and doubled by linear interpolation:
 * 22050 Hz is above what the chip's own rate (clock / 72 = 20833 Hz) gives. */
#define FM_DIV 2

/* MAME's mix levels, of the chips' own full scale: fm.cpp's (and ymfm's) 16
 * bits for the three FM channels together, 16382 for an SSG channel at full
 * volume (MAME's YM volume table, as ymfm has it). */
#define FM_GAIN_Q8 26   /* 0.10 */
#define SSG_GAIN_Q15 4915 /* 0.15 */

#define MAX_FM_CHUNK 64

typedef struct
{
    z80_t cpu;
    uint8_t ram[0x800];
    const uint8_t *rom;
    uint8_t latch;
    uint8_t in_reset;
    ym2203_t ym[2];

    /* the frame being made */
    int16_t *out;
    int nout, pos;       /* samples in the frame, and made so far */
    int32_t frame_cycle; /* sound CPU cycles of the frame done by the current slice's start */
    int32_t slice_start; /* the CPU's icount at the start of the slice */

    /* the FM's interpolation: its last two samples, and which half of the
     * pair of output samples comes next */
    int32_t fm_prev, fm_cur;
    int fm_half;

    uint32_t idle_cycles;
} sound_t;

static sound_t snd;

/* The host harness's choice of what is heard (--only fm|ssg): bit 0 the FM,
 * bit 1 the SSG. */
int c1943_snd_mask = 3;

/* The host harness's log of the chips' writes (--ymlog); NULL on the board. */
void (*c1943_ym_log)(int chip, int a0, uint8_t value, int sample);

#if C1943_FRAME_STATS
uint32_t c1943_prof[C1943_NPROF];
#endif

/* ---------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------ */

/* Renders the chips up to sample `upto` of the frame. */
static void C1943_HOT(render_upto)(int upto)
{
    if (upto > snd.nout)
        upto = snd.nout;
    int n = upto - snd.pos;
    if (n <= 0)
        return;
    int16_t *out = snd.out + snd.pos;

    C1943_PROF_START(t0);
    for (int k = 0; k < 2; k++)
        ym2203_render_ssg(&snd.ym[k], out, n, (c1943_snd_mask & 2) ? SSG_GAIN_Q15 : 0);
    C1943_PROF_ADD(3, t0);

    C1943_PROF_START(t1);
    int32_t fm[MAX_FM_CHUNK];
    int i = 0;
    while (i < n)
    {
        /* the FM samples the next output samples need: one per pair */
        const int left = n - i;
        int need = (left + (snd.fm_half ? 0 : 1)) / 2;
        if (need > MAX_FM_CHUNK)
            need = MAX_FM_CHUNK;
        if (need)
        {
            for (int k = 0; k < need; k++)
                fm[k] = 0;
            const int g = (c1943_snd_mask & 1) ? FM_GAIN_Q8 : 0;
            ym2203_render_fm(&snd.ym[0], fm, need, g);
            ym2203_render_fm(&snd.ym[1], fm, need, g);
        }
        int f = 0;
        for (; i < n; i++)
        {
            int32_t s;
            if (!snd.fm_half)
            {
                if (f == need)
                    break;
                snd.fm_prev = snd.fm_cur;
                snd.fm_cur = fm[f++];
                s = (snd.fm_prev + snd.fm_cur) >> 1;
            }
            else
            {
                s = snd.fm_cur;
            }
            snd.fm_half ^= 1;
            s += out[i];
            if (s > 32767)
                s = 32767;
            else if (s < -32768)
                s = -32768;
            out[i] = (int16_t)s;
        }
    }
    C1943_PROF_ADD(2, t1);
    snd.pos = upto;
}

/* The sample of the frame the sound CPU is at now. */
static inline int C1943_HOT(sample_now)(void)
{
    int32_t cycle = snd.frame_cycle + (snd.slice_start - snd.cpu.icount);
    if (cycle < 0)
        cycle = 0;
    /* 50304 * 735 fits in 32 bits: no 64-bit division (in flash) */
    return (int)(((uint32_t)cycle * (uint32_t)snd.nout) / C1943_SOUND_PER_FRAME);
}

/* ---------------------------------------------------------------------------
 * Sound CPU memory
 * ------------------------------------------------------------------------ */

static uint8_t C1943_HOT(s_read)(z80_t *cpu, uint16_t addr)
{
    (void)cpu;
    if (addr == 0xc800)
        return snd.latch;
    return 0; /* d800 (the MCU's port) and unmapped */
}

static void C1943_HOT(s_write)(z80_t *cpu, uint16_t addr, uint8_t value)
{
    (void)cpu;
    if (addr >= 0xe000 && addr <= 0xe003)
    {
        const int now = sample_now();
        if (addr & 1)
            render_upto(now); /* a register: up to now with the old value */
        if (c1943_ym_log)
            c1943_ym_log((addr >> 1) & 1, addr & 1, value, now);
        ym2203_write(&snd.ym[(addr >> 1) & 1], addr & 1, value);
    }
}

static uint8_t s_in(z80_t *cpu, uint16_t port)
{
    (void)cpu;
    (void)port;
    return 0xff;
}

static void s_out(z80_t *cpu, uint16_t port, uint8_t value)
{
    (void)cpu;
    (void)port;
    (void)value;
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------ */

void c1943_sound_start(const uint8_t *rom_sound)
{
    snd.rom = rom_sound;
    for (int k = 0; k < 2; k++)
        ym2203_init(&snd.ym[k], YM_CLOCK, (uint32_t)(m.samplerate / FM_DIV), (uint32_t)m.samplerate);
    c1943_sound_reset();
}

void c1943_sound_reset(void)
{
    z80_t *cpu = &snd.cpu;
    memset(snd.ram, 0, sizeof(snd.ram));
    snd.latch = 0;
    snd.in_reset = 0;
    z80_init(cpu);
    z80_reset(cpu);
    for (int p = 0; p < 256; p++)
    {
        cpu->rd_page[p] = NULL;
        cpu->wr_page[p] = NULL;
    }
    for (int p = 0; p < 0x80; p++)
        cpu->rd_page[p] = snd.rom + p * 256;
    for (int p = 0; p < 8; p++)
        cpu->rd_page[0xc0 + p] = cpu->wr_page[0xc0 + p] = snd.ram + p * 256;
    cpu->read = s_read;
    cpu->write = s_write;
    cpu->in = s_in;
    cpu->out = s_out;
    for (int k = 0; k < 2; k++)
        ym2203_reset(&snd.ym[k]);
    snd.fm_prev = snd.fm_cur = 0;
    snd.fm_half = 0;
}

/* ---------------------------------------------------------------------------
 * A frame
 * ------------------------------------------------------------------------ */

/* Between its interrupts the sound program waits in EI / JR $-1 (1943 at 007c,
 * 1943 Kai at 0008): with no interrupt to take, that loop only spins. */
static int C1943_HOT(sound_idle)(const z80_t *cpu)
{
    const uint16_t pc = cpu->pc.w;
    if (cpu->irq_state || pc < 1 || pc >= 0x7ffe)
        return 0;
    const uint8_t *r = snd.rom;
    return (r[pc] == 0xfb && r[pc + 1] == 0x18 && r[pc + 2] == 0xfd) ||
           (r[pc - 1] == 0xfb && r[pc] == 0x18 && r[pc + 1] == 0xfd);
}

/* Runs the sound CPU for `cycles`, a line (192 cycles) at a time, so that its
 * wait for the next interrupt is caught soon after it starts. */
static void C1943_HOT(run_sound)(int cycles)
{
    z80_t *cpu = &snd.cpu;
    while (cycles > 0)
    {
        const int n = cycles < C1943_SOUND_PER_LINE ? cycles : C1943_SOUND_PER_LINE;
        if (snd.in_reset || (cpu->halt && !cpu->irq_state) || sound_idle(cpu))
        {
            /* held in reset, halted or waiting with no interrupt to take */
            cpu->icount += n;
            if (cpu->icount > 0)
                cpu->icount = 0;
            snd.idle_cycles += n;
        }
        else
        {
            snd.slice_start = cpu->icount + n;
            z80_run(cpu, n);
        }
        snd.frame_cycle += n;
        cycles -= n;
    }
}

/* the sound CPU's interrupts: lines 32, 96, 160 and 224 */
static int32_t C1943_RAM_DATA irq_at[4] = {32 * C1943_SOUND_PER_LINE, 96 * C1943_SOUND_PER_LINE, 160 * C1943_SOUND_PER_LINE,
                            224 * C1943_SOUND_PER_LINE};

void C1943_HOT(c1943_sound_frame)(const c1943_event_t *events, int nevents, int16_t *audio, int n)
{
    for (int i = 0; i < n; i++)
        audio[i] = 0;
    if (m.set < 0)
        return;
    snd.out = audio;
    snd.nout = n;
    snd.pos = 0;
    snd.frame_cycle = 0;
    snd.slice_start = snd.cpu.icount;
    snd.idle_cycles = 0;

    C1943_PROF_START(t0);
    int next_irq = 0, ev = 0;
    int32_t t = 0;
    while (t < C1943_SOUND_PER_FRAME)
    {
        int32_t until = C1943_SOUND_PER_FRAME;
        if (next_irq < 4 && irq_at[next_irq] < until)
            until = irq_at[next_irq];
        if (ev < nevents && (int32_t)(events[ev].cycle / 2) < until)
            until = (int32_t)(events[ev].cycle / 2);
        run_sound(until - t);
        t = until;
        while (ev < nevents && (int32_t)(events[ev].cycle / 2) <= t)
        {
            const c1943_event_t *e = &events[ev++];
            if (e->kind == C1943_EV_LATCH)
            {
                snd.latch = e->value;
            }
            else if (e->kind == C1943_EV_RESET)
            {
                if (e->value && !snd.in_reset)
                    z80_reset(&snd.cpu);
                snd.in_reset = e->value;
            }
        }
        if (next_irq < 4 && t >= irq_at[next_irq])
        {
            if (!snd.in_reset)
                z80_hold_irq(&snd.cpu, 0xff);
            next_irq++;
        }
    }
    render_upto(n);
    C1943_PROF_ADD(1, t0); /* the whole board, the chips (2, 3) included */
    m.stats.sound_pc = snd.cpu.pc.w;
    m.stats.sound_idle_cycles = snd.idle_cycles;
}
