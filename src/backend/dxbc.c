/* dxbc.c: see dxbc.h. The checksum follows the layout documented by the
 * open-source DXBC tools (Wine, DXVK): MD5's compression function, with the
 * bit count stored before the tail and a second count word at the end. */
#include "dxbc.h"

#include <string.h>

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(uint32_t st[4], const unsigned char blk[64]) {
    static const int S[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    uint32_t m[16], a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 16; i++) m[i] = rd32(blk + 4 * i);
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else f = c ^ (b | ~d), g = (7 * i) % 16;
        f += a + K[i] + m[g];
        a = d, d = c, c = b;
        b += rotl(f, S[i / 16][i % 4]);
    }
    st[0] += a, st[1] += b, st[2] += c, st[3] += d;
}

void bare_dxbc_checksum(const unsigned char *blob, size_t size, uint32_t out[4]) {
    const unsigned char *data = blob + 20;
    size_t n = size - 20, left = n % 64, full = n - left;
    uint32_t bits1 = (uint32_t)(n * 8), bits2 = (bits1 >> 2) | 1;
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    unsigned char blk[64];
    for (size_t i = 0; i < full; i += 64) md5_block(st, data + i);
    memset(blk, 0, sizeof blk);
    if (left >= 56) {
        memcpy(blk, data + full, left);
        blk[left] = 0x80;
        md5_block(st, blk);
        memset(blk, 0, sizeof blk);
        wr32(blk, bits1);
        wr32(blk + 60, bits2);
    } else {
        wr32(blk, bits1);
        memcpy(blk + 4, data + full, left);
        blk[4 + left] = 0x80;
        wr32(blk + 60, bits2);
    }
    md5_block(st, blk);
    memcpy(out, st, sizeof st);
}

bool bare_dxbc_make_strict(unsigned char *blob, size_t size) {
    if (size < 32 || memcmp(blob, "DXBC", 4) != 0 || rd32(blob + 24) != size) return false;
    uint32_t chunks = rd32(blob + 28);
    if (32 + (size_t)chunks * 4 > size) return false;
    for (uint32_t k = 0; k < chunks; k++) {
        size_t off = rd32(blob + 32 + 4 * k);
        if (off + 16 > size) return false;
        if (memcmp(blob + off, "SHEX", 4) != 0 && memcmp(blob + off, "SHDR", 4) != 0) continue;
        size_t end = off + 8 + rd32(blob + off + 4);
        if (end > size) return false;
        /* After the version and length tokens, declarations come first; the
         * global flags one (opcode 0x6a) holds refactoringAllowed in bit 11. */
        for (size_t p = off + 16; p + 4 <= end;) {
            uint32_t tok = rd32(blob + p), len = (tok >> 24) & 0x7f;
            if ((tok & 0x7ff) == 0x6a) {
                wr32(blob + p, tok & ~(1u << 11));
                uint32_t sum[4];
                bare_dxbc_checksum(blob, size, sum);
                for (int i = 0; i < 4; i++) wr32(blob + 4 + 4 * i, sum[i]);
                return true;
            }
            if ((tok & 0x7ff) == 0x35 && p + 8 <= end) len = rd32(blob + p + 4);   /* custom data: length follows */
            if (!len) return false;
            p += 4 * (size_t)len;
        }
        return true;   /* no global flags: nothing allowed to refactor */
    }
    return false;
}
