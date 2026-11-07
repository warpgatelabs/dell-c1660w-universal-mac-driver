// SPDX-License-Identifier: Apache-2.0
/*
 * hbpl-decode: inspect an HBPL v1 job and optionally write its pages as PAM.
 *
 *   hbpl-decode [--model linear|row] [--out BASE] [--quiet] < job
 *
 * Prints the PJL header, every record and attribute, and each page's stream
 * lengths. With --out, writes BASE-NN.pam per page (NN = page number), as
 * P7 TUPLTYPE KCMY (depth 4) or GRAYSCALE (depth 1, raw K toner values).
 */
#include "hbpl_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr, "usage: hbpl-decode [--model linear|row] [--out BASE] [--quiet] < job\n");
    exit(2);
}

static uint8_t *read_all(FILE *f, size_t *len)
{
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0)
            break;
    }
    if (buf && ferror(f)) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case HBPL_T_A1: return "A1";
    case HBPL_T_A2: return "A2";
    case HBPL_T_A4: return "A4";
    case HBPL_T_C2: return "C2";
    case HBPL_T_C3: return "C3";
    case HBPL_T_C4: return "C4";
    default: return "??";
    }
}

static const char *attr_label(uint8_t rec, uint8_t id)
{
    if (rec == 'C') {
        switch (id) {
        case 0x91: return "feed direction (1 = wider than tall)";
        case 0x94: return "paper code";
        case 0x95: return "custom sheet size";
        case 0x97: return "image offset";
        case 0x99: return "page number";
        case 0x9A: return "content size";
        case 0x9B: return "blank (manual duplex)";
        case 0x9D: return "data format (0x8B color, 0x09 mono)";
        case 0xA2: return "stride, height";
        default: return "";
        }
    }
    if (rec == 'R' && id == 0xA4)
        return "data block length";
    return "";
}

static void print_pjl(const hbpl_parsed_job *job)
{
    printf("PJL header (%zu bytes):\n", job->pjl_len);
    for (size_t i = 0; i < job->pjl_len; i++) {
        unsigned char c = job->pjl[i];
        if (i == 0 || job->pjl[i - 1] == '\n')
            fputs("  ", stdout);
        if (c == 0x1B)
            fputs("<ESC>", stdout);
        else if (c == '\n' || (c >= 0x20 && c != 0x7F))
            putchar(c);
        else
            printf("<%02X>", c);
    }
}

static void print_record(const hbpl_record *r)
{
    printf("record %c at offset %zu\n", r->type, r->offset);
    for (int i = 0; i < r->nattrs; i++) {
        const hbpl_attr *a = &r->attrs[i];
        char val[48];
        if (a->type == HBPL_T_C2 || a->type == HBPL_T_C3 || a->type == HBPL_T_C4)
            snprintf(val, sizeof val, "%u, %u", (unsigned)a->v[0], (unsigned)a->v[1]);
        else
            snprintf(val, sizeof val, "%u (0x%X)", (unsigned)a->v[0], (unsigned)a->v[0]);
        const char *label = attr_label(r->type, a->id);
        printf("  %02X %s%s %-22s%s%s\n", a->id, a->prefixed ? "B1 " : "", type_name(a->type),
               val, *label ? "  " : "", label);
    }
}

static int write_pam(const char *path, const hbpl_page_info *pg, const uint8_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH %d\nMAXVAL 255\nTUPLTYPE %s\nENDHDR\n",
            (unsigned)pg->stride, (unsigned)pg->height, pg->color ? 4 : 1,
            pg->color ? "KCMY" : "GRAYSCALE");
    size_t n = hbpl_page_pixel_bytes(pg);
    int rc = fwrite(px, 1, n, f) == n ? 0 : -1;
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

int main(int argc, char **argv)
{
    int model = HBPL_MODEL_ROW;
    const char *out = NULL;
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "linear"))
                model = HBPL_MODEL_LINEAR;
            else if (!strcmp(m, "row"))
                model = HBPL_MODEL_ROW;
            else
                usage();
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else {
            usage();
        }
    }

    size_t len;
    uint8_t *data = read_all(stdin, &len);
    if (!data) {
        fprintf(stderr, "hbpl-decode: cannot read input\n");
        return 1;
    }
    hbpl_parsed_job job;
    char err[256];
    if (hbpl_parse_job(data, len, &job, err, sizeof err) < 0) {
        fprintf(stderr, "hbpl-decode: %s\n", err);
        free(data);
        return 1;
    }

    int rc = 0;
    if (!quiet) {
        print_pjl(&job);
        size_t page = 0;
        for (size_t i = 0; i < job.nrecords; i++) {
            const hbpl_record *r = &job.records[i];
            print_record(r);
            if (r->type == 'R' && page < job.npages) {
                const hbpl_page_info *pg = &job.pages[page++];
                hbpl_block_info bi;
                printf("  data block: %zu bytes (%s)\n", pg->data_len,
                       pg->color ? "color" : "monochrome");
                if (hbpl_parse_block(pg->data, pg->data_len, pg->color, &bi) == 0) {
                    static const char *const names[5] = { "token", "K", "C", "M", "Y" };
                    printf("  streams:");
                    for (int s = 0; s < bi.nstreams; s++)
                        printf(" %s=%u", names[s], (unsigned)bi.len[s]);
                    printf("\n");
                } else {
                    printf("  streams: invalid block header\n");
                }
            }
        }
        if (job.trailer_ok)
            printf("trailer: <ESC>%%-12345X@PJL EOJ\n");
        else
            printf("trailer: unexpected (%zu bytes)\n", job.trailer_len);
        printf("%zu page(s)\n", job.npages);
    }

    if (out) {
        for (size_t i = 0; i < job.npages; i++) {
            const hbpl_page_info *pg = &job.pages[i];
            uint8_t *px = malloc(hbpl_page_pixel_bytes(pg) ? hbpl_page_pixel_bytes(pg) : 1);
            if (!px || hbpl_decode_page(pg, model, px) < 0) {
                fprintf(stderr, "hbpl-decode: page %zu: decoding failed\n", i + 1);
                rc = 1;
                free(px);
                continue;
            }
            char path[4096];
            snprintf(path, sizeof path, "%s-%02u.pam", out, (unsigned)pg->page_number);
            if (write_pam(path, pg, px) < 0) {
                fprintf(stderr, "hbpl-decode: cannot write %s\n", path);
                rc = 1;
            } else if (!quiet) {
                printf("wrote %s\n", path);
            }
            free(px);
        }
    }

    hbpl_parsed_job_free(&job);
    free(data);
    return rc;
}
