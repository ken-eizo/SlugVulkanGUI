# SlugVulkan

SlugVulkan is a C++20 declarative vector renderer and provisional GUI library for Windows and
macOS. Geometry is converted by **slughorn** into Eric Lengyel's Slug curve/band representation;
the Vulkan fragment shader evaluates those quadratic curves directly. There is no per-frame path
tessellation and no glyph bitmap cache.

The current milestone is a working, testable foundation for the long-term goal of an exceptionally
fast and stable dynamic vector/UI renderer. “Fastest in the world” is a target that must be proven
with reproducible cross-library benchmarks; this repository does not make that unsupported claim.

## Documentation

- [Technical documentation index](docs/README.md)
- [Build and source integration](docs/BUILD_AND_INTEGRATION.md)
- [Architecture and CPU/GPU responsibilities](docs/ARCHITECTURE.md)
- [API guide](docs/API_GUIDE.md)
- [Performance and low-latency guide](docs/PERFORMANCE.md)
- [Generic native-host embedding design](docs/EMBEDDING.md), including the path toward dockable
  DCC/plugin panels without making the core host-specific

## Implemented

- Solid, linear, diamond, radial, and procedural shader paints; per-paint opacity. The same `Paint`
  value and fragment path are used by fills, strokes, and vector text (`DrawList::fill` and
  `DrawList::stroke` are semantic aliases over the common shape renderer). `stroke` accepts either
  a `Paint` directly or a `StrokeStyle`, whose `paint` member is forwarded unchanged.
- Rectangle, circle, ellipse, polygon, star, cubic and quadratic paths. `DrawList::roundedRect`
  applies its radius in absolute framebuffer pixels after width/height placement, including a
  0–100% continuous-corner control; resizing the rectangle never stretches its corner radius.
  The compact scalar overload remains the normal API. An optional `CornerRadii` / `CornerSmoothing`
  overload independently overrides the four corners without increasing the GPU instance size.
- Multiple independently painted stroke shapes, width, butt/round/square caps,
  miter/round/bevel joins, dash arrays, gap lengths, dash offset, and start/end width taper.
  `cap` remains the only required/default cap field. Optional whole-path `startCap` / `endCap`,
  dash-wide `dashStartCap` / `dashEndCap`, and indexed `dashCaps` override only explicitly supplied
  endpoints. Styles that omit them keep the original API and remain one immutable Slug shape at
  runtime.
  The Example exposes live dash-length, gap, and offset sliders; its straight-line preview emits
  only the visible analytic dash instances and does not rebuild the immutable Slug atlas.
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
  single instanced GPU batch but is emitted after ordinary panels and widgets.
- A low-latency frame path: one frame in flight, smooth MAILBOX present preferred by default,
  post-dispatch cursor resampling in framebuffer coordinates (including HiDPI), reusable CPU
  staging memory, cached framebuffer scaling for high-rate cursor callbacks, and `prepareFrame()`
  so waits happen before input sampling and DrawList creation.
  One-frame mode omits the redundant per-image fence wait. GPU timestamp commands are opt-in and
  can be sampled rather than emitted every frame. Set `RendererConfig::allowTearing` for the
  absolute-latency IMMEDIATE mode; the Example enables it and samples the pointer again immediately
  before declaring interactive controls. IMMEDIATE also uses the minimum legal swapchain image
  count; launch the Example with `--mailbox` when tear-free presentation is preferred.
- Generic `Tween<T>` with linear, ease-in/out, smooth-step, and spring easing.
- One immutable GPU atlas, a dynamic host-visible instance ring, and optional retained device-local
  text buffers. Quad vertices come from `gl_VertexIndex`; there is no index buffer. Adjacent dynamic
  content remains one batch, while retained documents add only the draw boundaries needed to keep
  declaration order correct.
- CPU culling removes off-clip lines/glyphs from ordinary dynamic text. `DrawList::textStatic`
  avoids a string copy but still lays out dynamic text; `VulkanRenderer::createRetainedText` resolves
  and uploads an immutable document once, then `DrawList::retainedText` changes only GPU transform
  and hardware clip during real-time zoom/pan.
- Analytic Slug antialiasing is performed in the fragment shader from the pixel footprint and exact
  quadratic intersections. MSAA or a bitmap/SDF glyph cache is not required.
- GLFW framebuffer-size/refresh callbacks redraw declarative content inside the Windows modal
  resize loop. Swapchain resize recreates only image resources; the render pass and graphics
  pipeline are reused unless the surface format actually changes.
- Vulkan timestamp queries and CPU timers expose layout/build time, mapped-buffer upload time, GPU
  render time, upload bytes, and FPS so 240 Hz can be checked against its 4.17 ms frame budget.
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
Declarative DrawList + UiContext -----+----> dynamic quad-instance batch
Retained document --------------------+----> device-local instance buffer
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

### CPU and GPU responsibilities

Slug is GPU vector **coverage**, not an all-GPU application pipeline. SlugVulkan keeps the CPU work
outside that coverage calculation deliberately small:

- At atlas-build time, FreeType and slughorn run on the CPU to read outlines and create the Slug
  quadratic-curve and band textures. This is not repeated each frame.
- Each frame, the CPU handles operating-system input, UI hit testing/state, small dynamic text
  layout, visible destination instances, one mapped-buffer copy, and Vulkan command submission.
  Retained documents skip layout and upload; their zoom/pan changes one 48-byte push-constant block.
  `DrawList` owns one ordered command stream and mixes both paths without mirroring geometry.
- The GPU transforms quads, finds candidate curves through the Slug band texture, solves quadratic
  antialiased coverage in the fragment shader, evaluates common fill/stroke/text paint, and blends.

The CPU cannot be removed: Vulkan requires host-side resource and command submission, while input,
layout, and outline-to-atlas conversion are not jobs performed by the Slug shaders. Retention is
explicit rather than mandatory, so frequently changing widgets keep the small immediate/declarative
path and large immutable documents opt into caching without global invalidation machinery.

Per-frame instances are copied once into persistently mapped host-visible/coherent Vulkan memory;
there is no second staging-buffer submission and no GPU-to-CPU readback in the interactive path.
Moving hit testing or layout to compute shaders would require synchronization/readback before the
application can update widget state, usually increasing input latency. Work that is naturally
one-way—coverage, paint, transforms, clipping, and blending—remains on the GPU.

### What “minimal” means here

`external/Slug` contains reference HLSL shaders, not a complete windowing/rendering/UI runtime, so
an executable-size or whole-library-size comparison with it would be misleading. SlugVulkan reuses
its curve/band algorithm, builds only slughorn's core plus its FreeType bridge, disables unused
FreeType codecs, and keeps the Example and tests optional. A renderer-only application that links
the static library does not pull unused Example/test object code.

The adapted Slug shaders are smaller in raw source than the two upstream reference shaders, but
source length is not a performance benchmark: SlugVulkan also includes common paints, clipping,
and absolute-pixel analytic rounded rectangles. Claims of equal or better speed require a matched
GPU, content, resolution, compiler, and benchmark harness; this project does not substitute binary
or line counts for that measurement.

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

The single canonical Windows Example output is `build\Release\slugvk_example.exe`. SPIR-V is
embedded and the MSVC runtime is linked statically, so no adjacent shader directory or project DLL
is required. Use `slugvk_example.exe --smoke` for a short Vulkan validation run.

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
open ./build/slugvk_example.app
```

For a directly built MoltenVK package rather than the SDK-installed ICD, point the loader at its
JSON manifest before running:

```bash
export VK_DRIVER_FILES=/path/to/MoltenVK/Package/Latest/MoltenVK/macOS/MoltenVK_icd.json
```

SlugVulkan enumerates `VK_KHR_portability_enumeration` before setting
`VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and enables `VK_KHR_portability_subset` only
when the chosen device advertises it. GLFW supplies `VK_EXT_metal_surface` and creates the surface.
The release workflow produces a Universal (`arm64` + `x86_64`) app bundle containing the Vulkan
Loader, MoltenVK, and its bundle-relative ICD manifest, so release users do not install the SDK.

## Release builds

Pushing a `v*` tag runs `.github/workflows/release.yml`. It builds/tests the single Windows x64 EXE,
builds/tests a macOS Universal app through MoltenVK, validates both SPIR-V modules, and publishes
`SlugVulkanGUI-Windows-x64.zip` plus `SlugVulkanGUI-macOS-Universal.zip` to the matching GitHub Release.

## Minimal API

```cpp
#include <slugvk/slugvk.hpp>

slugvk::VectorAtlas atlas;
atlas.loadFont(slugvk::findDefaultSystemFont());
atlas.build();

slugvk::Window window;
slugvk::VulkanRenderer renderer(window, atlas, {.vsync = false}); // MAILBOX when available
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

### Use from another CMake project

Source integration is the supported library-consumption path in the current `0.x` series. Example,
tests, install rules, and static MSVC runtime selection default to OFF when this repository is added
as a subproject.

```cmake
add_subdirectory(external/SlugVulkanGUI)
target_link_libraries(my_application PRIVATE SlugVulkan::slugvk)
```

The current install rule copies the raw archive and headers; it is not yet a complete exported
`find_package(SlugVulkan)` package. See the [integration guide](docs/BUILD_AND_INTEGRATION.md) for
options, CRT/ABI constraints, lifecycle, and MoltenVK packaging.

## Current boundaries

- The included text layout is intentionally small and currently performs codepoint layout, not
  full HarfBuzz shaping/Unicode bidi/line breaking. The glyph renderer itself is vector Slug.
- Bold and italic are synthetic presentation options. Load dedicated bold/italic font files under
  their family names when exact typeface masters are required.
- macOS is compiled as a Universal app in GitHub Actions. The hosted job verifies both slices,
  bundle linkage, unit tests, and SPIR-V; interactive latency still requires measurement on physical
  Apple hardware because hosted runners are not a display-performance benchmark.
- Custom paint currently selects the built-in procedural shader branch and a float parameter.
  Arbitrary user SPIR-V pipeline registration is intentionally deferred until its descriptor and
  synchronization contract can be made safe.
- `Path::roundedRect` remains an ordinary authored vector path and therefore scales like any other
  path when its destination transform changes. Use `DrawList::roundedRect` for layout rectangles
  whose corner radius must remain an absolute pixel value.
- The current `Window`/`VulkanRenderer` backend owns a GLFW top-level window and its Vulkan surface.
  Host-owned `HWND` / `NSView` embedding is a documented backend-decomposition roadmap, not an
  already supported constructor. Host SDK adapters remain separate from the general-purpose core.

## Dependency revisions

- EricLengyel/Slug: `be3c13eb7d63f9e8aa5c583e42d92c374cb91d98`
- AlphaPixel/slughorn: `eeb1b96ec88caef234db836a3213cf308b7e0c9d`
- GLFW 3.4: `7b6aead9fb88b3623e3b3725ebb42670cbe4c579`
- FreeType 2.14.1: `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b`

The Slug shader attribution required by its distribution terms is recorded in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and in the adapted fragment shader source.
