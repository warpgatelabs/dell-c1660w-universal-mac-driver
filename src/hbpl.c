// SPDX-License-Identifier: Apache-2.0
/*
 * HBPL version 1 encoder and job framing.
 */
#include "hbpl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Byte buffer                                                               */

void hbpl_buf_free(hbpl_buf *b)
{
    if (!b)
        return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int buf_reserve(hbpl_buf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len)
        return -1;
    size_t need = b->len + extra;
    if (need <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    uint8_t *p = realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

int hbpl_buf_append(hbpl_buf *b, const void *src, size_t n)
{
    if (!b)
        return -1;
    if (n == 0)
        return 0;
    if (buf_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int buf_byte(hbpl_buf *b, uint8_t v)
{
    return hbpl_buf_append(b, &v, 1);
}

static int buf_str(hbpl_buf *b, const char *s)
{
    return hbpl_buf_append(b, s, strlen(s));
}

/* ------------------------------------------------------------------------ */
/* MSB-first bit writer                                                      */

typedef struct {
    hbpl_buf buf;
    uint32_t acc;   /* pending bits, right-aligned */
    int nacc;       /* number of pending bits (< 8 between calls) */
    int err;
} bitw;

/* Append the low n bits of v (n <= 24), most significant first. */
static void bw_put(bitw *w, uint32_t v, int n)
{
    if (n <= 0)
        return;
    w->acc = (w->acc << n) | (v & ((1u << n) - 1u));
    w->nacc += n;
    while (w->nacc >= 8) {
        w->nacc -= 8;
        if (buf_byte(&w->buf, (uint8_t)(w->acc >> w->nacc)) < 0)
            w->err = 1;
    }
    w->acc &= (1u << w->nacc) - 1u;
}

/* Pad with 1 bits to a byte boundary (section 7.5). */
static void bw_finish(bitw *w)
{
    if (w->nacc > 0)
        bw_put(w, 0xFFu, 8 - w->nacc);
}

/* ------------------------------------------------------------------------ */
/* Code tables (section 7)                                                   */

/* Token codes, indexed [table][token]: {bits, length}. */
static const struct { uint8_t bits, len; } token_code[2][7] = {
    { {0x00, 1}, {0x06, 3}, {0x1C, 5}, {0x1D, 5}, {0x1E, 5}, {0x02, 2}, {0x3E, 6} },
    { {0x02, 2}, {0x06, 3}, {0x1C, 5}, {0x1D, 5}, {0x1E, 5}, {0x00, 1}, {0x3E, 6} },
};

static void put_token(bitw *w, int table, int token)
{
    bw_put(w, token_code[table][token].bits, token_code[table][token].len);
}

/* Run length groups following the `111` prefix: base and extra-bit count. */
static const struct { uint16_t base; uint8_t extra; } run_group[8] = {
    {26, 3}, {34, 3}, {42, 3}, {50, 4}, {66, 5}, {98, 6}, {162, 9}, {674, 14},
};

static void put_run_length(bitw *w, uint32_t n)
{
    if (n == 1) {
        bw_put(w, 0x0, 2);
    } else if (n <= 3) {
        bw_put(w, 0x2 | (n - 2), 3);              /* 010, 011 */
    } else if (n <= 5) {
        bw_put(w, 0x4, 3);
        bw_put(w, n - 4, 1);
    } else if (n <= 9) {
        bw_put(w, 0x5, 3);
        bw_put(w, n - 6, 2);
    } else if (n <= 25) {
        bw_put(w, 0x6, 3);
        bw_put(w, n - 10, 4);
    } else {
        int g = 7;
        while (g > 0 && n < run_group[g].base)
            g--;
        bw_put(w, 0x7, 3);
        bw_put(w, (uint32_t)g, 3);
        bw_put(w, n - run_group[g].base, run_group[g].extra);
    }
}

/* Difference d in -128..127 (section 7.3). */
static void put_diff(bitw *w, int d)
{
    if (d == 0) {
        bw_put(w, 0x0, 3);
        return;
    }
    if (d == 1) {
        bw_put(w, 0x1, 3);
        return;
    }
    if (d == -1) {
        bw_put(w, 0x2, 3);
        return;
    }
    uint32_t s = d < 0 ? 1u : 0u;
    uint32_t a = (uint32_t)(d < 0 ? -d : d);
    if (a <= 3) {
        bw_put(w, 0x3, 3); bw_put(w, s, 1); bw_put(w, a - 2, 1);
    } else if (a <= 7) {
        bw_put(w, 0x4, 3); bw_put(w, s, 1); bw_put(w, a - 4, 2);
    } else if (a <= 15) {
        bw_put(w, 0x5, 3); bw_put(w, s, 1); bw_put(w, a - 8, 3);
    } else if (a <= 47) {
        bw_put(w, 0x6, 3); bw_put(w, s, 1); bw_put(w, a - 16, 5);
    } else if (a <= 79) {
        bw_put(w, 0xE, 4); bw_put(w, s, 1); bw_put(w, a - 48, 5);
    } else {
        bw_put(w, 0xF, 4); bw_put(w, s, 1); bw_put(w, a - 80, 6);
    }
}

/* ------------------------------------------------------------------------ */
/* Page encoder (section 8)                                                  */

/* Number of consecutive pixels starting at p equal to pixel p + off, at most max. */
static long run_equal(const uint8_t *img, int bpp, long p, long off, long max)
{
    long n = 0;
    if (bpp == 1) {
        const uint8_t *a = img + p, *b = img + p + off;
        while (n < max && a[n] == b[n])
            n++;
    } else {
        const uint8_t *a = img + 4 * p, *b = img + 4 * (p + off);
        while (n < max) {
            uint32_t x, y;
            memcpy(&x, a + 4 * n, 4);
            memcpy(&y, b + 4 * n, 4);
            if (x != y)
                break;
            n++;
        }
    }
    return n;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int hbpl_encode_page(const uint8_t *pixels, int stride, int height, int color,
                     int profile, hbpl_buf *out)
{
    if (!pixels || !out || stride < 16 || stride % 8 != 0 || height < 1)
        return -1;
    if (profile != HBPL_PROFILE_PORTABLE && profile != HBPL_PROFILE_REFERENCE)
        return -1;

    const long W = stride;
    const long N = (long)stride * height;
    if (N / stride != height || (uint64_t)N > UINT32_MAX)
        return -1;
    const int bpp = color ? 4 : 1;
    const int nch = bpp;
    const uint8_t guard = color ? 0xFF : 0x00;
    const long pre = W + 1;    /* guard pixels before pixel 0 (linear model) */

    uint8_t *work = malloc((size_t)(pre + N) * (size_t)bpp);
    if (!work)
        return -1;
    memset(work, guard, (size_t)pre * (size_t)bpp);
    uint8_t *img = work + pre * bpp;
    memcpy(img, pixels, (size_t)N * (size_t)bpp);

    /* 8.1: overwrite column 0. */
    for (long y = 0; y < height; y++) {
        uint8_t v;
        if (!color || profile == HBPL_PROFILE_PORTABLE || y == 0)
            v = 0xFF;
        else
            v = 0x00;
        memset(img + y * W * bpp, v, (size_t)bpp);
    }

    /* 6.2 neighbour offsets in the linear model. */
    const long off[5] = { -1, -W, -W - 1, -W + 1, color ? -W + 2 : -8 };

    /* 8.2 forbidden column ranges; lo < 0 means no restriction. */
    long lo[5], hi[5];
    if (profile == HBPL_PROFILE_REFERENCE && !color) {
        for (int k = 0; k < 5; k++)
            lo[k] = hi[k] = 0;
        lo[1] = hi[1] = -1;
    } else {
        long first = profile == HBPL_PROFILE_REFERENCE ? 1 : 0;
        lo[0] = first;   hi[0] = 1;
        lo[1] = -1;      hi[1] = -1;
        lo[2] = first;   hi[2] = 1;
        lo[3] = W - 1;   hi[3] = W - 1;
        if (color) {
            lo[4] = W - 2; hi[4] = W - 1;
        } else {
            lo[4] = 0;     hi[4] = 8;
        }
    }

    bitw tok = {0}, diff[4] = {{0}};
    int rotor[5] = { 0, 1, 2, 3, 4 };
    int bit = 0, table = 0;
    long stat = 0;
    long p = 0;
    int rc = 0;

    while (p < N) {
        const long col = p % W;
        const long limit = (N - p) < HBPL_MAX_RUN ? (N - p) : HBPL_MAX_RUN;
        const long to_row_end = W - col;

        int best = -1;
        long best_key = 0, best_lim = 0;
        for (int s = 0; s < 5; s++) {
            int k = rotor[s];
            long lim = limit;
            if (lo[k] >= 0) {
                if (lo[k] <= col && col <= hi[k]) {
                    lim = 0;
                } else {
                    long d = ((lo[k] - col) % W + W) % W;
                    if (d < lim)
                        lim = d;
                }
            }
            /* key = min(run, W - col): only scan as far as the key can matter. */
            long cap = lim < to_row_end ? lim : to_row_end;
            long key = cap > 0 ? run_equal(img, bpp, p, off[k], cap) : 0;
            if (key > best_key) {
                best = s;
                best_key = key;
                best_lim = lim;
            }
        }

        if (best < 0) {
            /* Literal pixel. */
            put_token(&tok, table, 5);
            const uint8_t *cur = img + p * bpp, *left = img + (p - 1) * bpp;
            for (int c = 0; c < nch; c++)
                put_diff(&diff[c], (int)(int8_t)(uint8_t)(cur[c] - left[c]));
            bit = 0;
            stat--;
            p++;
            continue;
        }

        /* Run: the length is not capped by the key. */
        long n = best_key;
        if (n == to_row_end && best_lim > n)
            n += run_equal(img, bpp, p + n, off[rotor[best]], best_lim - n);

        if (best > 0) {
            int k = rotor[best];
            for (int s = best; s > 0; s--)
                rotor[s] = rotor[s - 1];
            rotor[0] = k;
        }
        if (p == 0 || (p + n - 1) / W > (p - 1) / W) {
            if ((stat > 8 || stat < -8) && ((stat < 0) != (table == 1))) {
                put_token(&tok, table, 6);
                table ^= 1;
            }
            stat = 0;
        }
        if (best == bit)
            stat++;
        if (best < bit) {   /* cannot happen; see 8.4 */
            rc = -1;
            break;
        }
        put_token(&tok, table, best - bit);
        put_run_length(&tok, (uint32_t)n);
        bit = n < HBPL_MAX_RUN ? 1 : 0;
        p += n;
    }

    free(work);

    /* 7.5: terminate streams. */
    bw_finish(&tok);
    if (buf_byte(&tok.buf, 0xFF) < 0)
        tok.err = 1;
    for (int c = 0; c < nch; c++)
        bw_finish(&diff[c]);

    int err = tok.err;
    for (int c = 0; c < nch; c++)
        err |= diff[c].err;

    if (rc == 0 && !err) {
        uint8_t hdr[48] = {0};
        hdr[0] = 0x20;
        if (color) {
            hdr[4] = 0x00; hdr[5] = 0x01; hdr[6] = 0x01; hdr[7] = 0x40;
        } else {
            hdr[4] = 0x08; hdr[5] = 0x01; hdr[6] = 0x00; hdr[7] = 0x00;
        }
        hdr[8] = 0x10; hdr[9] = 0x32; hdr[10] = 0x04; hdr[11] = 0x00;
        hdr[12] = 0xA1; hdr[13] = 0x42;
        hdr[18] = 0xFF;
        put_le32(hdr + 32, (uint32_t)tok.buf.len);
        put_le32(hdr + 36, (uint32_t)diff[0].buf.len);
        if (color) {
            put_le32(hdr + 40, (uint32_t)diff[1].buf.len);
            put_le32(hdr + 44, (uint32_t)diff[2].buf.len);
        }
        out->len = 0;
        if (hbpl_buf_append(out, hdr, sizeof hdr) < 0 ||
            hbpl_buf_append(out, tok.buf.data, tok.buf.len) < 0)
            rc = -1;
        for (int c = 0; rc == 0 && c < nch; c++)
            if (hbpl_buf_append(out, diff[c].buf.data, diff[c].buf.len) < 0)
                rc = -1;
    } else {
        rc = -1;
    }

    hbpl_buf_free(&tok.buf);
    for (int c = 0; c < 4; c++)
        hbpl_buf_free(&diff[c].buf);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Job framing (sections 1-3)                                                */

/* Append s with double quotes and bytes below 0x20 removed. */
static int buf_clean(hbpl_buf *b, const char *s)
{
    if (!s)
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == '"')
            continue;
        if (buf_byte(b, c) < 0)
            return -1;
    }
    return 0;
}

static int buf_line(hbpl_buf *b, const char *s)
{
    return (buf_str(b, s) < 0 || buf_byte(b, '\n') < 0) ? -1 : 0;
}

static int buf_line_kv(hbpl_buf *b, const char *prefix, const char *value,
                       const char *suffix)
{
    if (buf_str(b, prefix) < 0 || buf_clean(b, value) < 0 || buf_str(b, suffix) < 0 ||
        buf_byte(b, '\n') < 0)
        return -1;
    return 0;
}

static int buf_fmt_line(hbpl_buf *b, const char *fmt, const char *a, int v)
{
    char line[160];
    int n = a ? snprintf(line, sizeof line, fmt, a) : snprintf(line, sizeof line, fmt, v);
    if (n < 0 || (size_t)n >= sizeof line)
        return -1;
    return buf_line(b, line);
}

int hbpl_append_job_header(hbpl_buf *out, const hbpl_job *job)
{
    if (!out || !job)
        return -1;
    static const char *const ajx[] = {
        "AJCL", "AJCM", "AJCH", "AJML", "AJMM", "AJMH",
        "AJYL", "AJYM", "AJYH", "AJKL", "AJKM", "AJKH",
    };
    static const char *const cal_color[] = {
        "TCPR", "GCPR", "ICPR", "TUCR", "GUCR", "IUCR",
        "TTRC", "GTRC", "ITRC", "TSCR", "GSCR", "ISCR",
    };
    static const char *const cal_mono[] = { "TTRC", "GTRC", "ITRC", "TSCR", "GSCR", "ISCR" };

    char date[16], tim[16];
    time_t t = job->time;
    struct tm *lt = localtime(&t);
    if (!lt)
        return -1;
    snprintf(date, sizeof date, "%02d/%02d/%04d", lt->tm_mon + 1, lt->tm_mday,
             lt->tm_year + 1900);
    snprintf(tim, sizeof tim, "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);

    const int duplex = job->manual_duplex;
    if (duplex < 0 || duplex > 2)
        return -1;
    const int copies = job->copies < 1 ? 1 : job->copies;
    const char *media = job->media ? job->media : "NORMAL";

    hbpl_buf *b = out;
    int e = 0;
    e |= buf_line(b, "\x1b%-12345X@PJL JOB MODE=PRINTER");
    e |= buf_line(b, "@PJL SET STRINGCODESET=UTF8");
    e |= buf_fmt_line(b, "@PJL COMMENT DATE=%s", date, 0);
    e |= buf_fmt_line(b, "@PJL COMMENT TIME=%s", tim, 0);
    e |= buf_line_kv(b, "@PJL COMMENT DNAME=", job->title, "");
    e |= buf_line_kv(b, "@PJL SET JOBATTR=\"@LUNA=", job->user, "\"");
    e |= buf_fmt_line(b, "@PJL SET COPIES=%d", NULL, copies);
    e |= buf_line(b, "@PJL SET QTY=1");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@TRCH=OFF\"");
    if (duplex == 0) {
        e |= buf_line(b, "@PJL SET DUPLEX=OFF");
    } else {
        e |= buf_line(b, "@PJL SET DUPLEX=ON");
        e |= buf_line(b, duplex == 1 ? "@PJL SET BINDING=LONGEDGE"
                                     : "@PJL SET BINDING=SHORTEDGE");
    }
    for (size_t i = 0; i < sizeof ajx / sizeof ajx[0]; i++)
        e |= buf_fmt_line(b, "@PJL SET JOBATTR=\"@%s=3\"", ajx[i], 0);
    e |= buf_line(b, "@PJL SET JOBATTR=\"@APSP=OFF\"");
    e |= buf_line_kv(b, "@PJL SET JOBATTR=\"@MSIP=", media, "\"");
    e |= buf_line(b, job->color ? "@PJL SET RENDERMODE=COLOR" : "@PJL SET RENDERMODE=GRAYSCALE");
    e |= buf_line(b, job->economode ? "@PJL SET ECONOMODE=ON" : "@PJL SET ECONOMODE=OFF");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@IREC=OFF\"");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@TRAP=OFF\"");
    e |= buf_line(b, "@PJL SET RET=OFF");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@TDFT=0\"");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@GDFT=0\"");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@IDFT=0\"");
    /* NLPP precedes JOAU/CNAM in every reference job (SPEC.md section 2 lists
     * it after them). */
    e |= buf_line(b, "@PJL SET JOBATTR=\"@NLPP=1\"");
    e |= buf_line_kv(b, "@PJL SET JOBATTR=\"@JOAU=", job->user, "\"");
    e |= buf_line_kv(b, "@PJL SET JOBATTR=\"@CNAM=", job->host, "\"");
    e |= buf_line(b, "@PJL SET OUTBIN=FACEDOWN");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@PODR=NORMAL\"");
    e |= buf_line(b, "@PJL SET SLIPSHEET=OFF");
    e |= buf_line(b, job->long_edge_feed ? "@PJL SET PAPERDIRECTION=LEF"
                                         : "@PJL SET PAPERDIRECTION=SEF");
    e |= buf_line(b, "@PJL SET RESOLUTION=600");
    e |= buf_line(b, "@PJL SET BITSPERPIXEL=8");
    e |= buf_line(b, "@PJL SET JOBATTR=\"@DRDM=RASTER\"");
    if (job->color) {
        for (size_t i = 0; i < sizeof cal_color / sizeof cal_color[0]; i++)
            e |= buf_fmt_line(b, "@PJL SET JOBATTR=\"@%s=11\"", cal_color[i], 0);
    } else {
        for (size_t i = 0; i < sizeof cal_mono / sizeof cal_mono[0]; i++)
            e |= buf_fmt_line(b, "@PJL SET JOBATTR=\"@%s=33\"", cal_mono[i], 0);
        e |= buf_line(b, "@PJL SET JOBATTR=\"@PBLK=OFF\"");
    }
    e |= buf_line(b, "@PJL SET JOBATTR=\"@BANR=DEVICE\"");
    if (duplex == 0) {
        e |= buf_line(b, "@PJL SET IWAMANUALDUP=OFF");
    } else {
        e |= buf_line(b, "@PJL SET IWAMANUALDUP=ON");
        e |= buf_fmt_line(b, "@PJL SET IWASIDE2START=%d", NULL, job->side2_start);
    }
    e |= buf_line(b, job->jam_recovery ? "@PJL SET IWAJAMRECOVERY=ON"
                                       : "@PJL SET IWAJAMRECOVERY=AUTO");
    e |= buf_line(b, "@PJL ENTER LANGUAGE=HBPL");

    /* 3.2: job record A. */
    static const uint8_t rec_a[12] = {
        0x41, 0x81, 0xA1, 0x00, 0x82, 0xA2, 0x07, 0x00, 0x83, 0xA2, 0x01, 0x00,
    };
    e |= hbpl_buf_append(b, rec_a, sizeof rec_a);
    return e ? -1 : 0;
}

static int attr_a1(hbpl_buf *b, uint8_t id, uint8_t v)
{
    uint8_t r[3] = { id, 0xA1, v };
    return hbpl_buf_append(b, r, sizeof r);
}

static int attr_a4(hbpl_buf *b, uint8_t id, uint32_t v)
{
    uint8_t r[6] = { id, 0xA4 };
    put_le32(r + 2, v);
    return hbpl_buf_append(b, r, sizeof r);
}

static int attr_pair16(hbpl_buf *b, uint8_t id, uint8_t type, uint16_t x, uint16_t y)
{
    uint8_t r[6] = { id, type, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8) };
    return hbpl_buf_append(b, r, sizeof r);
}

static int attr_c4(hbpl_buf *b, uint8_t id, uint32_t x, uint32_t y)
{
    uint8_t r[10] = { id, 0xC4 };
    put_le32(r + 2, x);
    put_le32(r + 6, y);
    return hbpl_buf_append(b, r, sizeof r);
}

int hbpl_append_page(hbpl_buf *out, const hbpl_page *page, const hbpl_buf *data)
{
    if (!out || !page || !data || (data->len && !data->data) || data->len > UINT32_MAX)
        return -1;
    if (page->paper_code < 0 || page->paper_code > 255)
        return -1;
    const bool custom = page->paper_code == 255;
    hbpl_buf *b = out;
    int e = 0;
    e |= buf_byte(b, 0x43);
    e |= attr_a1(b, 0x91, page->long_edge_feed ? 1 : 0);
    e |= attr_a1(b, 0x92, 1);
    e |= attr_a1(b, 0x93, 1);
    e |= attr_a1(b, 0x94, (uint8_t)page->paper_code);
    e |= attr_pair16(b, 0x95, 0xC2, custom ? page->custom_width : 0,
                     custom ? page->custom_height : 0);
    e |= attr_a1(b, 0x96, 3);
    e |= attr_pair16(b, 0x97, 0xC3, page->offset_x, page->offset_y);
    e |= attr_a1(b, 0x98, 0);
    e |= attr_a4(b, 0x99, page->page_number);
    e |= attr_c4(b, 0x9A, page->content_width, page->height);
    e |= attr_a1(b, 0x9B, page->blank ? 1 : 0);
    e |= attr_a1(b, 0x9C, 1);
    e |= attr_a1(b, 0x9D, page->color ? 0x8B : 0x09);
    e |= attr_a1(b, 0x9E, 2);
    e |= attr_a1(b, 0x9F, 5);
    e |= attr_a1(b, 0xA0, 8);
    e |= attr_a1(b, 0xA1, 0);
    e |= attr_c4(b, 0xA2, page->stride, page->height);
    e |= buf_byte(b, 0x51);   /* Q */

    uint8_t r[9] = { 0x52, 0xA3, 0xA1, 0x00, 0xA4, 0xB1, 0xA4 };
    uint8_t len[4];
    put_le32(len, (uint32_t)data->len);
    e |= hbpl_buf_append(b, r, 7);
    e |= hbpl_buf_append(b, len, 4);
    e |= hbpl_buf_append(b, data->data, data->len);
    e |= hbpl_buf_append(b, "SD", 2);
    return e ? -1 : 0;
}

int hbpl_append_job_trailer(hbpl_buf *out)
{
    if (!out)
        return -1;
    int e = buf_byte(out, 0x42);
    e |= buf_line(out, "\x1b%-12345X@PJL EOJ");
    return e ? -1 : 0;
}
