// SPDX-License-Identifier: Apache-2.0
/*
 * Job framing tests.
 *
 * - Known-answer checks for the A record, C/Q/R/S/D page framing and trailer.
 * - PJL header details (sanitising, duplex, mono calibration lines).
 * - Rebuilding vector jobs byte for byte: a single-page and multi-page job from
 *   hard-coded parameters with re-encoded page data, and every given vector
 *   from its own parsed parameters.
 *
 * Arguments: the vector .prn files (optional; vector checks are skipped without).
 */
#include "hbpl.h"
#include "hbpl_decode.h"
#include "test_util.h"

static int contains(const hbpl_buf *b, const char *s)
{
    size_t n = strlen(s);
    if (n > b->len)
        return 0;
    for (size_t i = 0; i + n <= b->len; i++)
        if (memcmp(b->data + i, s, n) == 0)
            return 1;
    return 0;
}

static void test_records(void)
{
    hbpl_buf b = {0};
    hbpl_job job = {0};
    job.color = true;
    CHECK(hbpl_append_job_header(&b, &job) == 0, "header");
    static const uint8_t rec_a[12] = { 0x41, 0x81, 0xA1, 0x00, 0x82, 0xA2, 0x07, 0x00,
                                       0x83, 0xA2, 0x01, 0x00 };
    CHECK(b.len > 12 && memcmp(b.data + b.len - 12, rec_a, 12) == 0, "A record");
    static const char enter[] = "@PJL ENTER LANGUAGE=HBPL\n";
    CHECK(b.len > 12 + sizeof enter - 1 &&
          memcmp(b.data + b.len - 12 - (sizeof enter - 1), enter, sizeof enter - 1) == 0,
          "ENTER LANGUAGE precedes A");
    CHECK(memcmp(b.data, "\x1b%-12345X@PJL JOB MODE=PRINTER\n", 31) == 0, "UEL + JOB");
    CHECK(!memchr(b.data, '\r', b.len), "no CR in header");
    hbpl_buf_free(&b);

    /* Page framing, from SPEC.md 3.3 / 3.4. */
    hbpl_page pg = {0};
    pg.paper_code = 255;
    pg.long_edge_feed = true;
    pg.custom_width = 0x1234;
    pg.custom_height = 0x0567;
    pg.offset_x = 97;
    pg.offset_y = 0x102;
    pg.page_number = 0x01020304;
    pg.content_width = 4914;
    pg.stride = 4920;
    pg.height = 6406;
    pg.color = false;
    pg.blank = true;
    uint8_t payload[3] = { 0xAA, 0xBB, 0xCC };
    hbpl_buf data = { payload, 3, 3 };
    CHECK(hbpl_append_page(&b, &pg, &data) == 0, "append page");
    static const uint8_t want[] = {
        0x43,
        0x91, 0xA1, 0x01,
        0x92, 0xA1, 0x01,
        0x93, 0xA1, 0x01,
        0x94, 0xA1, 0xFF,
        0x95, 0xC2, 0x34, 0x12, 0x67, 0x05,
        0x96, 0xA1, 0x03,
        0x97, 0xC3, 0x61, 0x00, 0x02, 0x01,
        0x98, 0xA1, 0x00,
        0x99, 0xA4, 0x04, 0x03, 0x02, 0x01,
        0x9A, 0xC4, 0x32, 0x13, 0x00, 0x00, 0x06, 0x19, 0x00, 0x00,
        0x9B, 0xA1, 0x01,
        0x9C, 0xA1, 0x01,
        0x9D, 0xA1, 0x09,
        0x9E, 0xA1, 0x02,
        0x9F, 0xA1, 0x05,
        0xA0, 0xA1, 0x08,
        0xA1, 0xA1, 0x00,
        0xA2, 0xC4, 0x38, 0x13, 0x00, 0x00, 0x06, 0x19, 0x00, 0x00,
        0x51,
        0x52, 0xA3, 0xA1, 0x00, 0xA4, 0xB1, 0xA4, 0x03, 0x00, 0x00, 0x00,
        0xAA, 0xBB, 0xCC,
        0x53, 0x44,
    };
    CHECK(b.len == sizeof want && memcmp(b.data, want, sizeof want) == 0,
          "page framing bytes (len %zu)", b.len);

    /* Non-custom paper writes 0, 0 for attribute 95; color writes 0x8B. */
    b.len = 0;
    pg.paper_code = 2;
    pg.color = true;
    CHECK(hbpl_append_page(&b, &pg, &data) == 0, "append page");
    CHECK(b.len == sizeof want && b.data[15] == 0 && b.data[16] == 0 && b.data[17] == 0 &&
          b.data[18] == 0 && b.data[12] == 2 && b.data[55] == 0x8B,
          "attribute 95 zero for standard paper, 9D color");

    b.len = 0;
    CHECK(hbpl_append_job_trailer(&b) == 0, "trailer");
    static const uint8_t trailer[] = { 0x42, 0x1B, 0x25, 0x2D, 0x31, 0x32, 0x33, 0x34, 0x35,
                                       0x58, '@', 'P', 'J', 'L', ' ', 'E', 'O', 'J', 0x0A };
    CHECK(b.len == sizeof trailer && memcmp(b.data, trailer, sizeof trailer) == 0, "trailer");
    hbpl_buf_free(&b);
}

static void test_pjl_options(void)
{
    hbpl_buf b = {0};
    hbpl_job job = {0};
    job.title = "a\"b\rc\nd\x01" "e";
    job.user = NULL;
    job.host = "h\"o\"st";
    job.copies = 0;
    job.color = false;
    job.economode = true;
    job.media = "THICK";
    job.long_edge_feed = true;
    job.manual_duplex = 2;
    job.side2_start = 7;
    job.jam_recovery = true;
    CHECK(hbpl_append_job_header(&b, &job) == 0, "header");
    CHECK(contains(&b, "\n@PJL COMMENT DNAME=abcde\n"), "title sanitised");
    CHECK(contains(&b, "\n@PJL SET JOBATTR=\"@LUNA=\"\n"), "NULL user is empty");
    CHECK(contains(&b, "\n@PJL SET JOBATTR=\"@CNAM=host\"\n"), "host sanitised");
    CHECK(contains(&b, "\n@PJL SET COPIES=1\n"), "copies at least 1");
    CHECK(contains(&b, "\n@PJL SET DUPLEX=ON\n@PJL SET BINDING=SHORTEDGE\n"), "binding");
    CHECK(contains(&b, "\n@PJL SET JOBATTR=\"@MSIP=THICK\"\n"), "media");
    CHECK(contains(&b, "\n@PJL SET RENDERMODE=GRAYSCALE\n@PJL SET ECONOMODE=ON\n"), "mode");
    CHECK(contains(&b, "\n@PJL SET PAPERDIRECTION=LEF\n"), "LEF");
    CHECK(contains(&b, "\"@DRDM=RASTER\"\n"
                       "@PJL SET JOBATTR=\"@TTRC=33\"\n@PJL SET JOBATTR=\"@GTRC=33\"\n"
                       "@PJL SET JOBATTR=\"@ITRC=33\"\n@PJL SET JOBATTR=\"@TSCR=33\"\n"
                       "@PJL SET JOBATTR=\"@GSCR=33\"\n@PJL SET JOBATTR=\"@ISCR=33\"\n"
                       "@PJL SET JOBATTR=\"@PBLK=OFF\"\n@PJL SET JOBATTR=\"@BANR=DEVICE\"\n"),
          "mono calibration");
    CHECK(contains(&b, "\n@PJL SET IWAMANUALDUP=ON\n@PJL SET IWASIDE2START=7\n"
                       "@PJL SET IWAJAMRECOVERY=ON\n@PJL ENTER LANGUAGE=HBPL\n"),
          "manual duplex tail");
    CHECK(!contains(&b, "TCPR"), "no color calibration in mono job");
    b.len = 0;
    job.manual_duplex = 3;
    CHECK(hbpl_append_job_header(&b, &job) == -1, "invalid duplex mode rejected");
    hbpl_buf_free(&b);
}

/* ------------------------------------------------------------------------ */
/* Vector rebuilds                                                           */

static int pjl_time(const hbpl_parsed_job *pj, time_t *t)
{
    char date[32], tim[32];
    if (hbpl_pjl_get(pj, "DATE", date, sizeof date) < 0 ||
        hbpl_pjl_get(pj, "TIME", tim, sizeof tim) < 0)
        return -1;
    struct tm tm = {0};
    if (sscanf(date, "%d/%d/%d", &tm.tm_mon, &tm.tm_mday, &tm.tm_year) != 3 ||
        sscanf(tim, "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3)
        return -1;
    tm.tm_mon -= 1;
    tm.tm_year -= 1900;
    tm.tm_isdst = -1;
    *t = mktime(&tm);
    return *t == (time_t)-1 ? -1 : 0;
}

typedef struct {
    char title[256], user[256], host[256], media[64];
} job_strings;

/* Derive hbpl_job from a parsed job's PJL header. */
static int job_from_pjl(const hbpl_parsed_job *pj, hbpl_job *job, job_strings *s)
{
    char v[64];
    memset(job, 0, sizeof *job);
    if (hbpl_pjl_get(pj, "DNAME", s->title, sizeof s->title) < 0 ||
        hbpl_pjl_get(pj, "@LUNA", s->user, sizeof s->user) < 0 ||
        hbpl_pjl_get(pj, "@CNAM", s->host, sizeof s->host) < 0 ||
        hbpl_pjl_get(pj, "@MSIP", s->media, sizeof s->media) < 0 || pjl_time(pj, &job->time) < 0)
        return -1;
    job->title = s->title;
    job->user = s->user;
    job->host = s->host;
    job->media = s->media;
    if (hbpl_pjl_get(pj, "COPIES", v, sizeof v) < 0)
        return -1;
    job->copies = atoi(v);
    if (hbpl_pjl_get(pj, "RENDERMODE", v, sizeof v) < 0)
        return -1;
    job->color = !strcmp(v, "COLOR");
    if (hbpl_pjl_get(pj, "ECONOMODE", v, sizeof v) < 0)
        return -1;
    job->economode = !strcmp(v, "ON");
    if (hbpl_pjl_get(pj, "PAPERDIRECTION", v, sizeof v) < 0)
        return -1;
    job->long_edge_feed = !strcmp(v, "LEF");
    if (hbpl_pjl_get(pj, "DUPLEX", v, sizeof v) < 0)
        return -1;
    if (!strcmp(v, "ON")) {
        if (hbpl_pjl_get(pj, "BINDING", v, sizeof v) < 0)
            return -1;
        job->manual_duplex = !strcmp(v, "SHORTEDGE") ? 2 : 1;
        if (hbpl_pjl_get(pj, "IWASIDE2START", v, sizeof v) < 0)
            return -1;
        job->side2_start = atoi(v);
    }
    if (hbpl_pjl_get(pj, "IWAJAMRECOVERY", v, sizeof v) < 0)
        return -1;
    job->jam_recovery = !strcmp(v, "ON");
    return 0;
}

static void page_from_info(const hbpl_page_info *pi, hbpl_page *pg)
{
    memset(pg, 0, sizeof *pg);
    pg->paper_code = pi->paper_code;
    pg->long_edge_feed = pi->long_edge_feed != 0;
    pg->custom_width = (uint16_t)pi->custom_width;
    pg->custom_height = (uint16_t)pi->custom_height;
    pg->offset_x = (uint16_t)pi->offset_x;
    pg->offset_y = (uint16_t)pi->offset_y;
    pg->page_number = pi->page_number;
    pg->content_width = pi->content_width;
    pg->stride = pi->stride;
    pg->height = pi->height;
    pg->color = pi->color;
    pg->blank = pi->blank != 0;
}

/* Re-encode a vector page from its linear-model decode (reference profile). */
static int reencode(const hbpl_page_info *pi, hbpl_buf *out)
{
    uint8_t *px = malloc(hbpl_page_pixel_bytes(pi));
    int rc = px && hbpl_decode_page(pi, HBPL_MODEL_LINEAR, px) == 0 &&
                     hbpl_encode_page(px, (int)pi->stride, (int)pi->height, pi->color,
                                      HBPL_PROFILE_REFERENCE, out) == 0
                 ? 0 : -1;
    free(px);
    return rc;
}

static void compare(const char *what, const hbpl_buf *b, const uint8_t *d, size_t len,
                    size_t from)
{
    size_t k = from;
    while (k < b->len && k < len && b->data[k] == d[k])
        k++;
    CHECK(b->len == len && k == len, "%s: rebuilt job differs at byte %zu (len %zu vs %zu)",
          what, k, b->len, len);
}

/* Explicit parameters for specific vectors (SPEC.md 4.x values). */
typedef struct {
    const char *file;
    int npages;
    hbpl_page page;          /* page_number and blank are set per page */
    const int *blank_pages;  /* 1-based page numbers with attribute 9B = 1 */
} explicit_case;

static void rebuild_explicit(const char *path, const explicit_case *ec)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    CHECK(d != NULL, "read %s", path);
    if (!d)
        return;
    hbpl_parsed_job pj;
    char err[200];
    if (hbpl_parse_job(d, len, &pj, err, sizeof err) < 0) {
        CHECK(0, "%s: %s", path, err);
        free(d);
        return;
    }
    CHECK((int)pj.npages == ec->npages, "%s: %zu pages", ec->file, pj.npages);

    hbpl_buf b = {0}, block = {0};
    /* Everything after the PJL header comes from explicit parameters. */
    hbpl_job job;
    job_strings js;
    CHECK(job_from_pjl(&pj, &job, &js) == 0, "%s: PJL values", ec->file);
    CHECK(hbpl_append_job_header(&b, &job) == 0, "header");
    compare(ec->file, &b, d, pj.pjl_len + 12, 0);   /* header + A record */
    for (int i = 0; i < ec->npages && (size_t)i < pj.npages; i++) {
        hbpl_page pg = ec->page;
        pg.page_number = (uint32_t)i + 1;
        pg.blank = false;
        for (const int *bp = ec->blank_pages; bp && *bp; bp++)
            if (*bp == i + 1)
                pg.blank = true;
        CHECK(reencode(&pj.pages[i], &block) == 0, "%s: re-encode page %d", ec->file, i + 1);
        CHECK(hbpl_append_page(&b, &pg, &block) == 0, "append page");
    }
    CHECK(hbpl_append_job_trailer(&b) == 0, "trailer");
    compare(ec->file, &b, d, len, pj.pjl_len);
    CHECK(b.len >= pj.pjl_len && memcmp(b.data, d, pj.pjl_len) == 0,
          "%s: PJL header identical", ec->file);
    printf("  rebuilt %s from explicit parameters (%d page(s)): %s\n", ec->file, ec->npages,
           (b.len == len && !memcmp(b.data, d, len)) ? "identical" : "DIFFERENT");
    hbpl_buf_free(&b);
    hbpl_buf_free(&block);
    hbpl_parsed_job_free(&pj);
    free(d);
}

/* Rebuild a vector from its own parsed parameters and original data blocks. */
static int rebuild_generic(const char *path)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d)
        return 0;
    hbpl_parsed_job pj;
    char err[200];
    if (hbpl_parse_job(d, len, &pj, err, sizeof err) < 0) {
        free(d);
        return 0;
    }
    hbpl_buf b = {0};
    hbpl_job job;
    job_strings js;
    int ok = job_from_pjl(&pj, &job, &js) == 0 && hbpl_append_job_header(&b, &job) == 0;
    for (size_t i = 0; ok && i < pj.npages; i++) {
        hbpl_page pg;
        page_from_info(&pj.pages[i], &pg);
        hbpl_buf data = { (uint8_t *)pj.pages[i].data, pj.pages[i].data_len,
                          pj.pages[i].data_len };
        ok = hbpl_append_page(&b, &pg, &data) == 0;
    }
    ok = ok && hbpl_append_job_trailer(&b) == 0;
    ok = ok && b.len == len && memcmp(b.data, d, len) == 0;
    CHECK(ok, "%s: generic rebuild differs", path);
    hbpl_buf_free(&b);
    hbpl_parsed_job_free(&pj);
    free(d);
    return ok;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    test_records();
    test_pjl_options();

    if (argc < 2) {
        printf("test_framing: no vector files given; vector rebuilds skipped\n");
        return test_summary("test_framing");
    }

    static const int duplex3_blank[] = { 1, 0 };
    const explicit_case cases[] = {
        /* Single page, custom size 400 x 600 pt. */
        { "size-Custom.400x600.prn", 1,
          { .paper_code = 255, .custom_width = 3333, .custom_height = 5000, .offset_x = 92,
            .offset_y = 92, .content_width = 3158, .stride = 3160, .height = 4817,
            .color = true }, NULL },
        /* Single page, rotated DL envelope (feed direction 1). */
        { "size-DLEnvDLRotated.prn", 1,
          { .paper_code = 9, .long_edge_feed = true, .offset_x = 144, .offset_y = 97,
            .content_width = 4920, .stride = 4920, .height = 2406, .color = true }, NULL },
        /* Multi-page Letter. */
        { "two-pages.prn", 2,
          { .paper_code = 0, .offset_x = 97, .offset_y = 97, .content_width = 4914,
            .stride = 4920, .height = 6406, .color = true }, NULL },
        /* Multi-page manual duplex with an inserted blank page. */
        { "duplex-long-3p.prn", 4,
          { .paper_code = 0, .offset_x = 97, .offset_y = 97, .content_width = 4914,
            .stride = 4920, .height = 6406, .color = true }, duplex3_blank },
        /* Monochrome. */
        { "test-mono.prn", 1,
          { .paper_code = 0, .offset_x = 97, .offset_y = 97, .content_width = 4914,
            .stride = 4920, .height = 6406, .color = false }, NULL },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        int found = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(base_name(argv[i]), cases[c].file)) {
                rebuild_explicit(argv[i], &cases[c]);
                found = 1;
            }
        }
        CHECK(found, "vector %s not among the arguments", cases[c].file);
    }

    int ok = 0;
    for (int i = 1; i < argc; i++)
        ok += rebuild_generic(argv[i]);
    printf("  rebuilt %d/%d vector jobs byte for byte from their parsed parameters\n", ok,
           argc - 1);
    return test_summary("test_framing");
}
