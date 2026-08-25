#include "slugvk/window.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace slugvk {

namespace {
Window* windowFrom(GLFWwindow* window) {
  return static_cast<Window*>(glfwGetWindowUserPointer(window));
}

Vec2 cursorInFramebuffer(GLFWwindow* window, double x, double y) {
  int windowWidth = 0;
  int windowHeight = 0;
  int framebufferWidth = 0;
  int framebufferHeight = 0;
  glfwGetWindowSize(window, &windowWidth, &windowHeight);
  glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
  const double scaleX = windowWidth > 0 ? static_cast<double>(framebufferWidth) / windowWidth : 1.0;
  const double scaleY = windowHeight > 0 ? static_cast<double>(framebufferHeight) / windowHeight : 1.0;
  return {static_cast<float>(x * scaleX), static_cast<float>(y * scaleY)};
}
}

Window::Window(const WindowConfig& config) {
  glfwSetErrorCallback([](int, const char*) {});
  if (glfwInit() != GLFW_TRUE) throw std::runtime_error("glfwInit failed");

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
  window_ = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
  if (!window_) {
    glfwTerminate();
    throw std::runtime_error("glfwCreateWindow failed");
  }

  glfwSetWindowUserPointer(window_, this);
  glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
    const Vec2 position = cursorInFramebuffer(w, x, y);
    windowFrom(w)->input_.onCursor(position.x, position.y);
  });
  glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int) {
    windowFrom(w)->input_.onMouseButton(button, action);
  });
  glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
    windowFrom(w)->input_.onKey(key, action);
  });
  glfwSetScrollCallback(window_, [](GLFWwindow* w, double x, double y) {
    windowFrom(w)->input_.onScroll(x, y, glfwGetTime());
  });
  glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int codepoint) {
    windowFrom(w)->input_.onCodepoint(codepoint);
  });
  glfwSetWindowRefreshCallback(window_, [](GLFWwindow* w) {
    windowFrom(w)->invokeRefreshCallback();
  });
  glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int width, int height) {
    if (width > 0 && height > 0) windowFrom(w)->invokeRefreshCallback();
  });

  double x = 0.0;
  double y = 0.0;
  glfwGetCursorPos(window_, &x, &y);
  const Vec2 position = cursorInFramebuffer(window_, x, y);
  input_.onCursor(position.x, position.y);
}

Window::~Window() {
  if (window_) glfwDestroyWindow(window_);
  glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(window_) == GLFW_TRUE; }
void Window::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
void Window::setSize(int width, int height) {
  glfwSetWindowSize(window_, std::max(width, 1), std::max(height, 1));
}
void Window::setRefreshCallback(std::function<void()> callback) {
  refreshCallback_ = std::move(callback);
}

void Window::invokeRefreshCallback() noexcept {
  if (!refreshCallback_ || callbackException_) return;
  try {
    refreshCallback_();
  } catch (...) {
    callbackException_ = std::current_exception();
  }
}

void Window::pollEvents() {
  input_.beginFrame();
  glfwPollEvents();
  if (callbackException_) {
    auto error = std::exchange(callbackException_, {});
    std::rethrow_exception(error);
  }
  // Sample once more after dispatching callbacks so high-rate pointer motion cannot leave the
  // declarative frame on an older coalesced callback position.
  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window_, &cursorX, &cursorY);
  const Vec2 position = cursorInFramebuffer(window_, cursorX, cursorY);
  input_.onCursor(position.x, position.y);
  input_.finishFrame(glfwGetTime());
}

void Window::waitForVisibleFramebuffer() {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  while ((width == 0 || height == 0) && !shouldClose()) {
    glfwWaitEvents();
    glfwGetFramebufferSize(window_, &width, &height);
  }
}

void Window::setRawMouseMotion(bool enabled) {
  const bool rawMouseMotion = enabled && rawMouseMotionSupported();
  glfwSetInputMode(window_, GLFW_CURSOR, rawMouseMotion ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (rawMouseMotionSupported())
    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, rawMouseMotion ? GLFW_TRUE : GLFW_FALSE);
}

bool Window::rawMouseMotionSupported() const {
  return glfwRawMouseMotionSupported() == GLFW_TRUE;
}

Vec2 Window::framebufferSize() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  return {static_cast<float>(width), static_cast<float>(height)};
}

float Window::contentScale() const {
  float x = 1.0f;
  float y = 1.0f;
  glfwGetWindowContentScale(window_, &x, &y);
  return (x + y) * 0.5f;
}

} // namespace slugvk
