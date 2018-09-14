#include <string.h>
#include "chacha.h"

enum chacha_constants {
    CHACHA_BLOCKBYTES = 64,
};

typedef struct chacha_state_internal_t {
    uint8_t s[48];
    size_t rounds;
    size_t leftover;
    uint8_t buffer[CHACHA_BLOCKBYTES];
} chacha_state_internal;

extern void chacha_blocks_ref(chacha_state_internal *state, const uint8_t *in, uint8_t *out, size_t bytes);
extern void hchacha_ref(const uint8_t key[32], const uint8_t iv[16], uint8_t out[32], size_t rounds);

/* is the pointer aligned on a word boundary? */
static int
chacha_is_aligned(const void *p) {
    return ((size_t)p & (sizeof(size_t) - 1)) == 0;
}

/* initialize the state */
void
chacha_init(chacha_state *S, const chacha_key *key, const chacha_iv *iv, size_t rounds) {
    chacha_state_internal *state = (chacha_state_internal *)S;
    memcpy(state->s + 0, key, 32);
    memset(state->s + 32, 0, 8);
    memcpy(state->s + 40, iv, 8);
    state->rounds = rounds;
    state->leftover = 0;
}

/* processes inlen bytes (can do partial blocks), handling input/ouput alignment */
static void
chacha_consume(chacha_state_internal *state, const uint8_t *in, uint8_t *out, size_t inlen) {
    uint8_t buffer[16 * CHACHA_BLOCKBYTES];
    int in_aligned, out_aligned;

    /* it's ok to call with 0 bytes */
    if (!inlen)
        return;

    /* if everything is aligned, handle directly */
    in_aligned = chacha_is_aligned(in);
    out_aligned = chacha_is_aligned(out);
    if (in_aligned && out_aligned) {
        chacha_blocks_ref(state, in, out, inlen);
        return;
    }

    /* copy the unaligned data to an aligned buffer and process in chunks */
    while (inlen) {
        const size_t bytes = (inlen > sizeof(buffer)) ? sizeof(buffer) : inlen;
        const uint8_t *src = in;
        uint8_t *dst = (out_aligned) ? out : buffer;
        if (!in_aligned) {
            memcpy(buffer, in, bytes);
            src = buffer;
        }
        chacha_blocks_ref(state, src, dst, bytes);
        if (!out_aligned)
            memcpy(out, buffer, bytes);
        if (in) in += bytes;
        out += bytes;
        inlen -= bytes;
    }
}


/* hchacha */
void
hchacha(const uint8_t key[32], const uint8_t iv[16], uint8_t out[32], size_t rounds) {
    hchacha_ref(key, iv, out, rounds);
}


/* update, returns number of bytes written to out */
size_t
chacha_update(chacha_state *S, const uint8_t *in, uint8_t *out, size_t inlen) {
    chacha_state_internal *state = (chacha_state_internal *)S;
    uint8_t *out_start = out;
    size_t bytes;

    /* enough for at least one block? */
    if ((state->leftover + inlen) >= CHACHA_BLOCKBYTES) {
        /* handle the previous data */
        if (state->leftover) {
            bytes = (CHACHA_BLOCKBYTES - state->leftover);
            if (in) {
                memcpy(state->buffer + state->leftover, in, bytes);
                in += bytes;
            }
            chacha_consume(state, (in) ? state->buffer : NULL, out, CHACHA_BLOCKBYTES);
            inlen -= bytes;
            out += CHACHA_BLOCKBYTES;
            state->leftover = 0;
        }

        /* handle the direct data */
        bytes = (inlen & ~(CHACHA_BLOCKBYTES - 1));
        if (bytes) {
            chacha_consume(state, in, out, bytes);
            inlen -= bytes;
            if (in) in += bytes;
            out += bytes;
        }
    }

    /* handle leftover data */
    if (inlen) {
        if (in) memcpy(state->buffer + state->leftover, in, inlen);
        else memset(state->buffer + state->leftover, 0, inlen);
        state->leftover += inlen;
    }

    return out - out_start;
}

/* returns the 64 bit counter (stored in little endian) */
uint64_t
chacha_get_counter(chacha_state *S) {
    chacha_state_internal *state = (chacha_state_internal *)S;
    return 
        ((uint64_t)state->s[32]      ) |
        ((uint64_t)state->s[33] <<  8) |
        ((uint64_t)state->s[34] << 16) |
        ((uint64_t)state->s[35] << 24) |
        ((uint64_t)state->s[36] << 32) |
        ((uint64_t)state->s[37] << 40) |
        ((uint64_t)state->s[38] << 48) |
        ((uint64_t)state->s[39] << 56);
}

/* writes the 64 bit counter (stored in little endian) */
void
chacha_set_counter(chacha_state *S, uint64_t counter) {
    chacha_state_internal *state = (chacha_state_internal *)S;
    state->s[32] = (counter      ) & 0xff;
    state->s[33] = (counter >>  8) & 0xff;
    state->s[34] = (counter >> 16) & 0xff;
    state->s[35] = (counter >> 24) & 0xff;
    state->s[36] = (counter >> 32) & 0xff;
    state->s[37] = (counter >> 40) & 0xff;
    state->s[38] = (counter >> 48) & 0xff;
    state->s[39] = (counter >> 56);
}

/* finalize, write out any leftover data */
size_t
chacha_final(chacha_state *S, uint8_t *out) {
    chacha_state_internal *state = (chacha_state_internal *)S;
    if (state->leftover) {
        if (chacha_is_aligned(out)) {
            chacha_blocks_ref(state, state->buffer, out, state->leftover);
        } else {
            chacha_blocks_ref(state, state->buffer, state->buffer, state->leftover);
            memcpy(out, state->buffer, state->leftover);
        }
    }
    memset(S, 0, sizeof(chacha_state));
    return state->leftover;
}

/* one-shot */
void
chacha(const chacha_key *key, const chacha_iv *iv, const uint8_t *in, uint8_t *out, size_t inlen, size_t rounds) {
    chacha_state S;
    chacha_init(&S, key, iv, rounds);
    chacha_consume((chacha_state_internal *)&S, in, out, inlen);
}

/*
    xchacha, chacha with a 192 bit nonce
*/

void
xchacha_init(chacha_state *S, const chacha_key *key, const chacha_iv24 *iv, size_t rounds) {
    chacha_key subkey;
    hchacha(key->b, iv->b, subkey.b, rounds);
    chacha_init(S, &subkey, (chacha_iv *)(iv->b + 16), rounds);
}

/* one-shot */
void
xchacha(const chacha_key *key, const chacha_iv24 *iv, const uint8_t *in, uint8_t *out, size_t inlen, size_t rounds) {
    chacha_state S;
    xchacha_init(&S, key, iv, rounds);
    chacha_consume((chacha_state_internal *)&S, in, out, inlen);
}


/*
    chacha/8 test
    key [32,33,34,..63]
    iv [128,129,130,131,132,133,134,135]
    ctr [0x00000000ffffffff]
*/

static size_t chacha_test_rounds = 8;

/* 2048 bytes, enough to trigger the 'fast path' on all implementations */
#define CHACHA_TEST_LEN 2048

/* first block of the test sequence */
static const uint8_t expected_chacha_first[CHACHA_BLOCKBYTES] = {
    0xe7,0x01,0x10,0x8e,0x9a,0x2f,0x24,0xc6,0xcd,0xbb,0x77,0x7e,0x4f,0xcf,0x10,0xae,
    0xd9,0x49,0x5a,0xa6,0x02,0x86,0x9d,0xf9,0x3b,0xb5,0xe2,0xc7,0xe6,0xbd,0xf7,0xf5,
    0x7c,0x9e,0x65,0x91,0x8b,0x95,0x43,0xb8,0xd3,0xcc,0x2f,0x97,0xb8,0xab,0x58,0xd2,
    0xe9,0x94,0x8a,0x4c,0xc6,0xb7,0x78,0x30,0xc2,0x1a,0xbf,0xb7,0xc8,0x8c,0xcd,0xf7,
};

/* xor of all the blocks from the test sequence */
static const uint8_t expected_chacha[CHACHA_BLOCKBYTES] = {
    0xb3,0x1c,0xfa,0xcc,0x9a,0x8e,0xa9,0x82,0x19,0x61,0x3f,0x91,0x04,0x72,0x8f,0x66,
    0x6e,0x3f,0x15,0x6d,0xfd,0x20,0x1c,0x40,0x69,0x36,0x8a,0xc2,0x25,0xd9,0x2d,0xcb,
    0x99,0xdd,0x71,0x46,0x33,0x70,0x15,0x05,0x68,0x29,0x65,0x01,0x15,0x24,0x6d,0x20,
    0xe2,0x63,0xea,0x73,0x60,0x0c,0x94,0xc6,0x7c,0x22,0xf0,0xf7,0x9e,0xa4,0xf8,0x34,
};

/*
    hchacha/8 test
    key [192,193,194,..223]
    iv [16,17,18,..31]
*/

static const uint8_t expected_hchacha[32] = {
    0xe6,0x19,0x0f,0x48,0xf1,0xc0,0x2a,0x68,0xb8,0xf2,0x2e,0xf8,0xbc,0xfd,0x41,0x06,
    0x7b,0xa9,0x36,0xf3,0x63,0x2f,0x5c,0x6d,0x40,0x39,0x24,0xb3,0x74,0x68,0xcb,0xdd,
};

/* initialize state, set the counter right below the 32 bit boundary */
static void
chacha_test_init_state(chacha_state *st, chacha_key *key, chacha_iv *iv) {
    chacha_init(st, key, iv, chacha_test_rounds);
    chacha_set_counter(st, 0x100000000ull - 1);
}

/* test 1..64 byte generation, will use slow single-block codepath, also partial block generation. out must have at least 64 bytes of space */
static int
chacha_test_oneblock(chacha_key *key, chacha_iv *iv, const uint8_t *in, uint8_t *out) {
    chacha_state st;
    size_t i;
    uint8_t *p;
    int res = 1;

    for (i = 1; i <= CHACHA_BLOCKBYTES; i++) {
        memset(out, 0, i);
        p = out;
        chacha_test_init_state(&st, key, iv);
        p += chacha_update(&st, in, p, i);
        chacha_final(&st, p);
        res &= (memcmp(expected_chacha_first, out, i) == 0) ? 1 : 0;
    }
    return res;
}

/* xor all the blocks together in to one block */
static void
chacha_test_compact_array(uint8_t *dst, const uint8_t *src, size_t srclen) {
    size_t blocks = srclen / CHACHA_BLOCKBYTES;
    size_t i, j;
    memset(dst, 0, CHACHA_BLOCKBYTES);
    for (i = 0; i < blocks; i++) {
        for (j = 0; j < CHACHA_BLOCKBYTES; j++)
            dst[j] ^= src[(i * CHACHA_BLOCKBYTES) + j];
    }
}

/* test CHACHA_TEST_LEN byte generation, will trigger efficient multi-block codepath. out must have at least CHACHA_TEST_LEN bytes of space */
static int
chacha_test_multiblock(chacha_key *key, chacha_iv *iv, const uint8_t *in, uint8_t *out) {
    chacha_state st;
    uint8_t final[CHACHA_BLOCKBYTES];
    uint8_t *p = out;

    memset(out, 0, CHACHA_TEST_LEN);
    chacha_test_init_state(&st, key, iv);
    p += chacha_update(&st, in, p, CHACHA_TEST_LEN);
    chacha_final(&st, p);
    chacha_test_compact_array(final, out, CHACHA_TEST_LEN);
    return (memcmp(expected_chacha, final, sizeof(expected_chacha)) == 0) ? 1 : 0;
}



/* incremental, test CHACHA_TEST_LEN byte generation, will trigger single/multi-block codepath. out must have at least CHACHA_TEST_LEN bytes of space */
static int
chacha_test_multiblock_incremental(chacha_key *key, chacha_iv *iv, const uint8_t *in, uint8_t *out) {
    chacha_state st;
    uint8_t final[CHACHA_BLOCKBYTES];
    size_t i, inc;
    uint8_t *p;
    int res = 1;

    for (inc = 1; inc < CHACHA_TEST_LEN; inc += 61) {
        p = out;
        memset(out, 0, CHACHA_TEST_LEN);
        chacha_test_init_state(&st, key, iv);
        for(i = 0; i <= CHACHA_TEST_LEN; i += inc)
            p += chacha_update(&st, (in) ? (in + i) : NULL, p, ((i + inc) > CHACHA_TEST_LEN) ? (CHACHA_TEST_LEN - i) : inc);
        chacha_final(&st, p);
        chacha_test_compact_array(final, out, CHACHA_TEST_LEN);
        res &= (memcmp(expected_chacha, final, sizeof(expected_chacha)) == 0) ? 1 : 0;
    }

    return res;
}

/* input_buffer is either NULL, or a buffer with at least (CHACHA_TEST_LEN+1) bytes which is initialized to {0} */
static int
chacha_test(const uint8_t *input_buffer) {
    chacha_key key;
    chacha_iv iv;
    uint8_t h_key[32];
    uint8_t h_iv[16];
    uint8_t out[CHACHA_TEST_LEN+sizeof(size_t)], final_hchacha[32];
    const uint8_t *in_aligned, *in_unaligned;
    uint8_t *out_aligned, *out_unaligned;
    size_t i;
    int res = 1;

    /*
        key [32,33,34,..63], iv [128,129,130,131,132,133,134,135]
    */
    for (i = 0; i < sizeof(key); i++) key.b[i] = (uint8_t)(i + 32);
    for (i = 0; i < sizeof(iv); i++) iv.b[i] = (uint8_t)(i + 128);

    in_aligned = input_buffer;
    in_unaligned = (input_buffer) ? (input_buffer + 1) : NULL;
    out_aligned = out;
    out_unaligned = out + 1;

    /* single block */
    res &= chacha_test_oneblock(&key, &iv,   in_aligned,   out_aligned);
    res &= chacha_test_oneblock(&key, &iv,   in_aligned, out_unaligned);
    if (input_buffer) {
        res &= chacha_test_oneblock(&key, &iv, in_unaligned,   out_aligned);
        res &= chacha_test_oneblock(&key, &iv, in_unaligned, out_unaligned);
    }

    /* multi */
    res &= chacha_test_multiblock(&key, &iv,   in_aligned,   out_aligned);
    res &= chacha_test_multiblock(&key, &iv,   in_aligned, out_unaligned);
    if (input_buffer) {
        res &= chacha_test_multiblock(&key, &iv, in_unaligned,   out_aligned);
        res &= chacha_test_multiblock(&key, &iv, in_unaligned, out_unaligned);
    }

    /* incremental */
    res &= chacha_test_multiblock_incremental(&key, &iv,   in_aligned,   out_aligned);
    res &= chacha_test_multiblock_incremental(&key, &iv,   in_aligned, out_unaligned);
    if (input_buffer) {
        res &= chacha_test_multiblock_incremental(&key, &iv, in_unaligned,   out_aligned);
        res &= chacha_test_multiblock_incremental(&key, &iv, in_unaligned, out_unaligned);
    }

    /*
        hchacha
        key [192,193,194,..223], iv [16,17,18,..31]
    */
    for (i = 0; i < sizeof(h_key); i++) h_key[i] = (uint8_t)(i + 192);
    for (i = 0; i < sizeof(h_iv); i++) h_iv[i] = (uint8_t)(i + 16);

    memset(final_hchacha, 0, sizeof(final_hchacha));
    hchacha(h_key, h_iv, final_hchacha, chacha_test_rounds);
    res &= (memcmp(expected_hchacha, final_hchacha, sizeof(expected_hchacha)) == 0) ? 1 : 0;

    return res;
}

/* returns 1 if underlying implementation is valid, 0 if not */
int
chacha_check_validity() {
    /* unaligned generation is tested, so buffer must have max+1 bytes */
    uint8_t zero_buffer[CHACHA_TEST_LEN+1] = {0};
    int res = 1;
    res &= chacha_test(zero_buffer); /* xors stream of random bytes with zero_buffer */
    res &= chacha_test(NULL); /* writes stream of random bytes directly */
    return res;
}

