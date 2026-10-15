/*
 * The ROM sets, from MAME's capcom/1943.cpp. The files are found by size and
 * CRC32 (romload.c), so their names are only for messages.
 *
 * 1943's regional sets differ only in the main CPU's three ROMs: those are
 * listed once per variant, the rest once for all. MAME's merged 1943.zip holds
 * every variant (the clones in subfolders); a split or non-merged set holds
 * one. The variants are tried in this order:
 *   1943u   US, Rev C (the default; English, the program ROMs a good dump)
 *   1943    Euro (MAME marks its bme01.12d a bad dump: a hacked ROM)
 *   1943j   Japan, Rev B
 *   1943ua  US
 * The i8751 (bm.7k) is not needed: its answers are a table (c1943.c). The
 * PROMs bm4.12c (priority) and bm6.4b (video timing) are not used, by MAME
 * either.
 */
#include "c1943.h"

#define N(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

const uint32_t c1943_region_size[C1943_R_COUNT] = {
    [C1943_R_MAIN] = 0x8000,     [C1943_R_BANKS] = 0x20000,   [C1943_R_SOUND] = 0x8000,
    [C1943_R_CHARS] = 0x8000,    [C1943_R_BG] = 0x40000,      [C1943_R_BG2] = 0x10000,
    [C1943_R_SPRITES] = 0x40000, [C1943_R_TILEMAP] = 0x10000, [C1943_R_PROMS] = 0xa00,
};

#define V_1943U 0x01
#define V_1943 0x02
#define V_1943J 0x04
#define V_1943UA 0x08

static const c1943_romfile_t files_1943[] = {
    /* the main CPU, per variant */
    {"bmu01c.12d", 0xc686cc5c, 0x08000, C1943_R_MAIN, 0x00000, V_1943U},
    {"bmu02c.13d", 0xd8880a41, 0x10000, C1943_R_BANKS, 0x00000, V_1943U},
    {"bmu03c.14d", 0x3f0ee26c, 0x10000, C1943_R_BANKS, 0x10000, V_1943U},
    {"bme01.12d", 0x55fd447e, 0x08000, C1943_R_MAIN, 0x00000, V_1943},
    {"bme02.13d", 0x073fc57c, 0x10000, C1943_R_BANKS, 0x00000, V_1943},
    {"bme03.14d", 0x835822c2, 0x10000, C1943_R_BANKS, 0x10000, V_1943},
    {"bm01b.12d", 0x363f9f3d, 0x08000, C1943_R_MAIN, 0x00000, V_1943J},
    {"bm02b.13d", 0x7f0d7edc, 0x10000, C1943_R_BANKS, 0x00000, V_1943J},
    {"bm03b.14d", 0x7093da2a, 0x10000, C1943_R_BANKS, 0x10000, V_1943J},
    {"bmu01.12d", 0x793cf15f, 0x08000, C1943_R_MAIN, 0x00000, V_1943UA},
    {"bmu02.13d", 0x6f1353d5, 0x10000, C1943_R_BANKS, 0x00000, V_1943UA},
    {"bmu03.14d", 0x9e7c07f7, 0x10000, C1943_R_BANKS, 0x10000, V_1943UA},
    /* the rest, common to all */
    {"bm05.4k", 0xee2bd2d7, 0x8000, C1943_R_SOUND, 0x0000, 0},
    {"bm04.5h", 0x46cb9d3d, 0x8000, C1943_R_CHARS, 0x0000, 0},
    {"bm15.10f", 0x6b1a0443, 0x8000, C1943_R_BG, 0x00000, 0},
    {"bm16.11f", 0x23c908c2, 0x8000, C1943_R_BG, 0x08000, 0},
    {"bm17.12f", 0x46bcdd07, 0x8000, C1943_R_BG, 0x10000, 0},
    {"bm18.14f", 0xe6ae7ba0, 0x8000, C1943_R_BG, 0x18000, 0},
    {"bm19.10j", 0x868ababc, 0x8000, C1943_R_BG, 0x20000, 0},
    {"bm20.11j", 0x0917e5d4, 0x8000, C1943_R_BG, 0x28000, 0},
    {"bm21.12j", 0x9bfb0d89, 0x8000, C1943_R_BG, 0x30000, 0},
    {"bm22.14j", 0x04f3c274, 0x8000, C1943_R_BG, 0x38000, 0},
    {"bm24.14k", 0x11134036, 0x8000, C1943_R_BG2, 0x0000, 0},
    {"bm25.14l", 0x092cf9c1, 0x8000, C1943_R_BG2, 0x8000, 0},
    {"bm06.10a", 0x97acc8af, 0x8000, C1943_R_SPRITES, 0x00000, 0},
    {"bm07.11a", 0xd78f7197, 0x8000, C1943_R_SPRITES, 0x08000, 0},
    {"bm08.12a", 0x1a626608, 0x8000, C1943_R_SPRITES, 0x10000, 0},
    {"bm09.14a", 0x92408400, 0x8000, C1943_R_SPRITES, 0x18000, 0},
    {"bm10.10c", 0x8438a44a, 0x8000, C1943_R_SPRITES, 0x20000, 0},
    {"bm11.11c", 0x6c69351d, 0x8000, C1943_R_SPRITES, 0x28000, 0},
    {"bm12.12c", 0x5e7efdb7, 0x8000, C1943_R_SPRITES, 0x30000, 0},
    {"bm13.14c", 0x1143829a, 0x8000, C1943_R_SPRITES, 0x38000, 0},
    {"bm14.5f", 0x4d3c6401, 0x8000, C1943_R_TILEMAP, 0x0000, 0},
    {"bm23.8k", 0xa52aecbd, 0x8000, C1943_R_TILEMAP, 0x8000, 0},
    {"bm1.12a", 0x74421f18, 0x100, C1943_R_PROMS, 0x000, 0},   /* red */
    {"bm2.13a", 0xac27541f, 0x100, C1943_R_PROMS, 0x100, 0},   /* green */
    {"bm3.14a", 0x251fb6ff, 0x100, C1943_R_PROMS, 0x200, 0},   /* blue */
    {"bm5.7f", 0x206713d0, 0x100, C1943_R_PROMS, 0x300, 0},    /* characters */
    {"bm10.7l", 0x33c2491c, 0x100, C1943_R_PROMS, 0x400, 0},   /* front layer */
    {"bm9.6l", 0xaeea4af7, 0x100, C1943_R_PROMS, 0x500, 0},    /* front layer bank */
    {"bm12.12m", 0xc18aa136, 0x100, C1943_R_PROMS, 0x600, 0},  /* back layer */
    {"bm11.12l", 0x405aae37, 0x100, C1943_R_PROMS, 0x700, 0},  /* back layer bank */
    {"bm8.8c", 0xc2010a9e, 0x100, C1943_R_PROMS, 0x800, 0},    /* sprites */
    {"bm7.7c", 0xb56f30c3, 0x100, C1943_R_PROMS, 0x900, 0},    /* sprite bank, priority */
};

/* MAME's 1943kai: a parent set of its own (two of its tile ROMs are 1943's). */
static const c1943_romfile_t files_1943kai[] = {
    {"bmk01.12d", 0x7d2211db, 0x08000, C1943_R_MAIN, 0x00000, 0},
    {"bmk02.13d", 0x2ebbc8c5, 0x10000, C1943_R_BANKS, 0x00000, 0},
    {"bmk03.14d", 0x475a6ac5, 0x10000, C1943_R_BANKS, 0x10000, 0},
    {"bmk05.4k", 0x25f37957, 0x8000, C1943_R_SOUND, 0x0000, 0},
    {"bmk04.5h", 0x884a8692, 0x8000, C1943_R_CHARS, 0x0000, 0},
    {"bm15.10f", 0x6b1a0443, 0x8000, C1943_R_BG, 0x00000, 0},
    {"bmk16.11f", 0x9416fe0d, 0x8000, C1943_R_BG, 0x08000, 0},
    {"bmk17.12f", 0x3d5acab9, 0x8000, C1943_R_BG, 0x10000, 0},
    {"bmk18.14f", 0x7b62da1d, 0x8000, C1943_R_BG, 0x18000, 0},
    {"bm19.10j", 0x868ababc, 0x8000, C1943_R_BG, 0x20000, 0},
    {"bmk20.11j", 0xb90364c1, 0x8000, C1943_R_BG, 0x28000, 0},
    {"bmk21.12j", 0x8c7fe74a, 0x8000, C1943_R_BG, 0x30000, 0},
    {"bmk22.14j", 0xd5ef8a0e, 0x8000, C1943_R_BG, 0x38000, 0},
    {"bmk24.14k", 0xbf186ef2, 0x8000, C1943_R_BG2, 0x0000, 0},
    {"bmk25.14l", 0xa755faf1, 0x8000, C1943_R_BG2, 0x8000, 0},
    {"bmk06.10a", 0x5f7e38b3, 0x8000, C1943_R_SPRITES, 0x00000, 0},
    {"bmk07.11a", 0xff3751fd, 0x8000, C1943_R_SPRITES, 0x08000, 0},
    {"bmk08.12a", 0x159d51bd, 0x8000, C1943_R_SPRITES, 0x10000, 0},
    {"bmk09.14a", 0x8683e3d2, 0x8000, C1943_R_SPRITES, 0x18000, 0},
    {"bmk10.10c", 0x1e0d9571, 0x8000, C1943_R_SPRITES, 0x20000, 0},
    {"bmk11.11c", 0xf1fc5ee1, 0x8000, C1943_R_SPRITES, 0x28000, 0},
    {"bmk12.12c", 0x0f50c001, 0x8000, C1943_R_SPRITES, 0x30000, 0},
    {"bmk13.14c", 0xfd1acf8e, 0x8000, C1943_R_SPRITES, 0x38000, 0},
    {"bmk14.5f", 0xcf0f5a53, 0x8000, C1943_R_TILEMAP, 0x0000, 0},
    {"bmk23.8k", 0x17f77ef9, 0x8000, C1943_R_TILEMAP, 0x8000, 0},
    {"bmk1.12a", 0xe001ea33, 0x100, C1943_R_PROMS, 0x000, 0},
    {"bmk2.13a", 0xaf34d91a, 0x100, C1943_R_PROMS, 0x100, 0},
    {"bmk3.14a", 0x43e9f6ef, 0x100, C1943_R_PROMS, 0x200, 0},
    {"bmk5.7f", 0x41878934, 0x100, C1943_R_PROMS, 0x300, 0},
    {"bmk10.7l", 0xde44b748, 0x100, C1943_R_PROMS, 0x400, 0},
    {"bmk9.6l", 0x59ea57c0, 0x100, C1943_R_PROMS, 0x500, 0},
    {"bmk12.12m", 0x8765f8b0, 0x100, C1943_R_PROMS, 0x600, 0},
    {"bmk11.12l", 0x87a8854e, 0x100, C1943_R_PROMS, 0x700, 0},
    {"bmk8.8c", 0xdad17e2d, 0x100, C1943_R_PROMS, 0x800, 0},
    {"bmk7.7c", 0x76307f8d, 0x100, C1943_R_PROMS, 0x900, 0},
};

const c1943_set_info_t c1943_sets[C1943_SET_COUNT] = {
    [C1943_SET_1943] =
        {
            .title = "1943: The Battle of Midway",
            .folder = "1943",
            .mameset = "1943",
            .variant_set = {"1943u", "1943", "1943j", "1943ua"},
            .variant_name = {"US, Rev C", "Euro", "Japan, Rev B", "US"},
            .nvariants = 4,
            .files = files_1943,
            .nfiles = N(files_1943),
        },
    [C1943_SET_1943KAI] =
        {
            .title = "1943 Kai: Midway Kaisen",
            .folder = "1943KAI",
            .mameset = "1943kai",
            .variant_set = {"1943kai"},
            .variant_name = {"Japan"},
            .nvariants = 1,
            .files = files_1943kai,
            .nfiles = N(files_1943kai),
        },
};

c1943_files_t c1943_variant_files(c1943_set_id_t set, int v)
{
    const c1943_set_info_t *s = &c1943_sets[set];
    c1943_files_t m = 0;
    for (int i = 0; i < s->nfiles; i++)
        if (!s->files[i].variants || (s->files[i].variants & (1u << v)))
            m |= (c1943_files_t)1 << i;
    return m;
}

int c1943_variant_found(c1943_set_id_t set, c1943_files_t found)
{
    for (int v = 0; v < c1943_sets[set].nvariants; v++)
    {
        const c1943_files_t need = c1943_variant_files(set, v);
        if ((found & need) == need)
            return v;
    }
    return -1;
}

int c1943_variant_nearest(c1943_set_id_t set, c1943_files_t found)
{
    int best = 0, best_missing = 1000;
    for (int v = 0; v < c1943_sets[set].nvariants; v++)
    {
        const c1943_files_t missing = c1943_variant_files(set, v) & ~found;
        const int n = __builtin_popcountll(missing);
        if (n < best_missing)
        {
            best_missing = n;
            best = v;
        }
    }
    return best;
}
