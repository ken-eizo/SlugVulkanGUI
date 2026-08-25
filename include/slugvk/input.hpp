#pragma once

#include "slugvk/types.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace slugvk {

enum class MouseButton : std::uint8_t { Left, Right, Middle, Other1, Other2, Other3, Other4, Other5, Count };

struct ButtonState {
  bool pressed = false;
  bool released = false;
  bool down = false;
};

enum class ScrollDirection : std::int8_t { None = 0, Up = 1, Down = -1 };

struct ScrollState {
  bool started = false;
  bool ended = false;
  bool active = false;
  Vec2 delta = {};
  ScrollDirection direction = ScrollDirection::None;
};

class InputState {
public:
  static constexpr int keyCount = 512;

  [[nodiscard]] Vec2 cursorPosition() const { return cursor_; }
  [[nodiscard]] Vec2 cursorDelta() const { return cursorDelta_; }
  [[nodiscard]] Vec2 rawMouseDelta() const { return rawDelta_; }
  [[nodiscard]] const ScrollState& scroll() const { return scroll_; }
  [[nodiscard]] const ButtonState& mouse(MouseButton button) const;
  [[nodiscard]] const ButtonState& key(int key) const;
  [[nodiscard]] const std::u32string& textInput() const { return textInput_; }

private:
  friend class Window;
  void beginFrame();
  void finishFrame(double nowSeconds);
  void refreshCursorDelta();
  void onCursor(double x, double y);
  void onMouseButton(int button, int action);
  void onKey(int key, int action);
  void onScroll(double x, double y, double nowSeconds);
  void onCodepoint(std::uint32_t codepoint);

  std::array<ButtonState, static_cast<std::size_t>(MouseButton::Count)> mouse_{};
  std::array<ButtonState, keyCount> keys_{};
  Vec2 cursor_{};
  Vec2 previousCursor_{};
  Vec2 cursorDelta_{};
  Vec2 rawDelta_{};
  ScrollState scroll_{};
  std::u32string textInput_{};
  double lastScrollTime_ = -1.0;
};

} // namespace slugvk
