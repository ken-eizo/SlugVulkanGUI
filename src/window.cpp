#include "slugvk/window.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdexcept>

namespace slugvk {

namespace {
InputState* inputStateFrom(GLFWwindow* window) {
  return static_cast<InputState*>(glfwGetWindowUserPointer(window));
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

  glfwSetWindowUserPointer(window_, &input_);
  glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
    inputStateFrom(w)->onCursor(x, y);
  });
  glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int) {
    inputStateFrom(w)->onMouseButton(button, action);
  });
  glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
    inputStateFrom(w)->onKey(key, action);
  });
  glfwSetScrollCallback(window_, [](GLFWwindow* w, double x, double y) {
    inputStateFrom(w)->onScroll(x, y, glfwGetTime());
  });
  glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int codepoint) {
    inputStateFrom(w)->onCodepoint(codepoint);
  });

  double x = 0.0;
  double y = 0.0;
  glfwGetCursorPos(window_, &x, &y);
  input_.onCursor(x, y);
}

Window::~Window() {
  if (window_) glfwDestroyWindow(window_);
  glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(window_) == GLFW_TRUE; }
void Window::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }

void Window::pollEvents() {
  input_.beginFrame();
  glfwPollEvents();
  // Sample once more after dispatching callbacks so high-rate pointer motion cannot leave the
  // declarative frame on an older coalesced callback position.
  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window_, &cursorX, &cursorY);
  input_.onCursor(cursorX, cursorY);
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
