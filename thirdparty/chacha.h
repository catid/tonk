#ifndef CHACHA_H
#define CHACHA_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct chacha_state_t {
    uint8_t opaque[128];
} chacha_state;

typedef struct chacha_key_t {
    uint8_t b[32];
} chacha_key;

typedef struct chacha_iv_t {
    uint8_t b[8];
} chacha_iv;

typedef struct chacha_iv24_t {
    uint8_t b[24];
} chacha_iv24;

void hchacha(const uint8_t key[32], const uint8_t iv[16], uint8_t out[32], size_t rounds);

void chacha_init(chacha_state *S, const chacha_key *key, const chacha_iv *iv, size_t rounds);
void xchacha_init(chacha_state *S, const chacha_key *key, const chacha_iv24 *iv, size_t rounds);
size_t chacha_update(chacha_state *S, const uint8_t *in, uint8_t *out, size_t inlen);
uint64_t chacha_get_counter(chacha_state *S);
void chacha_set_counter(chacha_state *S, uint64_t counter);
size_t chacha_final(chacha_state *S, uint8_t *out);

void chacha(const chacha_key *key, const chacha_iv *iv, const uint8_t *in, uint8_t *out, size_t inlen, size_t rounds);
void xchacha(const chacha_key *key, const chacha_iv24 *iv, const uint8_t *in, uint8_t *out, size_t inlen, size_t rounds);

int chacha_check_validity();

#if defined(__cplusplus)
}
#endif

#endif /* CHACHA_H */

