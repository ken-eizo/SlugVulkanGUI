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
  // false selects the lowest-latency available mode: IMMEDIATE, then MAILBOX, then FIFO.
  bool vsync = false;
  std::size_t initialVertexCapacity = 1U << 16U;
};

struct RendererStats {
  std::uint32_t vertices = 0;
  std::uint32_t indices = 0;
  std::uint32_t drawCalls = 0;
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
