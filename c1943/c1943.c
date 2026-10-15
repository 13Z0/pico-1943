/*
 * 1943: the main CPU's side of the board, after MAME's capcom/1943.cpp
 * (license BSD-3-Clause, copyright-holders Paul Leaman).
 *
 * Main CPU (Z80, 6 MHz):
 *   0000-7fff  ROM
 *   8000-bfff  banked ROM: 8 banks of 16 KB, selected by c804 bits 2-4
 *   c000       SYSTEM: b0 start 1, b1 start 2, b3 vblank (active high),
 *              b6 coin 1, b7 coin 2; the rest unused (1)
 *   c001/c002  P1/P2: b0 right, b1 left, b2 down, b3 up, b4 fire, b5 special
 *   c003/c004  DIP switches A and B
 *   c007  (r)  the i8751's answer
 *   c800  (w)  sound latch
 *   c804  (w)  b0-1 coin counters, b2-4 bank, b5 sound CPU reset, b6 flip,
 *              b7 characters on
 *   c806  (w)  sprite DMA: the sprite RAM is copied to the buffer drawn
 *   c807  (w)  the command to the i8751
 *   d000-d3ff  character codes, d400-d7ff their attributes
 *   d800-d801  front layer X scroll, d802 its Y scroll, d803-d804 back layer
 *              X scroll
 *   d806  (w)  b4 front layer on, b5 back layer on, b6 sprites on
 *   e000-efff  RAM
 *   f000-ffff  sprite RAM: 128 entries of 32 bytes, 4 of them used
 * Inputs are active low. Interrupts (IRQ, RST 38h, held until taken) come at
 * lines 144 and 240 of the 262; the picture is lines 16..239.
 *
 * The i8751. The game writes a byte to c807 and reads the answer from c007;
 * a wrong answer sends it to 8000 (a crash, then a reset). The MCU only ever
 * answers from a table, which MAME 0.185 had as c1943_protection_r (the game
 * keeps the same table at $21a of its own ROM to check against). The MCU
 * program, bm.7k, is the same for every set, 1943 Kai included.
 */
#include "c1943_internal.h"

c1943_t m = {.set = -1};

/* MAME 0.185's c1943_protection_r: command -> answer; other commands give 0. */
static const uint8_t prot_table[256] = {
    [0x24] = 0x1d, [0x60] = 0xf7, [0x01] = 0xac, [0x55] = 0x50, [0x56] = 0xe2, [0x2a] = 0x58, [0xa8] = 0x13,
    [0x22] = 0x3e, [0x3b] = 0x5a, [0x1e] = 0x1b, [0xe9] = 0x41, [0x7d] = 0xd5, [0x43] = 0x54, [0x37] = 0x6f,
    [0x4c] = 0x59, [0x5f] = 0x56, [0x3f] = 0x2f, [0x3e] = 0x3d, [0xfb] = 0x36, [0x1d] = 0x3b, [0x27] = 0xae,
    [0x26] = 0x39, [0x58] = 0x3c, [0x32] = 0x51, [0x1a] = 0xa8, [0xbc] = 0x33, [0x30] = 0x4a, [0x64] = 0x12,
    [0x11] = 0x40, [0x33] = 0x35, [0x09] = 0x17, [0x25] = 0x04,
};

/* MAME's defaults: difficulty 8 (normal), 2 credits for 2 players, flip off,
 * screen stop off, service off; 1 coin 1 credit on both slots, continue
 * allowed, demo sounds on. */
#define DSWA 0xf8
#define DSWB 0xff

/* ---------------------------------------------------------------------------
 * Main CPU memory
 * ------------------------------------------------------------------------ */

/* word copies, not memcpy (in flash) */
static inline void C1943_HOT(copy_words)(void *dst, const void *src, uint32_t bytes)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    for (uint32_t i = 0; i < bytes / 4; i++)
        d[i] = s[i];
}

static void C1943_HOT(set_bank)(uint8_t bank)
{
    if (bank != m.bank)
        m.stats.bank_switches++;
    m.bank = bank;
    const uint8_t *base = m.rom_banks + (uint32_t)bank * 0x4000;
    for (int p = 0; p < 0x40; p++)
        m.cpu.rd_page[0x80 + p] = base + p * 256;
}

static void C1943_HOT(push_event)(uint8_t kind, uint8_t value)
{
    if (m.nevents >= C1943_MAX_EVENTS)
        return;
    int32_t cycle = m.line * C1943_MAIN_PER_LINE + (m.slice_start - m.cpu.icount);
    if (cycle < 0)
        cycle = 0;
    else if (cycle >= C1943_MAIN_PER_FRAME)
        cycle = C1943_MAIN_PER_FRAME - 1;
    c1943_event_t *e = &m.events[m.nevents++];
    e->cycle = (uint32_t)cycle;
    e->kind = kind;
    e->value = value;
}

static uint8_t C1943_HOT(port_player)(uint32_t in)
{
    return (uint8_t)~(in & 0x3f);
}

static uint8_t C1943_HOT(m_read)(z80_t *cpu, uint16_t addr)
{
    (void)cpu;
    switch (addr)
    {
    case 0xc000:
    {
        uint8_t v = 0xf7;
        if (m.inputs & C1943_IN_START1)
            v &= ~0x01;
        if (m.inputs & C1943_IN_START2)
            v &= ~0x02;
        if (m.line < 16 || m.line >= 240)
            v |= 0x08;
        if (m.inputs & C1943_IN_COIN1)
            v &= ~0x40;
        if (m.inputs & C1943_IN_COIN2)
            v &= ~0x80;
        return v;
    }
    case 0xc001:
        return port_player(m.inputs);
    case 0xc002:
        return port_player(m.inputs >> 8);
    case 0xc003:
        return DSWA;
    case 0xc004:
        return DSWB;
    case 0xc007:
    {
        const uint8_t v = prot_table[m.prot_cmd];
        m.stats.prot_reads++;
        if (!v)
            m.stats.prot_misses++;
        return v;
    }
    case 0xd800:
    case 0xd801:
    case 0xd802:
    case 0xd803:
    case 0xd804:
        return m.scroll[addr - 0xd800];
    }
    return 0; /* unmapped */
}

static void C1943_HOT(m_write)(z80_t *cpu, uint16_t addr, uint8_t value)
{
    (void)cpu;
    switch (addr)
    {
    case 0xc800:
        push_event(C1943_EV_LATCH, value);
        break;
    case 0xc804:
    {
        set_bank((value & 0x1c) >> 2);
        const uint8_t reset = (value >> 5) & 1;
        if (reset != m.sound_reset)
        {
            m.sound_reset = reset;
            push_event(C1943_EV_RESET, reset);
        }
        m.control = value;
        break;
    }
    case 0xc806:
        /* the 86S105 DMA: the sprite RAM to the buffer that is drawn */
        for (int i = 0; i < 128; i++)
            copy_words(&m.spritebuf[i * 4], &m.spriteram[i * 32], 4);
        break;
    case 0xc807:
        m.prot_cmd = value;
        m.stats.prot_writes++;
        break;
    case 0xd800:
    case 0xd801:
    case 0xd802:
    case 0xd803:
    case 0xd804:
        m.scroll[addr - 0xd800] = value;
        break;
    case 0xd806:
        m.layers = value;
        break;
    }
}

static uint8_t m_in(z80_t *cpu, uint16_t port)
{
    (void)cpu;
    (void)port;
    return 0xff;
}

static void m_out(z80_t *cpu, uint16_t port, uint8_t value)
{
    (void)cpu;
    (void)port;
    (void)value;
}

static void map_main(void)
{
    z80_t *cpu = &m.cpu;
    for (int p = 0; p < 256; p++)
    {
        cpu->rd_page[p] = NULL;
        cpu->wr_page[p] = NULL;
    }
    for (int p = 0; p < 0x80; p++)
        cpu->rd_page[p] = m.rom_main + p * 256;
    for (int p = 0; p < 8; p++) /* d000-d7ff */
        cpu->rd_page[0xd0 + p] = cpu->wr_page[0xd0 + p] = m.vram + p * 256;
    for (int p = 0; p < 0x10; p++)
    {
        cpu->rd_page[0xe0 + p] = cpu->wr_page[0xe0 + p] = m.ram + p * 256;
        cpu->rd_page[0xf0 + p] = cpu->wr_page[0xf0 + p] = m.spriteram + p * 256;
    }
    cpu->read = m_read;
    cpu->write = m_write;
    cpu->in = m_in;
    cpu->out = m_out;
    m.bank = 0xff;
    set_bank(0);
    m.stats.bank_switches = 0;
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------ */

static const uint8_t sched_code[32];
#define SCHED_START 0x00b6

void c1943_init(c1943_fmt_t fmt, int samplerate)
{
    m.fmt = fmt;
    m.samplerate = samplerate;
    m.set = -1;
}

/* A copy of a part in `fast` memory, or the part itself. */
static const void *keep(const uint8_t *part, uint32_t size, c1943_alloc_fn fast)
{
    void *p = fast ? fast(size) : NULL;
    if (!p)
        return part;
    memcpy(p, part, size);
    m.fast_bytes += size;
    return p;
}

int c1943_start(c1943_set_id_t set, const uint8_t *const *parts, c1943_alloc_fn fast)
{
    if (set < 0 || set >= C1943_SET_COUNT)
        return 0;
    m.fast_bytes = 0;
    /* in order of how much they are read: the CPUs' code, the characters,
     * the tile maps (random reads while drawing, the worst for a cache) */
    m.rom_main = (const uint8_t *)keep(parts[C1943_P_MAIN], c1943_part_size[C1943_P_MAIN], fast);
    const uint8_t *rom_sound = (const uint8_t *)keep(parts[C1943_P_SOUND], c1943_part_size[C1943_P_SOUND], fast);
    m.chars = (const uint16_t *)keep(parts[C1943_P_CHARS], c1943_part_size[C1943_P_CHARS], fast);
    m.tilemap = (const uint8_t *)keep(parts[C1943_P_TILEMAP], c1943_part_size[C1943_P_TILEMAP], fast);
    m.rom_banks = parts[C1943_P_BANKS];
    m.bg = (const uint32_t *)parts[C1943_P_BG];
    m.bg2 = (const uint32_t *)parts[C1943_P_BG2];
    m.sprites = (const uint32_t *)parts[C1943_P_SPRITES];
    m.proms = parts[C1943_P_PROMS];

    m.sched_ok = !memcmp(m.rom_main + SCHED_START, sched_code, sizeof(sched_code));

    c1943_video_init(m.proms);
    c1943_sound_start(rom_sound);
    m.set = set;
    c1943_reset();
    return 1;
}

int c1943_running(void)
{
    return m.set;
}

uint32_t c1943_fast_bytes(void)
{
    return m.fast_bytes;
}

const c1943_stats_t *c1943_stats(void)
{
    m.stats.main_pc = m.cpu.pc.w;
    return &m.stats;
}

void c1943_reset(void)
{
    if (m.set < 0)
        return;
    memset(m.ram, 0, sizeof(m.ram));
    memset(m.spriteram, 0, sizeof(m.spriteram));
    memset(m.vram, 0, sizeof(m.vram));
    memset(m.spritebuf, 0, sizeof(m.spritebuf));
    memset(m.scroll, 0, sizeof(m.scroll));
    memset(&m.video, 0, sizeof(m.video));
    m.prot_cmd = 0;
    m.nevents = 0;
    z80_init(&m.cpu);
    z80_reset(&m.cpu);
    map_main();
    /* these latches are cleared at reset (MAME's machine_reset) */
    m.control = 0;
    m.layers = 0;
    m.sound_reset = 0;
    memset(&m.stats, 0, sizeof(m.stats));
}

/* ---------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------ */

static void C1943_HOT(latch_video)(void)
{
    c1943_video_t *v = &m.video;
    copy_words(v->vram, m.vram, sizeof(v->vram));
    copy_words(v->spr, m.spritebuf, sizeof(v->spr));
    v->bg_scrollx = (uint16_t)(m.scroll[0] | (m.scroll[1] << 8));
    v->bg_scrolly = m.scroll[2];
    v->bg2_scrollx = (uint16_t)(m.scroll[3] | (m.scroll[4] << 8));
    v->layers = m.layers;
    v->chars_on = m.control & 0x80;
}

/* The main CPU's task scheduler (00b7-00d6, the same in every set): it scans
 * the 16 task slots at ef80, every eighth byte, for one in state 4 or more, and
 * starts over when the flag at ef86 is set. Once a scan comes up empty, only an
 * interrupt handler can change that: nothing else writes the main CPU's RAM.
 * Emulating that loop costs as much as real work, so while it would only spin
 * its cycles are counted instead of run (as pico-galagino does for 1942's).
 * Checked against the code itself, so a set that differs runs it in full. */
static const uint8_t sched_code[32] = {
    0xaf, 0x32, 0x86, 0xef, 0x21, 0x80, 0xef, 0x11, 0x08, 0x00, 0x01, 0x00, 0x10, 0xf3, 0x3a, 0x86,
    0xef, 0xb7, 0x20, 0xec, 0x7e, 0xfe, 0x04, 0x30, 0x07, 0xfb, 0x19, 0x0c, 0x10, 0xef, 0x18, 0xe0,
};
#define SCHED_END (SCHED_START + (int)sizeof(sched_code))

static int C1943_HOT(main_idle)(const z80_t *cpu)
{
    if (!m.sched_ok || cpu->irq_state || cpu->pc.w < SCHED_START || cpu->pc.w >= SCHED_END || cpu->sp.w != 0xef80 ||
        m.ram[0xf86])
        return 0;
    for (int i = 0; i < 16; i++)
        if (m.ram[0xf80 + 8 * i] >= 4)
            return 0;
    return 1;
}

/* A CPU halted with no interrupt to take, or idle in the scheduler, would
 * only spin: the cycles are counted instead of run. Interrupts are raised
 * only between slices, so one cannot come in the middle. */
static void C1943_HOT(run_main)(int cycles)
{
    z80_t *cpu = &m.cpu;
    if ((cpu->halt && !cpu->irq_state) || main_idle(cpu))
    {
        cpu->icount += cycles;
        if (cpu->icount > 0)
            cpu->icount = 0;
        m.stats.main_idle_cycles += cycles;
        return;
    }
    m.slice_start = cpu->icount + cycles;
    z80_run(cpu, cycles);
}

void C1943_HOT(c1943_run_frame)(uint32_t inputs)
{
    if (m.set < 0)
        return;
    m.inputs = inputs;
    m.nevents = 0;
    m.stats.main_idle_cycles = 0;
    C1943_PROF_START(t0);
    for (int line = 0; line < C1943_LINES; line++)
    {
        m.line = line;
        if (line == 144 || line == 240)
            z80_hold_irq(&m.cpu, 0xff);
        if (line == 240)
            latch_video(); /* MAME draws the screen at the start of vblank */
        run_main(C1943_MAIN_PER_LINE);
    }
    C1943_PROF_ADD(0, t0);
}

int c1943_take_events(const c1943_event_t **events)
{
    *events = m.events;
    return m.nevents;
}
