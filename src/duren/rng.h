#ifndef DUREN_RNG_H
#define DUREN_RNG_H
#include <stdint.h>
void duren_srand(unsigned int seed);
int duren_rand(void);
uint32_t duren_rng_get_state(void);
void duren_rng_set_state(uint32_t state);
#endif
