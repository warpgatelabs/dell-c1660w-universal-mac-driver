// SPDX-License-Identifier: Apache-2.0
/*
 * HBPL version 1 job parser and page decoder.
 *
 * Typical use:
 *
 *     hbpl_parsed_job job;
 *     if (hbpl_parse_job(bytes, len, &job, err, sizeof err) == 0) {
 *         for (size_t i = 0; i < job.npages; i++) {
 *             const hbpl_page_info *pg = &job.pages[i];
 *             uint8_t *px = malloc(hbpl_page_pixel_bytes(pg));
 *             hbpl_decode_page(pg, HBPL_MODEL_ROW, px);
 *             ...
 *         }
 *         hbpl_parsed_job_free(&job);
 *     }
 *
 * Parsed structures point into the caller's job buffer, which must outlive them.
 */
#ifndef HBPL_DECODE_H
#define HBPL_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interpretation of references that leave the row (SPEC.md 6.3). */
enum hbpl_model { HBPL_MODEL_LINEAR = 0, HBPL_MODEL_ROW = 1 };

/* Attribute value types. */
enum {
    HBPL_T_A1 = 0xA1, HBPL_T_A2 = 0xA2, HBPL_T_A4 = 0xA4,
    HBPL_T_B1 = 0xB1,
    HBPL_T_C2 = 0xC2, HBPL_T_C3 = 0xC3, HBPL_T_C4 = 0xC4,
};

typedef struct {
    uint8_t id;
    uint8_t type;      /* value type (after a B1 prefix, if any) */
    bool prefixed;     /* a B1 prefix was present */
    uint32_t v[2];     /* value; pair types (C2, C3, C4) use both */
} hbpl_attr;

#define HBPL_MAX_ATTRS 32

typedef struct {
    uint8_t type;      /* record type byte ('A', 'C', 'Q', 'R', 'S', 'D', 'B') */
    size_t offset;     /* byte offset of the type byte in the job */
    size_t size;       /* bytes taken by the record, excluding R's data block */
    int nattrs;
    hbpl_attr attrs[HBPL_MAX_ATTRS];
} hbpl_record;

/* One page: C record attributes plus the R record's data block. */
typedef struct {
    const hbpl_record *c;      /* the page record */
    size_t record_index;       /* index of that record in hbpl_parsed_job.records */
    /* Attribute values (0 when the attribute is absent). */
    int long_edge_feed;        /* 91 */
    int paper_code;            /* 94 */
    uint32_t custom_width, custom_height;   /* 95 */
    uint32_t offset_x, offset_y;            /* 97 */
    uint32_t page_number;      /* 99 */
    uint32_t content_width, content_height; /* 9A */
    int blank;                 /* 9B */
    int data_format;           /* 9D: 0x8B color, 0x09 monochrome */
    uint32_t stride, height;   /* A2 */
    bool color;                /* data_format == 0x8B */
    const uint8_t *data;       /* the data block */
    size_t data_len;
} hbpl_page_info;

typedef struct {
    const uint8_t *pjl;        /* PJL header, through "@PJL ENTER LANGUAGE=HBPL\n" */
    size_t pjl_len;
    hbpl_record *records;      /* every HBPL record in file order */
    size_t nrecords;
    hbpl_page_info *pages;
    size_t npages;
    const uint8_t *trailer;    /* bytes after the B record */
    size_t trailer_len;
    bool trailer_ok;           /* trailer is exactly ESC%-12345X@PJL EOJ LF */
} hbpl_parsed_job;

/* Parse a complete job. Returns 0, or -1 with a message in err (if non-NULL). */
int hbpl_parse_job(const uint8_t *data, size_t len, hbpl_parsed_job *job,
                   char *err, size_t errlen);
void hbpl_parsed_job_free(hbpl_parsed_job *job);

/* Look up a PJL variable: "COPIES" matches "@PJL SET COPIES=..." and
 * "@PJL COMMENT COPIES=...", "@LUNA" matches "@PJL SET JOBATTR="@LUNA=..."".
 * Copies the value into buf; returns 0, or -1 if absent or too long. */
int hbpl_pjl_get(const hbpl_parsed_job *job, const char *name, char *buf, size_t buflen);

/* Data block layout (SPEC.md section 5). */
typedef struct {
    bool color;
    int nstreams;              /* 2 (mono) or 5 (color): token, K[, C, M, Y] */
    const uint8_t *stream[5];
    uint32_t len[5];
} hbpl_block_info;

/* Split a data block into its streams. Returns 0 or -1. */
int hbpl_parse_block(const uint8_t *block, size_t len, bool color, hbpl_block_info *info);

/* Decode a data block into pixels (stride * height pixels of 4 bytes K,C,M,Y for
 * color or 1 byte K for monochrome). Returns 0 or -1. */
int hbpl_decode_block(const uint8_t *block, size_t len, int stride, int height, bool color,
                      int model, uint8_t *pixels);

/* Bytes needed for a page's decoded pixels. */
size_t hbpl_page_pixel_bytes(const hbpl_page_info *page);

/* Decode a parsed page (geometry and color from its attributes). Returns 0 or -1. */
int hbpl_decode_page(const hbpl_page_info *page, int model, uint8_t *pixels);

#ifdef __cplusplus
}
#endif

#endif
