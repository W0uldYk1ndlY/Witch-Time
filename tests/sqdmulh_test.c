/* SQDMULH / SQRDMULH by element: exact saturation, rounding, aliases and clearing.
 * gcc -std=c11 -O2 -Wall -Wextra -fsanitize=undefined -fno-sanitize-recover=undefined \
 *     -I runtime tests/sqdmulh_test.c -lm -o /tmp/sqdmulh_test && /tmp/sqdmulh_test
 * The oracle divides an undoubled 64-bit product; it does not use runtime lane,
 * saturation or shift helpers, nor the implementation's wide doubled product. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "rt_ext.h"

static unsigned long cases;
static uint64_t rng = UINT64_C(0x91a57c6e3b204df8);

static uint32_t random32(void) {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (uint32_t)((rng * UINT64_C(0x2545f4914f6cdd1d)) >> 32);
}

static uint64_t lane(const V128 *v, int bits, int i) {
    if (bits == 16) { uint16_t n; memcpy(&n, v->b + 2 * i, sizeof n); return n; }
    uint32_t n; memcpy(&n, v->b + 4 * i, sizeof n); return n;
}

static void put(V128 *v, int bits, int i, uint64_t n) {
    if (bits == 16) { uint16_t x = (uint16_t)n; memcpy(v->b + 2 * i, &x, sizeof x); }
    else { uint32_t x = (uint32_t)n; memcpy(v->b + 4 * i, &x, sizeof x); }
}

static int64_t signed_lane(uint64_t n, int bits) {
    uint64_t sign = UINT64_C(1) << (bits - 1), modulus = sign * 2;
    n &= modulus - 1;
    return n & sign ? (int64_t)n - (int64_t)modulus : (int64_t)n;
}

static uint64_t reference(uint64_t a, uint64_t b, int bits, int round) {
    int64_t divisor = INT64_C(1) << (bits - 1);
    int64_t numerator = signed_lane(a, bits) * signed_lane(b, bits);
    if (round) numerator += divisor / 2;
    /* C division truncates toward zero; the instruction needs floor. */
    int64_t result = numerator / divisor;
    if (numerator < 0 && numerator % divisor) --result;
    if (result >= divisor) result = divisor - 1;
    if (result < -divisor) result = -divisor;
    return (uint64_t)result & ((uint64_t)divisor * 2 - 1);
}

static void check(const V128 *input, uint64_t multiplier, int bits, int count, int round, int alias) {
    V128 source = *input, before = source, result, want = {{0}};
    memset(&result, 0xa5, sizeof result);
    for (int i = 0; i < count; ++i) put(&want, bits, i, reference(lane(input, bits, i), multiplier, bits, round));
    V128 *dest = alias ? &source : &result;
    v_ielem(dest, &source, multiplier, 3 + round, bits == 16 ? 1 : 2, count);
    ++cases;
    if (memcmp(dest, &want, sizeof want) || (!alias && memcmp(&source, &before, sizeof source))) {
        fprintf(stderr, "%s bits=%d count=%d alias=%d multiplier=%08" PRIx64 " case=%lu\n",
                round ? "SQRDMULH" : "SQDMULH", bits, count, alias, multiplier, cases);
        for (int i = 0; i < 128 / bits; ++i)
            fprintf(stderr, " lane %d input=%08" PRIx64 " got=%08" PRIx64 " want=%08" PRIx64 "\n",
                    i, lane(input, bits, i), lane(dest, bits, i), lane(&want, bits, i));
        exit(1);
    }
}

static void literal_fixtures(void) {
    static const struct { int bits; uint64_t a, b, trunc, rounded; } fixtures[] = {
        {32, 0x80000000, 0x80000000, 0x7fffffff, 0x7fffffff},
        {16, 0x8000, 0x8000, 0x7fff, 0x7fff},
        {32, 0x40000000, 0x40000000, 0x20000000, 0x20000000},
        {16, 0x4000, 0x4000, 0x2000, 0x2000},
        {32, 1, 0x40000000, 0, 1},
        {16, 1, 0x4000, 0, 1},
        {32, 0xffffffff, 0x40000000, 0xffffffff, 0},
        {16, 0xffff, 0x4000, 0xffff, 0},
        {32, 0x80000000, 0x7fffffff, 0x80000001, 0x80000001},
        {16, 0x8000, 0x7fff, 0x8001, 0x8001},
        {32, 0x7fffffff, 0x7fffffff, 0x7ffffffe, 0x7ffffffe},
        {16, 0x7fff, 0x7fff, 0x7ffe, 0x7ffe},
    };
    for (size_t i = 0; i < sizeof fixtures / sizeof *fixtures; ++i) {
        for (int round = 0; round <= 1; ++round) {
            uint64_t expected = round ? fixtures[i].rounded : fixtures[i].trunc;
            if (reference(fixtures[i].a, fixtures[i].b, fixtures[i].bits, round) != expected) {
                fprintf(stderr, "invalid arithmetic oracle at literal fixture %zu\n", i);
                exit(1);
            }
            V128 input; memset(&input, 0x5a, sizeof input);
            put(&input, fixtures[i].bits, 0, fixtures[i].a);
            check(&input, fixtures[i].b, fixtures[i].bits, 1, round, 0);
            check(&input, fixtures[i].b, fixtures[i].bits, 1, round, 1);
        }
    }
}

int main(void) {
    literal_fixtures(); /* First case reproduces the original signed overflow. */
    for (int bits = 16; bits <= 32; bits *= 2) {
        uint64_t sign = UINT64_C(1) << (bits - 1), mask = sign * 2 - 1;
        uint64_t edges[] = {0, 1, 2, sign / 2 - 1, sign / 2, sign / 2 + 1,
                            sign - 1, sign, sign + 1, mask - 1, mask};
        int counts[] = {1, 64 / bits, 128 / bits};
        for (int k = 0; k < 3; ++k) {
            for (int round = 0; round <= 1; ++round) {
                for (size_t a = 0; a < sizeof edges / sizeof *edges; ++a) {
                    for (size_t b = 0; b < sizeof edges / sizeof *edges; ++b) {
                        V128 input;
                        for (int i = 0; i < 128 / bits; ++i) put(&input, bits, i, edges[a]);
                        check(&input, edges[b], bits, counts[k], round, 0);
                        check(&input, edges[b], bits, counts[k], round, 1);
                    }
                }
                for (int j = 0; j < 10000; ++j) {
                    V128 input;
                    for (int i = 0; i < 128 / bits; ++i) put(&input, bits, i, random32());
                    uint64_t multiplier = random32() & mask;
                    check(&input, multiplier, bits, counts[k], round, 0);
                    check(&input, multiplier, bits, counts[k], round, 1);
                    /* The selected multiplier may come from the destination/source register. */
                    check(&input, lane(&input, bits, j % (128 / bits)), bits, counts[k], round, 1);
                }
            }
        }
    }
    printf("sqdmulh_test: %lu exact cases passed (16/32-bit, scalar/64/128-bit vectors, aliases, clearing)\n", cases);
    return 0;
}
