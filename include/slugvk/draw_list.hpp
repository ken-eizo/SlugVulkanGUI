#pragma once

#include "slugvk/types.hpp"

#include <string>
#include <variant>
#include <vector>

namespace slugvk {

struct DrawCommand {
  ShapeId shape = 0;
  Rect destination = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  Paint paint = {};
  float italicShear = 0.0f;
};

struct TextCommand {
  std::string utf8;
  std::string_view borrowedUtf8 = {};
  Rect bounds = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  TextStyle style = {};

  [[nodiscard]] std::string_view text() const {
    return borrowedUtf8.data() ? borrowedUtf8 : std::string_view(utf8);
  }
};

struct RoundedRectCommand {
  Rect destination = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  CornerRadii radiiPx = {};
  CornerSmoothing continuousCorners = {};
  Paint paint = {};
};

struct RetainedTextCommand {
  RetainedTextId text = 0;
  Vec2 position = {};
  float scale = 1.0f;
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
};

using DisplayCommand = std::variant<DrawCommand, TextCommand, RoundedRectCommand, RetainedTextCommand>;

class DrawList {
public:
  void clear();
  void setClip(Rect clip) { clip_ = clip; }
  [[nodiscard]] Rect clip() const { return clip_; }

  // Overlay commands are emitted after all regular commands while staying in the same GPU batch.
  void beginOverlay() { overlayMode_ = true; }
  void endOverlay() { overlayMode_ = false; }
  [[nodiscard]] bool overlayMode() const { return overlayMode_; }

  void shape(ShapeId shape, Rect destination, Paint paint);
  void shape(ShapeId shape, Rect destination, Paint paint, Rect clip);
  void fill(ShapeId shape, Rect destination, Paint paint) { this->shape(shape, destination, paint); }
  void stroke(ShapeId shape, Rect destination, Paint paint) { this->shape(shape, destination, paint); }
  void stroke(ShapeId shape, Rect destination, const StrokeStyle& style) {
    this->shape(shape, destination, style.paint);
  }
  // Applies radius after destination sizing. radiusPx is absolute framebuffer pixels.
  void roundedRect(Rect destination, float radiusPx, Paint paint,
                   float continuousCornersPercent = 0.0f);
  // Individual values are optional at the API level: use this overload only where a corner
  // differs. Radii remain absolute framebuffer pixels after destination sizing.
  void roundedRect(Rect destination, CornerRadii radiiPx, Paint paint,
                   CornerSmoothing continuousCorners = {});
  void text(std::string utf8, Rect bounds, TextStyle style);
  // The referenced bytes must remain alive until VulkanRenderer::draw() returns.
  void textStatic(std::string_view utf8, Rect bounds, TextStyle style);
  // Draws text whose glyph instances were retained by VulkanRenderer. Only this transform and
  // clip are dynamic, so zoom/pan does not relayout or upload the document.
  void retainedText(RetainedTextId text, Vec2 position, float scale);

  [[nodiscard]] const std::vector<DisplayCommand>& commands() const { return commands_; }
  [[nodiscard]] const std::vector<DisplayCommand>& overlayCommands() const { return overlayCommands_; }

private:
  Rect clip_ = {0.0f, 0.0f, 100000.0f, 100000.0f};
  std::vector<DisplayCommand> commands_{};
  std::vector<DisplayCommand> overlayCommands_{};
  bool overlayMode_ = false;
};

} // namespace slugvk
