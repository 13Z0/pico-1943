/*
 * 1943: the video, after MAME's capcom/1943_v.cpp (license BSD-3-Clause,
 * copyright-holders Paul Leaman), and the band renderer of pico-galagino.
 *
 * Four layers, back to front:
 *   back layer   32x32 tiles from the map at tilemap+0x8000, 2048 columns of
 *                8 tiles, opaque; scrolled in X only (d803-d804). Black when
 *                switched off (d806 bit 5).
 *   front layer  the same from tilemap+0, scrolled in X and Y (d800-d802); a
 *                pen whose colour lookup gives 0x0f is transparent.
 *   sprites      128 of 16x16, pen 0 transparent; the first in RAM wins. A
 *                pen with bit 3 of the sprite bank PROM set goes behind the
 *                front layer where that is opaque (MAME's _1943_drawgfx).
 *   characters   8x8, pen 0 transparent, unscrolled; on when c804 bit 7 is.
 * A map entry is two bytes: the code, then the attribute: bits 2-5 the
 * colour, bit 6 flip in x, bit 7 flip in y, and in the front layer bit 0 the
 * code's ninth bit.
 *
 * Colours: three 4-bit PROMs, one per gun (weights 0x0e, 0x1f, 0x43, 0x8f),
 * and lookup PROMs per layer from a layer's pen to one of the 256 colours.
 *
 * Everything is drawn in the upright picture, 224x256: its pixel (ux, uy) is
 * the raster's pixel (255 - uy, ux + 16). A row of the upright picture is a
 * column of the raster, so it crosses one column of each tile map, and draws
 * one row of each converted element (c1943_gfx.c) it meets.
 */
#include "c1943_internal.h"

/* ---------------------------------------------------------------------------
 * Colours
 * ------------------------------------------------------------------------ */

static uint8_t weight(uint8_t v)
{
    return (uint8_t)(0x0e * (v & 1) + 0x1f * ((v >> 1) & 1) + 0x43 * ((v >> 2) & 1) + 0x8f * ((v >> 3) & 1));
}

static uint16_t pack(uint8_t r, uint8_t g, uint8_t b)
{
    switch (m.fmt)
    {
    case C1943_FMT_RGB444:
        return (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
    case C1943_FMT_RGB565:
        return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    default:
        return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    }
}

void c1943_video_init(const uint8_t *p)
{
    uint16_t base[256];
    for (int i = 0; i < 256; i++)
        base[i] = pack(weight(p[i]), weight(p[0x100 + i]), weight(p[0x200 + i]));

    for (int c = 0; c < 32; c++)
        for (int pen = 0; pen < 4; pen++)
            m.pal_chars[c][pen] = base[(p[0x300 + 4 * c + pen] & 15) | 0x40];

    for (int c = 0; c < 16; c++)
    {
        m.bg_trans[c] = m.spr_behind[c] = 0;
        for (int pen = 0; pen < 16; pen++)
        {
            const int j = 16 * c + pen;
            const int fg = ((p[0x500 + j] & 3) << 4) | (p[0x400 + j] & 15);
            m.pal_bg[c][pen] = base[fg];
            if (fg == 0x0f)
                m.bg_trans[c] |= (uint16_t)(1 << pen);
            m.pal_bg2[c][pen] = base[((p[0x700 + j] & 3) << 4) | (p[0x600 + j] & 15)];
            m.pal_spr[c][pen] = base[((p[0x900 + j] & 7) << 4) | (p[0x800 + j] & 15) | 0x80];
            if (p[0x900 + j] & 0x08)
                m.spr_behind[c] |= (uint16_t)(1 << pen);
        }
    }

    /* the pens each front layer tile uses: a tile whose pens are all
     * transparent in its colour is skipped (most of the layer is) */
    for (int t = 0; t < 512; t++)
    {
        uint32_t used = 0;
        const uint32_t *w = m.bg + t * 32 * 4;
        for (int i = 0; i < 32 * 4; i++)
            for (uint32_t v = w[i], k = 0; k < 8; k++, v >>= 4)
                used |= 1u << (v & 15);
        m.bg_pens[t] = (uint16_t)used;
    }
}

/* ---------------------------------------------------------------------------
 * Drawing: the sprites of the frame and helpers of both kinds of bands
 * ------------------------------------------------------------------------ */

/* A sprite of the frame, in upright coordinates. */
typedef struct
{
    int16_t x, y;
    uint16_t code;
    uint8_t color;
} sprite_t;

static sprite_t sprites[128];
static int nsprites;

static inline uint32_t pen_at(const uint32_t *w, int e)
{
    return (w[e >> 3] >> ((e & 7) << 2)) & 15;
}

/* The eight four-bit pixels of a word in the opposite order. */
static inline uint32_t rev4(uint32_t v)
{
    v = __builtin_bswap32(v);
    return ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
}

/* Pixels 8g..8g+7 of a 32-pixel row of a tile, flipped in y (the upright
 * picture's x) if asked: pixel e is then the tile's pixel 31 - e. Only the
 * words a band needs are read: a band of the tate orientations is 8 pixels
 * wide. */
static inline uint32_t row_group(const uint32_t *w, int g, int flip)
{
    return flip ? rev4(w[3 - g]) : w[g];
}

/* A rectangle of the upright picture being drawn. Pixel (x, y) of the picture
 * is buf[(y - y0) * w + (x - x0)]. */
typedef struct
{
    uint16_t *buf;
    int x0, y0, w, h;
} band_t;

/* ---------------------------------------------------------------------------
 * The tate orientations draw bands 8 pixels wide and 256 rows tall (see
 * below), 28 of them a frame. Drawn a row at a time, each row of each band
 * would look up its tiles again; drawn a layer at a time instead, a band lies
 * in one column of 8-pixel groups of the tile maps, and a tile serves 32 rows.
 * The same rules, with the masks of the band's rows kept as a byte per row.
 * ------------------------------------------------------------------------ */

static uint8_t opaque8[C1943_H], drawn8[C1943_H];

/* n (<= 8) pixels of a tile row from pixel e (e + n <= 32), flipped as asked,
 * the first in the low nibble */
static inline uint32_t C1943_HOT(row_pixels)(const uint32_t *w, int flip, int e, int n)
{
    const int g = e >> 3, sh = (e & 7) << 2;
    uint32_t v = row_group(w, g, flip) >> sh;
    if (sh && n > 8 - (e & 7))
        v |= row_group(w, g + 1, flip) << (32 - sh);
    return v;
}

/* One of the band's (at most two) pieces of a tile column: n pixels from
 * pixel e of the tiles of row r of the map, put at k0 in the band. */
typedef struct
{
    int r, e, n, k0;
    const uint32_t *w; /* the tile's rows */
    int flipx, flipy;
    const uint16_t *colors;
    uint32_t trans; /* front layer: the transparent pens; ~0 skips the tile */
} piece_t;

static inline void C1943_HOT(piece_setup)(piece_t *p, const uint8_t *map, const uint32_t *tiles, int front)
{
    const uint8_t attr = map[2 * p->r + 1];
    const uint32_t code = front ? (map[2 * p->r] | ((uint32_t)(attr & 1) << 8)) : (map[2 * p->r] & 0x7f);
    const int color = (attr >> 2) & 15;
    p->w = tiles + code * 32 * 4;
    p->flipx = attr & 0x40;
    p->flipy = attr & 0x80;
    if (front)
    {
        p->colors = m.pal_bg[color];
        p->trans = m.bg_trans[color];
        if (!(m.bg_pens[code] & ~p->trans))
            p->trans = ~0u; /* nothing of it shows */
    }
    else
    {
        p->colors = m.pal_bg2[color];
        p->trans = 0;
    }
}

/* The front or the back layer into the band at x0 (a multiple of 8). A row
 * of the band crosses one column of the map: the band's 8 pixels are n from
 * tile row r0 at pixel e0, then 8 - n from the next row of tiles. A column
 * of tiles serves up to 32 rows: they are set up once for those. */
static void C1943_HOT(band_layer_tate)(uint16_t *buf, int x0, int front)
{
    const c1943_video_t *v = &m.video;
    const uint8_t *maps = m.tilemap + (front ? 0 : 0x8000);
    const uint32_t *tiles = front ? m.bg : m.bg2;
    const int sx = front ? v->bg_scrollx : v->bg2_scrollx;
    const int Y0 = (x0 + 16 + (front ? v->bg_scrolly : 0)) & 255;
    piece_t pc[2];
    pc[0].r = Y0 >> 5;
    pc[0].e = Y0 & 31;
    pc[0].n = (32 - pc[0].e < 8) ? 32 - pc[0].e : 8;
    pc[0].k0 = 0;
    pc[1].r = (pc[0].r + 1) & 7;
    pc[1].e = 0;
    pc[1].n = 8 - pc[0].n;
    pc[1].k0 = pc[0].n;
    const int npieces = pc[1].n ? 2 : 1;

    int y = 0;
    while (y < C1943_H)
    {
        const int X = (255 - y + sx) & 0xffff;
        const int tx = X & 31;
        int run = tx + 1; /* rows until the next column of tiles */
        if (run > C1943_H - y)
            run = C1943_H - y;
        const uint8_t *map = maps + (X >> 5) * 16;
        for (int p = 0; p < npieces; p++)
        {
            piece_t *q = &pc[p];
            piece_setup(q, map, tiles, front);
            if (q->trans == ~0u)
                continue;
            uint16_t *d = buf + y * 8 + q->k0;
            uint8_t *op = opaque8 + y;
            for (int i = 0; i < run; i++, d += 8)
            {
                const int t = tx - i;
                const uint32_t *w = q->w + (q->flipx ? t : 31 - t) * 4;
                uint32_t pix = row_pixels(w, q->flipy, q->e, q->n);
                if (!front)
                {
                    if (q->n == 8)
                    {
                        const uint16_t *c = q->colors;
                        d[0] = c[pix & 15];
                        d[1] = c[(pix >> 4) & 15];
                        d[2] = c[(pix >> 8) & 15];
                        d[3] = c[(pix >> 12) & 15];
                        d[4] = c[(pix >> 16) & 15];
                        d[5] = c[(pix >> 20) & 15];
                        d[6] = c[(pix >> 24) & 15];
                        d[7] = c[pix >> 28];
                    }
                    else
                    {
                        for (int k = 0; k < q->n; k++, pix >>= 4)
                            d[k] = q->colors[pix & 15];
                    }
                    continue;
                }
                uint8_t shown = 0;
                for (int k = 0; k < q->n; k++, pix >>= 4)
                {
                    const uint32_t pen = pix & 15;
                    if ((q->trans >> pen) & 1)
                        continue;
                    d[k] = q->colors[pen];
                    shown |= (uint8_t)(1 << k);
                }
                op[i] |= (uint8_t)(shown << q->k0);
            }
        }
        y += run;
    }
}

static void C1943_HOT(band_sprites_tate)(uint16_t *buf, int x0)
{
    for (int s = 0; s < nsprites; s++)
    {
        const sprite_t *sp = &sprites[s];
        if (sp->x >= x0 + 8 || sp->x + 16 <= x0)
            continue;
        /* the sprite's pixels k..k+7 cover the band from where it starts */
        int ks = x0 - sp->x, b0 = 0;
        if (ks < 0)
        {
            b0 = -ks;
            ks = 0;
        }
        const int n = (16 - ks < 8 - b0) ? 16 - ks : 8 - b0;
        const uint16_t *colors = m.pal_spr[sp->color];
        const uint32_t behind = m.spr_behind[sp->color];
        for (int r = 0; r < 16; r++)
        {
            const int y = sp->y + r;
            if ((unsigned)y >= C1943_H)
                continue;
            const uint32_t *w = m.sprites + ((uint32_t)sp->code * 16 + r) * 2;
            const uint64_t row = ((uint64_t)w[1] << 32) | w[0];
            uint32_t pix = (uint32_t)(row >> (4 * ks));
            if (!pix)
                continue;
            uint16_t *d = buf + y * 8;
            for (int k = 0; k < n; k++, pix >>= 4)
            {
                const uint32_t pen = pix & 15;
                if (!pen)
                    continue;
                const uint8_t bit = (uint8_t)(1 << (b0 + k));
                if (drawn8[y] & bit)
                    continue; /* an earlier sprite is there */
                drawn8[y] |= bit;
                if ((opaque8[y] & bit) && ((behind >> pen) & 1))
                    continue; /* behind the front layer */
                d[b0 + k] = colors[pen];
            }
        }
    }
}

static void C1943_HOT(band_chars_tate)(uint16_t *buf, int x0)
{
    const c1943_video_t *v = &m.video;
    const int frow = (x0 + 16) >> 3; /* the band is one column of characters */
    for (int y = 0; y < C1943_H; y++)
    {
        const int xn = 255 - y;
        const int idx = frow * 32 + (xn >> 3);
        const uint8_t attr = v->vram[0x400 + idx];
        const uint32_t code = v->vram[idx] | ((uint32_t)(attr & 0xe0) << 3);
        uint32_t bits = m.chars[code * 8 + 7 - (xn & 7)];
        if (!bits)
            continue;
        const uint16_t *colors = m.pal_chars[attr & 0x1f];
        uint16_t *d = buf + y * 8;
        for (int k = 0; k < 8; k++, bits >>= 2)
            if (bits & 3)
                d[k] = colors[bits & 3];
    }
}

/* A tate band: upright columns x0..x0+7, all rows, into buf (8 x 256). */
static void C1943_HOT(render_band_tate)(uint16_t *buf, int x0)
{
    const c1943_video_t *v = &m.video;
    for (int y = 0; y < C1943_H; y++)
        opaque8[y] = drawn8[y] = 0;
    if (v->layers & 0x20)
        band_layer_tate(buf, x0, 0);
    else
        for (int i = 0; i < 8 * C1943_H; i++)
            buf[i] = 0;
    if (v->layers & 0x10)
        band_layer_tate(buf, x0, 1);
    if (v->layers & 0x40)
        band_sprites_tate(buf, x0);
    if (v->chars_on)
        band_chars_tate(buf, x0);
}

/* ---------------------------------------------------------------------------
 * The upright orientations draw bands 224 pixels wide and 8 rows tall, 32 of
 * them a frame. A band is drawn a layer at a time, as the tate bands are: the
 * pieces a row of a tile layer is made of (where its tiles start and end
 * across the picture) are the same for every row of the band, and a tile
 * serves every row of the band that lies in the same column of the map, so it
 * is set up once for those rows. The back layer's pieces start and end on
 * words (its rows begin at raster line 16): it is written eight pixels a word.
 * ------------------------------------------------------------------------ */

#define BAND_ROWS 8

/* the masks of the band's rows, a bit per pixel: the front layer is opaque
 * there; a sprite drew there (or was hidden there) */
static uint32_t opaque_rows[BAND_ROWS][8], drawn_rows[BAND_ROWS][8];

/* n pixels of a row from x: from pixel e of the tiles of row r of the map */
typedef struct
{
    int x, n, r, e;
} seg_t;

static void C1943_HOT(band_layer_upright)(uint16_t *buf, int x0, int x1, int y0, int h, int front)
{
    const c1943_video_t *v = &m.video;
    const uint8_t *maps = m.tilemap + (front ? 0 : 0x8000);
    const uint32_t *tiles = front ? m.bg : m.bg2;
    const int sx = front ? v->bg_scrollx : v->bg2_scrollx;
    const int yoff = 16 + (front ? v->bg_scrolly : 0);
    const int bw = x1 - x0;
    seg_t seg[C1943_W / 32 + 2];
    int nseg = 0;
    for (int x = x0; x < x1;)
    {
        const int Y = (x + yoff) & 255, e = Y & 31;
        int n = 32 - e;
        if (n > x1 - x)
            n = x1 - x;
        seg[nseg].x = x;
        seg[nseg].n = n;
        seg[nseg].r = Y >> 5;
        seg[nseg].e = e;
        nseg++;
        x += n;
    }

    int y = y0;
    while (y < y0 + h)
    {
        const int X = (255 - y + sx) & 0xffff;
        const int tx = X & 31;
        int run = tx + 1; /* rows until the next column of tiles */
        if (run > y0 + h - y)
            run = y0 + h - y;
        const uint8_t *map = maps + (X >> 5) * 16;
        for (int s = 0; s < nseg; s++)
        {
            const seg_t *g = &seg[s];
            piece_t q;
            q.r = g->r;
            piece_setup(&q, map, tiles, front);
            if (q.trans == ~0u)
                continue;
            const uint16_t *c = q.colors;
            for (int i = 0; i < run; i++)
            {
                const int t = tx - i, row = y + i - y0;
                const uint32_t *w = q.w + (q.flipx ? t : 31 - t) * 4;
                uint16_t *d = buf + row * bw + (g->x - x0);
                if (!front && !((g->e | g->n) & 7))
                {
                    for (int k = 0; k < g->n; k += 8, d += 8)
                    {
                        const uint32_t pix = row_group(w, (g->e + k) >> 3, q.flipy);
                        d[0] = c[pix & 15];
                        d[1] = c[(pix >> 4) & 15];
                        d[2] = c[(pix >> 8) & 15];
                        d[3] = c[(pix >> 12) & 15];
                        d[4] = c[(pix >> 16) & 15];
                        d[5] = c[(pix >> 20) & 15];
                        d[6] = c[(pix >> 24) & 15];
                        d[7] = c[pix >> 28];
                    }
                    continue;
                }
                int e = g->e;
                uint32_t pix = row_group(w, e >> 3, q.flipy) >> ((e & 7) << 2);
                if (!front)
                {
                    for (int k = 0; k < g->n; k++, e++, pix >>= 4)
                    {
                        if (!(e & 7))
                            pix = row_group(w, e >> 3, q.flipy);
                        d[k] = c[pix & 15];
                    }
                    continue;
                }
                uint32_t shown = 0; /* bit k: pixel x + k is opaque */
                for (int k = 0; k < g->n; k++, e++, pix >>= 4)
                {
                    if (!(e & 7))
                        pix = row_group(w, e >> 3, q.flipy);
                    const uint32_t pen = pix & 15;
                    if ((q.trans >> pen) & 1)
                        continue;
                    d[k] = c[pen];
                    shown |= 1u << k;
                }
                uint32_t *op = opaque_rows[row];
                const int b = g->x & 31;
                op[g->x >> 5] |= shown << b;
                if (b && g->n > 32 - b)
                    op[(g->x >> 5) + 1] |= shown >> (32 - b);
            }
        }
        y += run;
    }
}

static void C1943_HOT(band_sprites_upright)(uint16_t *buf, int x0, int x1, int y0, int h)
{
    const int bw = x1 - x0;
    for (int s = 0; s < nsprites; s++)
    {
        const sprite_t *sp = &sprites[s];
        if (sp->x >= x1 || sp->x + 16 <= x0 || sp->y >= y0 + h || sp->y + 16 <= y0)
            continue;
        const uint16_t *colors = m.pal_spr[sp->color];
        const uint32_t behind = m.spr_behind[sp->color];
        int ks = 0, ke = 16;
        if (sp->x + ks < x0)
            ks = x0 - sp->x;
        if (sp->x + ke > x1)
            ke = x1 - sp->x;
        const int rs = (sp->y < y0) ? y0 - sp->y : 0;
        const int re = (sp->y + 16 > y0 + h) ? y0 + h - sp->y : 16;
        for (int r = rs; r < re; r++)
        {
            const uint32_t *w = m.sprites + ((uint32_t)sp->code * 16 + r) * 2;
            if (!(w[0] | w[1]))
                continue;
            const int row = sp->y + r - y0;
            uint16_t *dst = buf + row * bw - x0;
            uint32_t *dr = drawn_rows[row];
            const uint32_t *op = opaque_rows[row];
            for (int k = ks; k < ke; k++)
            {
                const uint32_t pen = pen_at(w, k);
                if (!pen)
                    continue;
                const int xx = sp->x + k;
                const uint32_t bit = 1u << (xx & 31);
                if (dr[xx >> 5] & bit)
                    continue; /* an earlier sprite is there */
                dr[xx >> 5] |= bit;
                if ((op[xx >> 5] & bit) && ((behind >> pen) & 1))
                    continue; /* behind the front layer */
                dst[xx] = colors[pen];
            }
        }
    }
}

static void C1943_HOT(row_chars)(uint16_t *dst, int x0, int x1, int y)
{
    const c1943_video_t *v = &m.video;
    const int xn = 255 - y;
    const int col = xn >> 3, erow = 7 - (xn & 7);
    int x = x0;
    while (x < x1)
    {
        const int yy = x + 16;
        const int ty = yy & 7;
        int n = 8 - ty;
        if (n > x1 - x)
            n = x1 - x;
        const int idx = (yy >> 3) * 32 + col;
        const uint8_t attr = v->vram[0x400 + idx];
        const uint32_t code = v->vram[idx] | ((uint32_t)(attr & 0xe0) << 3);
        uint32_t bits = (uint32_t)m.chars[code * 8 + erow] >> (2 * ty);
        if (bits)
        {
            const uint16_t *colors = m.pal_chars[attr & 0x1f];
            for (int k = 0; k < n; k++, bits >>= 2)
                if (bits & 3)
                    dst[x + k] = colors[bits & 3];
        }
        x += n;
    }
}

/* An upright band: columns x0..x1-1 of rows y0..y0+h-1 (h <= BAND_ROWS),
 * into buf (pixel (x, y) at buf[(y - y0) * (x1 - x0) + x - x0]). */
static void C1943_HOT(render_band_upright)(uint16_t *buf, int x0, int x1, int y0, int h)
{
    const c1943_video_t *v = &m.video;
    const int bw = x1 - x0;
    for (int r = 0; r < h; r++)
        for (int i = x0 >> 5; i <= (x1 - 1) >> 5; i++)
            opaque_rows[r][i] = drawn_rows[r][i] = 0;
    if (v->layers & 0x20)
        band_layer_upright(buf, x0, x1, y0, h, 0);
    else
        for (int i = 0; i < bw * h; i++)
            buf[i] = 0;
    if (v->layers & 0x10)
        band_layer_upright(buf, x0, x1, y0, h, 1);
    if (v->layers & 0x40)
        band_sprites_upright(buf, x0, x1, y0, h);
    if (v->chars_on)
        for (int r = 0; r < h; r++)
            row_chars(buf + r * bw - x0, x0, x1, y0 + r);
}

/* The sprite list of the frame, in the order of their priority. */
static void C1943_HOT(prepare_sprites)(void)
{
    const c1943_video_t *v = &m.video;
    nsprites = 0;
    for (int i = 0; i < 128; i++)
    {
        const uint8_t *e = &v->spr[4 * i];
        const uint8_t attr = e[1];
        const int sx = e[3] - ((attr & 0x10) << 4);
        const int ux = e[2] - 16, uy = 240 - sx;
        if (ux <= -16 || ux >= C1943_W || uy <= -16 || uy >= C1943_H)
            continue;
        sprite_t *s = &sprites[nsprites++];
        s->x = (int16_t)ux;
        s->y = (int16_t)uy;
        s->code = (uint16_t)(e[0] | ((attr & 0xe0) << 3));
        s->color = attr & 0x0f;
    }
}

/* ---------------------------------------------------------------------------
 * Into the canvas, as pico-galagino's gal_core.c does it.
 *
 * Upright (Tate off): the 256 rows become 240 by dropping every sixteenth; the
 * picture sits at x = 48. Bands are 224x8, top to bottom.
 *
 * Upright full (HSTX): the same bands, no row dropped, on a canvas `stride`
 * wide and 256 tall; the display scales the rows to its height.
 *
 * Tate: the raster, 256x224, pixel exact, centred. A canvas row is a column of
 * the upright picture, so the bands are upright columns, 8 x 256, transposed
 * into eight canvas rows each:
 *   Bottom left (CW):   canvas row 8 + (223 - ux), canvas x 32 + uy
 *   Bottom right (CCW): canvas row 8 + ux,         canvas x 32 + (255 - uy)
 * The bands run in the order that writes the canvas top to bottom.
 * ------------------------------------------------------------------------ */

#define UPRIGHT_X0 ((C1943_CANVAS_W - C1943_W) / 2) /* 48 */
#define TATE_Y0 ((C1943_CANVAS_H - C1943_W) / 2)    /* 8 */
#define TATE_X0 ((C1943_CANVAS_W - C1943_H) / 2)    /* 32 */

static uint16_t __attribute__((aligned(4))) band_buf[8 * C1943_H];

static void C1943_HOT(put_band_upright)(const band_t *b, int every, uint16_t *fb, int stride, int x0)
{
    for (int i = 0; i < b->h; i++)
    {
        int y = b->y0 + i;
        if (every)
        {
            if (y % every == every - 1)
                continue;
            y -= y / every;
        }
        uint16_t *dst = fb + y * stride + x0;
        const uint16_t *src = b->buf + i * b->w;
        if (((uintptr_t)dst | (uintptr_t)src) & 3)
        {
            for (int x = 0; x < C1943_W; x++)
                dst[x] = src[x];
            continue;
        }
        /* the usual case, both word aligned: two pixels at a time */
        uint32_t *d = (uint32_t *)dst;
        const uint32_t *s = (const uint32_t *)src;
        for (int x = 0; x < C1943_W / 2; x += 4)
        {
            const uint32_t w0 = s[x], w1 = s[x + 1], w2 = s[x + 2], w3 = s[x + 3];
            d[x] = w0;
            d[x + 1] = w1;
            d[x + 2] = w2;
            d[x + 3] = w3;
        }
    }
}

static void C1943_HOT(put_band_tate)(const band_t *b, c1943_orient_t orient, uint16_t *fb, int stride)
{
    const int h = b->h;
    for (int k = 0; k < 8; k++)
    {
        /* canvas rows top to bottom */
        const int col = (orient == C1943_ORIENT_TATE_CW) ? 7 - k : k;
        const int ux = b->x0 + col;
        const uint16_t *src = b->buf + col;
        if (orient == C1943_ORIENT_TATE_CW)
        {
            uint16_t *dst = fb + (TATE_Y0 + C1943_W - 1 - ux) * stride + TATE_X0;
            for (int y = 0; y < h; y++, src += 8)
                dst[y] = *src;
        }
        else
        {
            uint16_t *dst = fb + (TATE_Y0 + ux) * stride + TATE_X0 + h - 1;
            for (int y = 0; y < h; y++, src += 8)
                *dst-- = *src;
        }
    }
}

void C1943_HOT(c1943_render)(c1943_orient_t orient, uint16_t *fb, int stride)
{
    if (m.set < 0)
        return;
    C1943_PROF_START(t0);
    prepare_sprites();
    band_t b = {.buf = band_buf};
    if (orient == C1943_ORIENT_UPRIGHT || orient == C1943_ORIENT_UPRIGHT_FULL)
    {
        const int full = (orient == C1943_ORIENT_UPRIGHT_FULL);
        const int x0 = full ? (stride - C1943_W) / 2 : UPRIGHT_X0;
        b.x0 = 0;
        b.w = C1943_W;
        b.h = 8;
        for (b.y0 = 0; b.y0 < C1943_H; b.y0 += 8)
        {
            render_band_upright(b.buf, b.x0, b.x0 + b.w, b.y0, b.h);
            put_band_upright(&b, full ? 0 : 16, fb, stride, x0);
        }
    }
    else
    {
        b.y0 = 0;
        b.w = 8;
        b.h = C1943_H;
        for (int k = 0; k < C1943_W / 8; k++)
        {
            const int c = (orient == C1943_ORIENT_TATE_CW) ? C1943_W / 8 - 1 - k : k;
            b.x0 = 8 * c;
            render_band_tate(b.buf, b.x0);
            put_band_tate(&b, orient, fb, stride);
        }
    }
    C1943_PROF_ADD(4, t0);
}

static void fill_rect(uint16_t *fb, int stride, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
    {
        uint16_t *p = fb + (y + j) * stride + x;
        for (int i = 0; i < w; i++)
            p[i] = 0;
    }
}

void c1943_render_borders(c1943_orient_t orient, uint16_t *fb, int stride)
{
    if (orient == C1943_ORIENT_UPRIGHT)
    {
        fill_rect(fb, stride, 0, 0, UPRIGHT_X0, C1943_CANVAS_H);
        fill_rect(fb, stride, UPRIGHT_X0 + C1943_W, 0, C1943_CANVAS_W - UPRIGHT_X0 - C1943_W, C1943_CANVAS_H);
    }
    else if (orient == C1943_ORIENT_UPRIGHT_FULL)
    {
        const int x0 = (stride - C1943_W) / 2;
        fill_rect(fb, stride, 0, 0, x0, C1943_H);
        fill_rect(fb, stride, x0 + C1943_W, 0, stride - x0 - C1943_W, C1943_H);
    }
    else
    {
        fill_rect(fb, stride, 0, 0, C1943_CANVAS_W, TATE_Y0);
        fill_rect(fb, stride, 0, TATE_Y0 + C1943_W, C1943_CANVAS_W, C1943_CANVAS_H - TATE_Y0 - C1943_W);
        fill_rect(fb, stride, 0, TATE_Y0, TATE_X0, C1943_W);
        fill_rect(fb, stride, TATE_X0 + C1943_H, TATE_Y0, C1943_CANVAS_W - TATE_X0 - C1943_H, C1943_W);
    }
}
