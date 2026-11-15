/* Run twice with -DWT_NATIVE_INT_SIMD=0 and =1. The portable compiler-vector
 * backend can be tested on the host; Android code generation is checked by
 * native_integer_simd_codegen_test.py. No game files or graphics device needed. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

static unsigned long cases;
static uint64_t rng = UINT64_C(0xbe3104d72a895cf6);

static uint64_t random64(void) {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * UINT64_C(0x2545f4914f6cdd1d);
}

/* Build expectations from bytes and unsigned arithmetic, independent of the
 * runtime's lane accessors, sign extension and compiler vector operations. */
static uint64_t read_lane(const V128 *v, int size, int i) {
    uint64_t value = 0;
    for (int b = 0; b < size; ++b) value |= (uint64_t)v->b[i * size + b] << (8 * b);
    return value;
}

static uint64_t result_lane(uint64_t a, uint64_t b, int bits, int op) {
    uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t sign = UINT64_C(1) << (bits - 1);
    switch (op) {
    case I_ADD: return (a + b) & mask;
    case I_SUB: return (a - b) & mask;
    case I_MUL: return (a * b) & mask;
    case I_CMEQ: return a == b ? mask : 0;
    case I_CMGT: return (a ^ sign) > (b ^ sign) ? mask : 0;
    case I_CMGE: return (a ^ sign) >= (b ^ sign) ? mask : 0;
    case I_CMHI: return a > b ? mask : 0;
    case I_CMHS: return a >= b ? mask : 0;
    case I_CMTST: return (a & b) != 0 ? mask : 0;
    default: abort();
    }
}

static void reference(V128 *out, const V128 *a, const V128 *b, int sz, int count, int op) {
    memset(out, 0, sizeof *out);
    int size = 1 << sz;
    for (int i = 0; i < count; ++i) {
        uint64_t value = result_lane(read_lane(a, size, i), read_lane(b, size, i), size * 8, op);
        for (int j = 0; j < size; ++j) out->b[i * size + j] = (uint8_t)(value >> (8 * j));
    }
}

typedef struct { uint64_t before; V128 value; uint64_t after; } Guarded;

static void check(const V128 *a, const V128 *b, int sz, int count, int op, int alias) {
    Guarded storage[3];
    for (int i = 0; i < 3; ++i) {
        storage[i].before = UINT64_C(0x31a4eb6d9c7205f8);
        storage[i].after = UINT64_C(0x8d09f4c32e51b6a7);
        memset(&storage[i].value, 0xa5, sizeof(V128));
    }
    storage[0].value = *a; storage[1].value = *b;
    const V128 *left = &storage[0].value;
    const V128 *right = alias >= 3 ? left : &storage[1].value;
    V128 *out = alias == 1 || alias == 4 ? &storage[0].value
               : alias == 2 ? &storage[1].value : &storage[2].value;
    V128 expected, before[3] = {storage[0].value, storage[1].value, storage[2].value};
    reference(&expected, left, right, sz, count, op);
    v_i3v(out, left, right, op, sz, count);
    ++cases;
    if (memcmp(out, &expected, sizeof expected)) {
        fprintf(stderr, "integer SIMD mismatch sz=%d count=%d op=%d alias=%d case=%lu\n",
                sz, count, op, alias, cases);
        for (int i = 0; i < 16; ++i)
            fprintf(stderr, "%02x/%02x%s", out->b[i], expected.b[i], i == 15 ? "\n" : " ");
        exit(1);
    }
    for (int i = 0; i < 3; ++i) {
        if (storage[i].before != UINT64_C(0x31a4eb6d9c7205f8) ||
            storage[i].after != UINT64_C(0x8d09f4c32e51b6a7) ||
            (out != &storage[i].value && memcmp(&storage[i].value, &before[i], sizeof(V128)))) {
            fprintf(stderr, "integer SIMD overwrote source/guard sz=%d count=%d op=%d alias=%d\n",
                    sz, count, op, alias);
            exit(1);
        }
    }
}

static void fill_lanes(V128 *v, int sz, uint64_t value) {
    int size = 1 << sz;
    for (int i = 0; i < 16 / size; ++i)
        for (int j = 0; j < size; ++j) v->b[i * size + j] = (uint8_t)(value >> (j * 8));
}

int main(void) {
    for (int sz = 0; sz < 4; ++sz) {
        int bits = 8 << sz, counts[] = {1, 8 >> sz, 16 >> sz};
        uint64_t sign = UINT64_C(1) << (bits - 1), mask = sz == 3 ? UINT64_MAX : sign * 2 - 1;
        uint64_t edges[] = {0, 1, 2, sign - 1, sign, sign + 1, mask - 1, mask};
        for (int op = I_ADD; op <= I_CMTST; ++op) {
            if (sz == 3 && op == I_MUL) continue; /* No AArch64 vector MUL of 64-bit lanes. */
            for (int k = 0; k < 3; ++k) {
                for (size_t i = 0; i < sizeof edges / sizeof *edges; ++i) {
                    for (size_t j = 0; j < sizeof edges / sizeof *edges; ++j) {
                        V128 a, b;
                        fill_lanes(&a, sz, edges[i]); fill_lanes(&b, sz, edges[j]);
                        for (int alias = 0; alias < 5; ++alias) check(&a, &b, sz, counts[k], op, alias);
                    }
                }
                for (int i = 0; i < 2000; ++i) {
                    V128 a, b;
                    a.d[0] = random64(); a.d[1] = random64();
                    b.d[0] = random64(); b.d[1] = random64();
                    for (int alias = 0; alias < 5; ++alias) check(&a, &b, sz, counts[k], op, alias);
                }
            }
        }
    }
    printf("native_integer_simd_test: %lu exact cases passed (integer ops, all sizes, aliases, clearing, guards)\n", cases);
    return 0;
}
