#ifndef RENDERER_H
#define RENDERER_H

#include "life.h"
#include <raylib.h>
#include <stdbool.h>

typedef struct {
    Life      *life;
    int        screen_w, screen_h;

    /* View: pixels-per-cell zoom, world-space center (cells, fractional). */
    double     view_x, view_y;
    double     zoom;

    /* LOD image: one-byte-per-texel density buffer, matching visible region. */
    uint8_t   *pixels;
    int        img_w, img_h;
    Texture2D  tex;
    int        last_lod;

    /* Input state. */
    bool       painting;
    uint8_t    paint_state;
    bool       pre_paint_paused; /* pause state before stroke began */
    bool       panning;
    Vector2    pan_anchor_mouse;
    double     pan_anchor_view_x, pan_anchor_view_y;

    /* Timing. */
    double     render_ms_ema;

    bool       draw_grid_lines;

    /* Tutorial / help overlay. */
    bool       show_tutorial;
    void     (*on_tutorial_dismiss)(void *ud);
    void      *tutorial_ud;
} Renderer;

Renderer* renderer_create(Life *life, int w, int h);
void      renderer_destroy(Renderer *r);
void      renderer_resize(Renderer *r, int w, int h);
void      renderer_update(Renderer *r, float dt);
void      renderer_draw(Renderer *r);

#endif
