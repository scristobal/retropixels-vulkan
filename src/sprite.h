#ifndef RETROPIXELS_SPRITE_H
#define RETROPIXELS_SPRITE_H

typedef struct Renderer Renderer;
typedef struct RendererTexture RendererTexture;

/* Load once and share between instances. The renderer owns the GPU texture. */
typedef struct SpriteImage {
    RendererTexture *texture;
    int width, height;
} SpriteImage;

typedef struct Sprite {
    const SpriteImage *image;

    double x, y;

    /* Draw size in canvas pixels; defaults to the image dimensions. */
    int width, height;
    double speed;
} Sprite;

typedef struct SpriteBounds {
    int x, y;
    int width, height;
} SpriteBounds;

int sprite_image_load(SpriteImage *image, Renderer *renderer, const char *path);

Sprite sprite_create(const SpriteImage *image, double x, double y, double speed);
void sprite_move(Sprite *sprite, int horizontal, int vertical, double elapsed,
                 int canvas_width, int canvas_height);

SpriteBounds sprite_bounds(const Sprite *sprite);

#endif
