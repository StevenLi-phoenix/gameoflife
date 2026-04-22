#include "renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline int floor_div_i(int a, int b) {
    int q = a / b;
    int r = a - q * b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}
static inline int ceil_div_i(int a, int b) {
    int q = a / b;
    int r = a - q * b;
    if (r != 0 && ((r > 0) == (b > 0))) q++;
    return q;
}
static void ensure_texture(Renderer *r, int img_w, int img_h) {
    if (img_w == r->img_w && img_h == r->img_h && r->tex.id != 0) return;
    if (r->tex.id != 0) { UnloadTexture(r->tex); r->tex.id = 0; }
    free(r->pixels);
    r->pixels = calloc((size_t)img_w * (size_t)img_h, 1);
    r->img_w  = img_w;
    r->img_h  = img_h;
    Image img = {
        .data    = r->pixels,
        .width   = img_w,
        .height  = img_h,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE,
    };
    r->tex = LoadTextureFromImage(img);
    SetTextureFilter(r->tex, TEXTURE_FILTER_POINT);
}

Renderer *renderer_create(Life *life, int w, int h) {
    Renderer *r = calloc(1, sizeof *r);
    r->life     = life;
    r->screen_w = w;
    r->screen_h = h;

    /* Center on grid, fit the grid to screen. */
    int gw = life_width(life), gh = life_height(life);
    r->view_x = gw * 0.5;
    r->view_y = gh * 0.5;
    double zx = (double)w / (double)gw;
    double zy = (double)h / (double)gh;
    r->zoom = (zx < zy ? zx : zy) * 0.95;

    r->last_lod        = -1;
    r->draw_grid_lines = true;

    return r;
}

void renderer_destroy(Renderer *r) {
    if (!r) return;
    if (r->tex.id != 0) UnloadTexture(r->tex);
    free(r->pixels);
    free(r);
}

void renderer_resize(Renderer *r, int w, int h) {
    r->screen_w = w;
    r->screen_h = h;
}

static int compute_lod(double zoom) {
    /* cells per pixel = 1/zoom. pick smallest lod with block >= cpp. */
    double cpp = 1.0 / zoom;
    int lod = 0;
    while ((1 << lod) < cpp && lod < 12) lod++;
    return lod;
}

void renderer_update(Renderer *r, float dt) {
    /* Tutorial overlay swallows input except dismiss. */
    if (r->show_tutorial) {
        if (IsKeyPressed(KEY_ENTER)    || IsKeyPressed(KEY_KP_ENTER) ||
            IsKeyPressed(KEY_SPACE)    || IsKeyPressed(KEY_TAB)      ||
            IsKeyPressed(KEY_H)        || IsKeyPressed(KEY_F1)       ||
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            r->show_tutorial = false;
            if (r->on_tutorial_dismiss) r->on_tutorial_dismiss(r->tutorial_ud);
        }
        return;
    }
    /* Reopen with H or F1. */
    if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_F1)) {
        r->show_tutorial = true;
        return;
    }

    /* Keyboard pan: cells-per-second scales inversely with zoom to keep feel. */
    double pan_speed_px = 600.0;
    double pan_speed_cells = pan_speed_px / r->zoom * dt;
    double boost = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 3.0 : 1.0;
    pan_speed_cells *= boost;

    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    r->view_y -= pan_speed_cells;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  r->view_y += pan_speed_cells;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  r->view_x -= pan_speed_cells;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) r->view_x += pan_speed_cells;

    /* Zoom, anchored at mouse. */
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        Vector2 m = GetMousePosition();
        double mwx_b = r->view_x + (m.x - r->screen_w * 0.5) / r->zoom;
        double mwy_b = r->view_y + (m.y - r->screen_h * 0.5) / r->zoom;

        double factor = pow(1.2, (double)wheel);
        r->zoom *= factor;
        if (r->zoom < 1.0 / 2048.0) r->zoom = 1.0 / 2048.0;
        if (r->zoom > 64.0)         r->zoom = 64.0;

        double mwx_a = r->view_x + (m.x - r->screen_w * 0.5) / r->zoom;
        double mwy_a = r->view_y + (m.y - r->screen_h * 0.5) / r->zoom;
        r->view_x += mwx_b - mwx_a;
        r->view_y += mwy_b - mwy_a;
    }

    /* Middle-button drag pan. */
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
        r->panning = true;
        r->pan_anchor_mouse = GetMousePosition();
        r->pan_anchor_view_x = r->view_x;
        r->pan_anchor_view_y = r->view_y;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE)) r->panning = false;
    if (r->panning) {
        Vector2 m = GetMousePosition();
        r->view_x = r->pan_anchor_view_x - (m.x - r->pan_anchor_mouse.x) / r->zoom;
        r->view_y = r->pan_anchor_view_y - (m.y - r->pan_anchor_mouse.y) / r->zoom;
    }

    /* Sim control keys. */
    if (IsKeyPressed(KEY_SPACE))  life_set_paused(r->life, !life_is_paused(r->life));
    if (IsKeyPressed(KEY_PERIOD)) life_request_step(r->life);
    if (IsKeyPressed(KEY_R))      life_request_randomize(r->life, 0.30, (unsigned)time(NULL));
    if (IsKeyPressed(KEY_C))      life_request_clear(r->life);
    if (IsKeyPressed(KEY_G))      r->draw_grid_lines = !r->draw_grid_lines;

    /* TPS adjust: '[' slower, ']' faster, '0' uncap, '1' set 60, '2' set 240. */
    if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_MINUS)) {
        double t = life_get_target_tps(r->life);
        if (t <= 0) t = 240.0; else t /= 1.5;
        if (t < 1.0) t = 1.0;
        life_set_target_tps(r->life, t);
    }
    if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_EQUAL)) {
        double t = life_get_target_tps(r->life);
        if (t <= 0) t = 120.0; else t *= 1.5;
        if (t > 10000.0) t = 0.0; /* uncap */
        life_set_target_tps(r->life, t);
    }
    if (IsKeyPressed(KEY_ZERO)) life_set_target_tps(r->life, 0.0);
    if (IsKeyPressed(KEY_ONE))  life_set_target_tps(r->life, 60.0);
    if (IsKeyPressed(KEY_TWO))  life_set_target_tps(r->life, 240.0);

    /* Painting: left adds, right erases. Only when zoomed in enough. */
    Vector2 m = GetMousePosition();
    bool hovering_window = m.x >= 0 && m.y >= 0 &&
                           m.x < r->screen_w && m.y < r->screen_h;
    if (hovering_window && r->zoom >= 2.0) {
        double wx = r->view_x + (m.x - r->screen_w * 0.5) / r->zoom;
        double wy = r->view_y + (m.y - r->screen_h * 0.5) / r->zoom;
        int cx = (int)floor(wx);
        int cy = (int)floor(wy);

        bool start_l = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        bool start_r = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        if (start_l || start_r) {
            if (!r->painting) {
                r->pre_paint_paused = life_is_paused(r->life);
                life_set_paused(r->life, true);
            }
            r->painting = true;
            r->paint_state = start_l ? 1 : 0;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
            if (r->painting) life_set_paused(r->life, r->pre_paint_paused);
            r->painting = false;
        }

        if (r->painting) life_queue_edit(r->life, cx, cy, r->paint_state);
    } else {
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
            if (r->painting) life_set_paused(r->life, r->pre_paint_paused);
            r->painting = false;
        }
    }
}

static void fill_lod0(Renderer *r, const uint8_t *grid, int W, int H,
                      int cx0, int cy0) {
    uint8_t *px = r->pixels;
    int iw = r->img_w, ih = r->img_h;
    for (int py = 0; py < ih; py++) {
        int cy = cy0 + py;
        uint8_t *out = px + (size_t)py * iw;
        if (cy < 0 || cy >= H) { memset(out, 0, (size_t)iw); continue; }
        const uint8_t *row = grid + (size_t)cy * W;
        for (int x = 0; x < iw; x++) {
            int cx = cx0 + x;
            out[x] = (cx >= 0 && cx < W && row[cx]) ? 255 : 0;
        }
    }
}

static void fill_lodn(Renderer *r, const uint8_t *grid, int W, int H,
                      int cx0, int cy0, int block) {
    uint8_t *px = r->pixels;
    int iw = r->img_w, ih = r->img_h;
    int area = block * block;
    int scale_num = 255;
    for (int py = 0; py < ih; py++) {
        int by0 = cy0 + py * block;
        int by1 = by0 + block;
        int cby0 = by0 < 0 ? 0 : by0;
        int cby1 = by1 > H ? H : by1;
        uint8_t *out = px + (size_t)py * iw;
        if (cby0 >= cby1) { memset(out, 0, (size_t)iw); continue; }
        for (int x = 0; x < iw; x++) {
            int bx0 = cx0 + x * block;
            int bx1 = bx0 + block;
            int cbx0 = bx0 < 0 ? 0 : bx0;
            int cbx1 = bx1 > W ? W : bx1;
            if (cbx0 >= cbx1) { out[x] = 0; continue; }
            int sum = 0;
            for (int cy = cby0; cy < cby1; cy++) {
                const uint8_t *row = grid + (size_t)cy * W + cbx0;
                int n = cbx1 - cbx0;
                for (int i = 0; i < n; i++) sum += row[i];
            }
            out[x] = (uint8_t)((sum * scale_num) / area);
        }
    }
}

void renderer_draw(Renderer *r) {
    double t_draw = GetTime();

    int sw = r->screen_w, sh = r->screen_h;
    int W  = life_width(r->life);
    int H  = life_height(r->life);

    /* Visible world bounds in cells. */
    double half_w = 0.5 * sw / r->zoom;
    double half_h = 0.5 * sh / r->zoom;
    double wx0 = r->view_x - half_w, wy0 = r->view_y - half_h;
    double wx1 = r->view_x + half_w, wy1 = r->view_y + half_h;

    int lod   = compute_lod(r->zoom);
    int block = 1 << lod;

    int cx0 = floor_div_i((int)floor(wx0), block) * block;
    int cy0 = floor_div_i((int)floor(wy0), block) * block;
    int cx1 = ceil_div_i ((int)ceil (wx1), block) * block;
    int cy1 = ceil_div_i ((int)ceil (wy1), block) * block;

    /* Skip degenerate cases. */
    bool any_visible = !(cx1 <= 0 || cy1 <= 0 || cx0 >= W || cy0 >= H);

    /* Cap texture dimensions to avoid absurd GPU uploads. */
    const int MAX_DIM = 4096;
    int cells_w = cx1 - cx0;
    int cells_h = cy1 - cy0;
    int img_w = cells_w / block;
    int img_h = cells_h / block;
    if (img_w > MAX_DIM) { img_w = MAX_DIM; cells_w = img_w * block; cx1 = cx0 + cells_w; }
    if (img_h > MAX_DIM) { img_h = MAX_DIM; cells_h = img_h * block; cy1 = cy0 + cells_h; }
    if (img_w < 1) img_w = 1;
    if (img_h < 1) img_h = 1;

    ensure_texture(r, img_w, img_h);

    if (any_visible) {
        life_lock(r->life);
        const uint8_t *grid = life_read_grid(r->life);
        if (lod == 0) fill_lod0(r, grid, W, H, cx0, cy0);
        else          fill_lodn(r, grid, W, H, cx0, cy0, block);
        life_unlock(r->life);
        UpdateTexture(r->tex, r->pixels);
    } else {
        memset(r->pixels, 0, (size_t)img_w * (size_t)img_h);
        UpdateTexture(r->tex, r->pixels);
    }

    BeginDrawing();
    ClearBackground((Color){ 10, 10, 14, 255 });

    /* Draw world bounds backdrop for context (grid extent). */
    {
        double gx = (0 - r->view_x) * r->zoom + sw * 0.5;
        double gy = (0 - r->view_y) * r->zoom + sh * 0.5;
        double gw = W * r->zoom;
        double gh_ = H * r->zoom;
        DrawRectangleLines((int)gx - 1, (int)gy - 1,
                           (int)gw + 2, (int)gh_ + 2,
                           (Color){ 60, 80, 120, 180 });
    }

    /* Draw LOD texture scaled to cover the aligned cell region. */
    if (any_visible) {
        double dx = (cx0 - r->view_x) * r->zoom + sw * 0.5;
        double dy = (cy0 - r->view_y) * r->zoom + sh * 0.5;
        double dw = cells_w * r->zoom;
        double dh = cells_h * r->zoom;

        Rectangle src = { 0, 0, (float)img_w, (float)img_h };
        Rectangle dst = { (float)dx, (float)dy, (float)dw, (float)dh };
        /* Tint cells pale cyan on black background. */
        DrawTexturePro(r->tex, src, dst, (Vector2){0, 0}, 0.0f,
                       (Color){ 230, 240, 255, 255 });
    }

    /* Grid lines only when zoomed way in. */
    if (r->draw_grid_lines && r->zoom >= 10.0) {
        Color gc = (Color){ 40, 50, 70, 180 };
        int cxs = (int)floor(wx0) - 1;
        int cxe = (int)ceil(wx1) + 1;
        int cys = (int)floor(wy0) - 1;
        int cye = (int)ceil(wy1) + 1;
        for (int cx = cxs; cx <= cxe; cx++) {
            float x = (float)((cx - r->view_x) * r->zoom + sw * 0.5);
            DrawLine((int)x, 0, (int)x, sh, gc);
        }
        for (int cy = cys; cy <= cye; cy++) {
            float y = (float)((cy - r->view_y) * r->zoom + sh * 0.5);
            DrawLine(0, (int)y, sw, (int)y, gc);
        }
    }

    /* Cursor highlight when painting range. */
    if (r->zoom >= 2.0) {
        Vector2 m = GetMousePosition();
        double wx = r->view_x + (m.x - sw * 0.5) / r->zoom;
        double wy = r->view_y + (m.y - sh * 0.5) / r->zoom;
        int cx = (int)floor(wx);
        int cy = (int)floor(wy);
        if (cx >= 0 && cx < W && cy >= 0 && cy < H) {
            float rx = (float)((cx - r->view_x) * r->zoom + sw * 0.5);
            float ry = (float)((cy - r->view_y) * r->zoom + sh * 0.5);
            DrawRectangleLines((int)rx, (int)ry,
                               (int)ceil(r->zoom), (int)ceil(r->zoom),
                               (Color){ 255, 200, 80, 200 });
        }
    }

    double render_ms = (GetTime() - t_draw) * 1000.0;
    r->render_ms_ema = r->render_ms_ema * 0.9 + render_ms * 0.1;

    /* HUD. */
    char hud[384];
    double t_tps  = life_get_target_tps(r->life);
    double a_tps  = life_get_actual_tps(r->life);
    long long gen = life_get_generation(r->life);
    long long alv = life_get_alive_count(r->life);
    double density = (double)alv / ((double)W * (double)H) * 100.0;

    snprintf(hud, sizeof hud,
        "FPS %3d   frame %.2fms   render %.2fms\n"
        "sim TPS %7.1f (target %s%.1f)   gen %lld\n"
        "alive %lld (%.2f%%)\n"
        "zoom %.3f px/cell   LOD %d (%d\xc3\x97%d block)\n"
        "grid %d\xc3\x97%d   view (%.1f,%.1f)   %s",
        GetFPS(),
        GetFrameTime() * 1000.0,
        r->render_ms_ema,
        a_tps, (t_tps <= 0 ? "uncapped " : ""), t_tps,
        gen,
        alv, density,
        r->zoom, lod, block, block,
        W, H, r->view_x, r->view_y,
        life_is_paused(r->life) ? "PAUSED" : "running");

    DrawRectangle(8, 8, 360, 108, (Color){ 0, 0, 0, 180 });
    DrawText(hud, 14, 12, 12, (Color){ 200, 220, 255, 255 });

    const char *help =
        "WASD/arrows pan  wheel zoom  MMB drag  space pause  . step    H help\n"
        "R randomize  C clear  [ ] tps  0/1/2 uncap/60/240  G grid  LMB paint  RMB erase";
    DrawRectangle(8, sh - 40, sw - 16, 32, (Color){ 0, 0, 0, 160 });
    DrawText(help, 14, sh - 36, 11, (Color){ 180, 190, 210, 230 });

    if (r->show_tutorial) {
        /* Dim backdrop. */
        DrawRectangle(0, 0, sw, sh, (Color){ 0, 0, 0, 190 });

        /* Centered panel. */
        const int pw = 560;
        const int ph = 420;
        int px = (sw - pw) / 2;
        int py = (sh - ph) / 2;
        DrawRectangle(px, py, pw, ph, (Color){ 18, 22, 32, 245 });
        DrawRectangleLines(px, py, pw, ph, (Color){ 90, 140, 200, 255 });
        DrawRectangleLines(px + 1, py + 1, pw - 2, ph - 2, (Color){ 50, 80, 130, 255 });

        int tx = px + 24;
        int ty = py + 20;
        DrawText("Game of Life", tx, ty, 28, (Color){ 240, 245, 255, 255 });
        ty += 34;
        char sub[128];
        snprintf(sub, sizeof sub,
                 "threaded sim + LOD render  -  %dx%d grid",
                 W, H);
        DrawText(sub, tx, ty, 14, (Color){ 150, 180, 220, 255 });
        ty += 26;

        DrawLine(tx, ty, px + pw - 24, ty, (Color){ 60, 90, 130, 200 });
        ty += 14;

        const int k_col = tx;        /* key column */
        const int d_col = tx + 180;  /* description column */
        const int fs    = 14;
        const int lh    = 18;

        #define ROW(label, desc) do { \
            DrawText((label), k_col, ty, fs, (Color){ 255, 210, 120, 255 }); \
            DrawText((desc),  d_col, ty, fs, (Color){ 220, 225, 235, 255 }); \
            ty += lh; \
        } while (0)
        #define SECTION(name) do { \
            ty += 4; \
            DrawText((name), k_col - 6, ty, fs, (Color){ 120, 200, 255, 255 }); \
            ty += lh; \
        } while (0)

        SECTION("VIEW");
        ROW("WASD / arrows",   "pan  (Shift = 3x faster)");
        ROW("mouse wheel",     "zoom around cursor");
        ROW("middle drag",     "drag to pan");

        SECTION("SIMULATION");
        ROW("Space",           "pause / resume");
        ROW(".",               "single step (while paused)");
        ROW("R / C",           "randomize / clear");
        ROW("[  ]",            "slower / faster tps");
        ROW("0 / 1 / 2",       "uncap / 60 / 240 tps");

        SECTION("EDIT  (zoom in past 2x)");
        ROW("left drag",       "paint alive   (auto-pauses)");
        ROW("right drag",      "paint dead");
        ROW("G",               "toggle grid lines");
        ROW("H / F1",          "reopen this help");

        #undef ROW
        #undef SECTION

        /* Footer CTA. */
        const char *cta = "press  Space  or click anywhere to begin";
        int cta_w = MeasureText(cta, 16);
        DrawText(cta, px + (pw - cta_w) / 2, py + ph - 32, 16,
                 (Color){ 255, 240, 180, 255 });
    }

    EndDrawing();
}
