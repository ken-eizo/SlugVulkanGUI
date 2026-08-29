#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slugvk {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  [[nodiscard]] bool contains(Vec2 p) const {
    return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height;
  }
};

struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;

  static constexpr Color fromRgb8(std::uint32_t rgb, float alpha = 1.0f) {
    return {
      static_cast<float>((rgb >> 16U) & 0xffU) / 255.0f,
      static_cast<float>((rgb >> 8U) & 0xffU) / 255.0f,
      static_cast<float>(rgb & 0xffU) / 255.0f,
      alpha
    };
  }
};

inline Color lerp(Color a, Color b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return {
    a.r + (b.r - a.r) * t,
    a.g + (b.g - a.g) * t,
    a.b + (b.b - a.b) * t,
    a.a + (b.a - a.a) * t
  };
}

using ShapeId = std::uint32_t;
using RetainedTextId = std::uint32_t;
using WidgetId = std::uint64_t;

enum class GradientKind : std::uint32_t { Solid, Linear, Diamond, Radial, Shader, HsvConic };

struct Paint {
  GradientKind kind = GradientKind::Solid;
  Color start = {};
  Color end = {};
  Vec2 origin = {0.0f, 0.0f};
  Vec2 target = {1.0f, 1.0f};
  float opacity = 1.0f;
  float shaderParameter = 0.0f;

  static Paint solid(Color color, float opacity = 1.0f) {
    Paint result;
    result.start = color;
    result.end = color;
    result.opacity = opacity;
    return result;
  }

  static Paint gradient(GradientKind kind, Color start, Color end,
                        Vec2 origin = {0.0f, 0.0f}, Vec2 target = {1.0f, 1.0f},
                        float opacity = 1.0f) {
    Paint result;
    result.kind = kind;
    result.start = start;
    result.end = end;
    result.origin = origin;
    result.target = target;
    result.opacity = opacity;
    return result;
  }

  static Paint shader(Color start, Color end, float parameter = 0.0f,
                      Vec2 origin = {0.0f, 0.0f}, Vec2 target = {1.0f, 1.0f},
                      float opacity = 1.0f) {
    Paint result = gradient(GradientKind::Shader, start, end, origin, target, opacity);
    result.shaderParameter = parameter;
    return result;
  }

  // Full-saturation/value HSV wheel, with hue 0 at the top and increasing clockwise.
  // Useful for compact native colour pickers without a texture or per-frame tessellation.
  static Paint hsvConic(float opacity = 1.0f) {
    return gradient(GradientKind::HsvConic, Color::fromRgb8(0xff0000),
                    Color::fromRgb8(0xff0000), {0.5f, 0.5f}, {0.5f, 0.0f}, opacity);
  }
};

enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };
enum class StrokeAlign : std::uint8_t { Inside, Center, Outside };

struct DashCapOverride {
  std::optional<LineCap> start = {};
  std::optional<LineCap> end = {};
};

struct StrokeStyle {
  Paint paint = Paint::solid(Color::fromRgb8(0xffffff));
  float width = 1.0f;
  float startTaper = 1.0f;
  float endTaper = 1.0f;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  std::vector<float> dashLengths = {};
  float dashOffset = 0.0f;
  // Empty overrides preserve the compact legacy behavior: cap applies everywhere.
  std::optional<LineCap> startCap = {};
  std::optional<LineCap> endCap = {};
  std::optional<LineCap> dashStartCap = {};
  std::optional<LineCap> dashEndCap = {};
  // Optional per-visible-dash overrides, indexed from the path start after dashOffset.
  std::vector<DashCapOverride> dashCaps = {};
};

struct BorderWidths {
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  float left = 0.0f;

  static constexpr BorderWidths all(float width) { return {width, width, width, width}; }
  [[nodiscard]] constexpr float maximum() const {
    return std::max(std::max(top, right), std::max(bottom, left));
  }
};

struct BorderStyle {
  Paint paint = Paint::solid(Color::fromRgb8(0x000000), 0.0f);
  float width = 0.0f;
  StrokeAlign align = StrokeAlign::Center;
  std::optional<BorderWidths> individualWidths = {};

  [[nodiscard]] constexpr BorderWidths resolvedWidths() const {
    return individualWidths.value_or(BorderWidths::all(width));
  }
};

struct CornerRadii {
  float topLeft = 0.0f;
  float topRight = 0.0f;
  float bottomRight = 0.0f;
  float bottomLeft = 0.0f;

  static constexpr CornerRadii all(float radius) { return {radius, radius, radius, radius}; }
};

struct CornerSmoothing {
  float topLeftPercent = 0.0f;
  float topRightPercent = 0.0f;
  float bottomRightPercent = 0.0f;
  float bottomLeftPercent = 0.0f;

  static constexpr CornerSmoothing all(float percent) { return {percent, percent, percent, percent}; }
};

enum class HorizontalAlign : std::uint8_t { Left, Center, Right, Justify };
enum class VerticalAlign : std::uint8_t { Top, Center, Bottom };
enum class ListMarker : std::uint8_t { None, Bullet, Numbered };

struct TextStyle {
  std::string fontName = "system-ui";
  float size = 14.0f;
  std::uint16_t weight = 400;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  HorizontalAlign align = HorizontalAlign::Left;
  VerticalAlign verticalAlign = VerticalAlign::Top;
  float lineHeight = 1.25f;
  float letterSpacing = 0.0f;
  float indent = 0.0f;
  ListMarker listMarker = ListMarker::None;
  Paint paint = Paint::solid(Color::fromRgb8(0xffffff));
};

struct TextRun {
  std::string text;
  TextStyle style = {};
};

enum class ContinuousCorners : std::uint8_t { Circular = 0, IosLike = 100 };

inline WidgetId hashId(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

} // namespace slugvk
