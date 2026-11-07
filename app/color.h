// SPDX-License-Identifier: Apache-2.0
//
// RGB to printer color (K, C, M, Y toner amounts) for C1660w-family printers.
#ifndef COLOR_H
#define COLOR_H

#include <stdint.h>

/* n RGB pixels (3 bytes each) -> n KCMY pixels (4 bytes each; 0 = no toner). */
void color_rgb_to_kcmy(const uint8_t *rgb, uint8_t *kcmy, int n);
/* n RGB pixels -> n K bytes (monochrome printing). */
void color_rgb_to_k(const uint8_t *rgb, uint8_t *k, int n);
/* n gray pixels (0 = black, 255 = white) -> n KCMY pixels / n K bytes. */
void color_gray_to_kcmy(const uint8_t *gray, uint8_t *kcmy, int n);
void color_gray_to_k(const uint8_t *gray, uint8_t *k, int n);
/* Draft (toner saver): apply the toner-saving curve to n converted bytes in place. */
void color_draft(uint8_t *buf, int n);

#endif
