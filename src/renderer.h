#ifndef RETROPIXELS_RENDERER_H
#define RETROPIXELS_RENDERER_H

#include <stddef.h>

typedef struct GLFWwindow GLFWwindow;
typedef struct Renderer Renderer;
typedef struct RendererTexture RendererTexture;
typedef struct Sprite Sprite;

enum {
    RENDER_WIDTH = 320,
    RENDER_HEIGHT = 180,

    PRESENT_SCALE = 10,
    PRESENT_WIDTH = RENDER_WIDTH * PRESENT_SCALE,
    PRESENT_HEIGHT = RENDER_HEIGHT * PRESENT_SCALE
};

/* validation: -1 = enable if available, 0 = disable, 1 = require. */
Renderer *renderer_create(GLFWwindow *window, int validation);

/* Upload RGBA8 pixels once. Textures remain valid until renderer destruction. */
RendererTexture *renderer_texture_create(Renderer *renderer, const unsigned char *pixels,
                                         int width, int height);

void renderer_surface_changed(Renderer *renderer);

/* Draw in array order; later sprites appear over earlier sprites. Zero is valid. */
int renderer_draw(Renderer *renderer, const Sprite *sprites, size_t sprite_count);

/* Returns false if validation reported errors, including during destruction. */
int renderer_destroy(Renderer *renderer);

#endif
