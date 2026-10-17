# RetroPixels Vulkan

A C/GLFW sprite renderer built with Vulkan. It draws a 32×32 avatar
over a dark background in a resizable 640×480 window. The sprite fills the
framebuffer, including the original aspect-ratio stretching, nearest-neighbor
filtering, six mip levels, and transparent-fragment discard. Press **Escape** to quit.

## Build and run

Requirements:

- A C11 compiler and CMake 3.16 or newer.
- Vulkan headers, loader, and a working Vulkan driver (Vulkan 1.0 or newer).
- GLFW 3.3 or newer, including its development files and CMake package.
- `glslc` or `glslangValidator` to compile GLSL into SPIR-V.
- Optional: `VK_LAYER_KHRONOS_validation` for development and testing.

`stb_image` and the original avatar are included. The build does not download
dependencies or require the OpenGL project to remain beside this repository.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/RetroPixels
```

Shader changes are compiled automatically. Asset and shader paths are absolute
paths set by CMake, so the executable can run from any working directory; keep
the source assets and generated build shaders in place.

Debug builds enable Vulkan validation when the layer is available. Release builds
leave it off by default. `--validation` requires the layer and makes validation
errors cause a nonzero exit status, including errors during cleanup.

```sh
./build/RetroPixels --validation
./build/RetroPixels --no-validation
./build/RetroPixels --validation --hidden --frames 120
```

`--frames N` exits automatically after N render-loop iterations, which is useful
for smoke checks. A working window-system display is required even with `--hidden`.

## Rendering

The port uses a GLFW Vulkan surface, a FIFO (vsync) swapchain, a graphics pipeline,
and an indexed, instanced quad. Vertex, index, and model-matrix buffers live in
device memory and are uploaded through staging buffers. The texture is an RGBA8
image with six CPU-generated box-filtered mip levels, explicit layout transitions,
and a combined image sampler descriptor. A push constant supplies the texture
transform. The vertex shader accounts for Vulkan's framebuffer Y direction.

UNORM swapchain formats are preferred to match the OpenGL version's colors. When
an sRGB swapchain is used, the fragment shader and clear color compensate for the
attachment's encoding. Alpha blending stays disabled, as in the original.

Two frames can be in flight. Acquire semaphores and fences belong to frames;
presentation semaphores belong to swapchain images, following the
[Khronos semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
Resizing or an out-of-date swapchain rebuilds the dependent image views,
framebuffers, render pass, pipeline, and presentation semaphores. Zero-sized
framebuffers wait for window events. Cleanup handles partially initialized objects.

## Verification

With `spirv-val` installed, validate the compiled shaders:

```sh
spirv-val --target-env vulkan1.0 build/shaders/sprite.vert.spv
spirv-val --target-env vulkan1.0 build/shaders/sprite.frag.spv
```

The Linux X11 integration check requires Python 3, Pillow, `libX11`, the validation
layer, and an X11 display such as Xvfb. It compares rendered pixels with the avatar,
checks orientation and transparency, resizes through all six mip levels, unmaps
and remaps the window, and sends Escape. It fails on validation warnings or errors.
It creates an unmanaged test window so window-manager minimum sizes do not prevent
the 1×1 mip check.

```sh
XDG_SESSION_TYPE=x11 VK_VALIDATION_VALIDATE_SYNC=1 \
    python3 tests/smoke_x11.py build/RetroPixels
```

For software rendering, set `VK_DRIVER_FILES` to your installed Lavapipe or
SwiftShader ICD JSON. This is also useful when a hardware driver is unavailable.
The integration check inherits that setting. Normal launches use the system's
configured driver.

## Third-party code

`external/stb/stb_image.h` is copied from the original project's stb checkout
(`31c1ad37456438565541f4919958214b6e762fb4`); its MIT/public-domain license is
in `external/stb/LICENSE`. GLFW and Vulkan are system dependencies. No OpenGL or
GLAD code is used.
