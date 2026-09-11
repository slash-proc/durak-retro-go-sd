#include "rng.h"
static uint32_t g_rng_state = 0x6D2B79F5u;
void duren_srand(unsigned int seed) { g_rng_state = seed ? (uint32_t)seed : 0x6D2B79F5u; }
int duren_rand(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x ? x : 0x6D2B79F5u;
    return (int)(g_rng_state & 0x7FFFFFFFu);
}
uint32_t duren_rng_get_state(void) { return g_rng_state; }
void duren_rng_set_state(uint32_t state) { g_rng_state = state ? state : 0x6D2B79F5u; }
