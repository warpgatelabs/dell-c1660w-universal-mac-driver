# HBPL version 1: job format and encoder specification

This document specifies the print-job format accepted by printers that use HBPL version 1
(Host Based Printing Language), such as the C1660w color LED printer, and an encoder that
produces it. It was written from observation of real print jobs and is intended to be
sufficient, on its own, to implement a byte-exact encoder and a decoder.

Conventions:
- All multi-byte integers are **little-endian** unsigned.
- Bit streams are written and read **most significant bit first**: the first bit of a
  stream is bit 7 of its first byte.
- "Code `110`" means the three bits 1, 1, 0 in that order.
- `W` is the row stride in pixels, `H` the page height in pixels.

## 1. Job layout

A job is a byte stream:

1. A PJL header (text, section 2), ending with the line `@PJL ENTER LANGUAGE=HBPL`.
2. A job record `A` (section 3.2).
3. For each page, in output order: a page record `C`, a byte `Q` (0x51), a data record `R`
   followed by the page's data block (section 5), then the two bytes `S` `D` (0x53 0x44).
4. The byte `B` (0x42), immediately followed by the PJL trailer
   `ESC %-12345X@PJL EOJ` and a line feed (bytes `1B 25 2D 31 32 33 34 35 58` then the text
   `@PJL EOJ` then `0A`).

There is no padding or alignment anywhere.

## 2. PJL header

Lines end with a single line feed (0x0A), never CR LF. The header is exactly the following
lines, in this order. `ESC` is byte 0x1B. Values in `{braces}` are variables; lines marked
with a condition appear only when it holds.

```
ESC%-12345X@PJL JOB MODE=PRINTER
@PJL SET STRINGCODESET=UTF8
@PJL COMMENT DATE={date}
@PJL COMMENT TIME={time}
@PJL COMMENT DNAME={title}
@PJL SET JOBATTR="@LUNA={user}"
@PJL SET COPIES={copies}
@PJL SET QTY=1
@PJL SET JOBATTR="@TRCH=OFF"
@PJL SET DUPLEX=OFF                         (manual duplex off)
@PJL SET DUPLEX=ON                          (manual duplex on)
@PJL SET BINDING={binding}                  (manual duplex on)
@PJL SET JOBATTR="@AJCL=3"
@PJL SET JOBATTR="@AJCM=3"
@PJL SET JOBATTR="@AJCH=3"
@PJL SET JOBATTR="@AJML=3"
@PJL SET JOBATTR="@AJMM=3"
@PJL SET JOBATTR="@AJMH=3"
@PJL SET JOBATTR="@AJYL=3"
@PJL SET JOBATTR="@AJYM=3"
@PJL SET JOBATTR="@AJYH=3"
@PJL SET JOBATTR="@AJKL=3"
@PJL SET JOBATTR="@AJKM=3"
@PJL SET JOBATTR="@AJKH=3"
@PJL SET JOBATTR="@APSP=OFF"
@PJL SET JOBATTR="@MSIP={media}"
@PJL SET RENDERMODE={rendermode}
@PJL SET ECONOMODE={economode}
@PJL SET JOBATTR="@IREC=OFF"
@PJL SET JOBATTR="@TRAP=OFF"
@PJL SET RET=OFF
@PJL SET JOBATTR="@TDFT=0"
@PJL SET JOBATTR="@GDFT=0"
@PJL SET JOBATTR="@IDFT=0"
@PJL SET JOBATTR="@NLPP=1"
@PJL SET JOBATTR="@JOAU={user}"
@PJL SET JOBATTR="@CNAM={host}"
@PJL SET OUTBIN=FACEDOWN
@PJL SET JOBATTR="@PODR=NORMAL"
@PJL SET SLIPSHEET=OFF
@PJL SET PAPERDIRECTION={feed}
@PJL SET RESOLUTION=600
@PJL SET BITSPERPIXEL=8
@PJL SET JOBATTR="@DRDM=RASTER"
{calibration lines, see below}
@PJL SET JOBATTR="@BANR=DEVICE"
@PJL SET IWAMANUALDUP=OFF                   (manual duplex off)
@PJL SET IWAMANUALDUP=ON                    (manual duplex on)
@PJL SET IWASIDE2START={side2start}         (manual duplex on)
@PJL SET IWAJAMRECOVERY={jam}
@PJL ENTER LANGUAGE=HBPL
```

Calibration lines, color jobs (12 lines, in this order, each `@PJL SET JOBATTR="@{name}=11"`):
`TCPR, GCPR, ICPR, TUCR, GUCR, IUCR, TTRC, GTRC, ITRC, TSCR, GSCR, ISCR`.

Calibration lines, monochrome jobs (6 lines `@PJL SET JOBATTR="@{name}=33"` for
`TTRC, GTRC, ITRC, TSCR, GSCR, ISCR`, then one line `@PJL SET JOBATTR="@PBLK=OFF"`).

| variable | value |
|---|---|
| date | local date, `MM/DD/YYYY` (zero-padded) |
| time | local time, `HH:MM:SS` (24-hour, zero-padded) |
| title, user, host | caller-supplied text with double quotes, CR, LF and other control characters (bytes below 0x20) removed |
| copies | number of copies the printer should make, at least 1 |
| binding | `LONGEDGE` or `SHORTEDGE` |
| media | paper type keyword (section 4.4) |
| rendermode | `COLOR` or `GRAYSCALE` |
| economode | `ON` (draft / toner saver) or `OFF` |
| feed | `SEF` normally; `LEF` when the sheet is wider than it is tall |
| side2start | 1-based position, in output order, of the first page of the second manual-duplex pass |
| jam | `ON` to reprint on paper jam, otherwise `AUTO` |

## 3. HBPL records

### 3.1 Encoding

A record is one type byte followed, for records that have attributes, by a sequence of
attributes. Each attribute is: an attribute id byte, a value-type byte, and the value.

| value type | value bytes |
|---|---|
| `A1` | 1 byte, u8 |
| `A2` | 2 bytes, u16 |
| `A4` | 4 bytes, u32 |
| `C2` | 4 bytes: two u16 (first, second) |
| `C3` | 4 bytes: two u16 (first, second) |
| `C4` | 8 bytes: two u32 (first, second) |
| `B1` | prefix: no value of its own; the next byte is the actual value type |

The attribute list of a record ends after its terminating attribute: `83` for `A`, `A2`
for `C`, `A4` for `R`. Records `Q`, `S`, `D` and `B` have no attributes.

### 3.2 Job record `A`

Exactly these 12 bytes: `41  81 A1 00  82 A2 07 00  83 A2 01 00`.

### 3.3 Page record `C`

`43` followed by these attributes in this order:

| id | type | value |
|---|---|---|
| 91 | A1 | feed direction: 1 when the sheet is wider than tall, else 0 |
| 92 | A1 | 1 |
| 93 | A1 | 1 |
| 94 | A1 | paper code (section 4.3) |
| 95 | C2 | custom sheet width, height in device pixels when the paper code is 255; otherwise 0, 0 |
| 96 | A1 | 3 |
| 97 | C3 | image offset x, y on the sheet in device pixels (section 4.2) |
| 98 | A1 | 0 |
| 99 | A4 | page number: 1-based position of this page in the job's output order |
| 9A | C4 | content width, height in pixels (section 4.1) |
| 9B | A1 | 1 for a blank page inserted by manual duplex, else 0 |
| 9C | A1 | 1 |
| 9D | A1 | 0x8B for color data, 0x09 for monochrome data |
| 9E | A1 | 2 |
| 9F | A1 | 5 |
| A0 | A1 | 8 |
| A1 | A1 | 0 |
| A2 | C4 | row stride W, height H in pixels |

### 3.4 Data record `R`

`52  A3 A1 00  A4 B1 A4 {length:u32}` followed by `length` bytes of page data (section 5).
The `B1` prefix is always present and the length is always 4 bytes.

## 4. Page geometry and parameters

### 4.1 Size

The renderer produces an image of `w` x `H` pixels at 600 x 600 dpi covering the sheet's
imageable area: `w = round((sheet width - left margin - right margin) * 600 / 72)` and
`H = round((sheet height - top margin - bottom margin) * 600 / 72)`, with sizes in points
(round the difference, not each term). The page image sent to the printer is:

- **content width** = `w + 8`: 8 blank columns are added on the left;
- **stride** `W` = content width rounded up to a multiple of 8; columns from the content
  width up to `W` are blank;
- height `H` unchanged.

Rendered pixel (y, x) is page pixel (y, x + 8). Column 0 is a marker column (section 7.2),
not printed.

### 4.2 Offset

Attribute 97 is the imageable area's left and top margin converted to device pixels:
`round(margin_points * 600 / 72)`. For the usual 11.62 pt margins this is 97, 97.

### 4.3 Paper codes

| code | size (points) |
|---|---|
| 0 | Letter 612 x 792 |
| 1 | Legal 612 x 1008 |
| 2 | A4 595 x 842 |
| 3 | Executive 522 x 756 |
| 6 | Envelope #10 297 x 684 |
| 7 | Monarch envelope 279 x 540 |
| 8 | C5 envelope 459 x 649 |
| 9 | DL envelope 312 x 624 |
| 11 | B5 516 x 729 |
| 15 | A5 420 x 595 |
| 205 | Folio 612 x 936 |
| 255 | custom size (attribute 95 carries the sheet size in device pixels, `round(points * 600 / 72)`) |

A landscape sheet (for example a rotated envelope, 540 x 279) uses the code of the
corresponding portrait size, feed direction 1 and PJL `PAPERDIRECTION=LEF`.

### 4.4 Paper type keywords (PJL `@MSIP`)

`NORMAL`, `NORMALREV` (plain, second side), `HIGHQUALITY`, `HIGHQUALITYREV` (plain thick),
`RECYCLE`, `RECYCLEREV`, `LABEL`, `THICK`, `THICKSIDE2` (covers), `COATEDPAPER2`,
`COATEDPAPER2REV`, `ENVELOPE`.

## 5. Page data block

A data block is a 48-byte header followed by 2 (monochrome) or 5 (color) bit streams,
concatenated in this order: the token stream, then one difference stream per channel
(K, then C, M, Y for color).

Header bytes (offsets in decimal; all bytes not listed are 0):

| offset | color | monochrome |
|---|---|---|
| 0 | 0x20 | 0x20 |
| 4 | 0x00 | 0x08 |
| 5 | 0x01 | 0x01 |
| 6 | 0x01 | 0x00 |
| 7 | 0x40 | 0x00 |
| 8-11 | 10 32 04 00 | 10 32 04 00 |
| 12-13 | A1 42 | A1 42 |
| 18 | 0xFF | 0xFF |
| 32-35 | length of the token stream (u32) | same |
| 36-39 | length of the K stream | same |
| 40-43 | length of the C stream | 0 |
| 44-47 | length of the M stream | 0 |

For color, the Y stream's length is not stored: it is whatever remains of the block after
the token, K, C and M streams. For monochrome, both stream lengths are stored and the K
stream ends the block.

## 6. Pixels and the decoding model

### 6.1 Pixels

A color pixel is four bytes: K, C, M, Y toner amounts, 0 = none, 255 = full. A monochrome
pixel is one byte, K. Pixels are coded in raster order: row 0 left to right, then row 1, and
so on, `W x H` pixels in total.

### 6.2 Neighbors

A pixel can be copied from one of five neighbors, numbered 0-4:

| neighbor | color | monochrome | position relative to (y, x) |
|---|---|---|---|
| 0 | left | left | (y, x-1) |
| 1 | up | up | (y-1, x) |
| 2 | up-left | up-left | (y-1, x-1) |
| 3 | up-right | up-right | (y-1, x+1) |
| 4 | up 2 right | 8 left | color (y-1, x+2); monochrome (y, x-8) |

### 6.3 Guard value and row edges

The **guard value** is 0xFF in every channel for color pages and 0x00 for monochrome
pages. A reference above row 0 reads the guard value.

A reference whose column falls outside 0..W-1 can be interpreted two ways:

- **linear model**: the image is one array of `W x H` pixels preceded by `W + 1` guard
  pixels, and a reference is "current index + offset" with offsets -1, -W, -W-1, -W+1,
  -W+2 (monochrome neighbor 4: -8). Going left of column 0 reaches the end of the
  previous row; going right of column W-1 reaches the start of the current row.
- **row model**: such a reference reads the guard value.

Printers evidently decode with the row model. Real-world jobs from the original driver
decode correctly only that way at row edges. A conforming decoder implements both models
(selectable). The default (portable) encoder profile never produces a reference outside the
row, so its output decodes identically under both.

For literal pixels (section 7), the "left" pixel of column 0 is, in the linear model, the
last pixel of the previous row (the guard value for pixel (0, 0)); in the row model it is
the guard value.

## 7. Bit streams

### 7.1 Tokens

The token stream is a sequence of tokens. Tokens 0-4 start a **run**: the token is
followed, in the same stream, by a run length. Token 5 is a **literal** pixel. Token 6
switches between two code tables.

Code table 0 (in effect at the start of every page):

| token | code |
|---|---|
| 0 | `0` |
| 5 | `10` |
| 1 | `110` |
| 2 | `11100` |
| 3 | `11101` |
| 4 | `11110` |
| 6 | `111110` |

Code table 1 is identical except that tokens 0 and 5 swap codes: token 5 is `0`, token 0 is
`10`.

Token 6 is six bits long in both tables: the prefix `11111` identifies it and the sixth bit
is always written as 0 and ignored when read.

### 7.2 Run lengths

A run length `n` is between 1 and 17057 and follows its token in the token stream:

| n | code |
|---|---|
| 1 | `00` |
| 2 | `010` |
| 3 | `011` |
| 4-5 | `100` then 1 bit: n-4 |
| 6-9 | `101` then 2 bits: n-6 |
| 10-25 | `110` then 4 bits: n-10 |
| 26-33 | `111` `000` then 3 bits: n-26 |
| 34-41 | `111` `001` then 3 bits: n-34 |
| 42-49 | `111` `010` then 3 bits: n-42 |
| 50-65 | `111` `011` then 4 bits: n-50 |
| 66-97 | `111` `100` then 5 bits: n-66 |
| 98-161 | `111` `101` then 6 bits: n-98 |
| 162-673 | `111` `110` then 9 bits: n-162 |
| 674-17057 | `111` `111` then 14 bits: n-674 |

Extra bits are an unsigned number, most significant bit first.

### 7.3 Differences

A literal pixel is coded, per channel, as the signed difference `d` between its value and
the value of the same channel in the left pixel (section 6.3), taken modulo 256 and
interpreted as -128..127. Each channel's difference goes to that channel's own stream:

| d | code |
|---|---|
| 0 | `000` |
| +1 | `001` |
| -1 | `010` |
| ±2..3 | `011` s then 1 bit: |d|-2 |
| ±4..7 | `100` s then 2 bits: |d|-4 |
| ±8..15 | `101` s then 3 bits: |d|-8 |
| ±16..47 | `110` s then 5 bits: |d|-16 |
| ±48..79 | `1110` s then 5 bits: |d|-48 |
| ±80..128 | `1111` s then 6 bits: |d|-80 |

`s` is a sign bit: 0 for positive, 1 for negative. (-128 is coded as `1111` `1` `110000`.)

### 7.4 Run semantics and the rotor

Decoding keeps three pieces of state, reset at the start of every page:

- the **rotor**, an ordering of the five neighbors, initially (0, 1, 2, 3, 4);
- a flag **bit**, initially 0;
- the current code table, initially 0.

A run token `t` (0-4) selects rotor slot `s = t + bit` (always 0-4). The run copies `n`
pixels, one at a time in raster order, each from neighbor `rotor[s]` of the pixel being
written. Copying is pixel by pixel, so a run may copy pixels it has just written, and runs
may continue across row ends. If `s > 0`, the neighbor in slot `s` is then moved to slot 0
and slots 0..s-1 move up by one (move-to-front). Finally, `bit` becomes 1 if `n < 17057`,
else 0.

A literal (token 5) writes one pixel from its differences and sets `bit` to 0. Token 6
toggles the code table and changes nothing else.

Decoding a page ends when all `W x H` pixels have been written; any remaining bits are
ignored.

### 7.5 Stream termination

After the last token, each stream (token stream and every difference stream) is padded
with 1 bits to a byte boundary (no padding if it already ends on one). Then one further
byte 0xFF is appended to the token stream only. A stream with no bits at all is empty
(length 0).

## 8. Encoder

Any token sequence that decodes to the intended pixels is valid. This section specifies
the particular encoder whose output matches the reference jobs byte for byte, plus the
portable variant to use for printing. Both profiles use identical logic and differ only in
two tables (8.1, 8.2).

### 8.1 Column 0

Before encoding, the encoder overwrites column 0 of the page:

| profile | color | monochrome |
|---|---|---|
| reference | pixel (0, 0) = FF FF FF FF; column 0 of every other row = 00 00 00 00 | 0xFF in every row |
| portable | FF FF FF FF in every row | 0xFF in every row |

### 8.2 Forbidden columns

For each neighbor, a contiguous column range `[lo, hi]` that a run using that neighbor may
never cover (`-` = no restriction). `W` is the stride.

| neighbor | reference, color | reference, mono | portable, color | portable, mono |
|---|---|---|---|---|
| 0 left | [1, 1] | [0, 0] | [0, 1] | [0, 1] |
| 1 up | - | - | - | - |
| 2 up-left | [1, 1] | [0, 0] | [0, 1] | [0, 1] |
| 3 up-right | [W-1, W-1] | [0, 0] | [W-1, W-1] | [W-1, W-1] |
| 4 (see 6.2) | [W-2, W-1] | [0, 0] | [W-2, W-1] | [0, 8] |

The portable ranges exclude every reference outside the row, and every copy from column 0
into another column.

### 8.3 Pixel comparison and references

The encoder works in the linear model (6.3): it compares pixel `p` with pixel `p + offset`,
where positions before the first pixel hold the guard value. Two pixels are equal when all
their channels are equal (monochrome: K only).

### 8.4 Algorithm

State: rotor (0, 1, 2, 3, 4), `bit` = 0, table = 0, a counter `stat` = 0. Position `p` = 0
(raster index). `col` = p mod W. Repeat until `p = W x H`:

1. `limit` = min(17057, W x H - p).
2. For each rotor slot `s` = 0, 1, 2, 3, 4 in that order, with neighbor `k = rotor[s]`:
   - `lim` = `limit`; if neighbor `k` has a forbidden range `[lo, hi]`: if
     `lo <= col <= hi` then `lim` = 0, else `lim` = min(`lim`, (lo - col) mod W);
   - `run(s)` = the number of consecutive pixels, starting at `p`, that equal their
     neighbor `k`, counting at most `lim`;
   - `key(s)` = min(`run(s)`, W - col).

   The chosen slot `c` is the one with the largest key; on equal keys the lowest slot wins.
   If every key is 0 there is no run.
3. **No run** (literal): write token 5 in the current table; for each channel (K only for
   monochrome; K, C, M, Y for color) write the difference between pixel `p` and the
   linear-model left pixel `p - 1` to that channel's stream; set `bit` = 0; `stat` = `stat - 1`;
   `p` = `p + 1`.
4. **Run** of length `n = run(c)` (not capped by the key):
   1. If `c > 0`, move rotor slot `c` to the front (7.4).
   2. If the run's last pixel `p + n - 1` lies in a later row than pixel `p - 1` (for
      `p = 0` this test does not matter, since `stat` is 0): if |`stat`| > 8 and
      (`stat` < 0) differs from (table = 1), toggle the table and write token 6. Then set
      `stat` = 0.
   3. If `c = bit`, `stat` = `stat + 1`.
   4. Write token `c - bit` in the current table, then the run length `n`.
   5. `bit` = 1 if `n < 17057`, else 0. `p` = `p + n`.

`c >= bit` always holds in step 4: when `bit` is 1, the previous run stopped at this
position, so slot 0 (its neighbor) has run 0 here.

5. Terminate the streams (7.5) and assemble the data block (section 5).

## 9. Required implementation

Language C11, no dependencies beyond the C standard library. License: Apache-2.0 (SPDX
header in every file). Layout:

- `src/hbpl.h`, `src/hbpl.c`: encoder and job framing.
- `src/hbpl_decode.h`, `src/hbpl_decode.c`: decoder.
- `tools/hbpl-encode.c`: command-line encoder.
- `tools/hbpl-decode.c`: command-line decoder and inspector.
- `tests/`: unit tests runnable with `make test`.
- `Makefile` with targets `all` (library objects + tools into `build/`) and `test`.

### 9.1 Library API

All output is appended to a growable byte buffer:

```c
typedef struct { uint8_t *data; size_t len, cap; } hbpl_buf;
void hbpl_buf_free(hbpl_buf *b);

enum hbpl_profile { HBPL_PROFILE_PORTABLE = 0, HBPL_PROFILE_REFERENCE = 1 };

/* Compress one page into a data block (section 5), replacing out's contents.
 * pixels: H rows of W pixels; color = 4 bytes K,C,M,Y per pixel, mono = 1 byte.
 * W must be a multiple of 8 and at least 16. Column 0 is overwritten per 8.1 on an
 * internal copy (the caller's buffer is not modified). Returns 0 or -1. */
int hbpl_encode_page(const uint8_t *pixels, int stride, int height, int color,
                     int profile, hbpl_buf *out);
```

Job framing, each appending to `out` and returning 0 or -1:

- `int hbpl_append_job_header(hbpl_buf *out, const hbpl_job *job);` writes section 2 and the
  `A` record. `hbpl_job` carries: title, user, host (strings; NULL = empty), the time to
  print as DATE/TIME (`time_t`, formatted in local time), copies, color (bool), economode
  (bool), media keyword (NULL = `NORMAL`), long_edge_feed (bool), manual_duplex
  (0 off, 1 long edge, 2 short edge), side2_start, jam_recovery (bool).
- `int hbpl_append_page(hbpl_buf *out, const hbpl_page *page, const hbpl_buf *data);`
  writes the `C` record, `Q`, the `R` record with `data`, and `S` `D`. `hbpl_page` carries:
  paper code, long_edge_feed, custom_width, custom_height, offset_x, offset_y, page_number,
  content_width, stride, height, color, blank.
- `int hbpl_append_job_trailer(hbpl_buf *out);` writes `B` and the PJL trailer.

Decoder: parse a job (PJL header, records, pages), exposing each page's attributes and
decoded pixels; the edge model (linear or row) is a parameter.

### 9.2 Tools

- `hbpl-encode [--profile portable|reference] [--content-width N] [--offset X,Y] [--paper N] page.pam... > job`:
  reads PAM files (`P7`, `TUPLTYPE KCMY` depth 4 or `GRAYSCALE` depth 1, width = stride)
  and writes a complete job.
- `hbpl-decode [--model linear|row] [--out BASE] [--quiet] < job`: prints the PJL header,
  every record and attribute, and each page's stream lengths; with `--out`, writes
  `BASE-NN.pam` per page (`NN` = page number, two digits) in the same PAM format.

### 9.3 Verification vectors

The directory named by `HBPL_VECTORS` holds real jobs produced by the original driver (not
distributed, and not to be copied into this repository). For every page of every job, decoding with the linear model
and re-encoding the pixels with the reference profile must reproduce the page's data block
byte for byte. Color or monochrome follows attribute 9D. The `cube-*.prn` jobs contain
every RGB value once and are almost entirely literal pixels.
