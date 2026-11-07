// SPDX-License-Identifier: Apache-2.0
/*
 * Round trips on synthetic images: encode with each profile, decode, compare.
 *
 * - reference profile: the linear-model decode must reproduce the input with
 *   column 0 replaced as in SPEC.md 8.1;
 * - portable profile: the same, and the row-model decode must be identical to
 *   the linear-model decode.
 */
#include "hbpl.h"
#include "hbpl_decode.h"
#include "test_util.h"

enum pattern {
    P_NOISE, P_FLAT_ZERO, P_FLAT_FULL, P_LONG_RUNS, P_STRIPES, P_BLOCKS, P_PALETTE,
    P_TEXT, P_EDGES, P_GRADIENT, P_COUNT
};
static const char *const pattern_name[P_COUNT] = {
    "noise", "flat-zero", "flat-full", "long-runs", "stripes", "blocks", "palette",
    "text", "edges", "gradient",
};

static void fill(uint8_t *px, int W, int H, int bpp, enum pattern pat)
{
    static const uint8_t palette[4][4] = {
        { 0, 0, 0, 0 }, { 255, 255, 255, 255 }, { 0, 128, 0, 64 }, { 200, 0, 30, 0 },
    };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = px + ((size_t)y * W + x) * bpp;
            for (int c = 0; c < bpp; c++) {
                uint8_t v = 0;
                switch (pat) {
                case P_NOISE: v = (uint8_t)rnd(); break;
                case P_FLAT_ZERO: v = 0; break;
                case P_FLAT_FULL: v = 0xFF; break;
                case P_LONG_RUNS:
                    /* Mostly one value; a rare change every ~40000 pixels. */
                    v = (uint8_t)((((size_t)y * W + x) / 40000) * 37 + c);
                    break;
                case P_STRIPES: v = (uint8_t)(((x / 3) & 1) ? 0xFF : (y & 7) * 30); break;
                case P_BLOCKS: v = (uint8_t)(((x / 13) ^ (y / 7)) * 41 + c * 11); break;
                case P_PALETTE: v = palette[rnd() % 4][c]; break;
                case P_TEXT:
                    v = ((rnd() % 50) == 0 || ((x + y) % 23 == 0 && y % 5 < 2)) ? 0xFF : 0;
                    break;
                case P_EDGES:
                    /* Values that differ from the guard at every row edge. */
                    v = (uint8_t)((x == W - 1 || x == W - 2 || x < 10) ? 0x80 + y % 3
                                                                       : (y % 2) * 0x10);
                    break;
                case P_GRADIENT: v = (uint8_t)(x * 3 + y * 5 + c * 50); break;
                default: break;
                }
                p[c] = v;
            }
        }
    }
}

static int pages_run, pages_ok;

static void roundtrip(int W, int H, int color, int profile, enum pattern pat)
{
    const int bpp = color ? 4 : 1;
    const size_t n = (size_t)W * H * bpp;
    uint8_t *px = malloc(n), *orig = malloc(n), *want = malloc(n);
    uint8_t *lin = malloc(n), *row = malloc(n);
    if (!px || !orig || !want || !lin || !row) {
        CHECK(0, "out of memory");
        goto done;
    }
    fill(px, W, H, bpp, pat);
    memcpy(orig, px, n);
    memcpy(want, px, n);
    for (int y = 0; y < H; y++) {
        uint8_t v = (!color || profile == HBPL_PROFILE_PORTABLE || y == 0) ? 0xFF : 0x00;
        memset(want + (size_t)y * W * bpp, v, (size_t)bpp);
    }

    hbpl_buf out = {0};
    pages_run++;
    int ok = hbpl_encode_page(px, W, H, color, profile, &out) == 0;
    CHECK(ok, "encode %dx%d %s %s %s", W, H, color ? "color" : "mono",
          profile ? "reference" : "portable", pattern_name[pat]);
    if (ok) {
        CHECK(!memcmp(px, orig, n), "caller's buffer modified");
        int dl = hbpl_decode_block(out.data, out.len, W, H, color, HBPL_MODEL_LINEAR, lin);
        int good = dl == 0 && !memcmp(lin, want, n);
        CHECK(good, "linear round trip %dx%d %s %s %s", W, H, color ? "color" : "mono",
              profile ? "reference" : "portable", pattern_name[pat]);
        if (profile == HBPL_PROFILE_PORTABLE) {
            int dr = hbpl_decode_block(out.data, out.len, W, H, color, HBPL_MODEL_ROW, row);
            int same = dr == 0 && dl == 0 && !memcmp(row, lin, n);
            CHECK(same, "portable output differs between models %dx%d %s %s", W, H,
                  color ? "color" : "mono", pattern_name[pat]);
            good = good && same;
        }
        if (good)
            pages_ok++;
    }
    hbpl_buf_free(&out);
done:
    free(px);
    free(orig);
    free(want);
    free(lin);
    free(row);
}

/* Count how often the reference profile's output decodes differently under the
 * row model, to show the portable check above is not vacuous. */
static int reference_differs(int W, int H, int color, enum pattern pat)
{
    const int bpp = color ? 4 : 1;
    const size_t n = (size_t)W * H * bpp;
    uint8_t *px = malloc(n), *lin = malloc(n), *row = malloc(n);
    hbpl_buf out = {0};
    int differs = 0;
    if (px && lin && row) {
        fill(px, W, H, bpp, pat);
        if (hbpl_encode_page(px, W, H, color, HBPL_PROFILE_REFERENCE, &out) == 0 &&
            hbpl_decode_block(out.data, out.len, W, H, color, HBPL_MODEL_LINEAR, lin) == 0 &&
            hbpl_decode_block(out.data, out.len, W, H, color, HBPL_MODEL_ROW, row) == 0)
            differs = memcmp(lin, row, n) != 0;
    }
    hbpl_buf_free(&out);
    free(px);
    free(lin);
    free(row);
    return differs;
}

int main(void)
{
    static const struct { int w, h; } sizes[] = {
        { 16, 1 }, { 16, 37 }, { 24, 9 }, { 64, 64 }, { 200, 120 }, { 1000, 45 },
        { 4920, 12 }, { 9600, 5 },
    };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        for (int color = 0; color < 2; color++)
            for (int profile = 0; profile < 2; profile++)
                for (int pat = 0; pat < P_COUNT; pat++)
                    roundtrip(sizes[s].w, sizes[s].h, color, profile, (enum pattern)pat);

    /* Long runs: single runs hit the 17057 maximum (bit flag reset to 0). */
    for (int color = 0; color < 2; color++)
        for (int profile = 0; profile < 2; profile++) {
            roundtrip(256, 400, color, profile, P_FLAT_ZERO);
            roundtrip(256, 400, color, profile, P_FLAT_FULL);
            roundtrip(4096, 50, color, profile, P_LONG_RUNS);
            roundtrip(17064, 3, color, profile, P_FLAT_ZERO);
        }

    int diffs = 0;
    for (int color = 0; color < 2; color++) {
        diffs += reference_differs(64, 64, color, P_EDGES);
        diffs += reference_differs(64, 64, color, P_NOISE);
    }
    printf("  reference output decodes differently under the row model in %d of 4 cases\n",
           diffs);

    /* Argument validation. */
    uint8_t px[16 * 4] = {0};
    hbpl_buf out = {0};
    CHECK(hbpl_encode_page(px, 8, 1, 0, 0, &out) == -1, "stride 8 rejected");
    CHECK(hbpl_encode_page(px, 20, 1, 0, 0, &out) == -1, "stride 20 rejected");
    CHECK(hbpl_encode_page(px, 16, 0, 0, 0, &out) == -1, "height 0 rejected");
    CHECK(hbpl_encode_page(px, 16, 1, 0, 2, &out) == -1, "bad profile rejected");
    hbpl_buf_free(&out);

    printf("  %d/%d synthetic pages round-tripped\n", pages_ok, pages_run);
    return test_summary("test_roundtrip");
}
