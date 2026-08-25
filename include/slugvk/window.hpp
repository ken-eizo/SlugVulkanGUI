#pragma once

#include "slugvk/input.hpp"

#include <exception>
#include <functional>
#include <string>

struct GLFWwindow;

namespace slugvk {

struct WindowConfig {
  int width = 1280;
  int height = 800;
  std::string title = "SlugVulkan";
  bool resizable = true;
  bool visible = true;
};

class Window {
public:
  explicit Window(const WindowConfig& config = {});
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool shouldClose() const;
  void requestClose();
  void setSize(int width, int height);
  // Invoked from the platform refresh event, including the Windows modal resize loop.
  void setRefreshCallback(std::function<void()> callback);
  void pollEvents();
  void waitForVisibleFramebuffer();
  void setRawMouseMotion(bool enabled);
  [[nodiscard]] bool rawMouseMotionSupported() const;
  [[nodiscard]] Vec2 framebufferSize() const;
  [[nodiscard]] float contentScale() const;
  [[nodiscard]] const InputState& input() const { return input_; }
  [[nodiscard]] InputState& input() { return input_; }
  [[nodiscard]] GLFWwindow* native() const { return window_; }

private:
  void invokeRefreshCallback() noexcept;
  GLFWwindow* window_ = nullptr;
  InputState input_{};
  std::function<void()> refreshCallback_{};
  std::exception_ptr callbackException_{};
};

} // namespace slugvk
