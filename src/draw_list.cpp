#include "slugvk/draw_list.hpp"

#include <algorithm>
#include <utility>

namespace slugvk {

void DrawList::clear() {
  commands_.clear();
  overlayCommands_.clear();
  overlayMode_ = false;
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint) {
  shape(id, destination, paint, clip_);
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint, Rect clip) {
  if (id == 0 || destination.width <= 0.0f || destination.height <= 0.0f) return;
  DrawCommand command{id, destination, clip, paint, 0.0f};
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::roundedRect(Rect destination, float radiusPx, Paint paint,
                           float continuousCornersPercent) {
  if (destination.width <= 0.0f || destination.height <= 0.0f) return;
  RoundedRectCommand command{
    destination,
    clip_,
    std::max(radiusPx, 0.0f),
    std::clamp(continuousCornersPercent, 0.0f, 100.0f),
    paint
  };
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::text(std::string utf8, Rect bounds, TextStyle style) {
  if (utf8.empty() || style.size <= 0.0f) return;
  TextCommand command{std::move(utf8), bounds, clip_, std::move(style)};
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

} // namespace slugvk
