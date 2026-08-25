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
  Rect bounds = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  TextStyle style = {};
};

struct RoundedRectCommand {
  Rect destination = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  float radiusPx = 0.0f;
  float continuousCornersPercent = 0.0f;
  Paint paint = {};
};

using DisplayCommand = std::variant<DrawCommand, TextCommand, RoundedRectCommand>;

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
  void text(std::string utf8, Rect bounds, TextStyle style);

  [[nodiscard]] const std::vector<DrawCommand>& shapes() const { return shapes_; }
  [[nodiscard]] const std::vector<TextCommand>& texts() const { return texts_; }
  [[nodiscard]] const std::vector<DisplayCommand>& commands() const { return commands_; }
  [[nodiscard]] const std::vector<DisplayCommand>& overlayCommands() const { return overlayCommands_; }

private:
  Rect clip_ = {0.0f, 0.0f, 100000.0f, 100000.0f};
  std::vector<DrawCommand> shapes_{};
  std::vector<TextCommand> texts_{};
  std::vector<DisplayCommand> commands_{};
  std::vector<DisplayCommand> overlayCommands_{};
  bool overlayMode_ = false;
};

} // namespace slugvk
