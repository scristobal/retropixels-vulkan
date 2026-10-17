#ifndef RETROPIXELS_RENDERER_H
#define RETROPIXELS_RENDERER_H

typedef struct GLFWwindow GLFWwindow;
typedef struct Renderer Renderer;

/* validation: -1 = enable if available, 0 = disable, 1 = require. */
Renderer *renderer_create(GLFWwindow *window, int validation);
void renderer_resize(Renderer *renderer);
int renderer_draw(Renderer *renderer);
/* Returns false if validation reported errors, including during destruction. */
int renderer_destroy(Renderer *renderer);

#endif
