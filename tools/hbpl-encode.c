// SPDX-License-Identifier: Apache-2.0
/*
 * hbpl-encode: build an HBPL v1 job from PAM page images.
 *
 *   hbpl-encode [--profile portable|reference] [--content-width N] [--offset X,Y]
 *               [--paper N] [--custom-size W,H] page.pam... > job
 *
 * Each PAM image (a file may hold several) is P7 with MAXVAL 255 and either
 * TUPLTYPE KCMY, DEPTH 4 (bytes K, C, M, Y = toner amounts) or TUPLTYPE
 * GRAYSCALE, DEPTH 1 (raw K toner amount, 0 = none). WIDTH is the stride and
 * must be a multiple of 8, at least 16. All pages must be of the same kind.
 */
#include "hbpl.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int width, height, depth;
    uint8_t *pixels;
} pam_image;

static void usage(void)
{
    fprintf(stderr,
            "usage: hbpl-encode [--profile portable|reference] [--content-width N]\n"
            "                   [--offset X,Y] [--paper N] [--custom-size W,H] page.pam... > job\n");
    exit(2);
}

static int parse_long(const char *s, long lo, long hi, long *v)
{
    char *end;
    errno = 0;
    long x = strtol(s, &end, 10);
    if (errno || end == s || *end || x < lo || x > hi)
        return -1;
    *v = x;
    return 0;
}

static int parse_pair(const char *s, long lo, long hi, long *a, long *b)
{
    const char *comma = strchr(s, ',');
    if (!comma)
        return -1;
    char first[32];
    size_t n = (size_t)(comma - s);
    if (n == 0 || n >= sizeof first)
        return -1;
    memcpy(first, s, n);
    first[n] = '\0';
    return (parse_long(first, lo, hi, a) < 0 || parse_long(comma + 1, lo, hi, b) < 0) ? -1 : 0;
}

/* Read one header token, skipping whitespace and comments. */
static int pam_token(FILE *f, char *tok, size_t n)
{
    int c;
    for (;;) {
        c = getc(f);
        if (c == '#') {
            while (c != EOF && c != '\n')
                c = getc(f);
        }
        if (c == EOF)
            return -1;
        if (!isspace(c))
            break;
    }
    size_t i = 0;
    while (c != EOF && !isspace(c)) {
        if (i + 1 < n)
            tok[i++] = (char)c;
        c = getc(f);
    }
    tok[i] = '\0';
    return 0;
}

/* Read the rest of the line (the TUPLTYPE value). */
static int pam_rest_of_line(FILE *f, char *buf, size_t n)
{
    int c;
    do
        c = getc(f);
    while (c == ' ' || c == '\t');
    size_t i = 0;
    while (c != EOF && c != '\n') {
        if (i + 1 < n)
            buf[i++] = (char)c;
        c = getc(f);
    }
    while (i > 0 && isspace((unsigned char)buf[i - 1]))
        i--;
    buf[i] = '\0';
    return 0;
}

/* Returns 1 on success, 0 at clean EOF, -1 on error. */
static int pam_read(FILE *f, const char *name, pam_image *img)
{
    int c;
    do
        c = getc(f);
    while (c != EOF && isspace(c));
    if (c == EOF)
        return 0;
    if (c != 'P' || getc(f) != '7') {
        fprintf(stderr, "hbpl-encode: %s: not a PAM (P7) file\n", name);
        return -1;
    }
    long width = -1, height = -1, depth = -1, maxval = -1;
    char tupl[64] = "";
    char tok[64];
    for (;;) {
        if (pam_token(f, tok, sizeof tok) < 0) {
            fprintf(stderr, "hbpl-encode: %s: truncated PAM header\n", name);
            return -1;
        }
        if (!strcmp(tok, "ENDHDR")) {
            /* pam_token consumed the single whitespace after ENDHDR. */
            break;
        }
        if (!strcmp(tok, "TUPLTYPE")) {
            pam_rest_of_line(f, tupl, sizeof tupl);
            continue;
        }
        long *dst = !strcmp(tok, "WIDTH") ? &width : !strcmp(tok, "HEIGHT") ? &height
                  : !strcmp(tok, "DEPTH") ? &depth : !strcmp(tok, "MAXVAL") ? &maxval : NULL;
        char val[64];
        if (!dst || pam_token(f, val, sizeof val) < 0 || parse_long(val, 0, 1L << 30, dst) < 0) {
            fprintf(stderr, "hbpl-encode: %s: bad PAM header field %s\n", name, tok);
            return -1;
        }
    }
    int color;
    if (!strcmp(tupl, "KCMY") && depth == 4)
        color = 1;
    else if (!strcmp(tupl, "GRAYSCALE") && depth == 1)
        color = 0;
    else {
        fprintf(stderr, "hbpl-encode: %s: need TUPLTYPE KCMY depth 4 or GRAYSCALE depth 1\n",
                name);
        return -1;
    }
    if (maxval != 255 || width < 16 || width % 8 != 0 || height < 1) {
        fprintf(stderr, "hbpl-encode: %s: need MAXVAL 255 and WIDTH a multiple of 8 >= 16\n",
                name);
        return -1;
    }
    size_t n = (size_t)width * (size_t)height * (color ? 4u : 1u);
    img->pixels = malloc(n);
    if (!img->pixels || fread(img->pixels, 1, n, f) != n) {
        fprintf(stderr, "hbpl-encode: %s: truncated image data\n", name);
        free(img->pixels);
        img->pixels = NULL;
        return -1;
    }
    img->width = (int)width;
    img->height = (int)height;
    img->depth = (int)depth;
    return 1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    int profile = HBPL_PROFILE_PORTABLE;
    long content_width = -1, off_x = 97, off_y = 97, paper = 0, cw = 0, ch = 0;
    int first_file = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--profile") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "portable"))
                profile = HBPL_PROFILE_PORTABLE;
            else if (!strcmp(p, "reference"))
                profile = HBPL_PROFILE_REFERENCE;
            else
                usage();
        } else if (!strcmp(a, "--content-width") && i + 1 < argc) {
            if (parse_long(argv[++i], 1, 0x7FFFFFFF, &content_width) < 0)
                usage();
        } else if (!strcmp(a, "--offset") && i + 1 < argc) {
            if (parse_pair(argv[++i], 0, 65535, &off_x, &off_y) < 0)
                usage();
        } else if (!strcmp(a, "--paper") && i + 1 < argc) {
            if (parse_long(argv[++i], 0, 255, &paper) < 0)
                usage();
        } else if (!strcmp(a, "--custom-size") && i + 1 < argc) {
            if (parse_pair(argv[++i], 0, 65535, &cw, &ch) < 0)
                usage();
        } else if (a[0] == '-' && a[1] == '-') {
            usage();
        } else {
            first_file = i;
            break;
        }
    }
    if (first_file >= argc)
        usage();

    hbpl_buf job_buf = {0};
    hbpl_buf pages_buf = {0};
    hbpl_buf block = {0};
    int job_color = -1;
    uint32_t page_no = 0;
    int rc = 0;

    for (int i = first_file; i < argc && rc == 0; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "hbpl-encode: cannot open %s\n", argv[i]);
            rc = 1;
            break;
        }
        for (;;) {
            pam_image img = {0};
            int r = pam_read(f, argv[i], &img);
            if (r == 0)
                break;
            if (r < 0) {
                rc = 1;
                break;
            }
            int color = img.depth == 4;
            if (job_color < 0)
                job_color = color;
            if (color != job_color) {
                fprintf(stderr, "hbpl-encode: %s: all pages must be color or all mono\n",
                        argv[i]);
                free(img.pixels);
                rc = 1;
                break;
            }
            if (hbpl_encode_page(img.pixels, img.width, img.height, color, profile, &block) < 0) {
                fprintf(stderr, "hbpl-encode: %s: encoding failed\n", argv[i]);
                free(img.pixels);
                rc = 1;
                break;
            }
            hbpl_page pg = {0};
            pg.paper_code = (int)paper;
            pg.custom_width = (uint16_t)cw;
            pg.custom_height = (uint16_t)ch;
            pg.offset_x = (uint16_t)off_x;
            pg.offset_y = (uint16_t)off_y;
            pg.page_number = ++page_no;
            pg.content_width = content_width > 0 ? (uint32_t)content_width : (uint32_t)img.width;
            pg.stride = (uint32_t)img.width;
            pg.height = (uint32_t)img.height;
            pg.color = color;
            free(img.pixels);
            if (hbpl_append_page(&pages_buf, &pg, &block) < 0) {
                fprintf(stderr, "hbpl-encode: out of memory\n");
                rc = 1;
                break;
            }
        }
        fclose(f);
    }

    if (rc == 0 && page_no == 0) {
        fprintf(stderr, "hbpl-encode: no pages\n");
        rc = 1;
    }
    if (rc == 0) {
        const char *user = getenv("USER");
        hbpl_job job = {0};
        job.title = base_name(argv[first_file]);
        job.user = user ? user : "";
        job.host = "";
        job.time = time(NULL);
        job.copies = 1;
        job.color = job_color == 1;
        if (hbpl_append_job_header(&job_buf, &job) < 0 ||
            hbpl_buf_append(&job_buf, pages_buf.data, pages_buf.len) < 0 ||
            hbpl_append_job_trailer(&job_buf) < 0) {
            fprintf(stderr, "hbpl-encode: out of memory\n");
            rc = 1;
        } else if (fwrite(job_buf.data, 1, job_buf.len, stdout) != job_buf.len ||
                   fflush(stdout) != 0) {
            fprintf(stderr, "hbpl-encode: write error\n");
            rc = 1;
        }
    }
    hbpl_buf_free(&job_buf);
    hbpl_buf_free(&pages_buf);
    hbpl_buf_free(&block);
    return rc;
}
