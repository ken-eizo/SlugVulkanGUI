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
};

struct RendererStats {
  std::uint32_t quads = 0;
  std::uint32_t drawCalls = 0;
  std::size_t uploadedBytes = 0;
  float cpuBuildMilliseconds = 0.0f;
  float gpuMilliseconds = 0.0f;
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
