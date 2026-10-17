#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "renderer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void error_callback(int error, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

static void framebuffer_callback(GLFWwindow *window, int width, int height)
{
    (void)width;
    (void)height;
    renderer_resize(glfwGetWindowUserPointer(window));
}

static void usage(const char *program)
{
    printf("Usage: %s [--validation | --no-validation] [--frames N] [--hidden]\n"
           "  Escape closes the window. --frames N exits after N frames.\n"
           "  Debug builds enable validation when available.\n", program);
}

int main(int argc, char **argv)
{
    GLFWwindow *window = NULL;
    Renderer *renderer = NULL;
    unsigned long frame_limit = 0, frames = 0;
    int hidden = 0, exit_code = EXIT_FAILURE;
#ifdef NDEBUG
    int validation = 0;
#else
    int validation = -1;
#endif

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--validation") == 0) validation = 1;
        else if (strcmp(argv[i], "--no-validation") == 0) validation = 0;
        else if (strcmp(argv[i], "--hidden") == 0) hidden = 1;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            errno = 0;
            frame_limit = strtoul(value, &end, 10);
            if (errno || *value < '0' || *value > '9' || *end || !frame_limit) {
                fprintf(stderr, "--frames requires a positive integer.\n");
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    glfwSetErrorCallback(error_callback);
    if (!glfwInit()) goto cleanup;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
    window = glfwCreateWindow(640, 480, "RetroPixel (Vulkan)", NULL, NULL);
    if (!window) goto cleanup;
    glfwSetKeyCallback(window, key_callback);

    renderer = renderer_create(window, validation);
    if (!renderer) goto cleanup;
    glfwSetWindowUserPointer(window, renderer);
    glfwSetFramebufferSizeCallback(window, framebuffer_callback);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window)) break;
        if (!renderer_draw(renderer)) goto cleanup;
        if (frame_limit && ++frames >= frame_limit) break;
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    if (!renderer_destroy(renderer)) exit_code = EXIT_FAILURE;
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}
