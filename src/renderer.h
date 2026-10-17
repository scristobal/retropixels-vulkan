#ifndef RETROPIXELS_RENDERER_H
#define RETROPIXELS_RENDERER_H

typedef struct GLFWwindow GLFWwindow;
typedef struct Renderer Renderer;

enum {
    RENDER_WIDTH = 320,
    RENDER_HEIGHT = 180,

    PRESENT_SCALE = 10,
    PRESENT_WIDTH = RENDER_WIDTH * PRESENT_SCALE,
    PRESENT_HEIGHT = RENDER_HEIGHT * PRESENT_SCALE
};

/* validation: -1 = enable if available, 0 = disable, 1 = require. */
Renderer *renderer_create(GLFWwindow *window, int validation);

void renderer_surface_changed(Renderer *renderer);
int renderer_draw(Renderer *renderer);

/* Returns false if validation reported errors, including during destruction. */
int renderer_destroy(Renderer *renderer);

#endif
