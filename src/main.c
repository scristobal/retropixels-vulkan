#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool window_metrics_changed = true;

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

    window_metrics_changed = true;
    renderer_surface_changed(glfwGetWindowUserPointer(window));
}

static void content_scale_callback(GLFWwindow *window, float xscale, float yscale)
{
    (void)xscale;
    (void)yscale;

    window_metrics_changed = true;
    renderer_surface_changed(glfwGetWindowUserPointer(window));
}

/* Keep presentation in physical pixels while GLFW uses platform screen units.
 * Returns 1 when ready, 0 when closed, and -1 if the requested size cannot be set.
 * Window changes stay outside GLFW callbacks to avoid recursive configure events. */
static int prepare_window(GLFWwindow *window)
{
    int requested_width = 0, requested_height = 0;
    double deadline = glfwGetTime() + 2.0;

    for (;;) {
        int width, height, framebuffer_width, framebuffer_height;
        int logical_width, logical_height;

        glfwPollEvents();
        if (glfwWindowShouldClose(window)) return 0;

        glfwGetWindowSize(window, &width, &height);
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        if (!width || !height || !framebuffer_width || !framebuffer_height) {
            glfwWaitEvents();
            deadline = glfwGetTime() + 2.0;
            continue;
        }

        if (framebuffer_width == PRESENT_WIDTH && framebuffer_height == PRESENT_HEIGHT)
            return 1;

        logical_width = (int)((double)PRESENT_WIDTH * width / framebuffer_width + 0.5);
        logical_height = (int)((double)PRESENT_HEIGHT * height / framebuffer_height + 0.5);
        if (logical_width < 1 || logical_height < 1 ||
            (logical_width == width && logical_height == height)) {
            fprintf(stderr, "The display scale cannot represent an exact %dx%d framebuffer.\n",
                    PRESENT_WIDTH, PRESENT_HEIGHT);
            return -1;
        }

        if (glfwGetTime() > deadline) {
            fprintf(stderr, "The window system did not provide the requested %dx%d framebuffer.\n",
                    PRESENT_WIDTH, PRESENT_HEIGHT);
            return -1;
        }

        if (logical_width != requested_width || logical_height != requested_height) {
            glfwSetWindowSizeLimits(window, logical_width, logical_height, logical_width, logical_height);
            glfwSetWindowSize(window, logical_width, logical_height);
            requested_width = logical_width;
            requested_height = logical_height;
        }

        glfwWaitEventsTimeout(0.01);
    }
}

static void report_window_metrics(GLFWwindow *window)
{
    int width, height, framebuffer_width, framebuffer_height;
    float xscale, yscale;

#ifdef __APPLE__
    const double dpi_baseline = 72.0;
#else
    const double dpi_baseline = 96.0;
#endif

    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    glfwGetWindowContentScale(window, &xscale, &yscale);

    fprintf(stderr, "Window: %dx%d screen units; framebuffer: %dx%d physical pixels.\n",
            width, height, framebuffer_width, framebuffer_height);
    fprintf(stderr, "DPI: %.1fx%.1f effective (%.0f-DPI baseline); "
                    "content scale: %.2fx%.2f; framebuffer scale: %.2fx%.2f.\n",
            dpi_baseline * xscale, dpi_baseline * yscale, dpi_baseline,
            xscale, yscale, (double)framebuffer_width / width,
            (double)framebuffer_height / height);

    window_metrics_changed = false;
}

static void usage(const char *program)
{
    printf("Usage: %s [--validation | --no-validation]\n"
           "  Escape closes the window.\n"
           "  Debug builds enable validation when available.\n", program);
}

int main(int argc, char **argv)
{
    GLFWwindow *window = NULL;
    Renderer *renderer = NULL;
    int exit_code = EXIT_FAILURE;

#ifdef NDEBUG
    int validation = 0;
#else
    int validation = -1;
#endif

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--validation") == 0) validation = 1;
        else if (strcmp(argv[i], "--no-validation") == 0) validation = 0;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    glfwSetErrorCallback(error_callback);
    if (!glfwInit()) goto cleanup;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);

    window = glfwCreateWindow(PRESENT_WIDTH, PRESENT_HEIGHT, "RetroPixels Vulkan", NULL, NULL);
    if (!window) goto cleanup;

    glfwSetWindowSizeLimits(window, PRESENT_WIDTH, PRESENT_HEIGHT, PRESENT_WIDTH, PRESENT_HEIGHT);

    glfwSetKeyCallback(window, key_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_callback);
    glfwSetWindowContentScaleCallback(window, content_scale_callback);

    {
        int ready = prepare_window(window);

        if (ready < 0) goto cleanup;
        if (!ready) {
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        }
    }

    report_window_metrics(window);

    renderer = renderer_create(window, validation);
    if (!renderer) goto cleanup;

    glfwSetWindowUserPointer(window, renderer);

    while (!glfwWindowShouldClose(window)) {
        int ready = prepare_window(window);

        if (ready < 0) goto cleanup;
        if (!ready) break;

        if (window_metrics_changed) report_window_metrics(window);

        if (!renderer_draw(renderer)) goto cleanup;
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    if (!renderer_destroy(renderer)) exit_code = EXIT_FAILURE;
    if (window) glfwDestroyWindow(window);
    glfwTerminate();

    return exit_code;
}
