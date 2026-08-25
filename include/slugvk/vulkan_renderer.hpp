#pragma once

#include "slugvk/draw_list.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace slugvk {

class VectorAtlas;
class Window;

struct RendererConfig {
  Color clearColor = Color::fromRgb8(0x0b1020);
  bool validation = false;
  // false prefers refresh-synchronized MAILBOX for smooth low-latency interaction.
  bool vsync = false;
  // Select IMMEDIATE before MAILBOX when tearing is acceptable and absolute latency is primary.
  bool allowTearing = false;
  // Number of quad instances preallocated per frame; buffers grow geometrically when needed.
  std::size_t initialVertexCapacity = 1U << 12U;
  // GPU timestamps add query commands. Zero disables them; otherwise sample every Nth submission.
  std::uint32_t gpuTimingInterval = 0;
};

struct RendererStats {
  std::uint32_t quads = 0;
  std::uint32_t retainedQuads = 0;
  std::uint32_t drawCalls = 0;
  std::size_t uploadedBytes = 0;
  float cpuBuildMilliseconds = 0.0f;
  float cpuUploadMilliseconds = 0.0f;
  float gpuMilliseconds = 0.0f;
  float cpuSubmitMilliseconds = 0.0f;
};

class VulkanRenderer {
public:
  VulkanRenderer(Window& window, const VectorAtlas& atlas, const RendererConfig& config = {});
  ~VulkanRenderer();
  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  // Performs all potentially blocking fence/image acquisition work. Call this before polling
  // input, then build the DrawList and call draw() for the freshest possible interactive frame.
  void prepareFrame();
  // Resolves and uploads an immutable document once. The returned resource remains valid for
  // this renderer's lifetime and can be transformed cheaply with DrawList::retainedText().
  RetainedTextId createRetainedText(std::string_view utf8, Rect layoutBounds, TextStyle style);
  void draw(const DrawList& list);
  void waitIdle();
  [[nodiscard]] RendererStats stats() const;
  [[nodiscard]] const char* deviceName() const;
  [[nodiscard]] const char* presentModeName() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace slugvk
