// SPDX-License-Identifier: Apache-2.0
/*
 * SPEC.md 9.3 vector check: for every page of every job given on the command
 * line, decode with the linear model, re-encode with the reference profile and
 * require the data block to be reproduced byte for byte.
 */
#include "hbpl.h"
#include "hbpl_decode.h"
#include "test_util.h"

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("test_vectors: no vector files given (set HBPL_VECTORS); skipped\n");
        return 0;
    }
    int jobs = 0, jobs_ok = 0, pages = 0, pages_ok = 0;
    for (int i = 1; i < argc; i++) {
        size_t len;
        uint8_t *d = read_file(argv[i], &len);
        CHECK(d != NULL, "cannot read %s", argv[i]);
        if (!d)
            continue;
        jobs++;
        hbpl_parsed_job job;
        char err[200];
        int ok = hbpl_parse_job(d, len, &job, err, sizeof err) == 0;
        CHECK(ok, "%s: parse failed: %s", argv[i], err);
        if (!ok) {
            free(d);
            continue;
        }
        int job_ok = job.npages > 0 && job.trailer_ok;
        CHECK(job.trailer_ok, "%s: bad trailer", argv[i]);
        for (size_t p = 0; p < job.npages; p++) {
            const hbpl_page_info *pg = &job.pages[p];
            pages++;
            uint8_t *px = malloc(hbpl_page_pixel_bytes(pg));
            hbpl_buf out = {0};
            int dec = px && hbpl_decode_page(pg, HBPL_MODEL_LINEAR, px) == 0;
            int enc = dec && hbpl_encode_page(px, (int)pg->stride, (int)pg->height, pg->color,
                                              HBPL_PROFILE_REFERENCE, &out) == 0;
            int same = enc && out.len == pg->data_len &&
                       memcmp(out.data, pg->data, out.len) == 0;
            CHECK(dec, "%s page %zu: decode failed", argv[i], p + 1);
            CHECK(!dec || enc, "%s page %zu: encode failed", argv[i], p + 1);
            if (enc && !same) {
                size_t k = 0;
                while (k < out.len && k < pg->data_len && out.data[k] == pg->data[k])
                    k++;
                CHECK(same, "%s page %zu: block differs at byte %zu (lengths %zu vs %zu)",
                      argv[i], p + 1, k, out.len, pg->data_len);
            }
            if (same)
                pages_ok++;
            else
                job_ok = 0;
            hbpl_buf_free(&out);
            free(px);
        }
        if (job_ok)
            jobs_ok++;
        printf("  %-40s %zu page(s) %s\n", base_name(argv[i]), job.npages,
               job_ok ? "ok" : "FAILED");
        hbpl_parsed_job_free(&job);
        free(d);
    }
    printf("vector check: %d/%d jobs, %d/%d pages reproduced byte for byte\n", jobs_ok, jobs,
           pages_ok, pages);
    return test_summary("test_vectors");
}
