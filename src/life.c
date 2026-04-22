#define _POSIX_C_SOURCE 200809L
#include "life.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

struct Life {
    int        width, height;
    size_t     cells;

    uint8_t   *buf_a;
    uint8_t   *buf_b;
    uint8_t   *read_buf;   /* readable (latest complete) */
    uint8_t   *work_buf;   /* sim writes here */

    pthread_mutex_t swap_mtx;

    pthread_t  thread;
    atomic_bool running;
    atomic_bool paused;
    atomic_int  step_once;

    _Atomic double target_tps;
    _Atomic double actual_tps;
    atomic_llong   generation;
    atomic_llong   alive_count;

    atomic_int request_randomize;
    atomic_int request_clear;
    pthread_mutex_t req_mtx;
    double        pending_density;
    unsigned int  pending_seed;

    pthread_mutex_t edit_mtx;
    LifeEdit  *edits;
    int        edits_count;
    int        edits_cap;
};

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ns(long long ns) {
    if (ns <= 0) return;
    struct timespec ts = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
    nanosleep(&ts, NULL);
}

/* Core per-tick update. Edges clip to dead. */
static long long tick_kernel(const uint8_t * __restrict src,
                             uint8_t * __restrict dst,
                             int w, int h) {
    long long alive = 0;

    memset(dst, 0, (size_t)w);
    memset(dst + (size_t)(h - 1) * w, 0, (size_t)w);

    for (int y = 1; y < h - 1; y++) {
        const uint8_t *p = src + (size_t)(y - 1) * w;
        const uint8_t *c = src + (size_t) y      * w;
        const uint8_t *n = src + (size_t)(y + 1) * w;
        uint8_t       *d = dst + (size_t) y      * w;

        d[0]     = 0;
        d[w - 1] = 0;

        /* Rolling column sums: sL = col x-1, sM = col x, sR = col x+1. */
        int sL = p[0] + c[0] + n[0];
        int sM = p[1] + c[1] + n[1];

        for (int x = 1; x < w - 1; x++) {
            int sR = p[x + 1] + c[x + 1] + n[x + 1];
            int sum3 = sL + sM + sR;
            int self = c[x];
            /* neighbors = sum3 - self  (in {0..8}) */
            int nb = sum3 - self;
            uint8_t live = (uint8_t)((nb == 3) | ((nb == 2) & self));
            d[x] = live;
            alive += live;
            sL = sM;
            sM = sR;
        }
    }
    return alive;
}

static void apply_edits_locked(Life *life, uint8_t *buf) {
    pthread_mutex_lock(&life->edit_mtx);
    for (int i = 0; i < life->edits_count; i++) {
        LifeEdit e = life->edits[i];
        if ((unsigned)e.x >= (unsigned)life->width ||
            (unsigned)e.y >= (unsigned)life->height) continue;
        size_t idx = (size_t)e.y * life->width + e.x;
        if (e.state == 2) buf[idx] = !buf[idx];
        else              buf[idx] = e.state ? 1 : 0;
    }
    life->edits_count = 0;
    pthread_mutex_unlock(&life->edit_mtx);
}

static void apply_requests_locked(Life *life) {
    if (atomic_exchange(&life->request_clear, 0)) {
        memset(life->read_buf, 0, life->cells);
        atomic_store(&life->alive_count, 0);
    }
    if (atomic_exchange(&life->request_randomize, 0)) {
        pthread_mutex_lock(&life->req_mtx);
        double density = life->pending_density;
        unsigned int seed = life->pending_seed;
        pthread_mutex_unlock(&life->req_mtx);

        unsigned int s = seed;
        int thresh = (int)(density * (double)0x7fffffff);
        long long alive = 0;
        for (size_t i = 0; i < life->cells; i++) {
            /* xorshift32 inline RNG, fast and deterministic */
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int r = (int)(s & 0x7fffffff);
            uint8_t v = (r < thresh) ? 1 : 0;
            life->read_buf[i] = v;
            alive += v;
        }
        atomic_store(&life->alive_count, alive);
    }
}

static void* sim_main(void *arg) {
    Life *life = (Life*)arg;

    long long stats_t    = now_ns();
    long long stats_gen  = atomic_load(&life->generation);

    while (atomic_load(&life->running)) {
        /* Process edits/requests on the readable buffer. Done under swap_mtx so
           renderer snapshots always see a consistent grid. */
        pthread_mutex_lock(&life->swap_mtx);
        apply_requests_locked(life);
        apply_edits_locked(life, life->read_buf);
        pthread_mutex_unlock(&life->swap_mtx);

        bool step = !atomic_load(&life->paused) ||
                    atomic_exchange(&life->step_once, 0);

        if (!step) {
            sleep_ns(5 * 1000 * 1000); /* 5 ms idle */
            continue;
        }

        long long t0 = now_ns();
        long long alive = tick_kernel(life->read_buf, life->work_buf,
                                      life->width, life->height);

        pthread_mutex_lock(&life->swap_mtx);
        uint8_t *tmp = life->read_buf;
        life->read_buf = life->work_buf;
        life->work_buf = tmp;
        pthread_mutex_unlock(&life->swap_mtx);

        atomic_fetch_add(&life->generation, 1);
        atomic_store(&life->alive_count, alive);

        /* TPS cap */
        double tps = atomic_load(&life->target_tps);
        if (tps > 0.0) {
            long long target = (long long)(1.0e9 / tps);
            long long elapsed = now_ns() - t0;
            if (elapsed < target) sleep_ns(target - elapsed);
        }

        /* Stats */
        long long nt = now_ns();
        if (nt - stats_t >= 250 * 1000 * 1000LL) {
            long long gen_now = atomic_load(&life->generation);
            double actual = (double)(gen_now - stats_gen) * 1.0e9 /
                            (double)(nt - stats_t);
            atomic_store(&life->actual_tps, actual);
            stats_t   = nt;
            stats_gen = gen_now;
        }
    }
    return NULL;
}

Life *life_create(int width, int height) {
    if (width < 3 || height < 3) return NULL;
    Life *life = calloc(1, sizeof *life);
    if (!life) return NULL;
    life->width  = width;
    life->height = height;
    life->cells  = (size_t)width * (size_t)height;

    life->buf_a = calloc(life->cells, 1);
    life->buf_b = calloc(life->cells, 1);
    if (!life->buf_a || !life->buf_b) {
        free(life->buf_a); free(life->buf_b); free(life);
        return NULL;
    }
    life->read_buf = life->buf_a;
    life->work_buf = life->buf_b;

    pthread_mutex_init(&life->swap_mtx, NULL);
    pthread_mutex_init(&life->edit_mtx, NULL);
    pthread_mutex_init(&life->req_mtx,  NULL);

    atomic_init(&life->running, false);
    atomic_init(&life->paused,  false);
    atomic_init(&life->step_once, 0);
    atomic_init(&life->target_tps, 0.0);
    atomic_init(&life->actual_tps, 0.0);
    atomic_init(&life->generation, 0);
    atomic_init(&life->alive_count, 0);
    atomic_init(&life->request_randomize, 0);
    atomic_init(&life->request_clear, 0);

    life->edits_cap = 4096;
    life->edits     = malloc(sizeof(LifeEdit) * (size_t)life->edits_cap);
    life->edits_count = 0;

    return life;
}

void life_destroy(Life *life) {
    if (!life) return;
    life_stop(life);
    pthread_mutex_destroy(&life->swap_mtx);
    pthread_mutex_destroy(&life->edit_mtx);
    pthread_mutex_destroy(&life->req_mtx);
    free(life->buf_a);
    free(life->buf_b);
    free(life->edits);
    free(life);
}

void life_start(Life *life) {
    if (atomic_load(&life->running)) return;
    atomic_store(&life->running, true);
    pthread_create(&life->thread, NULL, sim_main, life);
}
void life_stop(Life *life) {
    if (!atomic_load(&life->running)) return;
    atomic_store(&life->running, false);
    pthread_join(life->thread, NULL);
}

void   life_set_paused(Life *life, bool p)     { atomic_store(&life->paused, p); }
bool   life_is_paused(const Life *life)        { return atomic_load(&((Life*)life)->paused); }
void   life_set_target_tps(Life *life, double t) { atomic_store(&life->target_tps, t); }
double life_get_target_tps(const Life *life)   { return atomic_load(&((Life*)life)->target_tps); }
double life_get_actual_tps(const Life *life)   { return atomic_load(&((Life*)life)->actual_tps); }
long long life_get_generation(const Life *life){ return atomic_load(&((Life*)life)->generation); }
long long life_get_alive_count(const Life *life){ return atomic_load(&((Life*)life)->alive_count); }

void life_request_randomize(Life *life, double density, unsigned int seed) {
    if (seed == 0) seed = 0x12345678u;
    pthread_mutex_lock(&life->req_mtx);
    life->pending_density = density;
    life->pending_seed    = seed;
    pthread_mutex_unlock(&life->req_mtx);
    atomic_store(&life->request_randomize, 1);
}
void life_request_clear(Life *life) { atomic_store(&life->request_clear, 1); }
void life_request_step(Life *life)  { atomic_store(&life->step_once, 1); }

void life_queue_edit(Life *life, int x, int y, uint8_t state) {
    pthread_mutex_lock(&life->edit_mtx);
    if (life->edits_count == life->edits_cap) {
        life->edits_cap *= 2;
        life->edits = realloc(life->edits, sizeof(LifeEdit) * (size_t)life->edits_cap);
    }
    life->edits[life->edits_count++] = (LifeEdit){ x, y, state };
    pthread_mutex_unlock(&life->edit_mtx);
}

int life_width(const Life *l)  { return l->width; }
int life_height(const Life *l) { return l->height; }
void life_lock(Life *l)        { pthread_mutex_lock(&l->swap_mtx); }
void life_unlock(Life *l)      { pthread_mutex_unlock(&l->swap_mtx); }
const uint8_t* life_read_grid(const Life *l) { return l->read_buf; }
