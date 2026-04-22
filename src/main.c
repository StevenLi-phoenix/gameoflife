#include "life.h"
#include "renderer.h"
#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-w WIDTH] [-h HEIGHT] [--win WxH] [--tps N] [--density F] [--seed N]\n"
        "\n"
        "  -w, --width       grid width in cells   (default 2048)\n"
        "  -h, --height      grid height in cells  (default 2048)\n"
        "      --win         window size WxH       (default 1280x800)\n"
        "      --tps         target ticks/sec      (default 120, 0 = uncapped)\n"
        "      --density     initial alive ratio   (default 0.30)\n"
        "      --seed        RNG seed              (default time-based)\n"
        "      --tutorial    force first-run tutorial overlay on launch\n"
        "      --no-tutorial skip tutorial even on first run\n",
        argv0);
}

static void mark_tutorial_done(void *ud) {
    const char *path = (const char *)ud;
    if (!path || !*path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("seen\n", f);
    fclose(f);
}

int main(int argc, char **argv) {
    int grid_w = 2048;
    int grid_h = 2048;
    int win_w  = 1280;
    int win_h  = 800;
    double tps = 120.0;
    double density = 0.30;
    unsigned int seed = (unsigned int)time(NULL);
    int force_tutorial = 0;
    int no_tutorial = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if ((!strcmp(a, "-w") || !strcmp(a, "--width")) && v)         { grid_w = atoi(v); i++; }
        else if ((!strcmp(a, "-h") || !strcmp(a, "--height")) && v)   { grid_h = atoi(v); i++; }
        else if (!strcmp(a, "--win") && v)                            { sscanf(v, "%dx%d", &win_w, &win_h); i++; }
        else if (!strcmp(a, "--tps") && v)                            { tps = atof(v); i++; }
        else if (!strcmp(a, "--density") && v)                        { density = atof(v); i++; }
        else if (!strcmp(a, "--seed") && v)                           { seed = (unsigned)strtoul(v, NULL, 0); i++; }
        else if (!strcmp(a, "--tutorial"))                            { force_tutorial = 1; }
        else if (!strcmp(a, "--no-tutorial"))                         { no_tutorial = 1; }
        else if (!strcmp(a, "--help"))                                { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (grid_w < 16)  grid_w = 16;
    if (grid_h < 16)  grid_h = 16;
    if (win_w  < 320) win_w  = 320;
    if (win_h  < 240) win_h  = 240;

    /* First-run marker: ~/.gameoflife_seen */
    static char marker_path[PATH_MAX];
    const char *home = getenv("HOME");
    if (home && *home) snprintf(marker_path, sizeof marker_path, "%s/.gameoflife_seen", home);
    int first_run = (marker_path[0] && access(marker_path, F_OK) != 0);
    int show_tut = !no_tutorial && (force_tutorial || first_run);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(win_w, win_h, "Game of Life - raylib (threaded sim + LOD render)");
    SetTargetFPS(120);
    SetExitKey(KEY_NULL); /* don't auto-quit on ESC */

    Life *life = life_create(grid_w, grid_h);
    if (!life) {
        fprintf(stderr, "life_create failed\n");
        CloseWindow();
        return 1;
    }
    life_request_randomize(life, density, seed);
    life_set_target_tps(life, tps);
    life_set_paused(life, true);
    life_start(life);

    Renderer *r = renderer_create(life, win_w, win_h);
    r->show_tutorial        = show_tut;
    r->on_tutorial_dismiss  = mark_tutorial_done;
    r->tutorial_ud          = marker_path;

    while (!WindowShouldClose()) {
        if (IsWindowResized()) {
            renderer_resize(r, GetScreenWidth(), GetScreenHeight());
        }
        renderer_update(r, GetFrameTime());
        renderer_draw(r);
    }

    life_stop(life);
    renderer_destroy(r);
    life_destroy(life);
    CloseWindow();
    return 0;
}
