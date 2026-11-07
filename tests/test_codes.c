// SPDX-License-Identifier: Apache-2.0
/*
 * Known-answer tests for the bit-level code tables (SPEC.md section 7), stream
 * termination (7.5), the data block header (5) and two hand-computed pages.
 *
 * The library sources are included directly so the static coding helpers of
 * both the encoder and the decoder can be exercised.
 */
#include "../src/hbpl.c"
#include "../src/hbpl_decode.c"
#include "test_util.h"

/* Bits written so far, as a string of '0'/'1' (not including padding). */
static void bits_of(const bitw *w, char *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < w->buf.len && k + 1 < n; i++)
        for (int b = 7; b >= 0 && k + 1 < n; b--)
            s[k++] = (char)('0' + ((w->buf.data[i] >> b) & 1));
    for (int b = w->nacc - 1; b >= 0 && k + 1 < n; b--)
        s[k++] = (char)('0' + ((w->acc >> b) & 1));
    s[k] = '\0';
}

/* Pack a '0'/'1' string (spaces ignored) into bytes, padding with 1 bits. */
static size_t pack_bits(const char *s, uint8_t *out, size_t cap)
{
    size_t nbits = 0;
    memset(out, 0xFF, cap);
    for (; *s; s++) {
        if (*s == ' ')
            continue;
        size_t byte = nbits / 8;
        if (byte >= cap)
            break;
        if (*s == '0')
            out[byte] &= (uint8_t)~(0x80u >> (nbits % 8));
        nbits++;
    }
    return nbits;
}

static void append_binary(char *s, uint32_t v, int nbits)
{
    size_t k = strlen(s);
    for (int b = nbits - 1; b >= 0; b--)
        s[k++] = (char)('0' + ((v >> b) & 1));
    s[k] = '\0';
}

/* ------------------------------------------------------------------------ */

static void test_tokens(void)
{
    /* SPEC.md 7.1, indexed by token 0..6. */
    static const char *const table0[7] = { "0", "110", "11100", "11101", "11110", "10",
                                           "111110" };
    static const char *const table1[7] = { "10", "110", "11100", "11101", "11110", "0",
                                           "111110" };
    for (int t = 0; t < 2; t++) {
        for (int tok = 0; tok < 7; tok++) {
            const char *want = t ? table1[tok] : table0[tok];
            bitw w = {0};
            put_token(&w, t, tok);
            char got[64];
            bits_of(&w, got, sizeof got);
            CHECK(!strcmp(got, want), "table %d token %d: got %s want %s", t, tok, got, want);
            hbpl_buf_free(&w.buf);

            uint8_t bytes[4];
            size_t nb = pack_bits(want, bytes, sizeof bytes);
            bitr r = { bytes, nb, 0 };
            int dec = read_token(&r, t);
            CHECK(dec == tok && r.pos == nb, "table %d decode %s: got %d", t, want, dec);
        }
        /* The sixth bit of token 6 is ignored when read. */
        uint8_t bytes[1];
        size_t nb = pack_bits("111111", bytes, 1);
        bitr r = { bytes, nb, 0 };
        CHECK(read_token(&r, t) == 6 && r.pos == 6, "token 6 with sixth bit 1");
    }
}

static void test_run_lengths(void)
{
    /* SPEC.md 7.2. */
    static const struct { uint32_t lo, hi; const char *prefix; int extra; } tab[] = {
        { 1, 1, "00", 0 },          { 2, 2, "010", 0 },         { 3, 3, "011", 0 },
        { 4, 5, "100", 1 },         { 6, 9, "101", 2 },         { 10, 25, "110", 4 },
        { 26, 33, "111000", 3 },    { 34, 41, "111001", 3 },    { 42, 49, "111010", 3 },
        { 50, 65, "111011", 4 },    { 66, 97, "111100", 5 },    { 98, 161, "111101", 6 },
        { 162, 673, "111110", 9 },  { 674, 17057, "111111", 14 },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        for (uint32_t n = tab[i].lo; n <= tab[i].hi; n++) {
            char want[40];
            strcpy(want, tab[i].prefix);
            append_binary(want, n - tab[i].lo, tab[i].extra);
            bitw w = {0};
            put_run_length(&w, n);
            char got[40];
            bits_of(&w, got, sizeof got);
            hbpl_buf_free(&w.buf);
            uint8_t bytes[4];
            size_t nb = pack_bits(want, bytes, sizeof bytes);
            bitr r = { bytes, nb, 0 };
            long dec = read_run_length(&r);
            if (strcmp(got, want) || dec != (long)n || r.pos != nb) {
                if (bad++ < 5)
                    CHECK(0, "run length %u: got %s want %s, decoded %ld", (unsigned)n, got,
                          want, dec);
            }
        }
    }
    CHECK(bad == 0, "%d run lengths wrong", bad);
    /* Spot checks written out in full. */
    struct { uint32_t n; const char *bits; } spot[] = {
        { 1, "00" }, { 5, "1001" }, { 25, "1101111" }, { 26, "111000000" },
        { 65, "1110111111" }, { 673, "111110111111111" },
        { 674, "11111100000000000000" }, { 17057, "11111111111111111111" },
    };
    for (size_t i = 0; i < sizeof spot / sizeof spot[0]; i++) {
        bitw w = {0};
        put_run_length(&w, spot[i].n);
        char got[40];
        bits_of(&w, got, sizeof got);
        hbpl_buf_free(&w.buf);
        CHECK(!strcmp(got, spot[i].bits), "run %u: %s", (unsigned)spot[i].n, got);
    }
}

static void test_differences(void)
{
    /* SPEC.md 7.3. */
    static const struct { int lo, hi; const char *prefix; int extra; } tab[] = {
        { 2, 3, "011", 1 },   { 4, 7, "100", 2 },   { 8, 15, "101", 3 },
        { 16, 47, "110", 5 }, { 48, 79, "1110", 5 }, { 80, 128, "1111", 6 },
    };
    int bad = 0;
    for (int d = -128; d <= 127; d++) {
        char want[40];
        if (d == 0)
            strcpy(want, "000");
        else if (d == 1)
            strcpy(want, "001");
        else if (d == -1)
            strcpy(want, "010");
        else {
            int a = d < 0 ? -d : d;
            size_t i = 0;
            while (a > tab[i].hi)
                i++;
            strcpy(want, tab[i].prefix);
            strcat(want, d < 0 ? "1" : "0");
            append_binary(want, (uint32_t)(a - tab[i].lo), tab[i].extra);
        }
        bitw w = {0};
        put_diff(&w, d);
        char got[40];
        bits_of(&w, got, sizeof got);
        hbpl_buf_free(&w.buf);
        uint8_t bytes[4];
        size_t nb = pack_bits(want, bytes, sizeof bytes);
        bitr r = { bytes, nb, 0 };
        int dec = 999;
        int rc = read_diff(&r, &dec);
        if (strcmp(got, want) || rc != 0 || dec != d || r.pos != nb) {
            if (bad++ < 5)
                CHECK(0, "diff %d: got %s want %s, decoded %d", d, got, want, dec);
        }
    }
    CHECK(bad == 0, "%d differences wrong", bad);
    bitw w = {0};
    put_diff(&w, -128);
    char got[40];
    bits_of(&w, got, sizeof got);
    hbpl_buf_free(&w.buf);
    CHECK(!strcmp(got, "11111110000"), "-128 coded as 1111 1 110000: %s", got);
}

static void test_termination(void)
{
    bitw w = {0};
    bw_put(&w, 0x5, 3);   /* 101 */
    bw_finish(&w);
    CHECK(w.buf.len == 1 && w.buf.data[0] == 0xBF, "101 pads to BF");
    hbpl_buf_free(&w.buf);

    bitw e = {0};
    bw_put(&e, 0x00, 8);
    bw_finish(&e);
    CHECK(e.buf.len == 1 && e.buf.data[0] == 0x00, "byte-aligned stream is not padded");
    hbpl_buf_free(&e.buf);

    bitw z = {0};
    bw_finish(&z);
    CHECK(z.buf.len == 0, "stream with no bits is empty");

    bitw m = {0};
    bw_put(&m, 0x0, 7);
    bw_put(&m, 0x3, 2);   /* 0000000 11 */
    bw_finish(&m);
    CHECK(m.buf.len == 2 && m.buf.data[0] == 0x01 && m.buf.data[1] == 0xFF,
          "9 bits pad to two bytes");
    hbpl_buf_free(&m.buf);

    /* A page with no literal pixels has empty difference streams; the token
     * stream carries the extra FF byte. */
    uint8_t px[16 * 2];
    memset(px, 0, sizeof px);
    px[0] = px[16] = 0xFF;   /* column 0 as the encoder writes it */
    hbpl_buf out = {0};
    CHECK(hbpl_encode_page(px, 16, 2, 0, HBPL_PROFILE_PORTABLE, &out) == 0, "encode");
    hbpl_block_info bi;
    CHECK(hbpl_parse_block(out.data, out.len, false, &bi) == 0, "parse");
    CHECK(bi.len[0] > 0 && bi.stream[0][bi.len[0] - 1] == 0xFF, "token stream ends in FF");
    hbpl_buf_free(&out);
}

static const uint8_t hdr_color[48] = {
    0x20, 0, 0, 0, 0x00, 0x01, 0x01, 0x40, 0x10, 0x32, 0x04, 0x00, 0xA1, 0x42, 0, 0,
    0, 0, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t hdr_mono[48] = {
    0x20, 0, 0, 0, 0x08, 0x01, 0x00, 0x00, 0x10, 0x32, 0x04, 0x00, 0xA1, 0x42, 0, 0,
    0, 0, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void test_hand_pages(void)
{
    /* Mono 16 x 1, all zero, portable profile:
     *   (0,0) = FF after 8.1: literal (token 5 "10", K diff -1 "010")
     *   (0,1..15): run of 15 from "up" (guard 0), slot 1, bit 0 -> token 1 "110",
     *              length 15 "110 0101".
     * Tokens: 10 110 110 0101 -> B6 5F (padded) + FF; K: 010 -> 5F. */
    {
        uint8_t px[16] = {0};
        hbpl_buf out = {0};
        CHECK(hbpl_encode_page(px, 16, 1, 0, HBPL_PROFILE_PORTABLE, &out) == 0, "encode");
        uint8_t want[48 + 4];
        memcpy(want, hdr_mono, 48);
        want[32] = 3;
        want[36] = 1;
        want[48] = 0xB6; want[49] = 0x5F; want[50] = 0xFF; want[51] = 0x5F;
        CHECK(out.len == sizeof want && !memcmp(out.data, want, sizeof want),
              "mono hand page (len %zu)", out.len);
        uint8_t dec[16];
        CHECK(hbpl_decode_block(out.data, out.len, 16, 1, false, HBPL_MODEL_ROW, dec) == 0 &&
              dec[0] == 0xFF && dec[1] == 0 && dec[15] == 0, "mono hand page decodes");
        hbpl_buf_free(&out);
    }
    /* Color 16 x 1, all zero, portable profile:
     *   (0,0) = FFFFFFFF equals the guard above: run 1 from "up", slot 1
     *         -> token 1 "110", length 1 "00"; rotor (1,0,2,3,4), bit 1.
     *   (0,1): no neighbour matches -> literal "10", diffs +1 "001" x 4; bit 0.
     *   (0,2..15): run 14 from "left" (slot 1) -> token 1 "110", length "110 0100".
     * Tokens: 110 00 10 110 1100100 -> C5 B2 7F + FF; each diff stream 3F. */
    {
        uint8_t px[16 * 4] = {0};
        hbpl_buf out = {0};
        CHECK(hbpl_encode_page(px, 16, 1, 1, HBPL_PROFILE_PORTABLE, &out) == 0, "encode");
        uint8_t want[48 + 8];
        memcpy(want, hdr_color, 48);
        want[32] = 4;
        want[36] = want[40] = want[44] = 1;
        const uint8_t tail[8] = { 0xC5, 0xB2, 0x7F, 0xFF, 0x3F, 0x3F, 0x3F, 0x3F };
        memcpy(want + 48, tail, 8);
        CHECK(out.len == sizeof want && !memcmp(out.data, want, sizeof want),
              "color hand page (len %zu)", out.len);
        hbpl_block_info bi;
        CHECK(hbpl_parse_block(out.data, out.len, true, &bi) == 0 && bi.len[4] == 1,
              "Y length is the remainder");
        hbpl_buf_free(&out);
    }
    /* Reference profile, color: column 0 is FF FF FF FF on row 0, 00 00 00 00 below. */
    {
        uint8_t px[16 * 3 * 4];
        memset(px, 0x55, sizeof px);
        hbpl_buf out = {0};
        CHECK(hbpl_encode_page(px, 16, 3, 1, HBPL_PROFILE_REFERENCE, &out) == 0, "encode");
        uint8_t dec[sizeof px];
        CHECK(hbpl_decode_block(out.data, out.len, 16, 3, true, HBPL_MODEL_LINEAR, dec) == 0,
              "decode");
        static const uint8_t ff[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, z[4] = {0};
        CHECK(!memcmp(dec, ff, 4) && !memcmp(dec + 64, z, 4) && !memcmp(dec + 128, z, 4) &&
              dec[4] == 0x55, "reference column 0");
        CHECK(px[0] == 0x55, "caller's pixels not modified");
        hbpl_buf_free(&out);
    }
}

static void test_decoder_semantics(void)
{
    /* Hand-assembled mono 16 x 2 page exercising the rotor and the bit flag.
     *   literal (0,0): K diff +5            tokens "10"         K "100 0 01"
     *   run t=0 n=15 (slot 0 = left)        "0" "110 0101"     -> row 0 all 5
     *   bit = 1: token 0 now selects slot 1 = up; n = 16      "0" "110 0110"
     */
    char tokens[64] = "10" "0" "1100101" "0" "1100110";
    uint8_t block[48 + 8];
    memcpy(block, hdr_mono, 48);
    uint8_t tb[4], kb[1];
    pack_bits(tokens, tb, 3);
    size_t tl = (strlen(tokens) + 7) / 8;
    tb[tl] = 0xFF;
    tl++;
    pack_bits("100001", kb, 1);
    block[32] = (uint8_t)tl;
    block[36] = 1;
    memcpy(block + 48, tb, tl);
    memcpy(block + 48 + tl, kb, 1);
    uint8_t dec[32];
    int rc = hbpl_decode_block(block, 48 + tl + 1, 16, 2, false, HBPL_MODEL_LINEAR, dec);
    int ok = rc == 0;
    for (int i = 0; i < 32; i++)
        ok = ok && dec[i] == 5;
    CHECK(ok, "rotor/bit semantics (rc %d)", rc);

    /* Truncated streams are rejected. */
    CHECK(hbpl_decode_block(block, 48, 16, 2, false, HBPL_MODEL_LINEAR, dec) == -1,
          "empty token stream rejected");
    CHECK(hbpl_decode_block(block, 40, 16, 2, false, HBPL_MODEL_LINEAR, dec) == -1,
          "short block rejected");
}

int main(void)
{
    test_tokens();
    test_run_lengths();
    test_differences();
    test_termination();
    test_hand_pages();
    test_decoder_semantics();
    return test_summary("test_codes");
}
