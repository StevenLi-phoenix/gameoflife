#ifndef LIFE_H
#define LIFE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Life Life;

typedef struct {
    int x, y;
    uint8_t state; /* 0 dead, 1 alive, 2 toggle */
} LifeEdit;

Life*     life_create(int width, int height);
void      life_destroy(Life *life);

void      life_start(Life *life);
void      life_stop(Life *life);

void      life_set_paused(Life *life, bool paused);
bool      life_is_paused(const Life *life);
void      life_set_target_tps(Life *life, double tps); /* 0 = uncapped */
double    life_get_target_tps(const Life *life);
double    life_get_actual_tps(const Life *life);
long long life_get_generation(const Life *life);
long long life_get_alive_count(const Life *life);

void      life_request_randomize(Life *life, double density, unsigned int seed);
void      life_request_clear(Life *life);
void      life_request_step(Life *life);
void      life_queue_edit(Life *life, int x, int y, uint8_t state);

int       life_width(const Life *life);
int       life_height(const Life *life);

/* Hold the lock while reading the grid pointer. Sim swaps are blocked in between. */
void              life_lock(Life *life);
void              life_unlock(Life *life);
const uint8_t*    life_read_grid(const Life *life);

#endif
