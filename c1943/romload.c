/*
 * ROM set loader, from pico-galagino's romload.c (itself generalised from
 * pico-mooncresta's).
 *
 * Finds a set's files in a directory, inside zips (merged, split or non-merged
 * MAME sets, clone subfolders included) or as loose files. Files are
 * identified by size and CRC32 only, so their names and their place in a zip
 * do not matter. That is what makes MAME's merged 1943.zip work: the US set's
 * program ROMs sit in its 1943u/ subfolder there.
 *
 * The 1943 sets have more files than a 32-bit mask holds (the variants' main
 * CPU ROMs), so masks are 64 bits. The caller names the files it wants and
 * where each goes (a callback), so that the flash store can load a pair of
 * ROMs at a time into a small buffer.
 *
 * Without a destination the loader only probes: zip entries are taken from
 * the central directory, without inflating anything.
 *
 * The file system is reached through c1943_io_t, so the same code runs on
 * FatFs on the board and on stdio in the host harness. Large buffers come from
 * the allocator of c1943_io_t, never the stack: the inflate state alone is
 * about 8 KB. Files are read CHUNK bytes at a time, never whole: on a board
 * without PSRAM that allocator is the SRAM heap, which has no room for a 64 KB
 * ROM. A loose file is read twice when it is loaded (once for its CRC, then
 * into place); a zip entry is inflated straight into place.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c1943.h"
#include "miniz.h"

typedef uint8_t *(*dest_fn)(void *arg, int i);

typedef struct
{
    const c1943_set_info_t *s;
    c1943_files_t want; /* the files asked for */
    dest_fn dest;       /* NULL: probe */
    void *arg;
} job_t;

#define BIT(i) ((c1943_files_t)1 << (i))

#define CHUNK 4096 /* the read buffer */

/* Index of a still-missing wanted file with this size and CRC, or -1. */
static int wanted(const job_t *j, c1943_files_t found, uint32_t size, uint32_t crc)
{
    for (int i = 0; i < j->s->nfiles; i++)
        if ((j->want & BIT(i)) && !(found & BIT(i)) && j->s->files[i].size == size && j->s->files[i].crc == crc)
            return i;
    return -1;
}

static int size_wanted(const job_t *j, uint32_t size)
{
    for (int i = 0; i < j->s->nfiles; i++)
        if ((j->want & BIT(i)) && j->s->files[i].size == size)
            return 1;
    return 0;
}

static void *tmp_alloc(const c1943_io_t *io, uint32_t size)
{
    if (!size)
        size = 1;
    return io->alloc ? io->alloc(io->ctx, size) : malloc(size);
}

static void tmp_free(const c1943_io_t *io, void *p)
{
    if (!p)
        return;
    if (io->free)
        io->free(io->ctx, p);
    else
        free(p);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    return (uint32_t)mz_crc32(MZ_CRC32_INIT, p, n);
}

/* The CRC of the file's first len bytes, read through buf (CHUNK bytes).
 * Returns 0 when the file is shorter or cannot be read. */
static int crc32_file(const c1943_io_t *io, void *fh, uint32_t len, uint8_t *buf, uint32_t *crc)
{
    mz_ulong c = MZ_CRC32_INIT;
    for (uint32_t ofs = 0; ofs < len;)
    {
        const uint32_t n = len - ofs < CHUNK ? len - ofs : CHUNK;
        if (io->read_at(io->ctx, fh, ofs, buf, n) != (int)n)
            return 0;
        c = mz_crc32(c, buf, n);
        ofs += n;
    }
    *crc = (uint32_t)c;
    return 1;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n)
        return 0;
    s += n - m;
    for (size_t i = 0; i < m; i++)
    {
        char a = s[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

static int equals_ci(const char *a, const char *b)
{
    return strlen(a) == strlen(b) && ends_with_ci(a, b);
}

/* ---------------------------------------------------------------------------
 * Zip
 * ------------------------------------------------------------------------ */

#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP_CDH_SIG 0x02014b50u
#define ZIP_LFH_SIG 0x04034b50u
#define ZIP_EOCD_SEARCH 1024 /* covers a TorrentZip comment and then some */

/* Extracts one entry to dst (exactly `size` bytes), verifying the CRC. */
static int zip_extract(const c1943_io_t *io, void *fh, uint32_t lfh_ofs, int method, uint32_t comp_size, uint8_t *dst,
                       uint32_t size, uint32_t crc)
{
    uint8_t lfh[30];
    if (io->read_at(io->ctx, fh, lfh_ofs, lfh, sizeof(lfh)) != (int)sizeof(lfh) || le32(lfh) != ZIP_LFH_SIG)
        return 0;
    uint32_t data_ofs = lfh_ofs + 30 + le16(lfh + 26) + le16(lfh + 28);

    if (method == 0 && comp_size == size)
    {
        /* stored: straight into place */
        return io->read_at(io->ctx, fh, data_ofs, dst, size) == (int)size && crc32_of(dst, size) == crc;
    }
    if (method != 8)
        return 0;

    /* deflated: inflated into place, the compressed data read a chunk at a
     * time */
    uint8_t *in = (uint8_t *)tmp_alloc(io, CHUNK);
    tinfl_decompressor *d = in ? (tinfl_decompressor *)tmp_alloc(io, sizeof(tinfl_decompressor)) : NULL;
    int ok = 0;
    if (d)
    {
        tinfl_init(d);
        tinfl_status st = TINFL_STATUS_FAILED;
        uint32_t in_ofs = 0, in_len = 0, in_pos = 0;
        size_t out_pos = 0;
        do
        {
            if (in_pos == in_len && in_ofs < comp_size)
            {
                in_len = comp_size - in_ofs < CHUNK ? comp_size - in_ofs : CHUNK;
                if (io->read_at(io->ctx, fh, data_ofs + in_ofs, in, in_len) != (int)in_len)
                    break;
                in_ofs += in_len;
                in_pos = 0;
            }
            size_t in_sz = in_len - in_pos, out_sz = size - out_pos;
            const int more = in_ofs < comp_size ? TINFL_FLAG_HAS_MORE_INPUT : 0;
            st = tinfl_decompress(d, in + in_pos, &in_sz, dst, dst + out_pos, &out_sz,
                                  TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | more);
            in_pos += (uint32_t)in_sz;
            out_pos += out_sz;
        } while (st == TINFL_STATUS_NEEDS_MORE_INPUT);
        ok = (st == TINFL_STATUS_DONE && out_pos == size);
    }
    tmp_free(io, d);
    tmp_free(io, in);
    return ok && crc32_of(dst, size) == crc;
}

static c1943_files_t load_zip(const job_t *j, const c1943_io_t *io, const char *path, c1943_files_t found)
{
    void *fh = io->open(io->ctx, path);
    if (!fh)
        return found;

    uint32_t fsize = io->size(io->ctx, fh);
    uint32_t tail = fsize < ZIP_EOCD_SEARCH ? fsize : ZIP_EOCD_SEARCH;
    uint8_t *buf = (uint8_t *)tmp_alloc(io, tail);
    if (!buf || tail < 22 || io->read_at(io->ctx, fh, fsize - tail, buf, tail) != (int)tail)
    {
        tmp_free(io, buf);
        io->close(io->ctx, fh);
        return found;
    }

    /* find the end of central directory record, last one wins */
    int eocd = -1;
    for (int i = (int)tail - 22; i >= 0; i--)
    {
        if (le32(buf + i) == ZIP_EOCD_SIG)
        {
            eocd = i;
            break;
        }
    }
    uint32_t entries = 0, cd_ofs = 0;
    if (eocd >= 0)
    {
        entries = le16(buf + eocd + 10);
        cd_ofs = le32(buf + eocd + 16);
    }
    tmp_free(io, buf);

    uint32_t ofs = cd_ofs;
    for (uint32_t e = 0; e < entries && (found & j->want) != j->want; e++)
    {
        uint8_t cdh[46];
        if (io->read_at(io->ctx, fh, ofs, cdh, sizeof(cdh)) != (int)sizeof(cdh) || le32(cdh) != ZIP_CDH_SIG)
            break;
        int method = le16(cdh + 10);
        uint32_t crc = le32(cdh + 16);
        uint32_t comp_size = le32(cdh + 20);
        uint32_t size = le32(cdh + 24);
        uint32_t lfh_ofs = le32(cdh + 42);
        ofs += 46 + le16(cdh + 28) + le16(cdh + 30) + le16(cdh + 32);

        int idx = wanted(j, found, size, crc);
        if (idx < 0)
            continue;
        if (!j->dest)
        {
            found |= BIT(idx); /* probe: the directory's CRC is enough */
            continue;
        }
        uint8_t *dst = j->dest(j->arg, idx);
        if (dst && zip_extract(io, fh, lfh_ofs, method, comp_size, dst, size, crc))
            found |= BIT(idx);
    }

    io->close(io->ctx, fh);
    return found;
}

/* ---------------------------------------------------------------------------
 * Loose files
 * ------------------------------------------------------------------------ */

static c1943_files_t load_loose(const job_t *j, const c1943_io_t *io, const char *path, uint32_t size,
                                c1943_files_t found)
{
    void *fh = io->open(io->ctx, path);
    if (!fh)
        return found;
    uint8_t *buf = (uint8_t *)tmp_alloc(io, CHUNK);
    uint32_t crc;
    if (buf && crc32_file(io, fh, size, buf, &crc))
    {
        int idx = wanted(j, found, size, crc);
        if (idx >= 0 && !j->dest)
        {
            found |= BIT(idx);
        }
        else if (idx >= 0)
        {
            /* read again, now that its place is known */
            uint8_t *dst = j->dest(j->arg, idx);
            if (dst && io->read_at(io->ctx, fh, 0, dst, size) == (int)size && crc32_of(dst, size) == crc)
                found |= BIT(idx);
        }
    }
    tmp_free(io, buf);
    io->close(io->ctx, fh);
    return found;
}

/* ---------------------------------------------------------------------------
 * Directory scan
 * ------------------------------------------------------------------------ */

#define MAX_CANDIDATES 64
#define MAX_NAME 96

typedef struct
{
    char name[MAX_NAME];
    uint32_t size;
    uint8_t is_zip;
} candidate_t;

typedef struct
{
    const job_t *j;
    candidate_t *list;
    int count;
} scan_t;

static void scan_cb(void *arg, const char *name, uint32_t size, int is_dir)
{
    scan_t *s = (scan_t *)arg;
    if (is_dir || s->count >= MAX_CANDIDATES || strlen(name) >= MAX_NAME)
        return;
    int is_zip = ends_with_ci(name, ".zip");
    if (!is_zip && !size_wanted(s->j, size))
        return; /* not one of the set's ROMs by size */
    candidate_t *c = &s->list[s->count++];
    strcpy(c->name, name);
    c->size = size;
    c->is_zip = (uint8_t)is_zip;
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    if (dl && dir[dl - 1] == '/')
        dl--;
    if (dl + 1 + strlen(name) + 1 > n)
    {
        out[0] = 0;
        return;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    strcpy(out + dl + 1, name);
}

/* The set's own zips: <mameset>.zip, or one named after a variant (a split
 * clone set, 1943u.zip). */
static int is_set_zip(const c1943_set_info_t *s, const char *name)
{
    char z[32];
    snprintf(z, sizeof(z), "%s.zip", s->mameset);
    if (equals_ci(name, z))
        return 1;
    for (int v = 0; v < s->nvariants; v++)
    {
        snprintf(z, sizeof(z), "%s.zip", s->variant_set[v]);
        if (equals_ci(name, z))
            return 1;
    }
    return 0;
}

static c1943_files_t scan_dir(const job_t *j, const c1943_io_t *io, const char *dir, c1943_files_t found)
{
    scan_t sc;
    sc.j = j;
    sc.count = 0;
    sc.list = (candidate_t *)tmp_alloc(io, sizeof(candidate_t) * MAX_CANDIDATES);
    if (!sc.list)
        return found;
    if (!io->list_dir(io->ctx, dir, scan_cb, &sc))
    {
        tmp_free(io, sc.list);
        return found;
    }

    char path[MAX_NAME * 2 + 64];
    /* pass 0: the set's own zips, pass 1: other zips, pass 2: loose files */
    for (int pass = 0; pass < 3 && (found & j->want) != j->want; pass++)
    {
        for (int i = 0; i < sc.count && (found & j->want) != j->want; i++)
        {
            candidate_t *c = &sc.list[i];
            const int own = c->is_zip && is_set_zip(j->s, c->name);
            if ((pass == 0 && !own) || (pass == 1 && (!c->is_zip || own)) || (pass == 2 && c->is_zip))
                continue;
            join(path, sizeof(path), dir, c->name);
            if (!path[0])
                continue;
            found = c->is_zip ? load_zip(j, io, path, found) : load_loose(j, io, path, c->size, found);
        }
    }

    tmp_free(io, sc.list);
    return found;
}

static c1943_files_t run(const job_t *j, const c1943_io_t *io, const char *dir)
{
    c1943_files_t found = scan_dir(j, io, dir, 0);
    /* Unzipping a set commonly puts its files in a folder named after it, and
     * a merged set's clone files in folders named after the clones. */
    char sub[160];
    if ((found & j->want) != j->want)
    {
        join(sub, sizeof(sub), dir, j->s->mameset);
        if (sub[0])
            found = scan_dir(j, io, sub, found);
    }
    for (int v = 0; v < j->s->nvariants && (found & j->want) != j->want; v++)
    {
        if (!strcmp(j->s->variant_set[v], j->s->mameset))
            continue;
        join(sub, sizeof(sub), dir, j->s->variant_set[v]);
        if (sub[0])
            found = scan_dir(j, io, sub, found);
        /* and below the set's folder, as an unzipped merged set has them */
        char sub2[200];
        join(sub2, sizeof(sub2), dir, j->s->mameset);
        if (sub2[0])
        {
            join(sub, sizeof(sub), sub2, j->s->variant_set[v]);
            if (sub[0])
                found = scan_dir(j, io, sub, found);
        }
    }
    return found & j->want;
}

c1943_files_t c1943_probe(c1943_set_id_t set, const c1943_io_t *io, const char *dir)
{
    const c1943_set_info_t *s = &c1943_sets[set];
    job_t j = {s, (s->nfiles >= 64) ? ~(c1943_files_t)0 : (BIT(s->nfiles) - 1), NULL, NULL};
    return run(&j, io, dir);
}

c1943_files_t c1943_load_files(c1943_set_id_t set, c1943_files_t want, const c1943_io_t *io, const char *dir,
                               uint8_t *(*dest)(void *arg, int i), void *arg)
{
    job_t j = {&c1943_sets[set], want, dest, arg};
    return run(&j, io, dir);
}
