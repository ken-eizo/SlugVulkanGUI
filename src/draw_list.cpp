#include "slugvk/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace slugvk {

void DrawList::clear() {
  commands_.clear();
  overlayCommands_.clear();
  overlayMode_ = false;
  opacity_ = 1.0f;
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint) {
  shape(id, destination, paint, clip_);
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint, Rect clip) {
  if (id == 0 || destination.width <= 0.0f || destination.height <= 0.0f)
    return;
  DrawCommand command{id, destination, clip, paint, 0.0f};
  command.opacity = opacity_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::roundedRect(Rect destination, float radiusPx, Paint paint,
                           float continuousCornersPercent, BorderStyle border) {
  roundedRect(destination, CornerRadii::all(radiusPx), std::move(paint),
              CornerSmoothing::all(continuousCornersPercent), std::move(border));
}

void DrawList::roundedRect(Rect destination, CornerRadii radiiPx, Paint paint,
                           CornerSmoothing continuousCorners, BorderStyle border) {
  if (destination.width <= 0.0f || destination.height <= 0.0f) return;
  RoundedRectCommand command{
    destination,
    clip_,
    radiiPx,
    continuousCorners,
    paint,
    border
  };
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::cubicBezier(Vec2 from, Vec2 control1, Vec2 control2, Vec2 to, StrokeStyle style) {
  if (style.width <= 0.0f || style.paint.opacity <= 0.0f) return;
  CubicBezierCommand command{from, control1, control2, to, clip_, std::move(style)};
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::externalImage(ExternalImageId image, Rect destination,
                             std::uint32_t width, std::uint32_t height) {
  if (image == 0 || width == 0 || height == 0 ||
      destination.width <= 0.0f || destination.height <= 0.0f) return;
  ExternalImageCommand command{image, destination, clip_, width, height, opacity_};
  if (overlayMode_) overlayCommands_.emplace_back(command);
  else commands_.emplace_back(command);
}

void DrawList::text(std::string utf8, Rect bounds, TextStyle style) {
  if (utf8.empty() || style.size <= 0.0f) return;
  TextCommand command{std::move(utf8), {}, {}, bounds, clip_, std::move(style)};
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::arc(Vec2 center, float radius, float startRadians, float sweepRadians,
                   StrokeStyle style) {
  if (radius <= 0.0f || style.width <= 0.0f || style.paint.opacity <= 0.0f ||
      std::abs(sweepRadians) < 0.000001f) return;
  ArcCommand command{center, radius, startRadians, sweepRadians, clip_, std::move(style)};
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::textStatic(std::string_view utf8, Rect bounds, TextStyle style) {
  if (utf8.empty() || style.size <= 0.0f) return;
  TextCommand command{{}, utf8, {}, bounds, clip_, std::move(style)};
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::textRunsStatic(std::span<const TextRun> runs, Rect bounds,
                              TextStyle paragraphStyle, float runScale) {
  if (runs.empty() || runScale <= 0.0f) return;
  TextCommand command{{}, {}, runs, bounds, clip_, std::move(paragraphStyle), runScale};
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(std::move(command));
  else commands_.emplace_back(std::move(command));
}

void DrawList::retainedDrawList(RetainedDrawListId drawList, Vec2 position, float scale) {
  if (drawList == 0 || scale <= 0.0f) return;
  RetainedDrawListCommand command{drawList, position, scale, clip_};
  command.opacity = opacity_;
  if (overlayMode_) overlayCommands_.emplace_back(command);
  else commands_.emplace_back(command);
}

void DrawList::retainedText(RetainedTextId text, Vec2 position, float scale) {
  if (text == 0 || scale <= 0.0f)
    return;
  RetainedTextCommand command{text, position, scale, clip_};
  command.opacity = opacity_;
  if (overlayMode_)
    overlayCommands_.emplace_back(command);
  else
    commands_.emplace_back(command);
}

} // namespace slugvk
