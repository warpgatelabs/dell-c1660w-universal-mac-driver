// SPDX-License-Identifier: Apache-2.0
//
// RGB to printer color using measured tables (color_tables.h).
//
// Neutral grays use exact 256-entry tables: the measured conversion jumps sharply between
// pure grays (printed with black toner only) and near-grays. Everything else uses
// tetrahedral interpolation in a 33x33x33 grid.

#include "color.h"

#include <string.h>

#include "color_tables.h"

#define N LUT_N

/* Grid cell index and fraction (0..255, in 1/255 units) for one input channel. */
static inline void locate(int v, int *idx, int *frac) {
    int pos = v * (N - 1);
    int i = pos / 255;
    if (i > N - 2) i = N - 2;
    *idx = i;
    *frac = pos - i * 255;
}

/* Tetrahedral interpolation over a grid of d-channel nodes laid out [r][g][b][d]. */
static inline void tetra(const uint8_t *grid, int d, const uint8_t *rgb, uint8_t *out) {
    int r, g, b, fr, fg, fb;
    locate(rgb[0], &r, &fr);
    locate(rgb[1], &g, &fg);
    locate(rgb[2], &b, &fb);
    const int sr = N * N * d, sg = N * d, sb = d;
    const uint8_t *c000 = grid + r * sr + g * sg + b * sb;
    const uint8_t *c111 = c000 + sr + sg + sb;
    const uint8_t *ca, *cb;
    int f1, f2, f3;
    if (fr >= fg && fg >= fb)      { ca = c000 + sr; cb = c000 + sr + sg; f1 = fr; f2 = fg; f3 = fb; }
    else if (fr >= fb && fb > fg)  { ca = c000 + sr; cb = c000 + sr + sb; f1 = fr; f2 = fb; f3 = fg; }
    else if (fb > fr && fr >= fg)  { ca = c000 + sb; cb = c000 + sr + sb; f1 = fb; f2 = fr; f3 = fg; }
    else if (fg > fr && fr >= fb)  { ca = c000 + sg; cb = c000 + sr + sg; f1 = fg; f2 = fr; f3 = fb; }
    else if (fg >= fb && fb > fr)  { ca = c000 + sg; cb = c000 + sg + sb; f1 = fg; f2 = fb; f3 = fr; }
    else                           { ca = c000 + sb; cb = c000 + sg + sb; f1 = fb; f2 = fg; f3 = fr; }
    for (int s = 0; s < d; s++) {
        int v = c000[s] * 255 + f1 * (ca[s] - c000[s]) + f2 * (cb[s] - ca[s]) + f3 * (c111[s] - cb[s]);
        v = (v + 127) / 255;   /* v >= 0: convex combination of node values */
        out[s] = (uint8_t)(v > 255 ? 255 : v);
    }
}

void color_rgb_to_kcmy(const uint8_t *rgb, uint8_t *kcmy, int n) {
    uint8_t last_in[3] = {255, 255, 255}, last_out[4];
    memcpy(last_out, lut_gray_kcmy[255], 4);
    for (int i = 0; i < n; i++, rgb += 3, kcmy += 4) {
        if (rgb[0] == last_in[0] && rgb[1] == last_in[1] && rgb[2] == last_in[2]) {
            memcpy(kcmy, last_out, 4);
            continue;
        }
        if (rgb[0] == rgb[1] && rgb[1] == rgb[2]) memcpy(kcmy, lut_gray_kcmy[rgb[0]], 4);
        else tetra(&lut_rgb_kcmy[0][0][0][0], 4, rgb, kcmy);
        memcpy(last_in, rgb, 3);
        memcpy(last_out, kcmy, 4);
    }
}

void color_rgb_to_k(const uint8_t *rgb, uint8_t *k, int n) {
    uint8_t last_in[3] = {255, 255, 255}, last_out = lut_gray_k[255];
    for (int i = 0; i < n; i++, rgb += 3, k++) {
        if (rgb[0] == last_in[0] && rgb[1] == last_in[1] && rgb[2] == last_in[2]) {
            *k = last_out;
            continue;
        }
        if (rgb[0] == rgb[1] && rgb[1] == rgb[2]) *k = lut_gray_k[rgb[0]];
        else tetra(&lut_rgb_k[0][0][0], 1, rgb, k);
        memcpy(last_in, rgb, 3);
        last_out = *k;
    }
}

void color_gray_to_kcmy(const uint8_t *gray, uint8_t *kcmy, int n) {
    for (int i = 0; i < n; i++) memcpy(kcmy + 4 * i, lut_gray_kcmy[gray[i]], 4);
}

void color_gray_to_k(const uint8_t *gray, uint8_t *k, int n) {
    for (int i = 0; i < n; i++) k[i] = lut_gray_k[gray[i]];
}

void color_draft(uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) buf[i] = lut_draft_curve[buf[i]];
}
