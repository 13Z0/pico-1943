/*
 * 1943: the flash store, for boards without PSRAM. Modeled on pico-galagino's
 * flash cache (gal_flash.c).
 *
 * The machine's data (c1943_part_t: the CPUs' ROMs, the converted graphics,
 * the tile maps and the PROMs, 868 KB) does not fit in SRAM. On a board
 * without PSRAM it is built in flash once, from the ROM set on the card, and
 * read from there (XIP) after that, also without the card. A slot per set:
 *
 *   0x0000  header (one 4 KB sector): magic, version, set, variant, the size
 *           and CRC32 of the data
 *   0x1000  the parts, each at a multiple of 4 KB, in c1943_part_t order
 *
 * Two slots when the flash has room for both sets, else one, which the other
 * set replaces. The header is written last, so a copy cut short is never used;
 * the data is checked against its CRC once per boot.
 *
 * Copying erases and programs flash, so it must run with nothing else using
 * flash: on the board before the display starts. It goes through a buffer of
 * C1943_STORE_TEMP bytes: a pair of 32 KB plane ROMs, or one 64 KB program ROM,
 * at a time, plus a 4 KB sector of converted data.
 */
#include "c1943_store.h"

#include <stdio.h>
#include <string.h>

#include "miniz.h"

#define MAGIC 0x33343931u /* "1943" */
#define VERSION 1
#define SECTOR 4096
#define PAGE 256
#define HDR_SIZE SECTOR

typedef struct
{
    uint32_t magic, version;
    uint32_t set, variant;
    uint32_t data_size, crc;
    uint32_t part_ofs[C1943_P_COUNT];
} hdr_t;

static c1943_flash_t fl;
static int opened;
static int nslots;
static uint32_t slot_size;
static int8_t checked[2]; /* 0 not yet, 1 good, -1 bad */

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

/* the parts' offsets in a slot, and the slot's size */
static uint32_t layout(uint32_t *ofs)
{
    uint32_t o = HDR_SIZE;
    for (int p = 0; p < C1943_P_COUNT; p++)
    {
        ofs[p] = o;
        o += align_up(c1943_part_size[p], SECTOR);
    }
    return align_up(o, 65536);
}

static const hdr_t *slot_hdr(int k)
{
    return (const hdr_t *)(fl.base + (uint32_t)k * slot_size);
}

static int hdr_valid(const hdr_t *h)
{
    return h->magic == MAGIC && h->version == VERSION && h->set < C1943_SET_COUNT;
}

void c1943_store_open(const c1943_flash_t *flash)
{
    fl = *flash;
    uint32_t ofs[C1943_P_COUNT];
    slot_size = layout(ofs);
    nslots = (int)(fl.size / slot_size);
    if (nslots > 2)
        nslots = 2;
    checked[0] = checked[1] = 0;
    opened = 1;
}

int c1943_store_room(void)
{
    return opened && nslots > 0;
}

/* the slot a set goes to: its own with two slots, else the only one */
static int slot_of(c1943_set_id_t set)
{
    return nslots >= 2 ? (int)set : 0;
}

static int slot_good(int k)
{
    if (!checked[k])
    {
        const hdr_t *h = slot_hdr(k);
        checked[k] = -1;
        if (hdr_valid(h) && h->data_size == slot_size - HDR_SIZE)
        {
            const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, fl.base + (uint32_t)k * slot_size + HDR_SIZE,
                                                    h->data_size);
            checked[k] = (crc == h->crc) ? 1 : -1;
        }
    }
    return checked[k] > 0;
}

int c1943_store_variant(c1943_set_id_t set)
{
    if (!c1943_store_room())
        return -1;
    const int k = slot_of(set);
    const hdr_t *h = slot_hdr(k);
    if (!hdr_valid(h) || h->set != (uint32_t)set || !slot_good(k))
        return -1;
    return (int)h->variant;
}

int c1943_store_parts(c1943_set_id_t set, const uint8_t **parts)
{
    if (c1943_store_variant(set) < 0)
        return 0;
    const int k = slot_of(set);
    const hdr_t *h = slot_hdr(k);
    for (int p = 0; p < C1943_P_COUNT; p++)
        parts[p] = fl.base + (uint32_t)k * slot_size + h->part_ofs[p];
    return 1;
}

/* ---------------------------------------------------------------------------
 * Copying
 * ------------------------------------------------------------------------ */

typedef struct
{
    c1943_set_id_t set;
    const c1943_set_info_t *s;
    uint8_t *buf;
    uint32_t base_ofs[C1943_R_COUNT]; /* where in buf each region's chunk starts */
    uint32_t lo[C1943_R_COUNT], hi[C1943_R_COUNT]; /* the region offsets loaded */
} chunk_t;

/* A file of the chunk to its place in the buffer: offsets lo..hi of the
 * region map to buf + base_ofs + (offset - lo). */
static uint8_t *dest_chunk(void *arg, int i)
{
    chunk_t *c = (chunk_t *)arg;
    const c1943_romfile_t *f = &c->s->files[i];
    if (f->offset < c->lo[f->region] || f->offset + f->size > c->hi[f->region])
        return NULL;
    return c->buf + c->base_ofs[f->region] + (f->offset - c->lo[f->region]);
}

/* The files of variant v that lie in region r between offsets lo and hi. */
static c1943_files_t files_in(const c1943_set_info_t *s, c1943_files_t want, int r, uint32_t lo, uint32_t hi)
{
    c1943_files_t m = 0;
    for (int i = 0; i < s->nfiles; i++)
        if ((want & ((c1943_files_t)1 << i)) && s->files[i].region == r && s->files[i].offset >= lo &&
            s->files[i].offset + s->files[i].size <= hi)
            m |= (c1943_files_t)1 << i;
    return m;
}

typedef struct
{
    uint32_t pos; /* flash offset written up to */
    uint32_t crc;
    int set;
    uint32_t since; /* bytes since copying() was last called */
} writer_t;

static void put(writer_t *w, const uint8_t *data, uint32_t len)
{
    /* len is a multiple of the page size here */
    fl.program(w->pos, data, len);
    w->crc = (uint32_t)mz_crc32(w->crc, data, len);
    w->pos += len;
    /* a sign of life about every 64 KB (the board blinks its LED) */
    w->since += len;
    if (w->since >= 0x10000 && fl.copying)
    {
        fl.copying(w->set);
        w->since = 0;
    }
}

/* Loads the files of region r between offsets lo and hi (lo..lo+half and, if
 * pair, lo+split..lo+split+half) into the buffer. Returns 0 when one is
 * missing. */
static int load_chunk(chunk_t *c, c1943_files_t want, const c1943_io_t *io, const char *dir, int r, uint32_t lo,
                      uint32_t len, uint32_t split)
{
    c1943_files_t need = files_in(c->s, want, r, lo, lo + len);
    for (int k = 0; k < C1943_R_COUNT; k++)
        c->lo[k] = c->hi[k] = 0;
    c->lo[r] = lo;
    c->hi[r] = lo + len;
    c->base_ofs[r] = 0;
    c1943_files_t got = c1943_load_files(c->set, need, io, dir, dest_chunk, c);
    if (got != need)
        return 0;
    if (split)
    {
        need = files_in(c->s, want, r, lo + split, lo + split + len);
        c->lo[r] = lo + split;
        c->hi[r] = lo + split + len;
        c->base_ofs[r] = len;
        got = c1943_load_files(c->set, need, io, dir, dest_chunk, c);
        if (got != need)
            return 0;
    }
    return 1;
}

static void pad_to(writer_t *w, uint32_t target, uint8_t *scratch)
{
    memset(scratch, 0xff, PAGE);
    while (w->pos < target)
        put(w, scratch, PAGE);
}

c1943_files_t c1943_store_copy(c1943_set_id_t set, int v, const c1943_io_t *io, const char *dir, uint8_t *temp,
                               uint32_t temp_size)
{
    if (!c1943_store_room() || temp_size < C1943_STORE_TEMP)
        return 0;
    const c1943_set_info_t *s = &c1943_sets[set];
    const c1943_files_t want = c1943_variant_files(set, v);
    /* first: is the set there at all */
    const c1943_files_t found = c1943_probe(set, io, dir) & want;
    if (found != want)
        return found;

    const int k = slot_of(set);
    const uint32_t slot = (uint32_t)k * slot_size;
    uint32_t ofs[C1943_P_COUNT];
    layout(ofs);
    if (fl.copying)
        fl.copying(set);
    fl.erase(fl.offset + slot, slot_size);
    checked[k] = 0;

    chunk_t c;
    c.set = set;
    c.s = s;
    c.buf = temp;
    uint8_t *out = temp + 0x10000; /* a converted sector */
    writer_t w = {fl.offset + slot + HDR_SIZE, MZ_CRC32_INIT, (int)set, 0};
    int ok = 1;

    /* main ROM, banks, sound ROM: as they are, 64 KB at a time */
    for (uint32_t o = 0; ok && o < c1943_region_size[C1943_R_MAIN]; o += 0x8000)
        if ((ok = load_chunk(&c, want, io, dir, C1943_R_MAIN, o, 0x8000, 0)))
            put(&w, temp, 0x8000);
    pad_to(&w, fl.offset + slot + ofs[C1943_P_BANKS], out);
    for (uint32_t o = 0; ok && o < c1943_region_size[C1943_R_BANKS]; o += 0x10000)
        if ((ok = load_chunk(&c, want, io, dir, C1943_R_BANKS, o, 0x10000, 0)))
            put(&w, temp, 0x10000);
    pad_to(&w, fl.offset + slot + ofs[C1943_P_SOUND], out);
    if (ok && (ok = load_chunk(&c, want, io, dir, C1943_R_SOUND, 0, 0x8000, 0)))
        put(&w, temp, 0x8000);

    /* the characters: 2048 of 16 bytes, converted 256 (4 KB) at a time */
    pad_to(&w, fl.offset + slot + ofs[C1943_P_CHARS], out);
    if (ok && (ok = load_chunk(&c, want, io, dir, C1943_R_CHARS, 0, 0x8000, 0)))
        for (int e = 0; e < 2048; e += 256)
        {
            c1943_conv_chars(temp + 16 * e, 256, (uint16_t *)out);
            put(&w, out, SECTOR);
        }

    /* the tiles: per pair of 32 KB ROMs (one of each half) 128 tiles, eight
     * (4 KB) at a time */
    pad_to(&w, fl.offset + slot + ofs[C1943_P_BG], out);
    for (uint32_t o = 0; ok && o < 0x20000; o += 0x8000)
        if ((ok = load_chunk(&c, want, io, dir, C1943_R_BG, o, 0x8000, 0x20000)))
            for (int e = 0; e < 128; e += 8)
            {
                c1943_conv_tiles(temp + 256 * e, temp + 0x8000 + 256 * e, 8, (uint32_t *)out);
                put(&w, out, SECTOR);
            }
    pad_to(&w, fl.offset + slot + ofs[C1943_P_BG2], out);
    if (ok && (ok = load_chunk(&c, want, io, dir, C1943_R_BG2, 0, 0x8000, 0x8000)))
        for (int e = 0; e < 128; e += 8)
        {
            c1943_conv_tiles(temp + 256 * e, temp + 0x8000 + 256 * e, 8, (uint32_t *)out);
            put(&w, out, SECTOR);
        }

    /* the sprites: per pair of 32 KB ROMs 512 sprites, 32 (4 KB) at a time */
    pad_to(&w, fl.offset + slot + ofs[C1943_P_SPRITES], out);
    for (uint32_t o = 0; ok && o < 0x20000; o += 0x8000)
        if ((ok = load_chunk(&c, want, io, dir, C1943_R_SPRITES, o, 0x8000, 0x20000)))
            for (int e = 0; e < 512; e += 32)
            {
                c1943_conv_sprites(temp + 64 * e, temp + 0x8000 + 64 * e, 32, (uint32_t *)out);
                put(&w, out, SECTOR);
            }

    /* the tile maps and the PROMs, as they are */
    pad_to(&w, fl.offset + slot + ofs[C1943_P_TILEMAP], out);
    if (ok && (ok = load_chunk(&c, want, io, dir, C1943_R_TILEMAP, 0, 0x10000, 0)))
        put(&w, temp, 0x10000);
    pad_to(&w, fl.offset + slot + ofs[C1943_P_PROMS], out);
    if (ok)
    {
        memset(temp, 0xff, SECTOR);
        if ((ok = load_chunk(&c, want, io, dir, C1943_R_PROMS, 0, c1943_region_size[C1943_R_PROMS], 0)))
            put(&w, temp, SECTOR);
    }
    pad_to(&w, fl.offset + slot + slot_size, out);

    if (!ok)
        return 0;

    /* the header, last */
    hdr_t *h = (hdr_t *)temp;
    memset(temp, 0xff, SECTOR);
    h->magic = MAGIC;
    h->version = VERSION;
    h->set = (uint32_t)set;
    h->variant = (uint32_t)v;
    h->data_size = slot_size - HDR_SIZE;
    h->crc = w.crc;
    for (int p = 0; p < C1943_P_COUNT; p++)
        h->part_ofs[p] = ofs[p];
    fl.program(fl.offset + slot, temp, SECTOR);
    checked[k] = 0;
    return want;
}

uint32_t c1943_store_slot_size(void)
{
    uint32_t ofs[C1943_P_COUNT];
    return layout(ofs);
}

int c1943_store_slots(void)
{
    return opened ? nslots : 0;
}
