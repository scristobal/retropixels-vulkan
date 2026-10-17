# RetroPixels Vulkan

A C23 sprite renderer built around Vulkan 1.4. The scene renders at **320×180**
and is presented with **5× nearest-neighbor scaling** in a window with a fixed
**1600×900 physical pixel framebuffer**. Each canvas pixel becomes a 5×5 block of
display pixels. The 32×32 avatar is centered at its native size on the canvas.
Press **Escape** to quit.

The window is DPI-aware. On a Wayland desktop at 200% scale, its content area is
800×450 logical screen units with a native 1600×900 framebuffer. Desktop DPI does
not change the canvas resolution or the 5× physical pixel multiplier.
Startup and display-scale changes log the effective DPI, its reference baseline,
the window content scale, and the measured framebuffer-to-window ratio. Effective
DPI describes desktop scaling rather than the monitor's physical pixel density.

## Build and run

Current stable release baseline, checked on 2026-10-17:

| Component | Version |
| --- | --- |
| C language | C23 |
| CMake | [4.4.4](https://github.com/Kitware/CMake/releases/tag/v4.4.4) or newer |
| Vulkan runtime API | 1.4 |
| Vulkan SDK headers and loader | [1.4.363.0](https://www.lunarg.com/lunarg-releases-vulkan-sdk-1-4-363-0/) or newer |
| GLFW | [3.5.1](https://github.com/glfw/glfw/releases/tag/3.5.1) or newer |
| GLSL / SPIR-V | 4.60 / 1.6 |
| Shader compiler | glslc 2026.4+ or glslangValidator 16.6.0+ |
| Ninja | 1.13.2 |
| Image loader | stb_image 2.30 (vendored) |

A Vulkan 1.4 driver is required; the runtime checks its supported API and features.
`VK_LAYER_KHRONOS_validation` is optional for normal development.
GCC 16.2.1 and Clang 23.1.3 are verified C23 toolchains.

The avatar and `stb_image` are included. The build uses system libraries and does
not download dependencies.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/RetroPixels
```

GLSL 4.60 shaders are compiled automatically for Vulkan 1.4 and SPIR-V 1.6.
The compiler defaults to glslc; select glslang explicitly with
`-DRETROPIXELS_SHADER_COMPILER=GLSLANG`. CMake sets asset and shader locations so
the executable can run from any working directory. Keep the source assets and
generated build shaders in place.

Debug builds enable validation when available. Release builds leave it off by
default. `--validation` requires the layer and makes validation errors cause a
nonzero exit status, including errors during cleanup.

```sh
./build/RetroPixels --validation
./build/RetroPixels --no-validation
```

## Rendering

The renderer requires Vulkan 1.4 and uses dynamic rendering, synchronization2,
and core push descriptors. Each of two frames in flight owns a 320×180
`R16G16B16A16_SFLOAT` color attachment. Drawing
uses `vkCmdBeginRendering`, a graphics pipeline, a combined image sampler pushed
with `vkCmdPushDescriptorSet`, and pixel-coordinate rectangles supplied through
push constants. The vertex shader generates the quad from `gl_VertexIndex`;
coordinates start at the canvas's top left.

The sprite is an `R8G8B8A8_SRGB` texture with nearest sampling and a single mip level.
Texture sampling decodes sRGB automatically. Alpha blending and the background
clear operate in linear color in the floating-point canvas. A nearest-neighbor
`vkCmdBlitImage2` scales the canvas into an sRGB swapchain image and performs the
presentation encoding. See the [Vulkan image blit specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage2.html).

Image layout transitions and access dependencies use `vkCmdPipelineBarrier2`.
Work is submitted with `vkQueueSubmit2`. The swapchain acquire wait covers the
blit and its layout transition, allowing scene rendering to proceed independently
of image acquisition. Acquire semaphores, command buffers, canvases, and submit
fences belong to frames. Presentation semaphores belong to swapchain images, as
specified in the [Khronos semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

The swapchain uses FIFO presentation. User resizing is disabled. Native framebuffer
DPI scaling is enabled, and the logical window dimensions are adjusted using the
measured framebuffer-to-window ratio to retain exactly 1600×900 physical pixels.
Content-scale changes are handled before rendering resumes. Surface invalidation
recreates presentation resources; the canvas and drawing pipeline keep their fixed
resolution. Zero-sized framebuffers wait for window events.

## Third-party code

`external/stb/stb_image.h` is the current upstream stb_image 2.30, verified against
[revision 013ac3b](https://github.com/nothings/stb/blob/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h).
Its MIT/public-domain license is included in `external/stb/LICENSE`.
GLFW and Vulkan are system dependencies.
