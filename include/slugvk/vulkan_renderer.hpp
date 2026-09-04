#pragma once

#include "slugvk/draw_list.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

namespace slugvk {

class VectorAtlas;
class Window;
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

struct VulkanDeviceContext {
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  std::uint32_t graphicsQueueFamily = 0;
};

struct VulkanFrameContext {
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  std::uint32_t framebufferWidth = 0;
  std::uint32_t framebufferHeight = 0;
};

using VulkanBeforeDrawRecorder = void (*)(void* context,
                                          const VulkanFrameContext& frame) noexcept;

class VulkanRenderer {
public:
  VulkanRenderer(PlatformSurface& surface, const VectorAtlas& atlas,
                 const RendererConfig& config = {});
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
  [[nodiscard]] FramePixels drawAndReadback(const DrawList& list);
  void waitIdle();
  [[nodiscard]] RendererStats stats() const;
  [[nodiscard]] const char* deviceName() const;
  [[nodiscard]] const char* presentModeName() const;
  [[nodiscard]] VulkanDeviceContext deviceContext() const noexcept;
  // The caller owns the buffer and must create it from deviceContext(). Call only after
  // prepareFrame() (or while no frame is in flight). Passing VK_NULL_HANDLE restores the internal
  // transparent pixel. The buffer contains tightly packed A,R,G,B float32 words.
  void setExternalPixelBuffer(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
  // Records compute/transfer work into the same submission immediately before the UI render pass.
  // The callback must not submit, wait, throw, or retain the command buffer.
  void setBeforeDrawRecorder(VulkanBeforeDrawRecorder recorder, void* context) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace slugvk
