#include "sprite.h"
#include "renderer.h"

#include <stb_image.h>

#include <math.h>
#include <stdio.h>

int sprite_image_load(SpriteImage *image, Renderer *renderer, const char *path)
{
    int width, height, channels;
    unsigned char *pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);

    if (!pixels) {
        fprintf(stderr, "Failed to load %s: %s\n", path, stbi_failure_reason());
        return 0;
    }

    RendererTexture *texture = renderer_texture_create(renderer, pixels, width, height);
    stbi_image_free(pixels);

    if (!texture) return 0;

    *image = (SpriteImage){ .texture = texture, .width = width, .height = height };

    return 1;
}

Sprite sprite_create(const SpriteImage *image, double x, double y, double speed)
{
    return (Sprite){
        .image = image, .x = x, .y = y,
        .width = image->width, .height = image->height, .speed = speed
    };
}

static double wrap_position(double position, int extent, int size)
{
    int pixel = (int)floor(position);
    int period = extent + size;
    int wrapped = ((pixel + size) % period + period) % period - size;

    return position + (wrapped - pixel);
}

void sprite_move(Sprite *sprite, int horizontal, int vertical, double elapsed,
                 int canvas_width, int canvas_height)
{
    if ((!horizontal && !vertical) || elapsed <= 0.0) return;

    double distance = sprite->speed * elapsed / hypot(horizontal, vertical);

    sprite->x = wrap_position(sprite->x + horizontal * distance, canvas_width, sprite->width);
    sprite->y = wrap_position(sprite->y + vertical * distance, canvas_height, sprite->height);
}

SpriteBounds sprite_bounds(const Sprite *sprite)
{
    return (SpriteBounds){
        .x = (int)floor(sprite->x), .y = (int)floor(sprite->y),
        .width = sprite->width, .height = sprite->height
    };
}
