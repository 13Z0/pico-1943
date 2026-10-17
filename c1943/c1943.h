/*
 * 1943: The Battle of Midway (Capcom, 1987) and 1943 Kai: Midway Kaisen, the
 * emulated board, for the Frens family of RP2350 emulators.
 *
 * Plain C with no Pico dependencies: the same sources build for the board and
 * for the host harness in hosttest/. One set runs at a time.
 *
 * The board, after MAME's capcom/1943.cpp and 1943_v.cpp (Paul Leaman):
 *   - a Z80 at 6 MHz with banked program ROM, two scrolling layers of 32x32
 *     tiles from ROM tile maps, 8x8 characters and 128 sprites of 16x16;
 *   - a Z80 at 3 MHz with two YM2203s at 1.5 MHz, fed by a latch;
 *   - an i8751 that answers the main CPU's protection checks, replaced by the
 *     table of its answers.
 * The raster is 256x224 at 59.64 Hz, turned 270 degrees (ROT270): upright the
 * picture is 224 wide and 256 tall. Every coordinate in this API is upright.
 *
 * Video. The picture is drawn in bands into the canvas of the framework in one
 * of the orientations of c1943_orient_t, so that canvas rows are written top
 * to bottom: the canvas is single buffered and is rewritten ahead of the
 * scan-out. The frame drawn is the one the machine latched at its last vblank.
 *
 * Sound. The main CPU only ever talks to the sound board through the latch
 * (and its reset line), so the sound board runs a frame after the main CPU,
 * from the frame's list of timestamped latch writes (c1943_sound.c). On the
 * board that is either on the same core, right after the frame, or on core1.
 */
#ifndef C1943_CORE_H
#define C1943_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The upright picture. */
#define C1943_W 224
#define C1943_H 256

/* The canvas of the framework (Tate modes and PicoDVI). */
#define C1943_CANVAS_W 320
#define C1943_CANVAS_H 240

/* The sets. 1943 stands for MAME's 1943 and its regional clones, which differ
 * only in the three main CPU ROMs; 1943 Kai is a set of its own. */
typedef enum
{
    C1943_SET_1943 = 0,
    C1943_SET_1943KAI,
    C1943_SET_COUNT
} c1943_set_id_t;

/* The value of settings.flags.tateMode, plus C1943_ORIENT_UPRIGHT_FULL. As in
 * pico-galagino: the orientations are defined on the upright picture, so they
 * mean the same monitor position for every vertical game of the family. */
typedef enum
{
    C1943_ORIENT_UPRIGHT = 0,  /* Tate off: upright, 224x240 (1 row in 16 dropped) */
    C1943_ORIENT_TATE_CW = 1,  /* Bottom left: for a monitor turned clockwise, 256x224 */
    C1943_ORIENT_TATE_CCW = 2, /* Bottom right: the same turned 180 degrees */
    /* Not a setting: Tate off on a display that scales the rows itself
     * (HSTX). All 256 rows, on a canvas `stride` pixels wide and 256 rows
     * tall, the picture centred at x = (stride - 224) / 2. */
    C1943_ORIENT_UPRIGHT_FULL = 3
} c1943_orient_t;

/* Pixel format of the canvas: RGB555 on HSTX, RGB444 on PicoDVI, RGB565 on
 * the host. */
typedef enum
{
    C1943_FMT_RGB555 = 0,
    C1943_FMT_RGB444,
    C1943_FMT_RGB565
} c1943_fmt_t;

/* Inputs, active high; the bits of the board's ports. */
#define C1943_IN_RIGHT 0x01 /* P1 in bits 0..5, P2 the same in bits 8..13 */
#define C1943_IN_LEFT 0x02
#define C1943_IN_DOWN 0x04
#define C1943_IN_UP 0x08
#define C1943_IN_B1 0x10 /* fire */
#define C1943_IN_B2 0x20 /* special attack; with B1 a loop */
#define C1943_IN_P2(b) ((uint32_t)(b) << 8)
#define C1943_IN_START1 0x10000u
#define C1943_IN_START2 0x20000u
#define C1943_IN_COIN1 0x40000u
#define C1943_IN_COIN2 0x80000u

/* --- File access, implemented by the board (FatFs) and the host (stdio) --- */

typedef void (*c1943_dir_cb)(void *arg, const char *name, uint32_t size, int is_dir);

typedef struct
{
    void *ctx;
    /* Calls cb for every entry of dir; returns 0 when dir cannot be opened. */
    int (*list_dir)(void *ctx, const char *dir, c1943_dir_cb cb, void *arg);
    void *(*open)(void *ctx, const char *path);
    uint32_t (*size)(void *ctx, void *file);
    /* Returns the number of bytes read, or -1. */
    int (*read_at)(void *ctx, void *file, uint32_t offset, void *buf, uint32_t len);
    void (*close)(void *ctx, void *file);
    /* The loader's scratch memory, all freed before it returns: the list of a
     * folder's files (6.5 KB), a 4 KB read buffer and the inflate state (about
     * 8 KB), 19 KB at most. NULL for malloc and free. */
    void *(*alloc)(void *ctx, uint32_t size);
    void (*free)(void *ctx, void *p);
} c1943_io_t;

/* --- The ROM sets ----------------------------------------------------------- */

/* The regions the files are loaded into, as MAME's, except that the main
 * CPU's is split into the fixed half and the banks. */
typedef enum
{
    C1943_R_MAIN = 0, /* 0000-7fff, 32 KB */
    C1943_R_BANKS,    /* 8000-bfff: 8 banks of 16 KB */
    C1943_R_SOUND,    /* 32 KB */
    C1943_R_CHARS,    /* gfx1: 2048 8x8 characters, 2 bits */
    C1943_R_BG,       /* gfx2: 512 32x32 tiles of the front layer, 4 bits */
    C1943_R_BG2,      /* gfx3: 128 tiles of the back layer */
    C1943_R_SPRITES,  /* gfx4: 2048 16x16 sprites, 4 bits */
    C1943_R_TILEMAP,  /* the front layer's map, then the back layer's */
    C1943_R_PROMS,    /* colours and lookup tables, as MAME's region */
    C1943_R_COUNT
} c1943_region_t;

extern const uint32_t c1943_region_size[C1943_R_COUNT];

typedef struct
{
    const char *name; /* MAME's name, for messages */
    uint32_t crc;
    uint32_t size;
    uint8_t region;  /* c1943_region_t */
    uint32_t offset; /* in the region */
    /* 0: every variant of the set needs the file; else the variants (bit v)
     * whose own file it is */
    uint8_t variants;
} c1943_romfile_t;

#define C1943_MAX_VARIANTS 8

typedef struct
{
    const char *title;   /* "1943: The Battle of Midway" */
    const char *folder;  /* below /roms/arcade: "1943" */
    const char *mameset; /* "1943": the zip the set is usually found in */
    /* The variants in order of preference: MAME's set names ("1943u"), and
     * what is shown for each ("US, Rev C"). A set with one has its own name. */
    const char *variant_set[C1943_MAX_VARIANTS];
    const char *variant_name[C1943_MAX_VARIANTS];
    uint8_t nvariants;
    const c1943_romfile_t *files;
    uint8_t nfiles;
} c1943_set_info_t;

extern const c1943_set_info_t c1943_sets[C1943_SET_COUNT];

/* A mask of a set's files: bit i = files[i]. */
typedef uint64_t c1943_files_t;

/* The files of the set's variant v (all of the set's files that variant v
 * needs). */
c1943_files_t c1943_variant_files(c1943_set_id_t set, int v);

/* The first variant (in order of preference) whose files are all in found, or
 * -1. */
int c1943_variant_found(c1943_set_id_t set, c1943_files_t found);

/* The variant closest to complete: for listing the missing files. */
int c1943_variant_nearest(c1943_set_id_t set, c1943_files_t found);

/* The mask of the set's files present in dir (and dir/<mameset>, and
 * dir/<variant set> for an unzipped merged set), from the zip directories and
 * the CRCs of loose files, without loading anything. */
c1943_files_t c1943_probe(c1943_set_id_t set, const c1943_io_t *io, const char *dir);

/* --- The machine's data ---------------------------------------------------
 * What the machine runs from, once converted from the ROMs: the CPUs' ROMs as
 * they are, the graphics turned upright in the drawing formats, the tile maps
 * and the PROMs. Built in memory (PSRAM on the board, see c1943_load) or in
 * the flash store (c1943_store.c); the core copies what it reads all the time
 * into SRAM itself. */
typedef enum
{
    C1943_P_MAIN = 0, /* 32 KB, as the ROM */
    C1943_P_BANKS,    /* 128 KB, as the ROMs */
    C1943_P_SOUND,    /* 32 KB, as the ROM */
    C1943_P_CHARS,    /* 2048 characters, 8 rows of 16 bits (2 bits a pixel) */
    C1943_P_BG,       /* 512 tiles, 32 rows of 4 words (4 bits a pixel) */
    C1943_P_BG2,      /* 128 tiles, the same */
    C1943_P_SPRITES,  /* 2048 sprites, 16 rows of 2 words */
    C1943_P_TILEMAP,  /* 64 KB, as the ROMs */
    C1943_P_PROMS,    /* as the region */
    C1943_P_COUNT
} c1943_part_t;

extern const uint32_t c1943_part_size[C1943_P_COUNT];

/* Converts `count` elements of the raw region data into a part's format. lo
 * and hi point at the first element's data in the two halves of the region
 * (MAME's RGN_FRAC(0,2) and RGN_FRAC(1,2)); the flash store converts a pair of
 * ROMs at a time with these. */
void c1943_conv_chars(const uint8_t *rom, int count, uint16_t *out);
void c1943_conv_tiles(const uint8_t *lo, const uint8_t *hi, int count, uint32_t *out);
void c1943_conv_sprites(const uint8_t *lo, const uint8_t *hi, int count, uint32_t *out);

/* Loads the set's variant v from dir into memory from alloc (raw regions and
 * parts, about 1.5 MB; on the board PSRAM), and converts it. Fills parts[]
 * and returns the mask of files found; the parts are complete only when the
 * variant's files are all in it. The raw regions are freed again with
 * free_fn (which may be NULL to keep them). */
c1943_files_t c1943_load(c1943_set_id_t set, int v, const c1943_io_t *io, const char *dir, void *(*alloc)(uint32_t),
                         void (*free_fn)(void *), const uint8_t **parts);

/* The ROM loader for the flash store: loads the files in `want` from dir, each
 * to dest(arg, i) (i = its index in the set's files), which may return NULL to
 * skip one. Returns the mask of the wanted files found. */
c1943_files_t c1943_load_files(c1943_set_id_t set, c1943_files_t want, const c1943_io_t *io, const char *dir,
                               uint8_t *(*dest)(void *arg, int i), void *arg);

/* --- Life cycle ------------------------------------------------------------ */

/* Once, before anything else. */
void c1943_init(c1943_fmt_t fmt, int samplerate);

/* Memory the core keeps for good: the data the CPUs and the renderer read
 * all the time, copied out of the parts (SRAM on the board, about 170 KB).
 * NULL when it cannot be had: the parts are then read where they are. */
typedef void *(*c1943_alloc_fn)(uint32_t size);

/* Starts the machine on the parts (which must stay where they are). `fast`
 * allocates the memory the core keeps in SRAM. Returns 0 when that memory
 * could not be had for what has to be in it. */
int c1943_start(c1943_set_id_t set, const uint8_t *const *parts, c1943_alloc_fn fast);

/* The running set, or -1. */
int c1943_running(void);

/* Power-on reset. */
void c1943_reset(void);

/* --- Running ---------------------------------------------------------------
 * A frame is the main CPU's c1943_run_frame, then the sound board's frame from
 * the latch writes it made: c1943_sound_frame on the same core, or the same
 * function on another core with the events of c1943_take_events. */

/* Runs a frame of the main CPU with these inputs (C1943_IN_*). */
void c1943_run_frame(uint32_t inputs);

/* A latch write (or sound CPU reset) of the main CPU, at its cycle in the
 * frame (0..100607). */
typedef struct
{
    uint32_t cycle;
    uint8_t kind; /* C1943_EV_* */
    uint8_t value;
} c1943_event_t;

#define C1943_EV_LATCH 0
#define C1943_EV_RESET 1 /* value: 1 hold the sound CPU in reset, 0 release */
/* The game writes the latch once or twice a frame (more is dropped; the host
 * harness reports the most it saw). */
#define C1943_MAX_EVENTS 32

/* The events of the frame just run; they stay valid until the next
 * c1943_run_frame. */
int c1943_take_events(const c1943_event_t **events);

/* The sound board: a frame from those events, n mono samples at the rate
 * given to c1943_init. */
void c1943_sound_frame(const c1943_event_t *events, int nevents, int16_t *audio, int n);

/* The sound board's own reset (with c1943_reset when it runs elsewhere). */
void c1943_sound_reset(void);

/* --- Video ------------------------------------------------------------------ */

/* Draws the picture latched at the last vblank into the canvas (stride in
 * pixels). */
void c1943_render(c1943_orient_t orient, uint16_t *fb, int stride);

/* Paints everything outside the picture black. */
void c1943_render_borders(c1943_orient_t orient, uint16_t *fb, int stride);

/* --- Diagnostics ------------------------------------------------------------ */

typedef struct
{
    uint32_t prot_writes, prot_reads, prot_misses; /* the i8751's checks */
    uint32_t bank_switches;                        /* writes of c804 that changed the bank */
    uint16_t main_pc, sound_pc;
    uint32_t resets;                               /* times the main CPU went through 0000 */
    uint32_t main_idle_cycles, sound_idle_cycles;  /* cycles skipped waiting for an IRQ, last frame */
} c1943_stats_t;

const c1943_stats_t *c1943_stats(void);

/* The core's SRAM use: the bytes from `fast` at the last c1943_start. */
uint32_t c1943_fast_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
