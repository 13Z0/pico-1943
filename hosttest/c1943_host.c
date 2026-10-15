/*
 * Host harness for the 1943 core: loads a set through the same loader as the
 * firmware, runs it, dumps frames as PPM and the audio as WAV, or plays it in
 * a window (live mode).
 *
 *   ./hosttest/c1943_host <romroot> [1943|1943kai] <frames> <dump-every-N> [outdir] [options]
 *
 * <romroot> holds the set folders as on the SD card (1943, 1943KAI); without
 * a set the first complete one is run, 1943 before 1943 Kai.
 *
 * Options:
 *   --tate 0|1|2   Tate mode: 0 Off (default), 1 Bottom left, 2 Bottom right
 *   --tall         Tate off as HSTX boards show it: every row of the picture,
 *                  scaled to 480 lines by the driver's code (hstx_scale.h)
 *   --variant N    1943: the regional variant (0 US Rev C, 1 Euro, 2 Japan
 *                  Rev B, 3 US); by default the first one found
 *   --wav FILE     write the audio (mono, 16-bit, 44.1 kHz)
 *   --coin F       insert a coin at frame F (held for 6 frames)
 *   --start F      press 1P start at frame F (held for 6 frames)
 *   --play F       from frame F: fire, steer around, now and then button 2
 *   --keys F:MASK  hold inputs MASK (C1943_IN_*, hex) from frame F for 6 frames
 *   --video FILE   every frame, as raw RGB24 (320x240, with --tall 640x480),
 *                  for ffmpeg -f rawvideo -pix_fmt rgb24 -s WxH -r 60 -i FILE
 *   --no-psram     run as a board without PSRAM: from the flash store (needs
 *                  --flash)
 *   --flash FILE   the flash store, kept in FILE between runs: a set not in it
 *                  is copied into it first, as the board does after its restart,
 *                  and runs from it. The simulated flash checks alignment and
 *                  only clears bits when programmed, as NOR flash does.
 *   --flash-kb N   the size of that flash (default 2048: two sets)
 *   --ymlog FILE   log every write to the YM2203s (for hosttest/ym_compare)
 *   --only fm|ssg  hear only the FM or only the SSG half of the chips
 *   --trace F      print the CPUs' PCs at the end of frames F..F+59 (to find
 *                  their idle loops)
 *
 * Every dumped frame is outdir/frame_NNNNN.ppm (320x240, the canvas the board
 * shows; with --tall the 640x480 the HSTX driver sends). A summary line per
 * second reports the time spent per frame and what the machine did.
 *
 * Live mode (built with SDL2, see build.sh):
 *
 *   ./hosttest/c1943_host <romroot> [1943|1943kai] [frames] [dump-every-N] [outdir] --live [options]
 *
 * shows the frames in a window, plays the sound and reads the keyboard, in
 * real time. Without a frame count it runs until the window is closed; frames
 * are dumped with F12, and every Nth when N is given. Scripted input is ORed
 * with the keyboard, and --wav and --video record what was played.
 *   --scale N      window size, N times the picture (default 3, with --tall 2)
 *   --mute         no sound output; paced on a timer
 * The keys are those of a USB keyboard on the board, mapped as on the board
 * (c1943_input.c): arrows joystick, Z (or C) button 1 (fire), X (or V) button
 * 2, A coin (when released), S start; also MAME's 5 coin, 1 start, 2 two
 * player start. A+S resets the game (the board opens its settings menu).
 * Space pauses, N steps a frame while paused, Tab (held) fast-forwards, F12
 * dumps the frame on screen, Esc quits. With --tate 1 or 2 the window shows
 * the picture upright; the dumps keep the canvas as it is.
 */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "c1943.h"
#include "c1943_input.h"
#include "c1943_store.h"
#include "hstx_scale.h"
#if HOST_SDL
#include "host_sdl.h"
#endif

#define SAMPLERATE 44100
#define SAMPLES_PER_FRAME 735
#define MAX_KEYS 64
#define TALL_W 256 /* the tall canvas of main.cpp */

#if C1943_FRAME_STATS
/* the core's profiling (c1943_internal.h): nanoseconds here */
extern uint32_t c1943_prof[8];
uint32_t c1943_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000000ull + ts.tv_nsec);
}
#endif

/* --ymlog: every write to the YM2203s, for hosttest/ym_compare. A line per
 * write: the output sample it lands on, the chip, the port, the value. */
extern void (*c1943_ym_log)(int chip, int a0, uint8_t value, int sample);
extern int c1943_snd_mask;
static FILE *ymlog;
static uint32_t ymlog_base; /* samples of the frames before this one */
static void ymlog_write(int chip, int a0, uint8_t value, int sample)
{
    fprintf(ymlog, "%u %d %d %u\n", ymlog_base + (uint32_t)sample, chip, a0, value);
}

/* The machine's data is kept until the end: no leak reports for it. */
const char *__asan_default_options(void)
{
    return "detect_leaks=0";
}

/* --- stdio implementation of c1943_io_t ------------------------------------ */

static int host_list_dir(void *ctx, const char *dir, c1943_dir_cb cb, void *arg)
{
    (void)ctx;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        cb(arg, e->d_name, (uint32_t)st.st_size, S_ISDIR(st.st_mode));
    }
    closedir(d);
    return 1;
}

static void *host_open(void *ctx, const char *path)
{
    (void)ctx;
    return fopen(path, "rb");
}

static uint32_t host_size(void *ctx, void *f)
{
    (void)ctx;
    long cur = ftell((FILE *)f);
    fseek((FILE *)f, 0, SEEK_END);
    long n = ftell((FILE *)f);
    fseek((FILE *)f, cur, SEEK_SET);
    return (uint32_t)n;
}

static int host_read_at(void *ctx, void *f, uint32_t ofs, void *buf, uint32_t len)
{
    (void)ctx;
    if (fseek((FILE *)f, ofs, SEEK_SET) != 0)
        return -1;
    return (int)fread(buf, 1, len, (FILE *)f);
}

static void host_close(void *ctx, void *f)
{
    (void)ctx;
    fclose((FILE *)f);
}

static const c1943_io_t io = {NULL, host_list_dir, host_open, host_size, host_read_at, host_close, NULL, NULL};

static void *host_alloc(uint32_t size)
{
    return malloc(size);
}

/* --- a simulated flash for the store ---------------------------------------- */

static uint32_t FLASH_SIZE = 2048 * 1024;
static uint8_t *flash_mem;
static uint32_t flash_erased, flash_programmed;
static int flash_errors;

static void sim_erase(uint32_t ofs, uint32_t len)
{
    if ((ofs | len) % 4096 || ofs + len > FLASH_SIZE)
    {
        flash_errors++;
        return;
    }
    memset(flash_mem + ofs, 0xff, len);
    flash_erased += len;
}

static void sim_program(uint32_t ofs, const uint8_t *data, uint32_t len)
{
    if ((ofs | len) % 256 || ofs + len > FLASH_SIZE)
    {
        flash_errors++;
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        flash_mem[ofs + i] &= data[i]; /* NOR: programming only clears bits */
    flash_programmed += len;
}

static void sim_copying(int set)
{
    static int told = -1;
    if (told != set)
        printf("flash store: copying %s\n", c1943_sets[set].title);
    told = set;
}

/* The board's SRAM for what the core keeps there: counted, so the harness
 * reports what the board would need. */
static uint32_t fast_total;
static void *fast_alloc(uint32_t size)
{
    fast_total += size;
    return malloc(size);
}

/* --- output helpers ------------------------------------------------------ */

static uint32_t rgb565(uint16_t p)
{
    return (uint32_t)(((p >> 11) & 0x1f) * 255 / 31) << 16 | (uint32_t)(((p >> 5) & 0x3f) * 255 / 63) << 8 |
           (uint32_t)((p & 0x1f) * 255 / 31);
}

static uint32_t rgb555(uint16_t p)
{
    return (uint32_t)(((p >> 10) & 0x1f) * 255 / 31) << 16 | (uint32_t)(((p >> 5) & 0x1f) * 255 / 31) << 8 |
           (uint32_t)((p & 0x1f) * 255 / 31);
}

/* --tall: a line of the TALL_W x 256 RGB555 canvas as the HSTX driver sends
 * it, 640 wide: the rows scaled to 480 lines, the pixels doubled between black
 * borders. */
static void tall_line(const uint16_t *fb, int rows, uint32_t line, uint32_t *out)
{
    const int pairs = TALL_W / 2, border = (640 - 2 * TALL_W) / 2;
    uint32_t q, a[TALL_W / 2], b[TALL_W / 2];
    uint16_t px[TALL_W];
    const uint32_t row = hstx_scale_row(line, (uint32_t)rows, 480, &q);
    memcpy(a, fb + row * TALL_W, sizeof(a));
    if (q)
    {
        memcpy(b, fb + (row + 1) * TALL_W, sizeof(b));
        hstx_scale_blend(a, a, b, pairs, q);
    }
    memcpy(px, a, sizeof(px));
    for (int x = 0; x < 640; x++)
        out[x] = (x < border || x >= 640 - border) ? 0 : rgb555(px[(x - border) / 2]);
}

/* The picture as 0xRRGGBB: the 320x240 RGB565 canvas, or with --tall the
 * 640x480 the HSTX driver makes of its canvas */
static void to_rgb32(const uint16_t *fb, int tall, uint32_t *out)
{
    if (tall)
        for (uint32_t line = 0; line < 480; line++)
            tall_line(fb, C1943_H, line, out + line * 640);
    else
        for (int i = 0; i < 320 * 240; i++)
            out[i] = rgb565(fb[i]);
}

static void put_rgb(FILE *f, const uint16_t *fb, int tall)
{
    static uint32_t px[640 * 480];
    static uint8_t rgb[640 * 480 * 3];
    const int n = tall ? 640 * 480 : 320 * 240;
    to_rgb32(fb, tall, px);
    for (int i = 0; i < n; i++)
    {
        rgb[3 * i] = (uint8_t)(px[i] >> 16);
        rgb[3 * i + 1] = (uint8_t)(px[i] >> 8);
        rgb[3 * i + 2] = (uint8_t)px[i];
    }
    fwrite(rgb, 3, n, f);
}

static void dump_frame(const char *outdir, int frame, const uint16_t *fb, int tall)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", outdir, frame);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", tall ? 640 : 320, tall ? 480 : 240);
    put_rgb(f, fb, tall);
    fclose(f);
}

static void put16(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f);
    fputc(v >> 8, f);
}

static void put32(FILE *f, uint32_t v)
{
    put16(f, v & 0xffff);
    put16(f, v >> 16);
}

static void wav_header(FILE *f, uint32_t samples)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + samples * 2);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, SAMPLERATE);
    put32(f, SAMPLERATE * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, samples * 2);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int set_by_name(const char *s)
{
    for (int i = 0; i < C1943_SET_COUNT; i++)
        if (!strcasecmp(s, c1943_sets[i].folder) || !strcasecmp(s, c1943_sets[i].mameset))
            return i;
    return -1;
}

/* --- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* the positional arguments come before the options */
    const char *pos[5] = {NULL};
    int npos = 0, a = 1;
    for (; a < argc && argv[a][0] != '-'; a++)
    {
        if (npos == 5)
        {
            fprintf(stderr, "unexpected argument %s\n", argv[a]);
            return 1;
        }
        pos[npos++] = argv[a];
    }
    /* the set is optional: a number there is the frame count */
    if (npos >= 2 && isdigit((unsigned char)pos[1][0]) && set_by_name(pos[1]) < 0)
    {
        for (int i = npos; i > 1; i--)
            pos[i] = pos[i - 1];
        pos[1] = NULL;
        npos++;
    }
    int tate = 0, tall = 0, coin_at = -1, start_at = -1, play_at = -1, live = 0, scale = 0, mute = 0;
    int variant = -1, trace_at = -1, no_psram = 0;
    const char *flash_path = NULL;
    const char *video_path = NULL, *wav_path = NULL;
    int key_at[MAX_KEYS], nkeys = 0;
    uint32_t key_mask[MAX_KEYS];
    for (; a < argc; a++)
    {
        if (!strcmp(argv[a], "--live"))
            live = 1;
        else if (!strcmp(argv[a], "--scale") && a + 1 < argc)
            scale = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--mute"))
            mute = 1;
        else if (!strcmp(argv[a], "--tate") && a + 1 < argc)
            tate = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--tall"))
            tall = 1;
        else if (!strcmp(argv[a], "--variant") && a + 1 < argc)
            variant = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--wav") && a + 1 < argc)
            wav_path = argv[++a];
        else if (!strcmp(argv[a], "--coin") && a + 1 < argc)
            coin_at = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--start") && a + 1 < argc)
            start_at = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--play") && a + 1 < argc)
            play_at = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--video") && a + 1 < argc)
            video_path = argv[++a];
        else if (!strcmp(argv[a], "--no-psram"))
            no_psram = 1;
        else if (!strcmp(argv[a], "--flash") && a + 1 < argc)
            flash_path = argv[++a];
        else if (!strcmp(argv[a], "--flash-kb") && a + 1 < argc)
            FLASH_SIZE = (uint32_t)atoi(argv[++a]) * 1024;
        else if (!strcmp(argv[a], "--only") && a + 1 < argc)
        {
            a++;
            c1943_snd_mask = !strcmp(argv[a], "fm") ? 1 : !strcmp(argv[a], "ssg") ? 2 : 3;
        }
        else if (!strcmp(argv[a], "--ymlog") && a + 1 < argc)
        {
            ymlog = fopen(argv[++a], "w");
            if (ymlog)
                c1943_ym_log = ymlog_write;
        }
        else if (!strcmp(argv[a], "--trace") && a + 1 < argc)
            trace_at = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--keys") && a + 1 < argc && nkeys < MAX_KEYS)
        {
            if (sscanf(argv[++a], "%d:%x", &key_at[nkeys], &key_mask[nkeys]) == 2)
                nkeys++;
        }
        else
        {
            fprintf(stderr, "unknown option %s\n", argv[a]);
            return 1;
        }
    }
#if !HOST_SDL
    (void)scale;
    (void)mute;
    if (live)
    {
        fprintf(stderr,
                "%s: built without SDL2, so --live is not available.\n"
                "Install libsdl2-dev and rerun hosttest/build.sh.\n",
                argv[0]);
        return 1;
    }
#endif
    if (npos < (live ? 1 : 4))
    {
        fprintf(stderr,
                "usage: %s <romroot> [1943|1943kai] <frames> <dump-every-N> [outdir] [options]\n"
                "       %s <romroot> [1943|1943kai] [frames] [dump-every-N] [outdir] --live [options]\n",
                argv[0], argv[0]);
        return 1;
    }
    const char *romroot = pos[0];
    int set = pos[1] ? set_by_name(pos[1]) : -1;
    if (pos[1] && set < 0)
    {
        fprintf(stderr, "unknown set %s\n", pos[1]);
        return 1;
    }
    /* live: until the window is closed, frames dumped with F12 */
    const int frames = pos[2] ? atoi(pos[2]) : INT_MAX;
    const int every = pos[3] ? atoi(pos[3]) : 0;
    const char *outdir = pos[4] ? pos[4] : "hosttest/out";

    /* --tall blends RGB555, as the HSTX driver does */
    c1943_init(tall ? C1943_FMT_RGB555 : C1943_FMT_RGB565, SAMPLERATE);

    /* what is on the "card" */
    char dir[1024];
    c1943_files_t found[C1943_SET_COUNT];
    for (int s = 0; s < C1943_SET_COUNT; s++)
    {
        const c1943_set_info_t *si = &c1943_sets[s];
        snprintf(dir, sizeof(dir), "%s/%s", romroot, si->folder);
        found[s] = c1943_probe((c1943_set_id_t)s, &io, dir);
        const int v = c1943_variant_found((c1943_set_id_t)s, found[s]);
        printf("%-28s %2d of %2d files: %s%s%s\n", si->title, __builtin_popcountll(found[s]), si->nfiles,
               v >= 0 ? "complete, " : "incomplete", v >= 0 ? si->variant_set[v] : "",
               v >= 0 ? "" : "");
        for (int w = 0; w < si->nvariants; w++)
            if (c1943_variant_found((c1943_set_id_t)s, found[s] & c1943_variant_files((c1943_set_id_t)s, w)) == w)
                printf("  variant %d %-8s (%s) complete\n", w, si->variant_set[w], si->variant_name[w]);
        if (set < 0 && v >= 0)
            set = s;
    }
    if (set < 0)
    {
        fprintf(stderr, "no complete set under %s\n", romroot);
        return 2;
    }
    const c1943_set_info_t *si = &c1943_sets[set];
    if (variant < 0)
        variant = c1943_variant_found((c1943_set_id_t)set, found[set]);
    if (variant < 0 || variant >= si->nvariants)
    {
        const int near = c1943_variant_nearest((c1943_set_id_t)set, found[set]);
        const c1943_files_t missing = c1943_variant_files((c1943_set_id_t)set, near) & ~found[set];
        printf("%s is incomplete; for %s these are missing:\n", si->title, si->variant_set[near]);
        for (int i = 0; i < si->nfiles; i++)
            if (missing & ((c1943_files_t)1 << i))
                printf("  %s (crc %08x)\n", si->files[i].name, si->files[i].crc);
        return 2;
    }

    snprintf(dir, sizeof(dir), "%s/%s", romroot, si->folder);
    const uint8_t *parts[C1943_P_COUNT];
    double t0 = now_s();
    if (no_psram)
    {
        /* the board without PSRAM: from the flash store, copied there first
         * when it does not hold the set (the board restarts for that) */
        if (!flash_path)
        {
            fprintf(stderr, "--no-psram needs --flash FILE\n");
            return 1;
        }
        flash_mem = malloc(FLASH_SIZE);
        memset(flash_mem, 0xff, FLASH_SIZE);
        FILE *f = fopen(flash_path, "rb");
        if (f)
        {
            if (fread(flash_mem, 1, FLASH_SIZE, f) != FLASH_SIZE)
                printf("flash store: %s is short, the rest reads erased\n", flash_path);
            fclose(f);
        }
        const c1943_flash_t fl = {flash_mem, 0, FLASH_SIZE, sim_erase, sim_program, sim_copying};
        c1943_store_open(&fl);
        printf("flash store: %u KB, %d slot(s) of %u KB; 1943 %d, 1943 Kai %d\n", FLASH_SIZE / 1024,
               c1943_store_slots(), c1943_store_slot_size() / 1024, c1943_store_variant(C1943_SET_1943),
               c1943_store_variant(C1943_SET_1943KAI));
        if (c1943_store_variant((c1943_set_id_t)set) != variant)
        {
            static uint8_t temp[C1943_STORE_TEMP];
            flash_erased = flash_programmed = 0;
            c1943_store_copy((c1943_set_id_t)set, variant, &io, dir, temp, sizeof(temp));
            printf("flash store: %s, %u KB erased, %u KB programmed, %d errors, %.0f ms\n",
                   c1943_store_variant((c1943_set_id_t)set) == variant ? "copied" : "COPY FAILED", flash_erased / 1024,
                   flash_programmed / 1024, flash_errors, (now_s() - t0) * 1000);
            f = fopen(flash_path, "wb");
            if (f)
            {
                fwrite(flash_mem, 1, FLASH_SIZE, f);
                fclose(f);
            }
        }
        if (!c1943_store_parts((c1943_set_id_t)set, parts))
        {
            printf("%s: not in the flash store\n", si->title);
            return 2;
        }
    }
    else
    {
        const c1943_files_t got = c1943_load((c1943_set_id_t)set, variant, &io, dir, host_alloc, free, parts);
        if (got != c1943_variant_files((c1943_set_id_t)set, variant))
        {
            printf("%s (%s): loading failed\n", si->title, si->variant_set[variant]);
            return 2;
        }
    }
    const double t_load = now_s() - t0;
    t0 = now_s();
    if (!c1943_start((c1943_set_id_t)set, parts, fast_alloc))
    {
        printf("start failed\n");
        return 2;
    }
    printf("%s (%s, %s): loaded and converted in %.0f ms, started in %.1f ms; %u bytes kept in SRAM\n", si->title,
           si->variant_set[variant], si->variant_name[variant], t_load * 1000, (now_s() - t0) * 1000,
           c1943_fast_bytes());
    (void)fast_total;

    mkdir(outdir, 0755);

    FILE *wav = NULL;
    if (wav_path)
    {
        wav = fopen(wav_path, "wb");
        if (wav)
            wav_header(wav, 0);
    }
    FILE *video = video_path ? fopen(video_path, "wb") : NULL;

    static uint16_t fb[320 * 240];
    int16_t audio[SAMPLES_PER_FRAME];
    uint32_t total_samples = 0;
    double t_emu = 0, t_snd = 0, t_render = 0, t_emu_max = 0, t_snd_max = 0, t_render_max = 0;
    int peak = 0;

    const c1943_orient_t orient = tall ? C1943_ORIENT_UPRIGHT_FULL : (c1943_orient_t)tate;
    const int stride = tall ? TALL_W : 320;
    c1943_render_borders(orient, fb, stride);

#if HOST_SDL
    static uint32_t px[640 * 480];
    if (live)
    {
        const int turn = tall ? 0 : tate == C1943_ORIENT_TATE_CW ? 90 : tate == C1943_ORIENT_TATE_CCW ? 270 : 0;
        if (!hsdl_init(tall ? 640 : 320, tall ? 480 : 240, turn, scale ? scale : tall ? 2 : 3, !mute, 16667))
            return 1;
    }
#endif

    c1943_pads_t pads;
    c1943_pads_reset(&pads);
    pads.wait_release = 0;
    uint32_t pad = 0; /* live: the keyboard */
    int quit = 0;
    int max_events = 0; /* the most latch writes in a frame (C1943_MAX_EVENTS) */
    for (int f = 0; f < frames && !quit; f++)
    {
        int settings = 0, fps = 0;
        uint32_t in = c1943_pads_map(&pads, pad, 0, 0, 0, &settings, &fps);
        if (settings)
        {
            printf("frame %d: SELECT+START: reset\n", f);
            c1943_reset();
            c1943_sound_reset();
            c1943_pads_reset(&pads);
        }
        if (coin_at >= 0 && f >= coin_at && f < coin_at + 6)
            in |= C1943_IN_COIN1;
        if (start_at >= 0 && f >= start_at && f < start_at + 6)
            in |= C1943_IN_START1;
        for (int k = 0; k < nkeys; k++)
            if (f >= key_at[k] && f < key_at[k] + 6)
                in |= key_mask[k];
        if (play_at >= 0 && f >= play_at)
        {
            static const uint32_t dirs[4] = {C1943_IN_LEFT, C1943_IN_UP, C1943_IN_RIGHT, C1943_IN_DOWN};
            const int p = f - play_at;
            if ((p & 15) < 4)
                in |= C1943_IN_B1;
            if ((p % 600) < 4)
                in |= C1943_IN_B1 | C1943_IN_B2;
            in |= dirs[(p / 45) & 3];
        }

        double ta = now_s();
        c1943_run_frame(in);
        double tb = now_s();
        const c1943_event_t *ev;
        const int nev = c1943_take_events(&ev);
        if (nev > max_events)
            max_events = nev;
        c1943_sound_frame(ev, nev, audio, SAMPLES_PER_FRAME);
        ymlog_base += SAMPLES_PER_FRAME;
        double tc = now_s();
        c1943_render(orient, fb, stride);
        double td = now_s();
        t_emu += tb - ta;
        t_snd += tc - tb;
        t_render += td - tc;
        if (tb - ta > t_emu_max)
            t_emu_max = tb - ta;
        if (tc - tb > t_snd_max)
            t_snd_max = tc - tb;
        if (td - tc > t_render_max)
            t_render_max = td - tc;

        for (int i = 0; i < SAMPLES_PER_FRAME; i++)
        {
            int v = audio[i] < 0 ? -audio[i] : audio[i];
            if (v > peak)
                peak = v;
        }
        if (wav)
        {
            fwrite(audio, sizeof(int16_t), SAMPLES_PER_FRAME, wav);
            total_samples += SAMPLES_PER_FRAME;
        }
        if (video)
            put_rgb(video, fb, tall);
        if (every > 0 && (f % every) == 0)
            dump_frame(outdir, f, fb, tall);

#if HOST_SDL
        if (live)
        {
            to_rgb32(fb, tall, px);
            hsdl_present(px);
            for (;;)
            {
                int evs = 0;
                pad = hsdl_poll(&evs);
                if (evs & HSDL_EV_DUMP)
                {
                    dump_frame(outdir, f, fb, tall);
                    printf("LIVE: frame %d dumped to %s/frame_%05d.ppm\n", f, outdir, f);
                }
                if (evs & HSDL_EV_QUIT)
                {
                    quit = 1;
                    break;
                }
                hsdl_status(si->title, f);
                if (!hsdl_paused() || (evs & HSDL_EV_STEP))
                    break;
                hsdl_idle();
            }
            if (!quit)
            {
                hsdl_audio(audio, SAMPLES_PER_FRAME);
                hsdl_pace();
            }
        }
#endif
        if (trace_at >= 0 && f >= trace_at && f < trace_at + 60)
        {
            const c1943_stats_t *st = c1943_stats();
            printf("trace frame %d: main pc %04x, sound pc %04x\n", f, st->main_pc, st->sound_pc);
        }
        if ((f + 1) % 60 == 0)
        {
            const c1943_stats_t *st = c1943_stats();
            printf("frame %5d  main %.2f ms (max %.2f)  sound %.2f (max %.2f)  render %.2f (max %.2f)  peak %5d  "
                   "pc %04x/%04x  prot %u/%u miss %u  banks %u  idle %u/%u\n",
                   f + 1, t_emu * 1000 / 60, t_emu_max * 1000, t_snd * 1000 / 60, t_snd_max * 1000,
                   t_render * 1000 / 60, t_render_max * 1000, peak, st->main_pc, st->sound_pc, st->prot_writes,
                   st->prot_reads, st->prot_misses, st->bank_switches, st->main_idle_cycles, st->sound_idle_cycles);
#if C1943_FRAME_STATS
            printf("  parts us a frame: main %u, sound board %u (FM %u, SSG %u), draw %u\n", c1943_prof[0] / 60000,
                   c1943_prof[1] / 60000, c1943_prof[2] / 60000, c1943_prof[3] / 60000, c1943_prof[4] / 60000);
            memset(c1943_prof, 0, sizeof(c1943_prof));
#endif
            t_emu = t_snd = t_render = t_emu_max = t_snd_max = t_render_max = 0;
            peak = 0;
        }
    }

    printf("most latch writes in a frame: %d (room for %d)\n", max_events, C1943_MAX_EVENTS);
    if (wav)
    {
        wav_header(wav, total_samples);
        fclose(wav);
    }
    if (video)
        fclose(video);
    if (ymlog)
        fclose(ymlog);
#if HOST_SDL
    if (live)
        hsdl_quit();
#endif
    return 0;
}
