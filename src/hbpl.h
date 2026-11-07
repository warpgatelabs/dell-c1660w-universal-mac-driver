// SPDX-License-Identifier: Apache-2.0
/*
 * HBPL version 1 encoder and job framing.
 *
 * hbpl_encode_page() compresses one page raster into an HBPL data block;
 * the hbpl_append_* functions build a complete print job around such blocks.
 */
#ifndef HBPL_H
#define HBPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Growable byte buffer. Zero-initialise before first use. */
typedef struct { uint8_t *data; size_t len, cap; } hbpl_buf;

void hbpl_buf_free(hbpl_buf *b);
/* Append n bytes; returns 0 or -1 (out of memory). */
int hbpl_buf_append(hbpl_buf *b, const void *src, size_t n);

enum hbpl_profile { HBPL_PROFILE_PORTABLE = 0, HBPL_PROFILE_REFERENCE = 1 };

/* Largest run length a single run token can carry. */
#define HBPL_MAX_RUN 17057

/* Compress one page into a data block, replacing out's contents.
 * pixels: H rows of W pixels; color = 4 bytes K,C,M,Y per pixel, mono = 1 byte.
 * W must be a multiple of 8 and at least 16. Column 0 is overwritten (per the
 * selected profile) on an internal copy; the caller's buffer is not modified.
 * Returns 0 or -1. */
int hbpl_encode_page(const uint8_t *pixels, int stride, int height, int color,
                     int profile, hbpl_buf *out);

/* Job-level parameters (PJL header). */
typedef struct {
    const char *title;      /* DNAME; NULL = empty */
    const char *user;       /* LUNA, JOAU; NULL = empty */
    const char *host;       /* CNAM; NULL = empty */
    time_t time;            /* printed as DATE / TIME in local time */
    int copies;             /* printer-side copies; values below 1 are written as 1 */
    bool color;             /* RENDERMODE=COLOR and color calibration lines */
    bool economode;         /* ECONOMODE=ON */
    const char *media;      /* @MSIP keyword; NULL = "NORMAL" */
    bool long_edge_feed;    /* PAPERDIRECTION=LEF */
    int manual_duplex;      /* 0 off, 1 long edge, 2 short edge */
    int side2_start;        /* IWASIDE2START (manual duplex only) */
    bool jam_recovery;      /* IWAJAMRECOVERY=ON instead of AUTO */
} hbpl_job;

/* Page-level parameters (C record). */
typedef struct {
    int paper_code;          /* attribute 94 */
    bool long_edge_feed;     /* attribute 91 */
    uint16_t custom_width;   /* attribute 95, used when paper_code == 255 */
    uint16_t custom_height;
    uint16_t offset_x;       /* attribute 97 */
    uint16_t offset_y;
    uint32_t page_number;    /* attribute 99, 1-based in output order */
    uint32_t content_width;  /* attribute 9A (height is the page height) */
    uint32_t stride;         /* attribute A2 */
    uint32_t height;
    bool color;              /* attribute 9D */
    bool blank;              /* attribute 9B */
} hbpl_page;

/* Write the PJL header and the A record. Returns 0 or -1. */
int hbpl_append_job_header(hbpl_buf *out, const hbpl_job *job);
/* Write C, Q, R (with data's bytes as the page data block), S, D. Returns 0 or -1. */
int hbpl_append_page(hbpl_buf *out, const hbpl_page *page, const hbpl_buf *data);
/* Write B and the PJL trailer. Returns 0 or -1. */
int hbpl_append_job_trailer(hbpl_buf *out);

#ifdef __cplusplus
}
#endif

#endif
