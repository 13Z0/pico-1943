/*
 * 1943: the graphics turned upright in the drawing formats, and the loader
 * that builds the machine's data in memory.
 *
 * MAME's layouts (capcom/1943.cpp), all planar with the planes of a pixel in
 * one byte, four pixels to a byte:
 *   characters  8x8, 2 bits: planes {4, 0}, x {0-3, 8-11}, y step 16 bits,
 *               16 bytes each
 *   tiles       32x32, 4 bits: planes {half+4, half+0, 4, 0}, x in groups of
 *               four at {0, 8, 512, 520, 1024, 1032, 1536, 1544} bits, y step
 *               16 bits, 256 bytes each (in each half of the region)
 *   sprites     16x16, 4 bits: the same at {0, 8, 256, 264}, 64 bytes each
 * The pen's top bit comes from the first plane listed.
 *
 * The game is ROT270: the element's pixel (ux, uy) of the upright picture is
 * MAME's pixel (s - 1 - uy, ux) of an s x s element. Converted, every element
 * is a row of the upright picture after another: characters 8 rows of 16 bits
 * (2 bits a pixel), tiles 32 rows of 4 words and sprites 16 rows of 2 words
 * (4 bits a pixel), the leftmost pixel in the low bits, as pico-galagino's
 * drawing formats.
 */
#include "c1943_internal.h"

const uint32_t c1943_part_size[C1943_P_COUNT] = {
    [C1943_P_MAIN] = 0x8000,       [C1943_P_BANKS] = 0x20000, [C1943_P_SOUND] = 0x8000,
    [C1943_P_CHARS] = 2048 * 8 * 2, [C1943_P_BG] = 512 * 32 * 16, [C1943_P_BG2] = 128 * 32 * 16,
    [C1943_P_SPRITES] = 2048 * 16 * 8, [C1943_P_TILEMAP] = 0x10000, [C1943_P_PROMS] = 0xa00,
};

/* A pixel of a 4-bit element: bit position `b` of its first plane in each
 * half, the second 4 bits further on in the same byte. */
static inline uint32_t pen4(const uint8_t *lo, const uint8_t *hi, uint32_t b)
{
    const uint8_t l = lo[b >> 3], h = hi[b >> 3];
    const int k = b & 7; /* 0..3 */
    return (uint32_t)((((h >> (3 - k)) & 1) << 3) | (((h >> (7 - k)) & 1) << 2) | (((l >> (3 - k)) & 1) << 1) |
                      ((l >> (7 - k)) & 1));
}

void c1943_conv_chars(const uint8_t *rom, int count, uint16_t *out)
{
    for (int i = 0; i < count; i++)
    {
        const uint8_t *e = rom + 16 * i;
        for (int uy = 0; uy < 8; uy++)
        {
            const int x = 7 - uy;
            uint16_t row = 0;
            for (int ux = 0; ux < 8; ux++)
            {
                const uint8_t byte = e[(ux * 16 + (x >> 2) * 8) >> 3];
                const int k = x & 3;
                const int pen = (((byte >> (3 - k)) & 1) << 1) | ((byte >> (7 - k)) & 1);
                row |= (uint16_t)(pen << (2 * ux));
            }
            *out++ = row;
        }
    }
}

void c1943_conv_tiles(const uint8_t *lo, const uint8_t *hi, int count, uint32_t *out)
{
    for (int i = 0; i < count; i++)
    {
        const uint8_t *l = lo + 256 * i, *h = hi + 256 * i;
        for (int uy = 0; uy < 32; uy++)
        {
            const int x = 31 - uy;
            const uint32_t xo = (uint32_t)((x & 3) + ((x >> 2) & 1) * 8 + (x >> 3) * 512);
            for (int w = 0; w < 4; w++)
            {
                uint32_t word = 0;
                for (int k = 0; k < 8; k++)
                    word |= pen4(l, h, (uint32_t)(8 * w + k) * 16 + xo) << (4 * k);
                *out++ = word;
            }
        }
    }
}

void c1943_conv_sprites(const uint8_t *lo, const uint8_t *hi, int count, uint32_t *out)
{
    for (int i = 0; i < count; i++)
    {
        const uint8_t *l = lo + 64 * i, *h = hi + 64 * i;
        for (int uy = 0; uy < 16; uy++)
        {
            const int x = 15 - uy;
            const uint32_t xo = (uint32_t)((x & 3) + ((x >> 2) & 1) * 8 + (x >> 3) * 256);
            for (int w = 0; w < 2; w++)
            {
                uint32_t word = 0;
                for (int k = 0; k < 8; k++)
                    word |= pen4(l, h, (uint32_t)(8 * w + k) * 16 + xo) << (4 * k);
                *out++ = word;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Loading into memory
 * ------------------------------------------------------------------------ */

typedef struct
{
    const c1943_set_info_t *s;
    uint8_t *raw[C1943_R_COUNT];
} load_t;

static uint8_t *dest_raw(void *arg, int i)
{
    load_t *l = (load_t *)arg;
    const c1943_romfile_t *f = &l->s->files[i];
    return l->raw[f->region] ? l->raw[f->region] + f->offset : NULL;
}

c1943_files_t c1943_load(c1943_set_id_t set, int v, const c1943_io_t *io, const char *dir, void *(*alloc)(uint32_t),
                         void (*free_fn)(void *), const uint8_t **parts)
{
    load_t l;
    l.s = &c1943_sets[set];
    for (int r = 0; r < C1943_R_COUNT; r++)
        l.raw[r] = NULL;
    for (int p = 0; p < C1943_P_COUNT; p++)
        parts[p] = NULL;

    int ok = 1;
    for (int r = 0; r < C1943_R_COUNT && ok; r++)
    {
        l.raw[r] = (uint8_t *)alloc(c1943_region_size[r]);
        ok = l.raw[r] != NULL;
    }
    c1943_files_t found = 0;
    const c1943_files_t want = c1943_variant_files(set, v);
    if (ok)
        found = c1943_load_files(set, want, io, dir, dest_raw, &l);

    uint8_t *conv[C1943_P_COUNT] = {0};
    if (ok && found == want)
    {
        conv[C1943_P_CHARS] = (uint8_t *)alloc(c1943_part_size[C1943_P_CHARS]);
        conv[C1943_P_BG] = (uint8_t *)alloc(c1943_part_size[C1943_P_BG]);
        conv[C1943_P_BG2] = (uint8_t *)alloc(c1943_part_size[C1943_P_BG2]);
        conv[C1943_P_SPRITES] = (uint8_t *)alloc(c1943_part_size[C1943_P_SPRITES]);
        ok = conv[C1943_P_CHARS] && conv[C1943_P_BG] && conv[C1943_P_BG2] && conv[C1943_P_SPRITES];
    }
    if (ok && found == want)
    {
        c1943_conv_chars(l.raw[C1943_R_CHARS], 2048, (uint16_t *)conv[C1943_P_CHARS]);
        c1943_conv_tiles(l.raw[C1943_R_BG], l.raw[C1943_R_BG] + 0x20000, 512, (uint32_t *)conv[C1943_P_BG]);
        c1943_conv_tiles(l.raw[C1943_R_BG2], l.raw[C1943_R_BG2] + 0x8000, 128, (uint32_t *)conv[C1943_P_BG2]);
        c1943_conv_sprites(l.raw[C1943_R_SPRITES], l.raw[C1943_R_SPRITES] + 0x20000, 2048,
                           (uint32_t *)conv[C1943_P_SPRITES]);
        parts[C1943_P_MAIN] = l.raw[C1943_R_MAIN];
        parts[C1943_P_BANKS] = l.raw[C1943_R_BANKS];
        parts[C1943_P_SOUND] = l.raw[C1943_R_SOUND];
        parts[C1943_P_TILEMAP] = l.raw[C1943_R_TILEMAP];
        parts[C1943_P_PROMS] = l.raw[C1943_R_PROMS];
        parts[C1943_P_CHARS] = conv[C1943_P_CHARS];
        parts[C1943_P_BG] = conv[C1943_P_BG];
        parts[C1943_P_BG2] = conv[C1943_P_BG2];
        parts[C1943_P_SPRITES] = conv[C1943_P_SPRITES];
        /* the raw graphics are converted: no longer needed */
        if (free_fn)
        {
            free_fn(l.raw[C1943_R_CHARS]);
            free_fn(l.raw[C1943_R_BG]);
            free_fn(l.raw[C1943_R_BG2]);
            free_fn(l.raw[C1943_R_SPRITES]);
        }
        return found;
    }

    /* incomplete, or out of memory */
    if (free_fn)
    {
        for (int r = 0; r < C1943_R_COUNT; r++)
            if (l.raw[r])
                free_fn(l.raw[r]);
        for (int p = 0; p < C1943_P_COUNT; p++)
            if (conv[p])
                free_fn(conv[p]);
    }
    return ok ? found : 0;
}
