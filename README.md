# RetroPixels Vulkan

A small 2D renderer for pixel-art sprites, written in C23 using Vulkan 1.4.

Sprites have independent positions and sizes, can share images, and support
transparency. Scenes are drawn to a low-resolution canvas and enlarged with
nearest-neighbor scaling to preserve the pixel grid.

Sprite state and movement are separate from the Vulkan renderer. The demo
application handles the window and keyboard input.

## Build and run

Requires a C23 compiler, CMake, Ninja, GLFW, and the Vulkan SDK with `glslc` or
`glslangValidator`. See [CMakeLists.txt](CMakeLists.txt) for required versions.
The GPU and driver must support Vulkan 1.4.

On Arch Linux, install the build dependencies with:

```sh
sudo pacman -Syu --needed gcc cmake ninja glfw vulkan-headers vulkan-icd-loader shaderc
```

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/RetroPixels
```

Use **WASD** to move the character and **Escape** to quit.
