#pragma once

#include "slugvk/draw_list.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace slugvk {

class VectorAtlas;
#if defined(SLUGVK_ENABLE_GLFW) && SLUGVK_ENABLE_GLFW
class Window;
#endif
class PlatformSurface;

struct RendererConfig {
  // Opt-in diagnostics only; normal rendering does not allocate/copy/wait for pixels.
  bool enableReadback = false;
  Color clearColor = Color::fromRgb8(0x0b1020);
  bool validation = false;
  // false prefers refresh-synchronized MAILBOX for smooth low-latency interaction.
  bool vsync = false;
  // Select IMMEDIATE before MAILBOX when tearing is acceptable and absolute latency is primary.
  bool allowTearing = false;
  // Retain authored radii while forcing circular (non-superellipse) corners when disabled.
  bool enableContinuousCorners = true;
  // Number of quad instances preallocated per frame; buffers grow geometrically when needed.
  std::size_t initialVertexCapacity = 1U << 12U;
  // GPU timestamps add query commands. Zero disables them; otherwise sample every Nth submission.
  std::uint32_t gpuTimingInterval = 0;
};

struct RendererStats {
  std::uint32_t quads = 0;
  std::uint32_t retainedQuads = 0;
  std::uint32_t primitiveQuads = 0;
  std::uint32_t drawCalls = 0;
  std::size_t uploadedBytes = 0;
  float cpuBuildMilliseconds = 0.0f;
  float cpuUploadMilliseconds = 0.0f;
  float gpuMilliseconds = 0.0f;
  float cpuSubmitMilliseconds = 0.0f;
};

struct FramePixels {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> rgba;
};

class VulkanRenderer {
public:
  VulkanRenderer(PlatformSurface& surface, const VectorAtlas& atlas,
                 const RendererConfig& config = {});
#if defined(SLUGVK_ENABLE_GLFW) && SLUGVK_ENABLE_GLFW
  VulkanRenderer(Window& window, const VectorAtlas& atlas, const RendererConfig& config = {});
#endif
  ~VulkanRenderer();
  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  // Performs all potentially blocking fence/image acquisition work. Call this before polling
  // input, then build the DrawList and call draw() for the freshest possible interactive frame.
  void prepareFrame();
  // Resolves and uploads an immutable document once. The returned resource remains valid for
  // this renderer's lifetime and can be transformed cheaply with DrawList::retainedText().
  RetainedTextId createRetainedText(std::string_view utf8, Rect layoutBounds, TextStyle style);
  // Retains an arbitrary static DrawList in device-local memory. Retained commands may be
  // transformed, clipped and faded every frame without rebuilding their instances.
  RetainedDrawListId createRetainedDrawList(const DrawList& list);
  // Rebuilds CPU instances, but uploads only changed contiguous ranges when capacity permits.
  // Returns the number of bytes transferred to the GPU (zero when the retained data is identical).
  std::size_t updateRetainedDrawList(RetainedDrawListId drawList, const DrawList& list);
  // Releases the backing GPU allocation. IDs are never recycled, so stale commands stay inert.
  void destroyRetainedDrawList(RetainedDrawListId drawList);
  void draw(const DrawList& list);
  [[nodiscard]] FramePixels drawAndReadback(const DrawList& list);
  void waitIdle();
  [[nodiscard]] RendererStats stats() const;
  [[nodiscard]] const char* deviceName() const;
  [[nodiscard]] const char* presentModeName() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace slugvk
