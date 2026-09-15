#include "slugvk/vulkan_renderer.hpp"

#include "slughorn/slughorn.hpp"
#include "slugvk/platform_surface.hpp"
#include "slugvk/vector_atlas.hpp"
#include "slugvk_embedded_shaders.hpp"

#if defined(SLUGVK_ENABLE_GLFW) && SLUGVK_ENABLE_GLFW
#include "slugvk/window.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace slugvk {

namespace {

// UI latency is the primary policy: do not let the CPU build a frame behind another queued frame.
constexpr std::size_t framesInFlight = 1;
constexpr const char* portabilitySubsetExtension = "VK_KHR_portability_subset";
constexpr std::uint32_t analyticRoundedRectShape = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t analyticStrokeSegmentShape = std::numeric_limits<std::uint32_t>::max() - 1U;

#if defined(SLUGVK_ENABLE_GLFW) && SLUGVK_ENABLE_GLFW
class GlfwPlatformSurface final : public PlatformSurface {
public:
  explicit GlfwPlatformSurface(Window& window) : window_(window) {
    std::uint32_t count = 0;
    const char** required = glfwGetRequiredInstanceExtensions(&count);
    if (!required || count == 0)
      throw std::runtime_error("GLFW returned no Vulkan extensions");
    extensions_.assign(required, required + count);
  }

  [[nodiscard]] std::span<const char* const> requiredInstanceExtensions() const noexcept override {
    return extensions_;
  }

  VkResult createVulkanSurface(VkInstance instance, const VkAllocationCallbacks* allocator,
                               VkSurfaceKHR* surface) const noexcept override {
    return glfwCreateWindowSurface(instance, window_.native(), allocator, surface);
  }

  [[nodiscard]] Vec2 framebufferSize() const noexcept override {
    return window_.framebufferSize();
  }

  [[nodiscard]] float contentScale() const noexcept override {
    return window_.contentScale();
  }

  [[nodiscard]] bool visible() const noexcept override {
    const Vec2 size = window_.framebufferSize();
    return size.x > 0.0f && size.y > 0.0f;
  }

  void requestRedraw() noexcept override {
    glfwPostEmptyEvent();
  }

  void waitForVisibleFramebuffer() override {
    window_.waitForVisibleFramebuffer();
  }

private:
  Window& window_;
  std::vector<const char*> extensions_;
};
#endif
constexpr float roundedRectFillCoverage = 0.0f;
constexpr float roundedRectStrokeRingCoverage = 1.0f;
constexpr float roundedRectOpaqueBorderCoverage = 2.0f;
constexpr float roundedRectInnerFillCoverage = -1.0f;
constexpr float paintAlphaEpsilon = 1.0f / 65535.0f;

std::uint32_t packFixed16(float first, float second, float scale, float maximum) {
  const auto quantize = [scale, maximum](float value) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, maximum) * scale));
  };
  return quantize(first) | (quantize(second) << 16U);
}

bool isSrgbFormat(VkFormat format) {
  return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

float srgbToLinear(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f ? value / 12.92f :
    std::pow((value + 0.055f) / 1.055f, 2.4f);
}

bool paintFullyOpaque(const Paint& paint) {
  return paint.opacity * std::min(paint.start.a, paint.end.a) >= 1.0f - paintAlphaEpsilon;
}

bool paintFullyTransparent(const Paint& paint) {
  return paint.opacity <= paintAlphaEpsilon ||
    std::max(paint.start.a, paint.end.a) <= paintAlphaEpsilon;
}

BorderWidths resolvedBorderWidths(const BorderStyle& border) {
  BorderWidths widths = border.resolvedWidths();
  widths.top = std::max(widths.top, 0.0f);
  widths.right = std::max(widths.right, 0.0f);
  widths.bottom = std::max(widths.bottom, 0.0f);
  widths.left = std::max(widths.left, 0.0f);
  return widths;
}

float strokeOutsideFactor(StrokeAlign align) {
  if (align == StrokeAlign::Center) return 0.5f;
  if (align == StrokeAlign::Outside) return 1.0f;
  return 0.0f;
}

Rect strokeBounds(Rect destination, BorderWidths widths, float outsideFactor) {
  const float outsideTop = widths.top * outsideFactor;
  const float outsideRight = widths.right * outsideFactor;
  const float outsideBottom = widths.bottom * outsideFactor;
  const float outsideLeft = widths.left * outsideFactor;
  return {
    destination.x - outsideLeft,
    destination.y - outsideTop,
    destination.width + outsideLeft + outsideRight,
    destination.height + outsideTop + outsideBottom
  };
}

bool sameRoundedRectGeometry(const RoundedRectCommand& first, const RoundedRectCommand& second) {
  const auto close = [](float a, float b) { return std::abs(a - b) <= 0.0001f; };
  const auto sameRect = [&close](Rect a, Rect b) {
    return close(a.x, b.x) && close(a.y, b.y) &&
      close(a.width, b.width) && close(a.height, b.height);
  };
  return sameRect(first.destination, second.destination) && sameRect(first.clip, second.clip) &&
    close(first.radiiPx.topLeft, second.radiiPx.topLeft) &&
    close(first.radiiPx.topRight, second.radiiPx.topRight) &&
    close(first.radiiPx.bottomRight, second.radiiPx.bottomRight) &&
    close(first.radiiPx.bottomLeft, second.radiiPx.bottomLeft) &&
    close(first.continuousCorners.topLeftPercent,
          second.continuousCorners.topLeftPercent) &&
    close(first.continuousCorners.topRightPercent,
          second.continuousCorners.topRightPercent) &&
    close(first.continuousCorners.bottomRightPercent,
          second.continuousCorners.bottomRightPercent) &&
    close(first.continuousCorners.bottomLeftPercent,
          second.continuousCorners.bottomLeftPercent) &&
    first.border.align == second.border.align;
}

Rect intersectRects(Rect first, Rect second) {
  const float left = std::max(first.x, second.x);
  const float top = std::max(first.y, second.y);
  const float right = std::min(first.x + first.width, second.x + second.width);
  const float bottom = std::min(first.y + first.height, second.y + second.height);
  return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

bool sameSolidPaint(const Paint& first, const Paint& second) {
  const auto close = [](float a, float b) { return std::abs(a - b) <= 0.0001f; };
  return first.kind == GradientKind::Solid && second.kind == GradientKind::Solid &&
    close(first.start.r, second.start.r) && close(first.start.g, second.start.g) &&
    close(first.start.b, second.start.b) && close(first.start.a, second.start.a) &&
    close(first.opacity, second.opacity);
}

bool opaqueInsideBorderCovers(const RoundedRectCommand& earlier,
                              const RoundedRectCommand& later) {
  if (earlier.border.align != StrokeAlign::Inside ||
      later.border.align != StrokeAlign::Inside ||
      !paintFullyOpaque(earlier.border.paint) ||
      !sameSolidPaint(earlier.border.paint, later.border.paint)) return false;
  const BorderWidths first = resolvedBorderWidths(earlier.border);
  const BorderWidths second = resolvedBorderWidths(later.border);
  const auto close = [](float a, float b) { return std::abs(a - b) <= 0.0001f; };
  const auto greaterOrEqual = [&close](float a, float b) { return a > b || close(a, b); };
  const float firstRight = earlier.destination.x + earlier.destination.width;
  const float firstBottom = earlier.destination.y + earlier.destination.height;
  const float secondRight = later.destination.x + later.destination.width;
  const float secondBottom = later.destination.y + later.destination.height;

  if (second.top > 0.0f &&
      (!greaterOrEqual(first.top, second.top) ||
       !close(earlier.destination.x, later.destination.x) ||
       !close(firstRight, secondRight) ||
       !close(earlier.destination.y, later.destination.y) ||
       !close(earlier.radiiPx.topLeft, later.radiiPx.topLeft) ||
       !close(earlier.radiiPx.topRight, later.radiiPx.topRight))) return false;
  if (second.bottom > 0.0f &&
      (!greaterOrEqual(first.bottom, second.bottom) ||
       !close(earlier.destination.x, later.destination.x) ||
       !close(firstRight, secondRight) || !close(firstBottom, secondBottom) ||
       !close(earlier.radiiPx.bottomLeft, later.radiiPx.bottomLeft) ||
       !close(earlier.radiiPx.bottomRight, later.radiiPx.bottomRight))) return false;
  if (second.left > 0.0f) {
    if (!greaterOrEqual(first.left, second.left) ||
        !close(earlier.destination.x, later.destination.x) ||
        earlier.destination.y > later.destination.y + 0.0001f ||
        firstBottom + 0.0001f < secondBottom) return false;
    if (close(earlier.destination.y, later.destination.y)) {
      if (!close(earlier.radiiPx.topLeft, later.radiiPx.topLeft)) return false;
    } else if (later.radiiPx.topLeft > 0.0001f) return false;
    if (close(firstBottom, secondBottom)) {
      if (!close(earlier.radiiPx.bottomLeft, later.radiiPx.bottomLeft)) return false;
    } else if (later.radiiPx.bottomLeft > 0.0001f) return false;
  }
  if (second.right > 0.0f) {
    if (!greaterOrEqual(first.right, second.right) || !close(firstRight, secondRight) ||
        earlier.destination.y > later.destination.y + 0.0001f ||
        firstBottom + 0.0001f < secondBottom) return false;
    if (close(earlier.destination.y, later.destination.y)) {
      if (!close(earlier.radiiPx.topRight, later.radiiPx.topRight)) return false;
    } else if (later.radiiPx.topRight > 0.0001f) return false;
    if (close(firstBottom, secondBottom)) {
      if (!close(earlier.radiiPx.bottomRight, later.radiiPx.bottomRight)) return false;
    } else if (later.radiiPx.bottomRight > 0.0001f) return false;
  }
  return second.maximum() > 0.0f;
}

void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    std::cerr << "[Vulkan] " << (data && data->pMessage ? data->pMessage : "validation message")
              << '\n';
  return VK_FALSE;
}

bool hasInstanceExtension(const char* name) {
  std::uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool hasLayer(const char* name) {
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::any_of(layers.begin(), layers.end(),
                     [name](const auto& layer) { return std::strcmp(layer.layerName, name) == 0; });
}

struct QueueFamilies {
  std::optional<std::uint32_t> graphics;
  std::optional<std::uint32_t> present;
  [[nodiscard]] bool complete() const {
    return graphics.has_value() && present.has_value();
  }
};

QueueFamilies findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
  QueueFamilies result;
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      result.graphics = i;
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
    if (present)
      result.present = i;
    if (result.complete())
      break;
  }
  return result;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

SwapchainSupport querySwapchain(VkPhysicalDevice device, VkSurfaceKHR surface) {
  SwapchainSupport result;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &result.capabilities);
  std::uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
  result.formats.resize(count);
  if (count)
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, result.formats.data());
  count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
  result.presentModes.resize(count);
  if (count)
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, result.presentModes.data());
  return result;
}

struct Instance {
  float positionRect[4];
  float emRect[4];
  float bandTransform[4];
  std::uint32_t shapeData[4];
  float color0[4];
  float color1[4];
  float paint[4];
  float gradient[4];
  float clip[4];
  float strokeWidths[4];
};

struct PrimitiveInstance {
  float positionRect[4];
  std::uint16_t color[4];
  std::uint16_t clip[4]; // normalized minX, minY, maxX, maxY
};
static_assert(sizeof(PrimitiveInstance) == 32);

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
};

struct DrawBatch {
  std::uint32_t retainedId = 0;
  bool retainedDrawList = false;
  std::uint32_t firstInstance = 0;
  std::uint32_t instanceCount = 0;
  Vec2 translation = {};
  float scale = 1.0f;
  Rect clip = {};
  float opacity = 1.0f;
  bool primitive = false;
};

// Private lowering target between public DrawList commands and GPU buffers.
// It intentionally separates compact primitives from the full Slug/vector instance stream.
struct RenderIR {
  std::vector<Instance> instances{};
  std::vector<PrimitiveInstance> primitives{};
  std::vector<DrawBatch> batches{};

  void clear() {
    instances.clear();
    primitives.clear();
    batches.clear();
  }
};

struct alignas(16) PushConstants {
  float viewportScale[4];
  float translationOverride[4];
  float overrideClip[4];
  float opacity[4];
};
static_assert(sizeof(PushConstants) == 64);

struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

} // namespace

struct VulkanRenderer::Impl {
  std::unique_ptr<PlatformSurface> ownedPlatformSurface;
  PlatformSurface& platformSurface;
  const VectorAtlas& vectorAtlas;
  RendererConfig config;
  RendererStats statistics{};
  Buffer readback{};
  bool captureFrame = false;
  FramePixels captured{};

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties deviceProperties{};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  VkQueue presentQueue = VK_NULL_HANDLE;
  QueueFamilies queueFamilies{};

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkExtent2D extent{};
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainViews;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VkSemaphore> renderFinished;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipeline primitivePipeline = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  Texture curveTexture{};
  Texture bandTexture{};

  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, framesInFlight> commandBuffers{};
  struct Frame {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    bool timestampsWritten = false;
    bool writeTimestamps = false;
    Buffer instances{};
    Buffer primitiveInstances{};
  };
  std::array<Frame, framesInFlight> frames{};
  std::size_t currentFrame = 0;
  RenderIR frameIR{};
  struct GlyphRunKey {
    std::string text;
    std::string fontName;
    std::uint16_t weight = 400;
    bool italic = false;
    bool operator==(const GlyphRunKey&) const = default;
  };
  struct GlyphRunKeyHash {
    std::size_t operator()(const GlyphRunKey& key) const noexcept {
      std::size_t hash = std::hash<std::string>{}(key.text);
      const auto combine = [&hash](std::size_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
      };
      combine(std::hash<std::string>{}(key.fontName));
      combine(key.weight);
      combine(key.italic ? 1U : 0U);
      return hash;
    }
  };
  struct RetainedGeometry {
    VkDeviceSize offset = 0;
    VkDeviceSize capacity = 0;
    std::vector<Instance> shadow{};
    std::uint32_t instanceCount = 0;
  };
  struct FreeRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
  };
  Buffer retainedArena{};
  VkDeviceSize retainedArenaUsed = 0;
  std::vector<FreeRange> retainedFreeRanges{};
  std::vector<RetainedGeometry> retainedTexts{};
  std::vector<RetainedGeometry> retainedDrawLists{};
  struct CachedGlyph {
    bool present = false;
    slughorn::Atlas::Shape shape{};
  };
  struct CachedGlyphRun {
    std::vector<CachedGlyph> glyphs{};
    std::uint64_t lastUsed = 0;
  };
  mutable std::unordered_map<GlyphRunKey, CachedGlyphRun, GlyphRunKeyHash> glyphRunCache{};
  mutable std::uint64_t glyphRunUseCounter = 0;
  std::uint32_t timestampValidBits = 0;
  std::uint64_t submittedFrameCount = 0;
  bool framePrepared = false;
  std::uint32_t preparedImageIndex = 0;
  VkResult preparedAcquireResult = VK_SUCCESS;

  Impl(PlatformSurface& inputSurface, const VectorAtlas& atlas, RendererConfig inputConfig)
      : platformSurface(inputSurface), vectorAtlas(atlas), config(inputConfig) {
    initialize();
  }

  Impl(std::unique_ptr<PlatformSurface> inputSurface, const VectorAtlas& atlas,
       RendererConfig inputConfig)
      : ownedPlatformSurface(std::move(inputSurface)), platformSurface(*ownedPlatformSurface),
        vectorAtlas(atlas), config(inputConfig) {
    initialize();
  }

  void initialize() {
    try {
      if (!vectorAtlas.built())
        throw std::invalid_argument("VectorAtlas::build() must be called first");
      createInstance();
      createSurface();
      pickPhysicalDevice();
      createDevice();
      createCommandPool();
      createTextures();
      createDescriptorResources();
      createSwapchainResources();
      createFrames();
    } catch (...) {
      destroy();
      throw;
    }
  }

  ~Impl() {
    destroy();
  }

  void createInstance() {
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
#ifndef SLUGVK_ENABLE_VALIDATION
    config.validation = false;
#endif
    config.validation = config.validation && hasLayer(validationLayer);

    const auto requiredExtensions = platformSurface.requiredInstanceExtensions();
    if (requiredExtensions.empty())
      throw std::runtime_error("PlatformSurface returned no Vulkan extensions");
    std::vector<const char*> extensions(requiredExtensions.begin(), requiredExtensions.end());
    if (config.validation && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateFlags flags = 0;
    if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "SlugVulkan";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "SlugVulkan";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.flags = flags;
    createInfo.pApplicationInfo = &application;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    if (config.validation) {
      createInfo.enabledLayerCount = 1;
      createInfo.ppEnabledLayerNames = &validationLayer;
      createInfo.pNext = &debugInfo;
    }
    check(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");

    if (config.validation) {
      auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
      if (create)
        check(create(instance, &debugInfo, nullptr, &debugMessenger),
              "vkCreateDebugUtilsMessengerEXT");
    }
  }

  void createSurface() {
    check(platformSurface.createVulkanSurface(instance, nullptr, &surface),
          "PlatformSurface::createVulkanSurface");
  }

  void pickPhysicalDevice() {
    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (!count)
      throw std::runtime_error("No Vulkan physical device was found");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
      const auto families = findQueueFamilies(candidate, surface);
      if (!families.complete() || !hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        continue;
      const auto support = querySwapchain(candidate, surface);
      if (support.formats.empty() || support.presentModes.empty())
        continue;
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      int score = static_cast<int>(properties.limits.maxImageDimension2D);
      if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 100000;
      if (score > bestScore) {
        bestScore = score;
        physicalDevice = candidate;
        queueFamilies = families;
        deviceProperties = properties;
      }
    }
    if (physicalDevice == VK_NULL_HANDLE)
      throw std::runtime_error("No present-capable Vulkan device supports VK_KHR_swapchain");
  }

  void createDevice() {
    std::set<std::uint32_t> uniqueFamilies{*queueFamilies.graphics, *queueFamilies.present};
    std::vector<VkDeviceQueueCreateInfo> queues;
    const float priority = 1.0f;
    for (auto family : uniqueFamilies) {
      VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue.queueFamilyIndex = family;
      queue.queueCount = 1;
      queue.pQueuePriorities = &priority;
      queues.push_back(queue);
    }
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (hasDeviceExtension(physicalDevice, portabilitySubsetExtension))
      extensions.push_back(portabilitySubsetExtension);

    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
    createInfo.pQueueCreateInfos = queues.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.pEnabledFeatures = &features;
    check(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, *queueFamilies.graphics, 0, &graphicsQueue);
    vkGetDeviceQueue(device, *queueFamilies.present, 0, &presentQueue);
  }

  void createCommandPool() {
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = *queueFamilies.graphics;
    check(vkCreateCommandPool(device, &info, nullptr, &commandPool), "vkCreateCommandPool");
  }

  std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No compatible Vulkan memory type");
  }

  void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                    Buffer& output, bool map) {
    output.size = std::max<VkDeviceSize>(size, 4);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = output.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &bufferInfo, nullptr, &output.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, output.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits, properties);
    check(vkAllocateMemory(device, &allocate, nullptr, &output.memory), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(device, output.buffer, output.memory, 0), "vkBindBufferMemory");
    if (map)
      check(vkMapMemory(device, output.memory, 0, output.size, 0, &output.mapped), "vkMapMemory");
  }

  void destroyBuffer(Buffer& buffer) {
    if (buffer.mapped)
      vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer)
      vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory)
      vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
  }

  VkCommandBuffer beginSingleUse() {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &allocate, &command),
          "vkAllocateCommandBuffers(upload)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(upload)");
    return command;
  }

  void endSingleUse(VkCommandBuffer command) {
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer(upload)");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(upload)");
    check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle(upload)");
    vkFreeCommandBuffers(device, commandPool, 1, &command);
  }

  VkFormat atlasTextureFormat(slughorn::Atlas::TextureData::Format format) const {
    using Format = slughorn::Atlas::TextureData::Format;
    switch (format) {
      case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
      case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
      case Format::RGBA16UI: return VK_FORMAT_R16G16B16A16_UINT;
      case Format::RG16UI: return VK_FORMAT_R16G16_UINT;
      case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
      case Format::RGB32F: break;
    }
    throw std::runtime_error("Unsupported Slug atlas texture format");
  }

  void createTexture(const slughorn::Atlas::TextureData& source, Texture& output) {
    const VkFormat format = atlasTextureFormat(source.format);
    if (source.empty() || source.width == 0 || source.height == 0)
      throw std::runtime_error("Slug atlas returned an empty texture");
    Buffer staging{};
    createBuffer(source.bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, true);
    std::memcpy(staging.mapped, source.bytes.data(), source.bytes.size());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {source.width, source.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(device, &imageInfo, nullptr, &output.image), "vkCreateImage(atlas)");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, output.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &allocate, nullptr, &output.memory), "vkAllocateMemory(image)");
    check(vkBindImageMemory(device, output.image, output.memory, 0), "vkBindImageMemory");

    VkCommandBuffer command = beginSingleUse();
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = output.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {source.width, source.height, 1};
    vkCmdCopyBufferToImage(command, staging.buffer, output.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = output.image;
    toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toShader);
    endSingleUse(command);
    destroyBuffer(staging);

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = output.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &output.view), "vkCreateImageView(atlas)");
  }

  void destroyTexture(Texture& texture) {
    if (texture.view)
      vkDestroyImageView(device, texture.view, nullptr);
    if (texture.image)
      vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory)
      vkFreeMemory(device, texture.memory, nullptr);
    texture = {};
  }

  void createTextures() {
    const auto& atlas = vectorAtlas.native();
    createTexture(atlas.getCurveTextureData(), curveTexture);
    createTexture(atlas.getBandTextureData(), bandTexture);
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.maxLod = 0.0f;
    check(vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler");
  }

  void createDescriptorResources() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &descriptorSetLayout),
          "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptorSetLayout;
    check(vkAllocateDescriptorSets(device, &allocate, &descriptorSet), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo curve{sampler, curveTexture.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo band{sampler, bandTexture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = descriptorSet;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    writes[0].pImageInfo = &curve;
    writes[1].pImageInfo = &band;
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
  }

  VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const auto& format : formats)
      if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return format;
    for (const auto& format : formats)
      if (format.format == VK_FORMAT_R8G8B8A8_SRGB &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return format;
    return formats.front();
  }

  VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    if (!config.vsync) {
      if (config.allowTearing)
        for (auto mode : modes)
          if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            return mode;
      for (auto mode : modes)
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
          return mode;
      for (auto mode : modes)
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
          return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }

  VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
      return capabilities.currentExtent;
    const Vec2 framebuffer = platformSurface.framebufferSize();
    return {std::clamp(static_cast<std::uint32_t>(std::max(framebuffer.x, 1.0f)),
                       capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(static_cast<std::uint32_t>(std::max(framebuffer.y, 1.0f)),
                       capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
  }

  void createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapchainFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    check(vkCreateRenderPass(device, &info, nullptr, &renderPass), "vkCreateRenderPass");
  }

  VkShaderModule createShaderModule(const unsigned char* bytes, std::size_t byteCount) {
    if (byteCount % 4 != 0)
      throw std::runtime_error("Invalid SPIR-V byte length");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = byteCount;
    info.pCode = reinterpret_cast<const std::uint32_t*>(bytes);
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
  }

  void createPipeline() {
    VkShaderModule vertexModule =
        createShaderModule(embedded::vectorVert, embedded::vectorVertSize);
    VkShaderModule fragmentModule =
        createShaderModule(embedded::vectorFrag, embedded::vectorFragSize);
    VkShaderModule primitiveVertexModule =
        createShaderModule(embedded::primitiveVert, embedded::primitiveVertSize);
    VkShaderModule primitiveFragmentModule =
        createShaderModule(embedded::primitiveFrag, embedded::primitiveFragSize);
    VkPipelineShaderStageCreateInfo vertexStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexModule;
    vertexStage.pName = "main";
    VkPipelineShaderStageCreateInfo fragmentStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentModule;
    fragmentStage.pName = "main";
    const std::array stages{vertexStage, fragmentStage};

    VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
    std::array<VkVertexInputAttributeDescription, 10> attributes{{
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, positionRect)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, emRect)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, bandTransform)},
      {3, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(Instance, shapeData)},
      {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color0)},
      {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color1)},
      {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, paint)},
      {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, gradient)},
      {8, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, clip)},
      {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, strokeWidths)}
    }};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptorSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout),
          "vkCreatePipelineLayout");
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
          "vkCreateGraphicsPipelines(vector)");

    VkPipelineShaderStageCreateInfo primitiveVertexStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    primitiveVertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    primitiveVertexStage.module = primitiveVertexModule;
    primitiveVertexStage.pName = "main";
    VkPipelineShaderStageCreateInfo primitiveFragmentStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    primitiveFragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    primitiveFragmentStage.module = primitiveFragmentModule;
    primitiveFragmentStage.pName = "main";
    const std::array primitiveStages{primitiveVertexStage, primitiveFragmentStage};
    VkVertexInputBindingDescription primitiveBinding{
      0, sizeof(PrimitiveInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
    std::array<VkVertexInputAttributeDescription, 3> primitiveAttributes{{
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PrimitiveInstance, positionRect)},
      {1, 0, VK_FORMAT_R16G16B16A16_UNORM, offsetof(PrimitiveInstance, color)},
      {2, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(PrimitiveInstance, clip)}
    }};
    VkPipelineVertexInputStateCreateInfo primitiveVertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    primitiveVertexInput.vertexBindingDescriptionCount = 1;
    primitiveVertexInput.pVertexBindingDescriptions = &primitiveBinding;
    primitiveVertexInput.vertexAttributeDescriptionCount =
      static_cast<std::uint32_t>(primitiveAttributes.size());
    primitiveVertexInput.pVertexAttributeDescriptions = primitiveAttributes.data();
    pipelineInfo.pStages = primitiveStages.data();
    pipelineInfo.pVertexInputState = &primitiveVertexInput;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                    &primitivePipeline),
          "vkCreateGraphicsPipelines(primitive)");

    vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr);
    vkDestroyShaderModule(device, primitiveVertexModule, nullptr);
    vkDestroyShaderModule(device, primitiveFragmentModule, nullptr);
  }

  void createSwapchainResources(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE) {
    platformSurface.waitForVisibleFramebuffer();
    const auto support = querySwapchain(physicalDevice, surface);
    const auto chosenFormat = chooseFormat(support.formats);
    extent = chooseExtent(support.capabilities);
    swapchainPresentMode = choosePresentMode(support.presentModes);
    // Absolute-latency IMMEDIATE uses the smallest legal swapchain. MAILBOX keeps one additional
    // replacement image so the presentation engine can always retain only the newest frame.
    std::uint32_t imageCount = support.capabilities.minImageCount +
                               (swapchainPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ? 1U : 0U);
    if (support.capabilities.maxImageCount && imageCount > support.capabilities.maxImageCount)
      imageCount = support.capabilities.maxImageCount;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosenFormat.format;
    info.imageColorSpace = chosenFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (config.enableReadback) {
      if (!(support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Surface does not support readback");
      info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    const std::array families{*queueFamilies.graphics, *queueFamilies.present};
    if (queueFamilies.graphics != queueFamilies.present) {
      info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      info.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
      info.pQueueFamilyIndices = families.data();
    } else
      info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = support.capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = swapchainPresentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;
    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(device, &info, nullptr, &newSwapchain), "vkCreateSwapchainKHR");
    swapchain = newSwapchain;
    if (oldSwapchain)
      vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    const bool formatChanged =
        swapchainFormat != VK_FORMAT_UNDEFINED && swapchainFormat != chosenFormat.format;
    if (formatChanged) {
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (primitivePipeline)
        vkDestroyPipeline(device, primitivePipeline, nullptr);
      if (pipelineLayout)
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      if (renderPass)
        vkDestroyRenderPass(device, renderPass, nullptr);
      pipeline = VK_NULL_HANDLE;
      primitivePipeline = VK_NULL_HANDLE;
      pipelineLayout = VK_NULL_HANDLE;
      renderPass = VK_NULL_HANDLE;
    }
    swapchainFormat = chosenFormat.format;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
    swapchainViews.resize(imageCount);
    for (std::size_t i = 0; i < imageCount; ++i) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = swapchainImages[i];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = swapchainFormat;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      check(vkCreateImageView(device, &view, nullptr, &swapchainViews[i]),
            "vkCreateImageView(swapchain)");
    }
    if (!renderPass) {
      createRenderPass();
      createPipeline();
    }
    framebuffers.resize(imageCount);
    for (std::size_t i = 0; i < imageCount; ++i) {
      VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer.renderPass = renderPass;
      framebuffer.attachmentCount = 1;
      framebuffer.pAttachments = &swapchainViews[i];
      framebuffer.width = extent.width;
      framebuffer.height = extent.height;
      framebuffer.layers = 1;
      check(vkCreateFramebuffer(device, &framebuffer, nullptr, &framebuffers[i]),
            "vkCreateFramebuffer");
    }
    renderFinished.resize(imageCount);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& value : renderFinished)
      check(vkCreateSemaphore(device, &semaphore, nullptr, &value), "vkCreateSemaphore(present)");
  }

  void destroySwapchainImages(bool destroySwapchain) {
    for (auto semaphore : renderFinished)
      if (semaphore)
        vkDestroySemaphore(device, semaphore, nullptr);
    renderFinished.clear();
    for (auto framebuffer : framebuffers)
      vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    for (auto view : swapchainViews)
      vkDestroyImageView(device, view, nullptr);
    swapchainViews.clear();
    swapchainImages.clear();
    if (destroySwapchain && swapchain) {
      vkDestroySwapchainKHR(device, swapchain, nullptr);
      swapchain = VK_NULL_HANDLE;
    }
  }

  void destroySwapchainResources() {
    destroySwapchainImages(true);
    if (pipeline)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (primitivePipeline)
      vkDestroyPipeline(device, primitivePipeline, nullptr);
    if (pipelineLayout)
      vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (renderPass)
      vkDestroyRenderPass(device, renderPass, nullptr);
    pipeline = VK_NULL_HANDLE;
    primitivePipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
  }

  void recreateSwapchain() {
    platformSurface.waitForVisibleFramebuffer();
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(resize)");
    const VkSwapchainKHR oldSwapchain = swapchain;
    destroySwapchainImages(false);
    swapchain = VK_NULL_HANDLE;
    createSwapchainResources(oldSwapchain);
    framePrepared = false;
  }

  void createFrames() {
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = commandPool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = static_cast<std::uint32_t>(commandBuffers.size());
    check(vkAllocateCommandBuffers(device, &command, commandBuffers.data()),
          "vkAllocateCommandBuffers(frames)");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    std::uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queueProperties.data());
    timestampValidBits =
        queueFamilies.graphics ? queueProperties[*queueFamilies.graphics].timestampValidBits : 0;
    const bool timestampsSupported = config.gpuTimingInterval > 0 && timestampValidBits > 0 &&
                                     deviceProperties.limits.timestampPeriod > 0.0f;
    const std::size_t initialCapacity = std::max<std::size_t>(1, config.initialVertexCapacity);
    for (auto& frame : frames) {
      check(vkCreateSemaphore(device, &semaphore, nullptr, &frame.imageAvailable),
            "vkCreateSemaphore(acquire)");
      check(vkCreateFence(device, &fence, nullptr, &frame.fence), "vkCreateFence");
      createBuffer(initialCapacity * sizeof(Instance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   frame.instances, true);
      createBuffer(initialCapacity * sizeof(PrimitiveInstance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   frame.primitiveInstances, true);
      if (timestampsSupported) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = 2;
        check(vkCreateQueryPool(device, &query, nullptr, &frame.timestamps),
              "vkCreateQueryPool(timestamp)");
      }
    }
  }

  void appendResolvedShape(std::vector<Instance>& instances, const slughorn::Atlas::Shape& shape,
                           Rect destination, const Paint& paintValue, Rect clip,
                           float italicShear = 0.0f) const {
    if (shape.width <= 0 || shape.height <= 0 || destination.width <= 0 || destination.height <= 0)
      return;
    constexpr float padding = 1.25f;
    const float emPaddingX = static_cast<float>(shape.width) / destination.width * padding;
    const float emPaddingY = static_cast<float>(shape.height) / destination.height * padding;
    const float emLeft = static_cast<float>(shape.bearingX) - emPaddingX;
    const float emRight = static_cast<float>(shape.bearingX + shape.width) + emPaddingX;
    const float emTop = static_cast<float>(shape.bearingY) + emPaddingY;
    const float emBottom = static_cast<float>(shape.bearingY - shape.height) - emPaddingY;
    const float left = destination.x - padding;
    const float right = destination.x + destination.width + padding;
    const float top = destination.y - padding;
    const float bottom = destination.y + destination.height + padding;
    const float shear = italicShear * destination.height;
    if (right + std::max(shear, 0.0f) <= clip.x ||
        left + std::min(shear, 0.0f) >= clip.x + clip.width || bottom <= clip.y ||
        top >= clip.y + clip.height)
      return;

    Instance quad{};
    const float positionRect[4]{left, top, right, bottom};
    const float emRect[4]{emLeft, emTop, emRight, emBottom};
    std::copy(std::begin(positionRect), std::end(positionRect), quad.positionRect);
    std::copy(std::begin(emRect), std::end(emRect), quad.emRect);
    quad.bandTransform[0] = static_cast<float>(shape.bandScaleX);
    quad.bandTransform[1] = static_cast<float>(shape.bandScaleY);
    quad.bandTransform[2] = static_cast<float>(shape.bandOffsetX);
    quad.bandTransform[3] = static_cast<float>(shape.bandOffsetY);
    quad.shapeData[0] = shape.bandTexX;
    quad.shapeData[1] = shape.bandTexY;
    quad.shapeData[2] = shape.bandMaxX;
    quad.shapeData[3] = shape.bandMaxY;
    const float first[4]{paintValue.start.r, paintValue.start.g, paintValue.start.b,
                         paintValue.start.a};
    const float second[4]{paintValue.end.r, paintValue.end.g, paintValue.end.b, paintValue.end.a};
    std::copy(std::begin(first), std::end(first), quad.color0);
    std::copy(std::begin(second), std::end(second), quad.color1);
    quad.paint[0] = static_cast<float>(paintValue.kind);
    quad.paint[1] = paintValue.opacity;
    quad.paint[2] = paintValue.shaderParameter;
    quad.paint[3] = shear;
    quad.gradient[0] = paintValue.origin.x;
    quad.gradient[1] = paintValue.origin.y;
    quad.gradient[2] = paintValue.target.x;
    quad.gradient[3] = paintValue.target.y;
    quad.clip[0] = clip.x;
    quad.clip[1] = clip.y;
    quad.clip[2] = clip.width;
    quad.clip[3] = clip.height;
    instances.push_back(quad);
  }

  void appendShape(std::vector<Instance>& instances, ShapeId id, Rect destination,
                   const Paint& paintValue, Rect clip, float italicShear = 0.0f) const {
    const auto shape = vectorAtlas.native().getShape(slughorn::Key(id));
    if (shape)
      appendResolvedShape(instances, *shape, destination, paintValue, clip, italicShear);
  }

  static std::uint16_t packUnorm16(float value) noexcept {
    return static_cast<std::uint16_t>(std::lround(
      std::clamp(value, 0.0f, 1.0f) * 65535.0f));
  }

  [[nodiscard]] std::uint16_t packClipCoordinate(float value, std::uint32_t dimension) const noexcept {
    if (dimension == 0) return 0;
    const float normalized = std::clamp(value / static_cast<float>(dimension), 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
  }

  static bool integralPixel(float value) noexcept {
    return std::isfinite(value) && std::abs(value - std::nearbyint(value)) <= 0.0001f;
  }

  [[nodiscard]] bool compactRectangleCompatible(const RoundedRectCommand& command) const noexcept {
    const auto& color = command.paint.start;
    const auto widths = resolvedBorderWidths(command.border);
    const bool borderInvisible = widths.maximum() <= 0.0f || paintFullyTransparent(command.border.paint);
    const bool square = command.radiiPx.topLeft <= 0.0001f &&
      command.radiiPx.topRight <= 0.0001f && command.radiiPx.bottomRight <= 0.0001f &&
      command.radiiPx.bottomLeft <= 0.0001f;
    const bool unitColor = color.r >= 0.0f && color.r <= 1.0f &&
      color.g >= 0.0f && color.g <= 1.0f && color.b >= 0.0f && color.b <= 1.0f &&
      color.a >= 0.0f && color.a <= 1.0f && command.paint.opacity >= 0.0f &&
      command.paint.opacity <= 1.0f && command.opacity >= 0.0f && command.opacity <= 1.0f;
    const float right = command.destination.x + command.destination.width;
    const float bottom = command.destination.y + command.destination.height;
    return command.destination.width > 0.0f && command.destination.height > 0.0f &&
      command.paint.kind == GradientKind::Solid && borderInvisible && square && unitColor &&
      integralPixel(command.destination.x) && integralPixel(command.destination.y) &&
      integralPixel(right) && integralPixel(bottom);
  }

  void appendCompactRectangle(std::vector<PrimitiveInstance>& primitives,
                              const RoundedRectCommand& command) const {
    const float right = command.destination.x + command.destination.width;
    const float bottom = command.destination.y + command.destination.height;
    const float clipRight = command.clip.x + command.clip.width;
    const float clipBottom = command.clip.y + command.clip.height;
    if (right <= command.clip.x || command.destination.x >= clipRight ||
        bottom <= command.clip.y || command.destination.y >= clipBottom) return;
    PrimitiveInstance primitive{};
    primitive.positionRect[0] = command.destination.x;
    primitive.positionRect[1] = command.destination.y;
    primitive.positionRect[2] = right;
    primitive.positionRect[3] = bottom;
    const Color color = command.paint.start;
    primitive.color[0] = packUnorm16(color.r);
    primitive.color[1] = packUnorm16(color.g);
    primitive.color[2] = packUnorm16(color.b);
    primitive.color[3] = packUnorm16(color.a * command.paint.opacity * command.opacity);
    const float leftClip = std::clamp(command.clip.x, 0.0f, static_cast<float>(extent.width));
    const float topClip = std::clamp(command.clip.y, 0.0f, static_cast<float>(extent.height));
    const float rightClip = std::clamp(clipRight, leftClip, static_cast<float>(extent.width));
    const float bottomClip = std::clamp(clipBottom, topClip, static_cast<float>(extent.height));
    primitive.clip[0] = packClipCoordinate(leftClip, extent.width);
    primitive.clip[1] = packClipCoordinate(topClip, extent.height);
    primitive.clip[2] = packClipCoordinate(rightClip, extent.width);
    primitive.clip[3] = packClipCoordinate(bottomClip, extent.height);
    primitives.push_back(primitive);
  }

  void appendRoundedRectInstance(std::vector<Instance>& instances,
                                 const RoundedRectCommand& command, Rect destination,
                                 const Paint& paintValue, BorderWidths strokeWidths,
                                 float outsetFactor, float coverageMode) const {
    if (destination.width <= 0.0f || destination.height <= 0.0f) return;
    constexpr float padding = 1.25f;
    const float left = destination.x - padding;
    const float top = destination.y - padding;
    const float right = destination.x + destination.width + padding;
    const float bottom = destination.y + destination.height + padding;
    if (right <= command.clip.x || left >= command.clip.x + command.clip.width ||
        bottom <= command.clip.y || top >= command.clip.y + command.clip.height)
      return;

    Instance quad{};
    const float positionRect[4]{left, top, right, bottom};
    const float emRect[4]{-padding, -padding, destination.width + padding,
                          destination.height + padding};
    std::copy(std::begin(positionRect), std::end(positionRect), quad.positionRect);
    std::copy(std::begin(emRect), std::end(emRect), quad.emRect);
    std::array<float, 4> radii{
        std::max(command.radiiPx.topLeft, 0.0f), std::max(command.radiiPx.topRight, 0.0f),
        std::max(command.radiiPx.bottomRight, 0.0f), std::max(command.radiiPx.bottomLeft, 0.0f)};
    // CSS-compatible normalization is used only when requested absolute radii cannot physically
    // fit. Otherwise every corner remains the exact authored pixel radius.
    float radiusScale = 1.0f;
    const auto fit = [&radiusScale](float available, float sum) {
      if (sum > 0.0f)
        radiusScale = std::min(radiusScale, available / sum);
    };
    fit(command.destination.width, radii[0] + radii[1]);
    fit(command.destination.width, radii[3] + radii[2]);
    fit(command.destination.height, radii[0] + radii[3]);
    fit(command.destination.height, radii[1] + radii[2]);
    for (float& radius : radii) radius *= std::clamp(radiusScale, 0.0f, 1.0f);
    const auto resolvedSmoothing = [&](float authored) {
      return config.enableContinuousCorners
               ? std::clamp(authored, 0.0f, 100.0f)
               : 0.0f;
    };
    const std::array<float, 4> smoothing{
        resolvedSmoothing(command.continuousCorners.topLeftPercent),
        resolvedSmoothing(command.continuousCorners.topRightPercent),
        resolvedSmoothing(command.continuousCorners.bottomRightPercent),
        resolvedSmoothing(command.continuousCorners.bottomLeftPercent)};
    quad.bandTransform[0] = destination.width;
    quad.bandTransform[1] = destination.height;
    quad.bandTransform[2] = coverageMode;
    quad.bandTransform[3] = std::clamp(outsetFactor, 0.0f, 1.0f);
    quad.shapeData[0] = analyticRoundedRectShape;
    // Four radii and four smoothing percentages remain compactly packed. The separate side-width
    // vector is zero for ordinary Slug glyph/shape instances and avoids extra draw calls.
    quad.shapeData[1] = packFixed16(radii[0], radii[1], 16.0f, 4095.9375f);
    quad.shapeData[2] = packFixed16(radii[2], radii[3], 16.0f, 4095.9375f);
    quad.shapeData[3] = packFixed16(smoothing[0], smoothing[1], 256.0f, 100.0f);
    const float first[4]{paintValue.start.r, paintValue.start.g,
                         paintValue.start.b, paintValue.start.a};
    const float second[4]{paintValue.end.r, paintValue.end.g,
                          paintValue.end.b, paintValue.end.a};
    std::copy(std::begin(first), std::end(first), quad.color0);
    std::copy(std::begin(second), std::end(second), quad.color1);
    quad.paint[0] = static_cast<float>(paintValue.kind);
    quad.paint[1] = paintValue.opacity;
    quad.paint[2] = paintValue.shaderParameter;
    quad.paint[3] =
      std::bit_cast<float>(packFixed16(smoothing[2], smoothing[3], 256.0f, 100.0f));
    quad.gradient[0] = paintValue.origin.x;
    quad.gradient[1] = paintValue.origin.y;
    quad.gradient[2] = paintValue.target.x;
    quad.gradient[3] = paintValue.target.y;
    quad.clip[0] = command.clip.x;
    quad.clip[1] = command.clip.y;
    quad.clip[2] = command.clip.width;
    quad.clip[3] = command.clip.height;
    quad.strokeWidths[0] = std::max(strokeWidths.top, 0.0f);
    quad.strokeWidths[1] = std::max(strokeWidths.right, 0.0f);
    quad.strokeWidths[2] = std::max(strokeWidths.bottom, 0.0f);
    quad.strokeWidths[3] = std::max(strokeWidths.left, 0.0f);
    instances.push_back(quad);
  }

  void appendOpaqueBorder(std::vector<Instance>& instances,
                          const RoundedRectCommand& command,
                          BorderWidths widths, float outsideFactor) const {
    if (widths.maximum() <= 0.0f || !paintFullyOpaque(command.border.paint)) return;
    appendRoundedRectInstance(instances, command,
                              strokeBounds(command.destination, widths, outsideFactor),
                              command.border.paint, widths, outsideFactor,
                              roundedRectOpaqueBorderCoverage);
  }

  void appendRoundedRect(std::vector<Instance>& instances,
                         const RoundedRectCommand& command) const {
    const BorderWidths widths = resolvedBorderWidths(command.border);
    if (widths.maximum() <= 0.0f || paintFullyTransparent(command.border.paint)) {
      if (!paintFullyTransparent(command.paint))
        appendRoundedRectInstance(instances, command, command.destination, command.paint, {},
                                  0.0f, roundedRectFillCoverage);
      return;
    }

    const float outsideFactor = strokeOutsideFactor(command.border.align);
    const Rect borderBounds = strokeBounds(command.destination, widths, outsideFactor);
    if (paintFullyOpaque(command.border.paint)) {
      // Opaque vector paints share one outside coverage domain: border first, then an inner fill.
      // This prevents the full-size fill from leaking through the border's AA pixels.
      appendOpaqueBorder(instances, command, widths, outsideFactor);
      if (!paintFullyTransparent(command.paint))
        appendRoundedRectInstance(instances, command, command.destination, command.paint, widths,
                                  outsideFactor, roundedRectInnerFillCoverage);
    } else {
      // A translucent stroke intentionally reveals the fill below and retains source-over order.
      if (!paintFullyTransparent(command.paint))
        appendRoundedRectInstance(instances, command, command.destination, command.paint, {},
                                  0.0f, roundedRectFillCoverage);
      appendRoundedRectInstance(instances, command, borderBounds, command.border.paint, widths,
                                outsideFactor, roundedRectStrokeRingCoverage);
    }
  }

  void appendOpaqueRoundedRectStack(std::vector<Instance>& instances,
                                    const std::vector<DisplayCommand>& commands,
                                    std::size_t begin, std::size_t end) const {
    const auto& base = std::get<RoundedRectCommand>(commands[begin]);
    const float outsideFactor = strokeOutsideFactor(base.border.align);
    BorderWidths combined{};
    for (std::size_t index = begin; index < end; ++index) {
      const auto& command = std::get<RoundedRectCommand>(commands[index]);
      const BorderWidths widths = resolvedBorderWidths(command.border);
      if (!paintFullyOpaque(command.border.paint) || widths.maximum() <= 0.0f) continue;
      combined.top = std::max(combined.top, widths.top);
      combined.right = std::max(combined.right, widths.right);
      combined.bottom = std::max(combined.bottom, widths.bottom);
      combined.left = std::max(combined.left, widths.left);

      bool coveredByEarlierEdge = false;
      for (std::size_t earlierIndex = 0; earlierIndex < index; ++earlierIndex) {
        const auto* earlier = std::get_if<RoundedRectCommand>(&commands[earlierIndex]);
        if (earlier && opaqueInsideBorderCovers(*earlier, command)) {
          coveredByEarlierEdge = true;
          break;
        }
      }
      if (coveredByEarlierEdge) continue;

      RoundedRectCommand visible = command;
      if (command.border.align == StrokeAlign::Inside) {
        BorderWidths later{};
        for (std::size_t laterIndex = index + 1; laterIndex < end; ++laterIndex) {
          const auto& overlay = std::get<RoundedRectCommand>(commands[laterIndex]);
          if (!paintFullyOpaque(overlay.border.paint)) continue;
          const BorderWidths overlayWidths = resolvedBorderWidths(overlay.border);
          later.top = std::max(later.top, overlayWidths.top);
          later.right = std::max(later.right, overlayWidths.right);
          later.bottom = std::max(later.bottom, overlayWidths.bottom);
          later.left = std::max(later.left, overlayWidths.left);
        }
        const Rect owner{
          command.destination.x + later.left,
          command.destination.y + later.top,
          std::max(0.0f, command.destination.width - later.left - later.right),
          std::max(0.0f, command.destination.height - later.top - later.bottom)
        };
        visible.clip = intersectRects(command.clip, owner);
      }
      appendOpaqueBorder(instances, visible, widths, outsideFactor);
    }
    // Figma commonly represents different side paints as coincident rectangles. Their opaque
    // border masks are emitted in painter order, while the one visible fill is inset by the union
    // of all four side widths and emitted once. The outer AA therefore cannot double-composite.
    if (!paintFullyTransparent(base.paint))
      appendRoundedRectInstance(instances, base, base.destination, base.paint, combined,
                                outsideFactor, roundedRectInnerFillCoverage);
  }

  void appendStrokeSegment(std::vector<Instance>& instances, Vec2 from, Vec2 to,
                           const CubicBezierCommand& command, bool firstSegment,
                           bool lastSegment, Vec2 startPartition, Vec2 endPartition) const {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    if (dx * dx + dy * dy <= 1.0e-8f) return;

    const float padding = command.style.width * 2.0f + 1.5f;
    const float left = std::min(from.x, to.x) - padding;
    const float top = std::min(from.y, to.y) - padding;
    const float right = std::max(from.x, to.x) + padding;
    const float bottom = std::max(from.y, to.y) + padding;
    if (right <= command.clip.x || left >= command.clip.x + command.clip.width ||
        bottom <= command.clip.y || top >= command.clip.y + command.clip.height) return;

    Instance quad{};
    const float bounds[4]{left, top, right, bottom};
    std::copy(std::begin(bounds), std::end(bounds), quad.positionRect);
    std::copy(std::begin(bounds), std::end(bounds), quad.emRect);
    quad.bandTransform[0] = from.x;
    quad.bandTransform[1] = from.y;
    quad.bandTransform[2] = to.x;
    quad.bandTransform[3] = to.y;
    quad.shapeData[0] = analyticStrokeSegmentShape;
    const bool roundStart = firstSegment && command.style.cap == LineCap::Round;
    const bool roundEnd = lastSegment && command.style.cap == LineCap::Round;
    quad.shapeData[1] = (roundStart ? 1U : 0U) | (roundEnd ? 2U : 0U) |
                       (firstSegment ? 0U : 4U) | (lastSegment ? 0U : 8U);
    quad.strokeWidths[0] = startPartition.x;
    quad.strokeWidths[1] = startPartition.y;
    quad.strokeWidths[2] = endPartition.x;
    quad.strokeWidths[3] = endPartition.y;
    const auto& paint = command.style.paint;
    const float first[4]{paint.start.r, paint.start.g, paint.start.b, paint.start.a};
    const float second[4]{paint.end.r, paint.end.g, paint.end.b, paint.end.a};
    std::copy(std::begin(first), std::end(first), quad.color0);
    std::copy(std::begin(second), std::end(second), quad.color1);
    quad.paint[0] = static_cast<float>(paint.kind);
    quad.paint[1] = paint.opacity;
    quad.paint[2] = paint.shaderParameter;
    quad.paint[3] = command.style.width;
    quad.gradient[0] = paint.origin.x;
    quad.gradient[1] = paint.origin.y;
    quad.gradient[2] = paint.target.x;
    quad.gradient[3] = paint.target.y;
    quad.clip[0] = command.clip.x;
    quad.clip[1] = command.clip.y;
    quad.clip[2] = command.clip.width;
    quad.clip[3] = command.clip.height;
    instances.push_back(quad);
  }

  void appendCubicBezier(std::vector<Instance>& instances,
                         const CubicBezierCommand& command) const {
    struct CubicSegment {
      Vec2 p0;
      Vec2 p1;
      Vec2 p2;
      Vec2 p3;
      std::uint8_t depth = 0;
    };

    constexpr float maximumFlatnessError = 0.25f;
    constexpr std::uint8_t maximumDepth = 7;
    const auto midpoint = [](Vec2 a, Vec2 b) { return (a + b) * 0.5f; };
    const auto pointLineDistanceSquared = [](Vec2 point, Vec2 a, Vec2 b) {
      const Vec2 chord = b - a;
      const float lengthSquared = chord.x * chord.x + chord.y * chord.y;
      if (lengthSquared <= 1.0e-12f) {
        const Vec2 delta = point - a;
        return delta.x * delta.x + delta.y * delta.y;
      }
      const float cross = chord.x * (a.y - point.y) - chord.y * (a.x - point.x);
      return (cross * cross) / lengthSquared;
    };
    const float flatnessSquared = maximumFlatnessError * maximumFlatnessError;
    std::array<CubicSegment, 128> stack{};
    std::size_t stackSize = 1;
    stack[0] = {command.from, command.control1, command.control2, command.to, 0};
    std::array<Vec2, 129> points{};
    std::size_t pointCount = 1;
    points[0] = command.from;

    while (stackSize != 0) {
      const CubicSegment segment = stack[--stackSize];
      const float d1 = pointLineDistanceSquared(segment.p1, segment.p0, segment.p3);
      const float d2 = pointLineDistanceSquared(segment.p2, segment.p0, segment.p3);
      const bool flat = std::max(d1, d2) <= flatnessSquared;
      if (flat || segment.depth >= maximumDepth || pointCount == points.size()) {
        points[pointCount++] = segment.p3;
        continue;
      }

      const Vec2 p01 = midpoint(segment.p0, segment.p1);
      const Vec2 p12 = midpoint(segment.p1, segment.p2);
      const Vec2 p23 = midpoint(segment.p2, segment.p3);
      const Vec2 p012 = midpoint(p01, p12);
      const Vec2 p123 = midpoint(p12, p23);
      const Vec2 split = midpoint(p012, p123);
      const auto nextDepth = static_cast<std::uint8_t>(segment.depth + 1);
      stack[stackSize++] = {split, p123, p23, segment.p3, nextDepth};
      stack[stackSize++] = {segment.p0, p01, p012, split, nextDepth};
    }

    if (pointCount < 2) return;
    const std::size_t segments = pointCount - 1;
    std::array<Vec2, 128> tangents{};
    for (std::size_t index = 0; index < segments; ++index) {
      const auto delta = points[index + 1] - points[index];
      const float magnitude = std::max(std::hypot(delta.x, delta.y), 0.00001f);
      tangents[index] = delta * (1.0f / magnitude);
    }
    for (std::size_t index = 0; index < segments; ++index) {
      const auto startTangent = index == 0 ? tangents[index]
                                           : tangents[index - 1] + tangents[index];
      const auto endTangent = index + 1 == segments ? tangents[index]
                                                     : tangents[index] + tangents[index + 1];
      appendStrokeSegment(instances, points[index], points[index + 1], command,
                          index == 0, index + 1 == segments,
                          startTangent, endTangent);
    }
  }

  void appendArc(std::vector<Instance>& instances, const ArcCommand& command) const {
    const float padding = command.radius + command.style.width * 0.5f + 1.5f;
    const auto first = instances.size();
    CubicBezierCommand stroke{};
    stroke.clip = command.clip;
    stroke.style = command.style;
    appendStrokeSegment(instances, command.center - Vec2{padding, padding},
                        command.center + Vec2{padding, padding}, stroke, true, true, {}, {});
    if (instances.size() == first) return;
    auto& quad = instances.back();
    const float bounds[4]{command.center.x - padding, command.center.y - padding,
                          command.center.x + padding, command.center.y + padding};
    std::copy(std::begin(bounds), std::end(bounds), quad.positionRect);
    std::copy(std::begin(bounds), std::end(bounds), quad.emRect);
    quad.shapeData[0] = 0xFFFFFFFDU;
    quad.bandTransform[0] = command.center.x;
    quad.bandTransform[1] = command.center.y;
    quad.bandTransform[2] = command.radius;
    quad.bandTransform[3] = command.startRadians;
    quad.strokeWidths[0] = command.sweepRadians;
  }

  const CachedGlyphRun& glyphRun(std::string_view text, const TextStyle& style) const {
    GlyphRunKey key{std::string(text), style.fontName, style.weight, style.italic};
    if (auto found = glyphRunCache.find(key); found != glyphRunCache.end()) {
      found->second.lastUsed = ++glyphRunUseCounter;
      return found->second;
    }
    const std::size_t cacheCapacity = std::max<std::size_t>(1, config.glyphRunCacheCapacity);
    if (glyphRunCache.size() >= cacheCapacity) {
      auto oldest = glyphRunCache.begin();
      for (auto it = std::next(glyphRunCache.begin()); it != glyphRunCache.end(); ++it)
        if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
      glyphRunCache.erase(oldest);
    }
    CachedGlyphRun run;
    const auto codepoints = decodeUtf8(text);
    run.glyphs.reserve(codepoints.size());
    for (const auto codepoint : codepoints) {
      const ShapeId glyphId = vectorAtlas.glyph(
        codepoint, style.fontName, style.weight, style.italic);
      const auto glyph = vectorAtlas.native().getShape(slughorn::Key(glyphId));
      if (glyph) run.glyphs.push_back({true, *glyph});
      else run.glyphs.push_back({});
    }
    run.lastUsed = ++glyphRunUseCounter;
    return glyphRunCache.emplace(std::move(key), std::move(run)).first->second;
  }

  float textWidth(const CachedGlyphRun& run, const TextStyle& style) const {
    float width = 0.0f;
    for (const auto& cached : run.glyphs) {
      const float advance = cached.present ? static_cast<float>(cached.shape.advance) : 0.6f;
      width += advance * style.size + style.letterSpacing;
    }
    return std::max(0.0f, width - style.letterSpacing);
  }

  float textWidth(std::string_view text, const TextStyle& style, float scale) const {
    auto scaled = style;
    scaled.size *= scale;
    scaled.letterSpacing *= scale;
    return textWidth(glyphRun(text, style), scaled);
  }

  void appendRichText(std::vector<Instance>& instances, const TextCommand& command) const {
    struct Fragment {
      std::string_view text;
      const TextStyle* style = nullptr;
    };
    struct Line {
      std::size_t first = 0;
      std::size_t count = 0;
      float width = 0.0f;
      float height = 0.0f;
      float maximumSize = 0.0f;
      float trailingSpacing = 0.0f;
    };

    const float runScale = std::max(command.runScale, 0.01f);
    std::vector<Fragment> fragments;
    fragments.reserve(command.runs.size() + 1);
    std::vector<Line> lines(1);
    lines.reserve(command.runs.size() + 1);
    std::string marker;
    if (command.style.listMarker != ListMarker::None) {
      marker = command.style.listMarker == ListMarker::Bullet ? "* " : "1. ";
      fragments.push_back({marker, &command.style});
      auto& first = lines.front();
      first.count = 1;
      first.width = textWidth(marker, command.style, 1.0f);
      first.height = command.style.size * std::max(command.style.lineHeight, 0.01f);
      first.maximumSize = command.style.size;
      first.trailingSpacing = command.style.letterSpacing;
    }
    for (const auto& run : command.runs) {
      std::size_t start = 0;
      while (start <= run.text.size()) {
        const std::size_t end = run.text.find('\n', start);
        const std::string_view fragment(
          run.text.data() + start,
          (end == std::string::npos ? run.text.size() : end) - start);
        auto& line = lines.back();
        if (!fragment.empty()) {
          if (line.count != 0) line.width += line.trailingSpacing;
          fragments.push_back({fragment, &run.style});
          ++line.count;
          line.width += textWidth(fragment, run.style, runScale);
          line.trailingSpacing = run.style.letterSpacing * runScale;
          line.maximumSize = std::max(line.maximumSize, run.style.size * runScale);
          line.height = std::max(
            line.height, run.style.size * runScale * std::max(run.style.lineHeight, 0.01f));
        }
        if (end == std::string::npos) break;
        lines.push_back(Line{.first = fragments.size()});
        start = end + 1;
      }
    }

    const float fallbackHeight = command.style.size * std::max(command.style.lineHeight, 0.01f);
    float totalHeight = 0.0f;
    for (auto& line : lines) {
      if (line.height <= 0.0f) line.height = fallbackHeight;
      if (line.maximumSize <= 0.0f) line.maximumSize = command.style.size;
      totalHeight += line.height;
    }
    float top = command.bounds.y;
    if (command.style.verticalAlign == VerticalAlign::Center)
      top += (command.bounds.height - totalHeight) * 0.5f;
    else if (command.style.verticalAlign == VerticalAlign::Bottom)
      top += command.bounds.height - totalHeight;

    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
      const auto& line = lines[lineIndex];
      if (top + line.height >= command.clip.y &&
          top <= command.clip.y + command.clip.height) {
        const float indent = lineIndex == 0 ? std::max(command.style.indent, 0.0f) : 0.0f;
        const float availableWidth = std::max(0.0f, command.bounds.width - indent);
        float x = command.bounds.x + indent;
        if (command.style.align == HorizontalAlign::Center)
          x += (availableWidth - line.width) * 0.5f;
        else if (command.style.align == HorizontalAlign::Right)
          x += availableWidth - line.width;
        const float baseline = top + line.maximumSize;
        for (std::size_t index = 0; index < line.count; ++index) {
          const auto& fragment = fragments[line.first + index];
          const auto& authored = *fragment.style;
          const float size = authored.size * runScale;
          const float spacing = authored.letterSpacing * runScale;
          const float fragmentStart = x;
          const float fragmentWidth = textWidth(fragment.text, authored, runScale);
          const float slant = authored.italic &&
            !vectorAtlas.hasFontFace(authored.fontName, true) ? 0.18f : 0.0f;
          const auto& resolvedRun = glyphRun(fragment.text, authored);
          for (const auto& cached : resolvedRun.glyphs) {
            if (!cached.present) {
              x += size * 0.6f + spacing;
              continue;
            }
            const auto& glyph = cached.shape;
            const float advance = static_cast<float>(glyph.advance) * size;
            if (glyph.width > 0 && glyph.height > 0) {
              Rect destination{
                x + static_cast<float>(glyph.bearingX) * size,
                baseline - static_cast<float>(glyph.bearingY) * size,
                static_cast<float>(glyph.width) * size,
                static_cast<float>(glyph.height) * size,
              };
              appendResolvedShape(
                instances, glyph, destination, authored.paint, command.clip, slant);
              if (authored.bold) {
                destination.x += std::max(0.55f, size * 0.035f);
                appendResolvedShape(
                  instances, glyph, destination, authored.paint, command.clip, slant);
              }
            }
            x += advance + spacing;
          }
          x = fragmentStart + fragmentWidth;
          if (authored.underline && fragmentWidth > 0.0f) {
            appendShape(
              instances, vectorAtlas.glyph(
                '_', authored.fontName, authored.weight, authored.italic),
              {fragmentStart, baseline + size * 0.07f, fragmentWidth,
               std::max(1.0f, size * 0.065f)},
              authored.paint, command.clip);
          }
          if (authored.strikethrough && fragmentWidth > 0.0f) {
            appendShape(
              instances, vectorAtlas.glyph(
                '-', authored.fontName, authored.weight, authored.italic),
              {fragmentStart, baseline - size * 0.32f, fragmentWidth,
               std::max(1.0f, size * 0.06f)},
              authored.paint, command.clip);
          }
          if (index + 1 < line.count) x += spacing;
        }
      }
      top += line.height;
    }
  }

  void appendText(std::vector<Instance>& instances, const TextCommand& command) const {
    if (!command.runs.empty()) {
      appendRichText(instances, command);
      return;
    }
    std::string prefixed;
    std::string_view text = command.text();
    if (command.style.listMarker != ListMarker::None) {
      const std::string_view marker = command.style.listMarker == ListMarker::Bullet ? "* " : "1. ";
      prefixed.reserve(marker.size() + text.size());
      prefixed.append(marker);
      prefixed.append(text);
      text = prefixed;
    }
    std::size_t lineStart = 0;
    std::size_t lineNumber = 0;
    const std::size_t lineCount =
      1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    const float lineAdvance = command.style.size * std::max(command.style.lineHeight, 0.01f);
    const float textBlockHeight = lineAdvance * static_cast<float>(lineCount);
    float firstLineTop = command.bounds.y;
    if (command.style.verticalAlign == VerticalAlign::Center)
      firstLineTop += (command.bounds.height - textBlockHeight) * 0.5f;
    else if (command.style.verticalAlign == VerticalAlign::Bottom)
      firstLineTop += command.bounds.height - textBlockHeight;
    const float slant = command.style.italic &&
      !vectorAtlas.hasFontFace(command.style.fontName, true) ? 0.18f : 0.0f;
    while (lineStart <= text.size()) {
      const std::size_t lineEnd = text.find('\n', lineStart);
      const std::string_view line(text.data() + lineStart,
                                  (lineEnd == std::string::npos ? text.size() : lineEnd) -
                                      lineStart);
      const float top = firstLineTop + static_cast<float>(lineNumber) * lineAdvance;
      const float lineBottom = top + command.style.size * std::max(command.style.lineHeight, 1.25f);
      if (lineBottom < command.clip.y ||
          top - command.style.size * 0.5f > command.clip.y + command.clip.height) {
        if (lineEnd == std::string::npos)
          break;
        lineStart = lineEnd + 1;
        ++lineNumber;
        continue;
      }
      const auto& resolvedRun = glyphRun(line, command.style);
      const bool needsWidth = command.style.align != HorizontalAlign::Left ||
                              command.style.underline || command.style.strikethrough;
      const float width = needsWidth ? textWidth(resolvedRun, command.style) : 0.0f;
      const float indent = lineNumber == 0 ? std::max(command.style.indent, 0.0f) : 0.0f;
      const float availableWidth = std::max(0.0f, command.bounds.width - indent);
      float x = command.bounds.x + indent;
      if (command.style.align == HorizontalAlign::Center) x += (availableWidth - width) * 0.5f;
      else if (command.style.align == HorizontalAlign::Right) x += availableWidth - width;
      const float baseline = top + command.style.size;
      const float lineX = x;
      for (const auto& cached : resolvedRun.glyphs) {
        if (!cached.present) {
          x += command.style.size * 0.6f + command.style.letterSpacing;
          continue;
        }
        const auto& glyph = cached.shape;
        const float advance = static_cast<float>(glyph.advance) * command.style.size;
        if (glyph.width > 0 && glyph.height > 0) {
          Rect destination{x + static_cast<float>(glyph.bearingX) * command.style.size,
                           baseline - static_cast<float>(glyph.bearingY) * command.style.size,
                           static_cast<float>(glyph.width) * command.style.size,
                           static_cast<float>(glyph.height) * command.style.size};
          appendResolvedShape(instances, glyph, destination, command.style.paint, command.clip,
                              slant);
          if (command.style.bold) {
            destination.x += std::max(0.55f, command.style.size * 0.035f);
            appendResolvedShape(instances, glyph, destination, command.style.paint, command.clip,
                                slant);
          }
        }
        x += advance + command.style.letterSpacing;
      }
      if (width > 0.0f && command.style.underline) {
        Rect decoration{lineX, baseline + command.style.size * 0.07f, width, std::max(1.0f, command.style.size * 0.065f)};
        appendShape(instances, vectorAtlas.glyph(
                      '_', command.style.fontName, command.style.weight, command.style.italic), decoration,
                    command.style.paint, command.clip);
      }
      if (width > 0.0f && command.style.strikethrough) {
        Rect decoration{lineX, baseline - command.style.size * 0.32f, width, std::max(1.0f, command.style.size * 0.06f)};
        appendShape(instances, vectorAtlas.glyph(
                      '-', command.style.fontName, command.style.weight, command.style.italic), decoration,
                    command.style.paint, command.clip);
      }
      if (lineEnd == std::string::npos)
        break;
      lineStart = lineEnd + 1;
      ++lineNumber;
    }
  }

  static VkDeviceSize alignRetainedBytes(VkDeviceSize value) noexcept {
    constexpr VkDeviceSize alignment = 16;
    return (value + alignment - 1) & ~(alignment - 1);
  }

  void growRetainedArena(VkDeviceSize requiredCapacity) {
    if (requiredCapacity <= retainedArena.size) return;
    VkDeviceSize nextSize = retainedArena.size
      ? retainedArena.size * 2
      : std::max<VkDeviceSize>(16, static_cast<VkDeviceSize>(config.retainedArenaInitialBytes));
    while (nextSize < requiredCapacity) nextSize *= 2;
    Buffer replacement{};
    createBuffer(nextSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, replacement, false);
    if (retainedArena.buffer && retainedArenaUsed != 0) {
      VkCommandBuffer command = beginSingleUse();
      VkBufferMemoryBarrier readable{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      readable.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      readable.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      readable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      readable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      readable.buffer = retainedArena.buffer;
      readable.size = retainedArenaUsed;
      vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &readable, 0, nullptr);
      VkBufferCopy copy{0, 0, retainedArenaUsed};
      vkCmdCopyBuffer(command, retainedArena.buffer, replacement.buffer, 1, &copy);
      VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ready.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
      ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.buffer = replacement.buffer;
      ready.size = retainedArenaUsed;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &ready,
                           0, nullptr);
      endSingleUse(command);
    }
    destroyBuffer(retainedArena);
    retainedArena = replacement;
  }

  VkDeviceSize allocateRetainedRange(VkDeviceSize requestedBytes) {
    const VkDeviceSize bytes = alignRetainedBytes(requestedBytes);
    for (std::size_t index = 0; index < retainedFreeRanges.size(); ++index) {
      auto& range = retainedFreeRanges[index];
      if (range.size < bytes) continue;
      const VkDeviceSize offset = range.offset;
      if (range.size == bytes) {
        retainedFreeRanges.erase(retainedFreeRanges.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        range.offset += bytes;
        range.size -= bytes;
      }
      return offset;
    }
    const VkDeviceSize offset = retainedArenaUsed;
    const VkDeviceSize required = offset + bytes;
    growRetainedArena(required);
    retainedArenaUsed = required;
    return offset;
  }

  void freeRetainedRange(VkDeviceSize offset, VkDeviceSize size) {
    if (size == 0) return;
    retainedFreeRanges.push_back({offset, size});
    std::sort(retainedFreeRanges.begin(), retainedFreeRanges.end(),
              [](const FreeRange& a, const FreeRange& b) { return a.offset < b.offset; });
    std::vector<FreeRange> merged;
    merged.reserve(retainedFreeRanges.size());
    for (const auto& range : retainedFreeRanges) {
      if (!merged.empty() && merged.back().offset + merged.back().size >= range.offset) {
        const VkDeviceSize end = std::max(merged.back().offset + merged.back().size,
                                          range.offset + range.size);
        merged.back().size = end - merged.back().offset;
      } else {
        merged.push_back(range);
      }
    }
    retainedFreeRanges = std::move(merged);
    while (!retainedFreeRanges.empty()) {
      const auto& tail = retainedFreeRanges.back();
      if (tail.offset + tail.size != retainedArenaUsed) break;
      retainedArenaUsed = tail.offset;
      retainedFreeRanges.pop_back();
    }
  }

  void releaseRetainedGeometry(RetainedGeometry& retained) {
    freeRetainedRange(retained.offset, retained.capacity);
    retained.offset = 0;
    retained.capacity = 0;
    retained.instanceCount = 0;
    retained.shadow.clear();
    retained.shadow.shrink_to_fit();
  }

  std::size_t uploadRetainedGeometry(RetainedGeometry& retained,
                                     const std::vector<Instance>& instances) {
    const VkDeviceSize requiredBytes = instances.size() * sizeof(Instance);
    if (instances.empty()) {
      if (retained.capacity != 0) releaseRetainedGeometry(retained);
      return 0;
    }

    if (requiredBytes > retained.capacity) {
      const VkDeviceSize grownCapacity = alignRetainedBytes(
        retained.capacity == 0 ? requiredBytes
                               : std::max(requiredBytes, retained.capacity + retained.capacity / 2));
      const VkDeviceSize oldOffset = retained.offset;
      const VkDeviceSize oldCapacity = retained.capacity;
      const VkDeviceSize newOffset = allocateRetainedRange(grownCapacity);
      Buffer staging{};
      createBuffer(requiredBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   staging, true);
      std::memcpy(staging.mapped, instances.data(), static_cast<std::size_t>(requiredBytes));
      VkCommandBuffer command = beginSingleUse();
      VkBufferCopy copy{0, newOffset, requiredBytes};
      vkCmdCopyBuffer(command, staging.buffer, retainedArena.buffer, 1, &copy);
      VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ready.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
      ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.buffer = retainedArena.buffer;
      ready.offset = newOffset;
      ready.size = requiredBytes;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &ready,
                           0, nullptr);
      endSingleUse(command);
      destroyBuffer(staging);
      if (oldCapacity != 0) freeRetainedRange(oldOffset, oldCapacity);
      retained.offset = newOffset;
      retained.capacity = grownCapacity;
      retained.shadow = instances;
      retained.instanceCount = static_cast<std::uint32_t>(instances.size());
      return static_cast<std::size_t>(requiredBytes);
    }

    struct DirtyRange { std::size_t first = 0; std::size_t count = 0; };
    std::vector<DirtyRange> ranges;
    for (std::size_t index = 0; index < instances.size();) {
      const bool changed = index >= retained.shadow.size() ||
        std::memcmp(&instances[index], &retained.shadow[index], sizeof(Instance)) != 0;
      if (!changed) { ++index; continue; }
      const std::size_t first = index++;
      while (index < instances.size()) {
        const bool nextChanged = index >= retained.shadow.size() ||
          std::memcmp(&instances[index], &retained.shadow[index], sizeof(Instance)) != 0;
        if (!nextChanged) break;
        ++index;
      }
      ranges.push_back({first, index - first});
    }
    if (ranges.empty()) {
      retained.shadow = instances;
      retained.instanceCount = static_cast<std::uint32_t>(instances.size());
      return 0;
    }

    std::size_t dirtyInstances = 0;
    for (const auto& range : ranges) dirtyInstances += range.count;
    if (ranges.size() > 1024 && dirtyInstances * 4 > instances.size()) {
      ranges.clear();
      ranges.push_back({0, instances.size()});
      dirtyInstances = instances.size();
    }
    const VkDeviceSize stagingBytes = dirtyInstances * sizeof(Instance);
    Buffer staging{};
    createBuffer(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, true);
    std::vector<VkBufferCopy> copies;
    copies.reserve(ranges.size());
    VkDeviceSize sourceOffset = 0;
    for (const auto& range : ranges) {
      const VkDeviceSize bytes = range.count * sizeof(Instance);
      std::memcpy(static_cast<std::byte*>(staging.mapped) + sourceOffset,
                  instances.data() + range.first, static_cast<std::size_t>(bytes));
      copies.push_back({sourceOffset, retained.offset + range.first * sizeof(Instance), bytes});
      sourceOffset += bytes;
    }
    VkCommandBuffer command = beginSingleUse();
    vkCmdCopyBuffer(command, staging.buffer, retainedArena.buffer,
                    static_cast<std::uint32_t>(copies.size()), copies.data());
    VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ready.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.buffer = retainedArena.buffer;
    ready.offset = retained.offset;
    ready.size = requiredBytes;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &ready,
                         0, nullptr);
    endSingleUse(command);
    destroyBuffer(staging);
    retained.shadow = instances;
    retained.instanceCount = static_cast<std::uint32_t>(instances.size());
    return static_cast<std::size_t>(stagingBytes);
  }

  void buildRetainedTextInstances(std::string_view utf8, Rect layoutBounds, TextStyle style,
                                  std::vector<Instance>& instances) const {
    instances.clear();
    if (utf8.empty() || style.size <= 0.0f || layoutBounds.width <= 0.0f) return;
    TextCommand command{{}, utf8, {}, layoutBounds,
                        {-10000000.0f, -10000000.0f, 20000000.0f, 20000000.0f},
                        std::move(style)};
    appendText(instances, command);
  }

  RetainedTextId createRetainedText(std::string_view utf8, Rect layoutBounds, TextStyle style) {
    std::vector<Instance> instances;
    buildRetainedTextInstances(utf8, layoutBounds, std::move(style), instances);
    if (instances.empty()) return 0;
    RetainedGeometry retained;
    uploadRetainedGeometry(retained, instances);
    retainedTexts.push_back(std::move(retained));
    return static_cast<RetainedTextId>(retainedTexts.size());
  }

  std::size_t updateRetainedText(RetainedTextId id, std::string_view utf8,
                                 Rect layoutBounds, TextStyle style) {
    if (id == 0 || id > retainedTexts.size())
      throw std::out_of_range("Invalid retained text id");
    std::vector<Instance> instances;
    buildRetainedTextInstances(utf8, layoutBounds, std::move(style), instances);
    return uploadRetainedGeometry(retainedTexts[id - 1U], instances);
  }

  void destroyRetainedText(RetainedTextId id) {
    if (id == 0 || id > retainedTexts.size())
      throw std::out_of_range("Invalid retained text id");
    auto& retained = retainedTexts[id - 1U];
    if (retained.capacity == 0 && retained.instanceCount == 0) return;
    check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle(destroy retained text)");
    releaseRetainedGeometry(retained);
  }

  RetainedDrawListId createRetainedDrawList(const DrawList& list) {
    RenderIR compiled;
    compileRenderIR(list, compiled, false);
    for (const DrawBatch& batch : compiled.batches) {
      if (batch.retainedId != 0)
        throw std::invalid_argument("Retained DrawLists cannot contain retained resources");
    }
    if (compiled.instances.empty()) return 0;
    RetainedGeometry retained;
    uploadRetainedGeometry(retained, compiled.instances);
    retainedDrawLists.push_back(std::move(retained));
    return static_cast<RetainedDrawListId>(retainedDrawLists.size());
  }

  std::size_t updateRetainedDrawList(RetainedDrawListId id, const DrawList& list) {
    if (id == 0 || id > retainedDrawLists.size())
      throw std::out_of_range("Invalid retained DrawList id");
    RenderIR compiled;
    compileRenderIR(list, compiled, false);
    for (const DrawBatch& batch : compiled.batches) {
      if (batch.retainedId != 0)
        throw std::invalid_argument("Retained DrawLists cannot contain retained resources");
    }
    return uploadRetainedGeometry(retainedDrawLists[id - 1U], compiled.instances);
  }

  void destroyRetainedDrawList(RetainedDrawListId id) {
    if (id == 0 || id > retainedDrawLists.size())
      throw std::out_of_range("Invalid retained DrawList id");
    auto& retained = retainedDrawLists[id - 1U];
    if (retained.capacity == 0 && retained.instanceCount == 0) return;
    check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle(destroy retained DrawList)");
    releaseRetainedGeometry(retained);
  }

  void compileRenderIR(const DrawList& list, RenderIR& ir, bool compactPrimitives) {
    ir.clear();
    auto& instances = ir.instances;
    auto* primitives = compactPrimitives ? &ir.primitives : nullptr;
    auto& drawBatches = ir.batches;
    const std::size_t commandCount = list.commands().size() + list.overlayCommands().size();
    instances.reserve(commandCount);
    if (primitives) primitives->reserve(commandCount);
    drawBatches.reserve(commandCount);
    const auto appendCommands = [&](const auto& commands) {
      for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
        const auto& display = commands[commandIndex];
        if (const auto* retainedCommand = std::get_if<RetainedTextCommand>(&display)) {
          if (retainedCommand->text == 0 || retainedCommand->text > retainedTexts.size())
            continue;
          const RetainedGeometry& retained = retainedTexts[retainedCommand->text - 1U];
          if (retained.instanceCount == 0)
            continue;
          drawBatches.push_back({retainedCommand->text, false, 0, retained.instanceCount,
                                 retainedCommand->position, retainedCommand->scale,
                                 retainedCommand->clip, retainedCommand->opacity});
          continue;
        }
        if (const auto* retainedCommand = std::get_if<RetainedDrawListCommand>(&display)) {
          if (retainedCommand->drawList == 0 || retainedCommand->drawList > retainedDrawLists.size())
            continue;
          const RetainedGeometry& retained = retainedDrawLists[retainedCommand->drawList - 1U];
          if (retained.instanceCount == 0) continue;
          drawBatches.push_back({retainedCommand->drawList, true, 0, retained.instanceCount,
                                 retainedCommand->position, retainedCommand->scale,
                                 retainedCommand->clip, retainedCommand->opacity});
          continue;
        }
        const std::size_t first = instances.size();
        const std::size_t primitiveFirst = primitives ? primitives->size() : 0;
        bool emittedPrimitive = false;
        if (const auto* shapeCommand = std::get_if<DrawCommand>(&display))
          appendShape(instances, shapeCommand->shape, shapeCommand->destination,
                      shapeCommand->paint, shapeCommand->clip, shapeCommand->italicShear);
        else if (const auto* textCommand = std::get_if<TextCommand>(&display))
          appendText(instances, *textCommand);
        else if (const auto* roundedCommand = std::get_if<RoundedRectCommand>(&display)) {
          std::size_t stackEnd = commandIndex + 1;
          const BorderWidths baseWidths = resolvedBorderWidths(roundedCommand->border);
          const bool baseCanStack = roundedCommand->border.align == StrokeAlign::Inside &&
            (baseWidths.maximum() <= 0.0f ||
             paintFullyOpaque(roundedCommand->border.paint) ||
             paintFullyTransparent(roundedCommand->border.paint));
          if (baseCanStack) {
            while (stackEnd < commands.size()) {
              const auto* overlay = std::get_if<RoundedRectCommand>(&commands[stackEnd]);
              if (!overlay || overlay->opacity != roundedCommand->opacity ||
                  !sameRoundedRectGeometry(*roundedCommand, *overlay)) break;
              const BorderWidths overlayWidths = resolvedBorderWidths(overlay->border);
              if (!paintFullyTransparent(overlay->paint) ||
                  overlayWidths.maximum() <= 0.0f ||
                  !paintFullyOpaque(overlay->border.paint)) break;
              ++stackEnd;
            }
          }
          if (stackEnd > commandIndex + 1) {
            appendOpaqueRoundedRectStack(instances, commands, commandIndex, stackEnd);
            commandIndex = stackEnd - 1;
          } else if (primitives && compactRectangleCompatible(*roundedCommand)) {
            appendCompactRectangle(*primitives, *roundedCommand);
            emittedPrimitive = primitives->size() != primitiveFirst;
          } else {
            appendRoundedRect(instances, *roundedCommand);
          }
        }
        else if (const auto* cubicCommand = std::get_if<CubicBezierCommand>(&display))
          appendCubicBezier(instances, *cubicCommand);
        else if (const auto* arcCommand = std::get_if<ArcCommand>(&display))
          appendArc(instances, *arcCommand);
        if (emittedPrimitive) {
          const auto count = static_cast<std::uint32_t>(primitives->size() - primitiveFirst);
          if (!drawBatches.empty() && drawBatches.back().retainedId == 0 &&
              drawBatches.back().primitive &&
              drawBatches.back().firstInstance + drawBatches.back().instanceCount == primitiveFirst) {
            drawBatches.back().instanceCount += count;
          } else {
            DrawBatch batch{0, false, static_cast<std::uint32_t>(primitiveFirst), count};
            batch.primitive = true;
            drawBatches.push_back(batch);
          }
          continue;
        }
        const float opacity = std::visit([](const auto& command) { return command.opacity; }, display);
        if (opacity < 1.0f)
          for (std::size_t i = first; i < instances.size(); ++i) instances[i].paint[1] *= opacity;
        const std::uint32_t count = static_cast<std::uint32_t>(instances.size() - first);
        if (count == 0)
          continue;
        if (!drawBatches.empty() && drawBatches.back().retainedId == 0 &&
            !drawBatches.back().primitive &&
            drawBatches.back().firstInstance + drawBatches.back().instanceCount == first) {
          drawBatches.back().instanceCount += count;
        } else {
          drawBatches.push_back({0, false, static_cast<std::uint32_t>(first), count});
        }
      }
    };
    appendCommands(list.commands());
    appendCommands(list.overlayCommands());
  }

  void ensureCapacity(Buffer& buffer, VkDeviceSize required, VkBufferUsageFlags usage) {
    if (required <= buffer.size)
      return;
    VkDeviceSize newSize = std::max<VkDeviceSize>(buffer.size, 4);
    while (newSize < required)
      newSize *= 2;
    destroyBuffer(buffer);
    createBuffer(newSize, usage,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer,
                 true);
  }

  void record(VkCommandBuffer command, std::uint32_t imageIndex, Frame& frame) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(frame)");
    if (frame.writeTimestamps) {
      vkCmdResetQueryPool(command, frame.timestamps, 0, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestamps, 0);
    }
    Color clearColor = config.clearColor;
    if (isSrgbFormat(swapchainFormat)) {
      clearColor.r = srgbToLinear(clearColor.r);
      clearColor.g = srgbToLinear(clearColor.g);
      clearColor.b = srgbToLinear(clearColor.b);
    }
    VkClearValue clear{{{clearColor.r, clearColor.g, clearColor.b, clearColor.a}}};
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render.renderPass = renderPass;
    render.framebuffer = framebuffers[imageIndex];
    render.renderArea.extent = extent;
    render.clearValueCount = 1;
    render.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    if (!frameIR.batches.empty()) {
      VkViewport viewport{
          0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
          0.0f, 1.0f};
      vkCmdSetViewport(command, 0, 1, &viewport);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                              &descriptorSet, 0, nullptr);
      VkPipeline currentPipeline = VK_NULL_HANDLE;
      for (const DrawBatch& batch : frameIR.batches) {
        const VkPipeline desiredPipeline = batch.primitive ? primitivePipeline : pipeline;
        if (desiredPipeline != currentPipeline) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
          currentPipeline = desiredPipeline;
        }
        const bool retained = batch.retainedId != 0;
        const RetainedGeometry* retainedGeometry = retained
          ? (batch.retainedDrawList ? &retainedDrawLists[batch.retainedId - 1U]
                                    : &retainedTexts[batch.retainedId - 1U])
          : nullptr;
        const Buffer& buffer = batch.primitive
          ? frame.primitiveInstances
          : retained ? retainedArena : frame.instances;
        const VkDeviceSize vertexOffset = retainedGeometry ? retainedGeometry->offset : 0;
        VkRect2D scissor{{0, 0}, extent};
        if (retained) {
          const float left = std::clamp(batch.clip.x, 0.0f, static_cast<float>(extent.width));
          const float top = std::clamp(batch.clip.y, 0.0f, static_cast<float>(extent.height));
          const float right =
              std::clamp(batch.clip.x + batch.clip.width, left, static_cast<float>(extent.width));
          const float bottom =
              std::clamp(batch.clip.y + batch.clip.height, top, static_cast<float>(extent.height));
          if (right <= left || bottom <= top)
            continue;
          scissor.offset = {static_cast<std::int32_t>(std::floor(left)),
                            static_cast<std::int32_t>(std::floor(top))};
          scissor.extent = {static_cast<std::uint32_t>(std::ceil(right) - std::floor(left)),
                            static_cast<std::uint32_t>(std::ceil(bottom) - std::floor(top))};
        }
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindVertexBuffers(command, 0, 1, &buffer.buffer, &vertexOffset);
        PushConstants push{{static_cast<float>(extent.width), static_cast<float>(extent.height),
                            retained ? batch.scale : 1.0f, retained ? batch.scale : 1.0f},
                            {retained ? batch.translation.x : 0.0f,
                             retained ? batch.translation.y : 0.0f, retained ? 1.0f : 0.0f,
                             isSrgbFormat(swapchainFormat) ? 1.0f : 0.0f},
                           {batch.clip.x, batch.clip.y, batch.clip.width, batch.clip.height},
                           {batch.opacity, 0.0f, 0.0f, 0.0f}};
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(command, 6, batch.instanceCount, 0, retained ? 0 : batch.firstInstance);
      }
    }
    vkCmdEndRenderPass(command);
    if (captureFrame) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = swapchainImages[imageIndex];
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {extent.width, extent.height, 1};
      vkCmdCopyImageToBuffer(command, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &region);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = 0;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    if (frame.writeTimestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestamps, 1);
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer(frame)");
  }

  void prepareFrame() {
    if (framePrepared)
      return;
    const Vec2 framebuffer = platformSurface.framebufferSize();
    if (framebuffer.x > 0.0f && framebuffer.y > 0.0f &&
        (static_cast<std::uint32_t>(framebuffer.x) != extent.width ||
         static_cast<std::uint32_t>(framebuffer.y) != extent.height))
      recreateSwapchain();
    while (!framePrepared) {
      Frame& frame = frames[currentFrame];
      check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX),
            "vkWaitForFences(frame)");
      if (frame.timestamps && frame.timestampsWritten) {
        std::uint64_t ticks[2]{};
        check(vkGetQueryPoolResults(device, frame.timestamps, 0, 2, sizeof(ticks), ticks,
                                    sizeof(std::uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults(timestamp)");
        const std::uint64_t timestampMask = timestampValidBits >= 64
                                                ? ~std::uint64_t{0}
                                                : (std::uint64_t{1} << timestampValidBits) - 1;
        const std::uint64_t elapsedTicks = (ticks[1] - ticks[0]) & timestampMask;
        statistics.gpuMilliseconds = static_cast<float>(elapsedTicks) *
                                     deviceProperties.limits.timestampPeriod / 1'000'000.0f;
        frame.timestampsWritten = false;
      }
      preparedAcquireResult = vkAcquireNextImageKHR(
          device, swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &preparedImageIndex);
      if (preparedAcquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        continue;
      }
      if (preparedAcquireResult != VK_SUCCESS && preparedAcquireResult != VK_SUBOPTIMAL_KHR)
        check(preparedAcquireResult, "vkAcquireNextImageKHR");
      framePrepared = true;
    }
  }

  void draw(const DrawList& list) {
    prepareFrame();
    Frame& frame = frames[currentFrame];
    const std::uint32_t imageIndex = preparedImageIndex;

    if (captureFrame) {
      if (swapchainFormat != VK_FORMAT_B8G8R8A8_SRGB && swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM &&
          swapchainFormat != VK_FORMAT_R8G8B8A8_SRGB && swapchainFormat != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("Readback requires an RGBA8/BGRA8 swapchain");
      ensureCapacity(readback, static_cast<VkDeviceSize>(extent.width) * extent.height * 4,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    const auto cpuBuildStart = std::chrono::steady_clock::now();
    compileRenderIR(list, frameIR, true);
    const auto cpuBuildEnd = std::chrono::steady_clock::now();
    ensureCapacity(frame.instances, frameIR.instances.size() * sizeof(Instance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensureCapacity(frame.primitiveInstances, frameIR.primitives.size() * sizeof(PrimitiveInstance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!frameIR.instances.empty())
      std::memcpy(frame.instances.mapped, frameIR.instances.data(),
                  frameIR.instances.size() * sizeof(Instance));
    if (!frameIR.primitives.empty())
      std::memcpy(frame.primitiveInstances.mapped, frameIR.primitives.data(),
                  frameIR.primitives.size() * sizeof(PrimitiveInstance));
    const auto cpuUploadEnd = std::chrono::steady_clock::now();

    frame.writeTimestamps = config.gpuTimingInterval > 0 && frame.timestamps != VK_NULL_HANDLE &&
                            submittedFrameCount % config.gpuTimingInterval == 0;
    check(vkResetFences(device, 1, &frame.fence), "vkResetFences");
    check(vkResetCommandBuffer(commandBuffers[currentFrame], 0), "vkResetCommandBuffer");
    record(commandBuffers[currentFrame], imageIndex, frame);
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffers[currentFrame];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished[imageIndex];
    check(vkQueueSubmit(graphicsQueue, 1, &submit, frame.fence), "vkQueueSubmit(frame)");
    frame.timestampsWritten = frame.writeTimestamps;
    if (captureFrame) {
      check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(readback)");
      captured.width = extent.width;
      captured.height = extent.height;
      captured.rgba.resize(static_cast<std::size_t>(extent.width) * extent.height * 4);
      std::memcpy(captured.rgba.data(), readback.mapped, captured.rgba.size());
      if (swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB || swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM)
        for (std::size_t i = 0; i < captured.rgba.size(); i += 4)
          std::swap(captured.rgba[i], captured.rgba[i + 2]);
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(presentQueue, &present);
    const bool needsRecreate = presented == VK_ERROR_OUT_OF_DATE_KHR;
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR && !needsRecreate)
      check(presented, "vkQueuePresentKHR");
    const auto cpuSubmitEnd = std::chrono::steady_clock::now();

    std::uint32_t retainedQuadCount = 0;
    for (const DrawBatch& batch : frameIR.batches)
      if (batch.retainedId != 0)
        retainedQuadCount += batch.instanceCount;
    statistics.quads = static_cast<std::uint32_t>(
      frameIR.instances.size() + frameIR.primitives.size()) + retainedQuadCount;
    statistics.retainedQuads = retainedQuadCount;
    statistics.primitiveQuads = static_cast<std::uint32_t>(frameIR.primitives.size());
    statistics.drawCalls = static_cast<std::uint32_t>(frameIR.batches.size());
    statistics.uploadedBytes = frameIR.instances.size() * sizeof(Instance) +
                               frameIR.primitives.size() * sizeof(PrimitiveInstance);
    statistics.cpuBuildMilliseconds =
        std::chrono::duration<float, std::milli>(cpuBuildEnd - cpuBuildStart).count();
    statistics.cpuUploadMilliseconds =
        std::chrono::duration<float, std::milli>(cpuUploadEnd - cpuBuildEnd).count();
    statistics.cpuSubmitMilliseconds =
        std::chrono::duration<float, std::milli>(cpuSubmitEnd - cpuUploadEnd).count();
    ++submittedFrameCount;
    framePrepared = false;
    currentFrame = (currentFrame + 1) % framesInFlight;
    if (needsRecreate)
      recreateSwapchain();
  }

  void destroy() noexcept {
    if (device)
      vkDeviceWaitIdle(device);
    if (device) {
      destroyBuffer(readback);
      retainedTexts.clear();
      retainedDrawLists.clear();
      retainedFreeRanges.clear();
      retainedArenaUsed = 0;
      destroyBuffer(retainedArena);
      for (auto& frame : frames) {
        destroyBuffer(frame.instances);
        destroyBuffer(frame.primitiveInstances);
        if (frame.timestamps)
          vkDestroyQueryPool(device, frame.timestamps, nullptr);
        if (frame.imageAvailable)
          vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        if (frame.fence)
          vkDestroyFence(device, frame.fence, nullptr);
        frame = {};
      }
      destroySwapchainResources();
      if (descriptorPool)
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      if (descriptorSetLayout)
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
      if (sampler)
        vkDestroySampler(device, sampler, nullptr);
      destroyTexture(curveTexture);
      destroyTexture(bandTexture);
      if (commandPool)
        vkDestroyCommandPool(device, commandPool, nullptr);
      vkDestroyDevice(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (surface && instance)
      vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (debugMessenger && instance) {
      auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroyDebug)
        destroyDebug(instance, debugMessenger, nullptr);
    }
    debugMessenger = VK_NULL_HANDLE;
    if (instance)
      vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
  }
};

VulkanRenderer::VulkanRenderer(PlatformSurface& surface, const VectorAtlas& atlas,
                               const RendererConfig& config)
    : impl_(std::make_unique<Impl>(surface, atlas, config)) {}
#if defined(SLUGVK_ENABLE_GLFW) && SLUGVK_ENABLE_GLFW
VulkanRenderer::VulkanRenderer(Window& window, const VectorAtlas& atlas,
                               const RendererConfig& config)
    : impl_(std::make_unique<Impl>(std::make_unique<GlfwPlatformSurface>(window), atlas, config)) {}
#endif
VulkanRenderer::~VulkanRenderer() = default;
void VulkanRenderer::prepareFrame() {
  impl_->prepareFrame();
}
RetainedTextId VulkanRenderer::createRetainedText(std::string_view utf8, Rect layoutBounds,
                                                  TextStyle style) {
  return impl_->createRetainedText(utf8, layoutBounds, std::move(style));
}
std::size_t VulkanRenderer::updateRetainedText(RetainedTextId text, std::string_view utf8,
                                               Rect layoutBounds, TextStyle style) {
  return impl_->updateRetainedText(text, utf8, layoutBounds, std::move(style));
}
void VulkanRenderer::destroyRetainedText(RetainedTextId text) {
  impl_->destroyRetainedText(text);
}
RetainedDrawListId VulkanRenderer::createRetainedDrawList(const DrawList& list) {
  return impl_->createRetainedDrawList(list);
}
std::size_t VulkanRenderer::updateRetainedDrawList(RetainedDrawListId drawList,
                                                   const DrawList& list) {
  return impl_->updateRetainedDrawList(drawList, list);
}
void VulkanRenderer::destroyRetainedDrawList(RetainedDrawListId drawList) {
  impl_->destroyRetainedDrawList(drawList);
}
void VulkanRenderer::draw(const DrawList& list) {
  impl_->draw(list);
}
FramePixels VulkanRenderer::drawAndReadback(const DrawList& list) {
  if (!impl_->config.enableReadback) throw std::runtime_error("Readback was not enabled");
  impl_->captureFrame = true;
  try { impl_->draw(list); }
  catch (...) { impl_->captureFrame = false; throw; }
  impl_->captureFrame = false;
  return std::move(impl_->captured);
}
void VulkanRenderer::waitIdle() {
  if (impl_->device)
    vkDeviceWaitIdle(impl_->device);
}
RendererStats VulkanRenderer::stats() const {
  return impl_->statistics;
}
const char* VulkanRenderer::deviceName() const {
  return impl_->deviceProperties.deviceName;
}
const char* VulkanRenderer::presentModeName() const {
  switch (impl_->swapchainPresentMode) {
  case VK_PRESENT_MODE_IMMEDIATE_KHR:
    return "IMMEDIATE";
  case VK_PRESENT_MODE_MAILBOX_KHR:
    return "MAILBOX";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
    return "FIFO_RELAXED";
  default:
    return "FIFO";
  }
}

} // namespace slugvk
