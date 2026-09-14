/* BMP24 transform: modulo-256 color differences, then horizontal residuals.
 * The mapping is a bijection for every block and validated descriptor.
 * Headers, row padding, tails, and pixels crossing a block boundary pass
 * unchanged. Predictor history restarts at each row and block boundary. */
#ifndef ADAPT_BMP_H
#define ADAPT_BMP_H
#include "adapt.h"

static inline uint32_t bmp_load32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void bmp_store32(uint8_t *p, uint32_t x) {
    unsigned i;
    for (i = 0; i < 4; i++) p[i] = (uint8_t)(x >> (i * 8));
}
static inline int bmp_descriptor_valid(adapt_sizes s) {
    return s.bmp_offset >= 54 && s.bmp_width > 0 && s.bmp_width <= INT32_MAX &&
           s.bmp_rows > 0 && s.bmp_rows <= INT32_MAX;
}
static inline int bmp_probe(const uint8_t *p, size_t n, adapt_sizes *s) {
    uint32_t dib, width, height, offset, rows;
    uint64_t stride;
    if (p == NULL || s == NULL || n < 54 || p[0] != 'B' || p[1] != 'M') return 0;
    dib = bmp_load32(p + 14); width = bmp_load32(p + 18); height = bmp_load32(p + 22);
    offset = bmp_load32(p + 10);
    rows = height & UINT32_C(0x80000000) ? 0u - height : height;
    if (dib < 40 || (uint64_t)dib + 14 > offset || width == 0 || width > INT32_MAX ||
        rows == 0 || rows > INT32_MAX || p[26] != 1 || p[27] != 0 || p[28] != 24 || p[29] != 0 ||
        bmp_load32(p + 30) != 0) return 0;
    stride = ((uint64_t)width * 3 + 3) & ~UINT64_C(3);
    if ((uint64_t)offset + stride * rows > bmp_load32(p + 2)) return 0;
    s->bmp = 1; s->bmp_offset = offset; s->bmp_width = width; s->bmp_rows = rows;
    return 1;
}
static inline void bmp_transform(uint8_t *p, size_t n, uint64_t position, adapt_sizes s, int inverse) {
    uint64_t stride = ((uint64_t)s.bmp_width * 3 + 3) & ~UINT64_C(3);
    uint64_t row, end = position <= UINT64_MAX - n ? position + n : UINT64_MAX;
    if (!bmp_descriptor_valid(s)) return;
    row = position > s.bmp_offset ? (position - s.bmp_offset) / stride : 0;
    for (; row < s.bmp_rows; row++) {
        uint64_t start = s.bmp_offset + row * stride;
        uint64_t x = position > start ? (position - start + 2) / 3 : 0;
        uint8_t previous[3] = {0, 0, 0};
        if (start >= end) break;
        for (; x < s.bmp_width; x++) {
            uint64_t at = start + x * 3;
            uint8_t value[3];
            unsigned c;
            if (at + 3 > end) break;
            if (inverse) {
                for (c = 0; c < 3; c++) {
                    value[c] = (uint8_t)(p[(size_t)(at - position) + c] + previous[c]);
                    previous[c] = value[c];
                }
                p[(size_t)(at - position)] = (uint8_t)(value[0] + value[1]);
                p[(size_t)(at - position) + 1] = value[1];
                p[(size_t)(at - position) + 2] = (uint8_t)(value[2] + value[1]);
            } else {
                uint8_t *pixel = p + (size_t)(at - position);
                value[0] = (uint8_t)(pixel[0] - pixel[1]);
                value[1] = pixel[1];
                value[2] = (uint8_t)(pixel[2] - pixel[1]);
                for (c = 0; c < 3; c++) { pixel[c] = (uint8_t)(value[c] - previous[c]); previous[c] = value[c]; }
            }
        }
    }
}
#endif
