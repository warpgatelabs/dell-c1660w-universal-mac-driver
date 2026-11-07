// SPDX-License-Identifier: Apache-2.0
/*
 * HBPL version 1 job parser and page decoder.
 */
#include "hbpl_decode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HBPL_DEC_MAX_RUN 17057

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static uint32_t get_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------------ */
/* Records                                                                   */

static const char pjl_enter[] = "@PJL ENTER LANGUAGE=HBPL\n";
static const char pjl_trailer[] = "\x1b%-12345X@PJL EOJ\n";

/* Parse one attribute at d[*pos]. Returns 0 or -1. */
static int parse_attr(const uint8_t *d, size_t len, size_t *pos, hbpl_attr *a)
{
    size_t p = *pos;
    if (p + 2 > len)
        return -1;
    a->id = d[p++];
    a->type = d[p++];
    a->prefixed = false;
    a->v[0] = a->v[1] = 0;
    if (a->type == HBPL_T_B1) {
        if (p >= len)
            return -1;
        a->prefixed = true;
        a->type = d[p++];
    }
    switch (a->type) {
    case HBPL_T_A1:
        if (p + 1 > len) return -1;
        a->v[0] = d[p];
        p += 1;
        break;
    case HBPL_T_A2:
        if (p + 2 > len) return -1;
        a->v[0] = get_le16(d + p);
        p += 2;
        break;
    case HBPL_T_A4:
        if (p + 4 > len) return -1;
        a->v[0] = get_le32(d + p);
        p += 4;
        break;
    case HBPL_T_C2:
    case HBPL_T_C3:
        if (p + 4 > len) return -1;
        a->v[0] = get_le16(d + p);
        a->v[1] = get_le16(d + p + 2);
        p += 4;
        break;
    case HBPL_T_C4:
        if (p + 8 > len) return -1;
        a->v[0] = get_le32(d + p);
        a->v[1] = get_le32(d + p + 4);
        p += 8;
        break;
    default:
        return -1;
    }
    *pos = p;
    return 0;
}

static int push_record(hbpl_parsed_job *job, size_t *cap, const hbpl_record *r)
{
    if (job->nrecords == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        hbpl_record *n = realloc(job->records, nc * sizeof *n);
        if (!n)
            return -1;
        job->records = n;
        *cap = nc;
    }
    job->records[job->nrecords++] = *r;
    return 0;
}

static int push_page(hbpl_parsed_job *job, size_t *cap)
{
    if (job->npages == *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        hbpl_page_info *n = realloc(job->pages, nc * sizeof *n);
        if (!n)
            return -1;
        job->pages = n;
        *cap = nc;
    }
    memset(&job->pages[job->npages++], 0, sizeof job->pages[0]);
    return 0;
}

static void page_from_record(hbpl_page_info *pg, const hbpl_record *r)
{
    for (int i = 0; i < r->nattrs; i++) {
        const hbpl_attr *a = &r->attrs[i];
        switch (a->id) {
        case 0x91: pg->long_edge_feed = (int)a->v[0]; break;
        case 0x94: pg->paper_code = (int)a->v[0]; break;
        case 0x95: pg->custom_width = a->v[0]; pg->custom_height = a->v[1]; break;
        case 0x97: pg->offset_x = a->v[0]; pg->offset_y = a->v[1]; break;
        case 0x99: pg->page_number = a->v[0]; break;
        case 0x9A: pg->content_width = a->v[0]; pg->content_height = a->v[1]; break;
        case 0x9B: pg->blank = (int)a->v[0]; break;
        case 0x9D: pg->data_format = (int)a->v[0]; break;
        case 0xA2: pg->stride = a->v[0]; pg->height = a->v[1]; break;
        default: break;
        }
    }
    pg->color = pg->data_format == 0x8B;
}

void hbpl_parsed_job_free(hbpl_parsed_job *job)
{
    if (!job)
        return;
    free(job->records);
    free(job->pages);
    memset(job, 0, sizeof *job);
}

int hbpl_parse_job(const uint8_t *d, size_t len, hbpl_parsed_job *job, char *err,
                   size_t errlen)
{
    if (!d || !job) {
        set_err(err, errlen, "invalid arguments");
        return -1;
    }
    memset(job, 0, sizeof *job);

    /* PJL header: everything through the ENTER LANGUAGE line. */
    const size_t elen = sizeof pjl_enter - 1;
    size_t pos = 0;
    bool found = false;
    for (size_t i = 0; i + elen <= len; i++) {
        if ((i == 0 || d[i - 1] == '\n') && memcmp(d + i, pjl_enter, elen) == 0) {
            pos = i + elen;
            found = true;
            break;
        }
    }
    if (!found) {
        set_err(err, errlen, "no \"@PJL ENTER LANGUAGE=HBPL\" line");
        return -1;
    }
    job->pjl = d;
    job->pjl_len = pos;

    size_t rcap = 0, pcap = 0;
    bool in_page = false, ended = false;
    while (pos < len && !ended) {
        hbpl_record r;
        memset(&r, 0, sizeof r);
        r.offset = pos;
        r.type = d[pos++];
        uint8_t term = 0;
        switch (r.type) {
        case 'A': term = 0x83; break;
        case 'C': term = 0xA2; break;
        case 'R': term = 0xA4; break;
        case 'Q': case 'S': case 'D': case 'B': break;
        default:
            set_err(err, errlen, "unknown record type 0x%02X at offset %zu", r.type, r.offset);
            goto fail;
        }
        if (term) {
            for (;;) {
                if (r.nattrs == HBPL_MAX_ATTRS) {
                    set_err(err, errlen, "too many attributes in record at offset %zu",
                            r.offset);
                    goto fail;
                }
                hbpl_attr *a = &r.attrs[r.nattrs];
                if (parse_attr(d, len, &pos, a) < 0) {
                    set_err(err, errlen, "bad attribute in record '%c' at offset %zu",
                            r.type, r.offset);
                    goto fail;
                }
                r.nattrs++;
                if (a->id == term)
                    break;
            }
        }
        r.size = pos - r.offset;

        if (r.type == 'C') {
            if (push_page(job, &pcap) < 0)
                goto oom;
            hbpl_page_info *pg = &job->pages[job->npages - 1];
            pg->record_index = job->nrecords;
            page_from_record(pg, &r);
            in_page = true;
        } else if (r.type == 'R') {
            if (!in_page || job->pages[job->npages - 1].data) {
                set_err(err, errlen, "R record without a page at offset %zu", r.offset);
                goto fail;
            }
            uint32_t dlen = 0;
            for (int i = 0; i < r.nattrs; i++)
                if (r.attrs[i].id == 0xA4)
                    dlen = r.attrs[i].v[0];
            if (dlen > len - pos) {
                set_err(err, errlen, "data block at offset %zu overruns the job", pos);
                goto fail;
            }
            hbpl_page_info *pg = &job->pages[job->npages - 1];
            pg->data = d + pos;
            pg->data_len = dlen;
            pos += dlen;
        } else if (r.type == 'D') {
            in_page = false;
        } else if (r.type == 'B') {
            ended = true;
        }
        if (push_record(job, &rcap, &r) < 0)
            goto oom;
    }
    if (!ended) {
        set_err(err, errlen, "job ends without a B record");
        goto fail;
    }
    for (size_t i = 0; i < job->npages; i++) {
        job->pages[i].c = &job->records[job->pages[i].record_index];
        if (!job->pages[i].data) {
            set_err(err, errlen, "page %zu has no data block", i + 1);
            goto fail;
        }
    }
    job->trailer = d + pos;
    job->trailer_len = len - pos;
    job->trailer_ok = job->trailer_len == sizeof pjl_trailer - 1 &&
                      memcmp(job->trailer, pjl_trailer, job->trailer_len) == 0;
    return 0;

oom:
    set_err(err, errlen, "out of memory");
fail:
    hbpl_parsed_job_free(job);
    return -1;
}

int hbpl_pjl_get(const hbpl_parsed_job *job, const char *name, char *buf, size_t buflen)
{
    if (!job || !job->pjl || !name || !buf || buflen == 0)
        return -1;
    const char *prefixes[3] = { "@PJL SET ", "@PJL COMMENT ", "@PJL SET JOBATTR=\"" };
    const int first = name[0] == '@' ? 2 : 0, last = name[0] == '@' ? 2 : 1;
    const size_t nlen = strlen(name);
    const char *s = (const char *)job->pjl, *end = s + job->pjl_len;
    while (s < end) {
        const char *eol = memchr(s, '\n', (size_t)(end - s));
        if (!eol)
            eol = end;
        for (int k = first; k <= last; k++) {
            size_t pl = strlen(prefixes[k]);
            if ((size_t)(eol - s) > pl + nlen && memcmp(s, prefixes[k], pl) == 0 &&
                memcmp(s + pl, name, nlen) == 0 && s[pl + nlen] == '=') {
                const char *v = s + pl + nlen + 1, *ve = eol;
                if (k == 2 && ve > v && ve[-1] == '"')
                    ve--;
                size_t vl = (size_t)(ve - v);
                if (vl + 1 > buflen)
                    return -1;
                memcpy(buf, v, vl);
                buf[vl] = '\0';
                return 0;
            }
        }
        s = eol + 1;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Data block                                                                */

int hbpl_parse_block(const uint8_t *b, size_t len, bool color, hbpl_block_info *info)
{
    if (!b || !info || len < 48)
        return -1;
    memset(info, 0, sizeof *info);
    info->color = color;
    info->nstreams = color ? 5 : 2;
    uint64_t total = 48;
    for (int i = 0; i < (color ? 4 : 2); i++) {
        info->len[i] = get_le32(b + 32 + 4 * i);
        total += info->len[i];
    }
    if (total > len)
        return -1;
    if (color)
        info->len[4] = (uint32_t)(len - total);
    size_t off = 48;
    for (int i = 0; i < info->nstreams; i++) {
        info->stream[i] = b + off;
        off += info->len[i];
    }
    return 0;
}

/* MSB-first bit reader. */
typedef struct {
    const uint8_t *p;
    size_t nbits;
    size_t pos;
} bitr;

/* Read n bits (n <= 24). Returns 0 or -1 at end of stream. */
static inline int br_get(bitr *r, int n, uint32_t *v)
{
    if (r->pos + (size_t)n > r->nbits)
        return -1;
    size_t byte = r->pos >> 3;
    int shift = (int)(r->pos & 7);
    uint32_t acc = 0;
    int have = 0;
    while (have < shift + n) {
        acc = (acc << 8) | r->p[byte++];
        have += 8;
    }
    acc >>= have - shift - n;
    *v = acc & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
    r->pos += (size_t)n;
    return 0;
}

static inline int br_bit(bitr *r)
{
    if (r->pos >= r->nbits)
        return -1;
    int b = (r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return b;
}

/* Token (section 7.1). Returns 0-6 or -1. */
static int read_token(bitr *r, int table)
{
    int b = br_bit(r);
    if (b < 0) return -1;
    if (b == 0) return table ? 5 : 0;
    if ((b = br_bit(r)) < 0) return -1;
    if (b == 0) return table ? 0 : 5;
    if ((b = br_bit(r)) < 0) return -1;
    if (b == 0) return 1;
    if ((b = br_bit(r)) < 0) return -1;
    if (b == 0) {
        if ((b = br_bit(r)) < 0) return -1;
        return b ? 3 : 2;
    }
    if ((b = br_bit(r)) < 0) return -1;
    if (b == 0) return 4;
    if (br_bit(r) < 0) return -1;   /* sixth bit of token 6, ignored */
    return 6;
}

/* Run length (section 7.2). Returns 1-17057 or -1. */
static long read_run_length(bitr *r)
{
    static const struct { uint16_t base; uint8_t extra; } grp[8] = {
        {26, 3}, {34, 3}, {42, 3}, {50, 4}, {66, 5}, {98, 6}, {162, 9}, {674, 14},
    };
    uint32_t v, x;
    if (br_get(r, 2, &v) < 0) return -1;
    switch (v) {
    case 0:
        return 1;
    case 1:
        if (br_get(r, 1, &x) < 0) return -1;
        return 2 + (long)x;
    case 2:
        if (br_get(r, 1, &x) < 0) return -1;
        if (x == 0) {
            if (br_get(r, 1, &x) < 0) return -1;
            return 4 + (long)x;
        }
        if (br_get(r, 2, &x) < 0) return -1;
        return 6 + (long)x;
    default:
        if (br_get(r, 1, &x) < 0) return -1;
        if (x == 0) {
            if (br_get(r, 4, &x) < 0) return -1;
            return 10 + (long)x;
        }
        if (br_get(r, 3, &v) < 0) return -1;
        if (br_get(r, grp[v].extra, &x) < 0) return -1;
        return grp[v].base + (long)x;
    }
}

/* Difference (section 7.3). Returns 0 and stores d, or -1. */
static int read_diff(bitr *r, int *d)
{
    uint32_t v, s, x;
    int base, extra;
    if (br_get(r, 3, &v) < 0) return -1;
    switch (v) {
    case 0: *d = 0; return 0;
    case 1: *d = 1; return 0;
    case 2: *d = -1; return 0;
    case 3: base = 2; extra = 1; break;
    case 4: base = 4; extra = 2; break;
    case 5: base = 8; extra = 3; break;
    case 6: base = 16; extra = 5; break;
    default:
        if (br_get(r, 1, &x) < 0) return -1;
        if (x == 0) { base = 48; extra = 5; } else { base = 80; extra = 6; }
        break;
    }
    if (br_get(r, 1, &s) < 0 || br_get(r, extra, &x) < 0)
        return -1;
    int a = base + (int)x;
    *d = s ? -a : a;
    return 0;
}

int hbpl_decode_block(const uint8_t *block, size_t len, int stride, int height, bool color,
                      int model, uint8_t *pixels)
{
    if (!pixels || stride < 1 || height < 1)
        return -1;
    if (model != HBPL_MODEL_LINEAR && model != HBPL_MODEL_ROW)
        return -1;
    hbpl_block_info bi;
    if (hbpl_parse_block(block, len, color, &bi) < 0)
        return -1;

    const long W = stride;
    const long N = (long)stride * height;
    const int bpp = color ? 4 : 1;
    const uint8_t guard_px[4] = { color ? 0xFF : 0, color ? 0xFF : 0, color ? 0xFF : 0,
                                  color ? 0xFF : 0 };
    const bool row = model == HBPL_MODEL_ROW;

    const long off[5] = { -1, -W, -W - 1, -W + 1, color ? -W + 2 : -8 };
    const long dx[5] = { -1, 0, -1, 1, color ? 2 : -8 };

    bitr tok = { bi.stream[0], (size_t)bi.len[0] * 8, 0 };
    bitr ch[4];
    for (int c = 0; c < bpp; c++) {
        ch[c].p = bi.stream[c + 1];
        ch[c].nbits = (size_t)bi.len[c + 1] * 8;
        ch[c].pos = 0;
    }

    int rotor[5] = { 0, 1, 2, 3, 4 };
    int bit = 0, table = 0;
    long p = 0, col = 0;
    while (p < N) {
        int t = read_token(&tok, table);
        if (t < 0)
            return -1;
        if (t == 6) {
            table ^= 1;
            continue;
        }
        if (t == 5) {
            const uint8_t *left;
            if (p == 0 || (row && col == 0))
                left = guard_px;
            else
                left = pixels + (p - 1) * bpp;
            uint8_t *dst = pixels + p * bpp;
            for (int c = 0; c < bpp; c++) {
                int d;
                if (read_diff(&ch[c], &d) < 0)
                    return -1;
                dst[c] = (uint8_t)(left[c] + d);
            }
            bit = 0;
            p++;
            if (++col == W)
                col = 0;
            continue;
        }
        int s = t + bit;
        if (s > 4)
            return -1;
        long n = read_run_length(&tok);
        if (n < 0 || n > N - p)
            return -1;
        const int k = rotor[s];
        const long o = off[k], ddx = dx[k];
        for (long i = 0; i < n; i++) {
            const uint8_t *src;
            long q = p + o;
            long sc = col + ddx;
            if (q < 0 || (row && (sc < 0 || sc >= W)))
                src = guard_px;
            else
                src = pixels + q * bpp;
            if (bpp == 1)
                pixels[p] = *src;
            else
                memcpy(pixels + p * 4, src, 4);
            p++;
            if (++col == W)
                col = 0;
        }
        if (s > 0) {
            for (int j = s; j > 0; j--)
                rotor[j] = rotor[j - 1];
            rotor[0] = k;
        }
        bit = n < HBPL_DEC_MAX_RUN ? 1 : 0;
    }
    return 0;
}

size_t hbpl_page_pixel_bytes(const hbpl_page_info *page)
{
    if (!page)
        return 0;
    return (size_t)page->stride * page->height * (page->color ? 4u : 1u);
}

int hbpl_decode_page(const hbpl_page_info *page, int model, uint8_t *pixels)
{
    if (!page || page->stride == 0 || page->height == 0 || page->stride > 0x7FFFFFFF ||
        page->height > 0x7FFFFFFF)
        return -1;
    return hbpl_decode_block(page->data, page->data_len, (int)page->stride,
                             (int)page->height, page->color, model, pixels);
}
