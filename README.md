# SlugVulkan

SlugVulkan is a C++20 declarative vector renderer and provisional GUI library for Windows and
macOS. Geometry is converted by **slughorn** into Eric Lengyel's Slug curve/band representation;
the Vulkan fragment shader evaluates those quadratic curves directly. There is no per-frame path
tessellation and no glyph bitmap cache.

The current milestone is a working, testable foundation for the long-term goal of an exceptionally
fast and stable dynamic vector/UI renderer. “Fastest in the world” is a target that must be proven
with reproducible cross-library benchmarks; this repository does not make that unsupported claim.

## Implemented

- Solid, linear, diamond, radial, and procedural shader paints; per-paint opacity. The same `Paint`
  value and fragment path are used by fills, strokes, and vector text (`DrawList::fill` and
  `DrawList::stroke` are semantic aliases over the common shape renderer). `stroke` accepts either
  a `Paint` directly or a `StrokeStyle`, whose `paint` member is forwarded unchanged.
- Rectangle, circle, ellipse, polygon, star, cubic and quadratic paths. `DrawList::roundedRect`
  applies its radius in absolute framebuffer pixels after width/height placement, including a
  0–100% continuous-corner control; resizing the rectangle never stretches its corner radius.
- Multiple independently painted stroke shapes, width, butt/round/square caps,
  miter/round/bevel joins, dash arrays, gap lengths, dash offset, and start/end width taper.
- FreeType outlines loaded into the same Slug atlas. Multiple fonts are namespaced by font family
  or filename. Size, simulated bold/italic, underline, strikethrough, horizontal alignment,
  line-height, letter-spacing, indent, bullet and numbered-list state are represented by `TextStyle`.
- Mouse hover and position; press/release/held states for left/right/middle and five extra buttons;
  scroll start/active/end and direction; cursor delta; opt-in GLFW raw mouse motion; arbitrary GLFW
  key press/release/held state; Unicode text input.
- Button, spin button, slider, list box, checkbox, combo box, dropdown, radio, editable string field,
  switch, horizontal/vertical scrollbar, tooltip, tree view, and grid view.
- Press-edge state updates, cursor-position drag mapping, hover feedback for interactive controls,
  and a final overlay command layer for dropdowns and tooltips. Overlay content remains part of the
  single indexed GPU batch but is emitted after ordinary panels and widgets.
- A low-latency frame path: one frame in flight, IMMEDIATE present preferred over MAILBOX,
  post-dispatch cursor resampling, reusable CPU staging vectors, and `prepareFrame()` so fence and
  swapchain-image waits happen before the application samples input and builds its DrawList.
- Generic `Tween<T>` with linear, ease-in/out, smooth-step, and spring easing.
- One immutable GPU atlas and a dynamic host-visible vertex/index ring. All visible shapes and text
  are submitted as one indexed draw call per frame.
- Resize/minimize-safe swapchain recreation; frame fences; acquire semaphores per frame; present
  semaphores per swapchain image; device-loss errors are surfaced as exceptions.
- MoltenVK portability enumeration and portability-subset device-extension handling.
- A two-page Example: the complete component gallery with a clearly labelled text input, plus a
  long-form Slug text page that zooms from 25% to 800% in real time by slider, buttons, or mouse
  wheel at the cursor position, and pans 1:1 while the document is dragged.

## Architecture

```text
Path / FreeType glyph
       |
       v
slughorn CurveDecomposer -> Slug curve + band atlas (build once)
                                      |
Declarative DrawList + UiContext -----+----> dynamic quad batch
                                      |
                                      v
                      Vulkan Slug coverage shader + GPU paint
                                      |
                                      v
                                  swapchain
```

The atlas is deliberately frozen after `VectorAtlas::build()`. Animation changes destination
rectangles, colors, opacity, gradient parameters, and shader parameters without rebuilding curves.
For truly new paths at runtime, build a replacement atlas off the render thread, wait for the old
renderer, and swap renderer/atlas instances. A future generation-safe atlas swap API can automate
that policy without weakening the immutable fast path.

## Build on Windows

Requirements: Visual Studio 2022 with C++, CMake 3.24+, and the LunarG Vulkan SDK with `glslc`.
Dependencies are pinned as Git submodules under `external/`. Clone with `--recurse-submodules`, or
run `git submodule update --init --recursive` after an ordinary clone.

```powershell
$env:VULKAN_SDK = "C:\VulkanSDK\1.x.y"
cmake -S . -B build -DVulkan_ROOT="$env:VULKAN_SDK"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\slugvk_example.exe
```

Debug validation smoke test:

```powershell
cmake --build build --config Debug --parallel
.\build\Debug\slugvk_example.exe --smoke
```

## Build on macOS with MoltenVK

Install the current macOS Vulkan SDK, then source its `setup-env.sh` so CMake, the loader,
validation layers, `glslc`, and the MoltenVK ICD are visible. The Vulkan SDK is the approach
recommended by the [MoltenVK project](https://github.com/KhronosGroup/MoltenVK).

```bash
source "$HOME/VulkanSDK/1.x.y/setup-env.sh"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVulkan_ROOT="$VULKAN_SDK"
cmake --build build
ctest --test-dir build --output-on-failure
./build/slugvk_example
```

For a directly built MoltenVK package rather than the SDK-installed ICD, point the loader at its
JSON manifest before running:

```bash
export VK_DRIVER_FILES=/path/to/MoltenVK/Package/Latest/MoltenVK/macOS/MoltenVK_icd.json
```

SlugVulkan enumerates `VK_KHR_portability_enumeration` before setting
`VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and enables `VK_KHR_portability_subset` only
when the chosen device advertises it. GLFW supplies `VK_EXT_metal_surface` and creates the surface.

## Minimal API

```cpp
#include <slugvk/slugvk.hpp>

slugvk::VectorAtlas atlas;
atlas.loadFont(slugvk::findDefaultSystemFont());
atlas.build();

slugvk::Window window;
slugvk::VulkanRenderer renderer(window, atlas, {.vsync = false}); // prefer mailbox/immediate
slugvk::DrawList draw;

while (!window.shouldClose()) {
  renderer.prepareFrame(); // finish any blocking wait before taking the next input sample
  window.pollEvents();
  draw.clear();
  draw.roundedRect({30, 30, 240, 48}, 9.0f,
    slugvk::Paint::gradient(slugvk::GradientKind::Linear,
      slugvk::Color::fromRgb8(0x6d5dfc), slugvk::Color::fromRgb8(0x28c7fa)),
    100.0f);
  renderer.draw(draw);
}
```

See [`examples/kitchen_sink.cpp`](examples/kitchen_sink.cpp) for all components in one executable.

## Current boundaries

- The included text layout is intentionally small and currently performs codepoint layout, not
  full HarfBuzz shaping/Unicode bidi/line breaking. The glyph renderer itself is vector Slug.
- Bold and italic are synthetic presentation options. Load dedicated bold/italic font files under
  their family names when exact typeface masters are required.
- macOS code paths are implemented against Vulkan portability rules but cannot be executed by the
  Windows CI machine used for this milestone. Build and validation on Apple Silicon should be added
  to CI before claiming release-grade macOS support.
- Custom paint currently selects the built-in procedural shader branch and a float parameter.
  Arbitrary user SPIR-V pipeline registration is intentionally deferred until its descriptor and
  synchronization contract can be made safe.
- `Path::roundedRect` remains an ordinary authored vector path and therefore scales like any other
  path when its destination transform changes. Use `DrawList::roundedRect` for layout rectangles
  whose corner radius must remain an absolute pixel value.

## Dependency revisions

- EricLengyel/Slug: `be3c13eb7d63f9e8aa5c583e42d92c374cb91d98`
- AlphaPixel/slughorn: `eeb1b96ec88caef234db836a3213cf308b7e0c9d`
- GLFW 3.4: `7b6aead9fb88b3623e3b3725ebb42670cbe4c579`
- FreeType 2.14.1: `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b`

The Slug shader attribution required by its distribution terms is recorded in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and in the adapted fragment shader source.
