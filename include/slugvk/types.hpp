#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
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

enum class GradientKind : std::uint32_t { Solid, Linear, Diamond, Radial, Shader };

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
};

enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };

struct StrokeStyle {
  Paint paint = Paint::solid(Color::fromRgb8(0xffffff));
  float width = 1.0f;
  float startTaper = 1.0f;
  float endTaper = 1.0f;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  std::vector<float> dashLengths = {};
  float dashOffset = 0.0f;
};

enum class HorizontalAlign : std::uint8_t { Left, Center, Right, Justify };
enum class ListMarker : std::uint8_t { None, Bullet, Numbered };

struct TextStyle {
  std::string fontName = "system-ui";
  float size = 14.0f;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  HorizontalAlign align = HorizontalAlign::Left;
  float lineHeight = 1.25f;
  float letterSpacing = 0.0f;
  float indent = 0.0f;
  ListMarker listMarker = ListMarker::None;
  Paint paint = Paint::solid(Color::fromRgb8(0xffffff));
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
