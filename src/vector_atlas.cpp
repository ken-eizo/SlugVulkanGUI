#include "slugvk/vector_atlas.hpp"

#include "slughorn/canvas.hpp"
#include "slughorn/freetype.hpp"

#include FT_MULTIPLE_MASTERS_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace slugvk {

namespace {
constexpr float pi = 3.14159265358979323846f;
slughorn::slug_t sv(float value) { return static_cast<slughorn::slug_t>(value); }

std::string normalizedFamily(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value)
    if (std::isalnum(character)) result.push_back(static_cast<char>(std::tolower(character)));
  return result;
}

std::uint16_t inferredWeight(std::string_view style) {
  const std::string value = normalizedFamily(style);
  if (value.find("thin") != std::string::npos) return 100;
  if (value.find("extralight") != std::string::npos ||
      value.find("ultralight") != std::string::npos) return 200;
  if (value.find("light") != std::string::npos) return 300;
  if (value.find("medium") != std::string::npos) return 500;
  if (value.find("semibold") != std::string::npos ||
      value.find("demibold") != std::string::npos) return 600;
  if (value.find("extrabold") != std::string::npos ||
      value.find("ultrabold") != std::string::npos) return 800;
  if (value.find("black") != std::string::npos ||
      value.find("heavy") != std::string::npos) return 900;
  if (value.find("bold") != std::string::npos) return 700;
  return 400;
}

struct FreeTypeFace {
  FT_Library library = nullptr;
  FT_Face face = nullptr;

  ~FreeTypeFace() {
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
  }

  bool open(const std::string& path) {
    return FT_Init_FreeType(&library) == 0 &&
           FT_New_Face(library, path.c_str(), 0, &face) == 0;
  }

  bool open(std::span<const std::uint8_t> data) {
    return !data.empty() && FT_Init_FreeType(&library) == 0 &&
      FT_New_Memory_Face(
        library, reinterpret_cast<const FT_Byte*>(data.data()),
        static_cast<FT_Long>(data.size()), 0, &face) == 0;
  }

  void setWeight(std::uint16_t weight) {
    if (!weight || !FT_HAS_MULTIPLE_MASTERS(face)) return;
    FT_MM_Var* variation = nullptr;
    if (FT_Get_MM_Var(face, &variation) != 0 || !variation) return;
    std::vector<FT_Fixed> coordinates(variation->num_axis);
    for (FT_UInt i = 0; i < variation->num_axis; ++i) {
      const FT_Var_Axis& axis = variation->axis[i];
      coordinates[i] = axis.def;
      if (axis.tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
        const FT_Fixed requested = static_cast<FT_Fixed>(weight) * 65536L;
        coordinates[i] = std::clamp(requested, axis.minimum, axis.maximum);
      }
    }
    FT_Set_Var_Design_Coordinates(face, variation->num_axis, coordinates.data());
    FT_Done_MM_Var(library, variation);
  }
};

slughorn::canvas::LineCap cap(LineCap value) {
  switch (value) {
    case LineCap::Round: return slughorn::canvas::LineCap::Round;
    case LineCap::Square: return slughorn::canvas::LineCap::Square;
    default: return slughorn::canvas::LineCap::Butt;
  }
}

slughorn::canvas::LineJoin join(LineJoin value) {
  switch (value) {
    case LineJoin::Round: return slughorn::canvas::LineJoin::Round;
    case LineJoin::Bevel: return slughorn::canvas::LineJoin::Bevel;
    default: return slughorn::canvas::LineJoin::Miter;
  }
}

LineCap resolvedStartCap(const StrokeStyle& style, std::size_t dashIndex,
                         bool atPathStart, bool dashed) {
  if (dashed && dashIndex < style.dashCaps.size() && style.dashCaps[dashIndex].start)
    return *style.dashCaps[dashIndex].start;
  if (atPathStart && style.startCap) return *style.startCap;
  if (dashed && style.dashStartCap) return *style.dashStartCap;
  return style.cap;
}

LineCap resolvedEndCap(const StrokeStyle& style, std::size_t dashIndex,
                       bool atPathEnd, bool dashed) {
  if (dashed && dashIndex < style.dashCaps.size() && style.dashCaps[dashIndex].end)
    return *style.dashCaps[dashIndex].end;
  if (atPathEnd && style.endCap) return *style.endCap;
  if (dashed && style.dashEndCap) return *style.dashEndCap;
  return style.cap;
}

void appendCap(slughorn::canvas::Path& output, const slughorn::canvas::Path::Sample& endpoint,
               float halfWidth, LineCap lineCap, bool start) {
  if (halfWidth <= 0.0f || lineCap == LineCap::Butt) return;
  slughorn::canvas::Path capPath;
  if (lineCap == LineCap::Round) {
    capPath.circle(endpoint.x, endpoint.y, sv(halfWidth));
  } else {
    const float direction = start ? -1.0f : 1.0f;
    const float tx = std::cos(static_cast<float>(endpoint.angle)) * direction;
    const float ty = std::sin(static_cast<float>(endpoint.angle)) * direction;
    const float nx = -std::sin(static_cast<float>(endpoint.angle));
    const float ny = std::cos(static_cast<float>(endpoint.angle));
    const float ax = static_cast<float>(endpoint.x) + nx * halfWidth;
    const float ay = static_cast<float>(endpoint.y) + ny * halfWidth;
    const float bx = static_cast<float>(endpoint.x) - nx * halfWidth;
    const float by = static_cast<float>(endpoint.y) - ny * halfWidth;
    capPath.moveTo(sv(ax), sv(ay));
    capPath.lineTo(sv(bx), sv(by));
    capPath.lineTo(sv(bx + tx * halfWidth), sv(by + ty * halfWidth));
    capPath.lineTo(sv(ax + tx * halfWidth), sv(ay + ty * halfWidth));
    capPath.closePath();
  }
  output.addPath(capPath);
}

bool appendStrokedPath(slughorn::canvas::Path& output, slughorn::canvas::Path centerline,
                       float width, LineJoin lineJoin, LineCap startCap, LineCap endCap) {
  const auto start = centerline.sample(0.0f);
  const auto end = centerline.sample(1.0f);
  if (startCap == endCap) {
    if (!centerline.strokePath(sv(width), false, join(lineJoin), cap(startCap), sv(4.0f))) return false;
    output.addPath(centerline);
    return true;
  }
  if (!centerline.strokePath(sv(width), false, join(lineJoin),
                             slughorn::canvas::LineCap::Butt, sv(4.0f))) return false;
  output.addPath(centerline);
  appendCap(output, start, width * 0.5f, startCap, true);
  appendCap(output, end, width * 0.5f, endCap, false);
  return true;
}

struct DashSegment {
  slughorn::canvas::Path path;
  std::size_t visibleIndex = 0;
  bool atPathStart = false;
  bool atPathEnd = false;
};

std::vector<DashSegment> splitDashes(const slughorn::canvas::Path& source,
                                     const StrokeStyle& style) {
  std::vector<DashSegment> segments;
  std::vector<float> pattern;
  pattern.reserve(style.dashLengths.size() * 2U);
  for (float value : style.dashLengths) pattern.push_back(std::max(value, 0.001f));
  if (pattern.size() & 1U) pattern.insert(pattern.end(), pattern.begin(), pattern.end());
  float patternLength = 0.0f;
  for (float value : pattern) patternLength += value;
  const float totalLength = static_cast<float>(source.arcLength());
  if (patternLength <= 0.0f || totalLength <= 0.00001f) return {};

  float phase = std::fmod(style.dashOffset, patternLength);
  if (phase < 0.0f) phase += patternLength;
  std::size_t patternIndex = 0;
  while (phase >= pattern[patternIndex]) {
    phase -= pattern[patternIndex];
    patternIndex = (patternIndex + 1U) % pattern.size();
  }
  float distance = 0.0f;
  float remaining = pattern[patternIndex] - phase;
  std::size_t visibleIndex = 0;
  constexpr float samplesPerPath = 1024.0f;
  const float sampleStep = totalLength / samplesPerPath;
  while (distance < totalLength - 0.00001f) {
    const float intervalEnd = std::min(distance + remaining, totalLength);
    if ((patternIndex & 1U) == 0U && intervalEnd > distance + 0.00001f) {
      DashSegment segment;
      segment.visibleIndex = visibleIndex++;
      segment.atPathStart = distance <= 0.00001f;
      segment.atPathEnd = intervalEnd >= totalLength - 0.00001f;
      const auto first = source.sample(sv(distance / totalLength));
      segment.path.moveTo(first.x, first.y);
      float sampleDistance = (std::floor(distance / sampleStep) + 1.0f) * sampleStep;
      while (sampleDistance < intervalEnd - 0.00001f) {
        const auto point = source.sample(sv(sampleDistance / totalLength));
        segment.path.lineTo(point.x, point.y);
        sampleDistance += sampleStep;
      }
      const auto last = source.sample(sv(intervalEnd / totalLength));
      segment.path.lineTo(last.x, last.y);
      segments.push_back(std::move(segment));
    }
    const float consumed = intervalEnd - distance;
    distance = intervalEnd;
    remaining -= consumed;
    if (remaining <= 0.00001f) {
      patternIndex = (patternIndex + 1U) % pattern.size();
      remaining = pattern[patternIndex];
    }
  }
  return segments;
}
}

struct Path::Impl { slughorn::canvas::Path value; };

Path::Path() : impl_(std::make_unique<Impl>()) {}
Path::Path(const Path& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Path::Path(Path&&) noexcept = default;
Path& Path::operator=(const Path& other) {
  if (this != &other) impl_ = std::make_unique<Impl>(*other.impl_);
  return *this;
}
Path& Path::operator=(Path&&) noexcept = default;
Path::~Path() = default;

Path& Path::moveTo(float x, float y) { impl_->value.moveTo(sv(x), sv(y)); return *this; }
Path& Path::lineTo(float x, float y) { impl_->value.lineTo(sv(x), sv(y)); return *this; }
Path& Path::quadraticTo(float cx, float cy, float x, float y) {
  impl_->value.quadTo(sv(cx), sv(cy), sv(x), sv(y)); return *this;
}
Path& Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
  impl_->value.bezierTo(sv(c1x), sv(c1y), sv(c2x), sv(c2y), sv(x), sv(y)); return *this;
}

Path& Path::svgPath(std::string_view data, float viewBoxHeight) {
  struct Parser {
    Path& output;
    std::string source;
    const char* cursor = nullptr;
    const char* end = nullptr;
    Vec2 current{};
    Vec2 subpath{};
    Vec2 lastCubicControl{};
    Vec2 lastQuadraticControl{};
    char command = 0;
    char previousCommand = 0;
    float invertHeight = 0.0f;

    explicit Parser(Path& target, std::string_view value, float height)
        : output(target), source(value), cursor(source.c_str()), end(cursor + source.size()),
          current{0.0f, height > 0.0f ? height : 0.0f},
          subpath(current), invertHeight(height) {}

    [[noreturn]] void fail() const { throw std::invalid_argument("Invalid SVG path data"); }
    void separators() {
      while (cursor < end && (std::isspace(static_cast<unsigned char>(*cursor)) || *cursor == ','))
        ++cursor;
    }
    bool hasNumber() {
      separators();
      return cursor < end && (*cursor == '+' || *cursor == '-' || *cursor == '.' ||
                              std::isdigit(static_cast<unsigned char>(*cursor)));
    }
    float number() {
      separators();
      if (cursor >= end) fail();
      char* parsed = nullptr;
      const float value = std::strtof(cursor, &parsed);
      if (parsed == cursor || parsed > end || !std::isfinite(value)) fail();
      cursor = parsed;
      return value;
    }
    Vec2 point(bool relative) {
      Vec2 value{number(), number()};
      if (relative) {
        value.x += current.x;
        value.y = current.y + (invertHeight > 0.0f ? -value.y : value.y);
      } else if (invertHeight > 0.0f) {
        value.y = invertHeight - value.y;
      }
      return value;
    }
    static float vectorAngle(float ux, float uy, float vx, float vy) {
      return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    }
    void arc(float rx, float ry, float rotationDegrees, bool largeArc, bool sweep, Vec2 target) {
      rx = std::abs(rx);
      ry = std::abs(ry);
      if (rx <= 1.0e-6f || ry <= 1.0e-6f ||
          (std::abs(target.x - current.x) <= 1.0e-6f &&
           std::abs(target.y - current.y) <= 1.0e-6f)) {
        output.lineTo(target.x, target.y);
        current = target;
        return;
      }
      const float phi = rotationDegrees * pi / 180.0f;
      const float cosPhi = std::cos(phi);
      const float sinPhi = std::sin(phi);
      const float dx = (current.x - target.x) * 0.5f;
      const float dy = (current.y - target.y) * 0.5f;
      const float x1 = cosPhi * dx + sinPhi * dy;
      const float y1 = -sinPhi * dx + cosPhi * dy;
      float radiiScale = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
      if (radiiScale > 1.0f) {
        radiiScale = std::sqrt(radiiScale);
        rx *= radiiScale;
        ry *= radiiScale;
      }
      const float rx2 = rx * rx;
      const float ry2 = ry * ry;
      const float numerator =
          std::max(0.0f, rx2 * ry2 - rx2 * y1 * y1 - ry2 * x1 * x1);
      const float denominator =
          std::max(1.0e-12f, rx2 * y1 * y1 + ry2 * x1 * x1);
      const float coefficient =
          (largeArc == sweep ? -1.0f : 1.0f) * std::sqrt(numerator / denominator);
      const float cx1 = coefficient * (rx * y1 / ry);
      const float cy1 = coefficient * (-ry * x1 / rx);
      const float centerX =
          cosPhi * cx1 - sinPhi * cy1 + (current.x + target.x) * 0.5f;
      const float centerY =
          sinPhi * cx1 + cosPhi * cy1 + (current.y + target.y) * 0.5f;
      const float ux = (x1 - cx1) / rx;
      const float uy = (y1 - cy1) / ry;
      const float vx = (-x1 - cx1) / rx;
      const float vy = (-y1 - cy1) / ry;
      const float startAngle = vectorAngle(1.0f, 0.0f, ux, uy);
      float sweepAngle = vectorAngle(ux, uy, vx, vy);
      if (!sweep && sweepAngle > 0.0f) sweepAngle -= 2.0f * pi;
      if (sweep && sweepAngle < 0.0f) sweepAngle += 2.0f * pi;
      const int segments = std::max(
          1, static_cast<int>(std::ceil(std::abs(sweepAngle) / (pi * 0.5f))));
      const float step = sweepAngle / static_cast<float>(segments);
      const auto mapped = [&](float x, float y) {
        return Vec2{centerX + cosPhi * rx * x - sinPhi * ry * y,
                    centerY + sinPhi * rx * x + cosPhi * ry * y};
      };
      for (int index = 0; index < segments; ++index) {
        const float a0 = startAngle + step * static_cast<float>(index);
        const float a1 = a0 + step;
        const float alpha = 4.0f / 3.0f * std::tan((a1 - a0) * 0.25f);
        const float x0 = std::cos(a0);
        const float y0 = std::sin(a0);
        const float x1Unit = std::cos(a1);
        const float y1Unit = std::sin(a1);
        const Vec2 c1 = mapped(x0 - alpha * y0, y0 + alpha * x0);
        const Vec2 c2 = mapped(x1Unit + alpha * y1Unit, y1Unit - alpha * x1Unit);
        const Vec2 endpoint =
            index + 1 == segments ? target : mapped(x1Unit, y1Unit);
        output.cubicTo(c1.x, c1.y, c2.x, c2.y, endpoint.x, endpoint.y);
      }
      current = target;
    }
    void parse() {
      while (true) {
        separators();
        if (cursor >= end) return;
        if (std::isalpha(static_cast<unsigned char>(*cursor))) command = *cursor++;
        else if (command == 0) fail();
        const bool relative =
            std::islower(static_cast<unsigned char>(command)) != 0;
        const char upper =
            static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        if (upper == 'Z') {
          output.close();
          current = subpath;
          previousCommand = command;
          command = 0;
          continue;
        }
        bool first = true;
        while (hasNumber()) {
          if (upper == 'M') {
            const Vec2 target = point(relative);
            if (first) {
              output.moveTo(target.x, target.y);
              subpath = target;
            } else {
              output.lineTo(target.x, target.y);
            }
            current = target;
          } else if (upper == 'L') {
            current = point(relative);
            output.lineTo(current.x, current.y);
          } else if (upper == 'H') {
            float x = number();
            if (relative) x += current.x;
            current.x = x;
            output.lineTo(current.x, current.y);
          } else if (upper == 'V') {
            float y = number();
            if (relative)
              y = current.y + (invertHeight > 0.0f ? -y : y);
            else if (invertHeight > 0.0f)
              y = invertHeight - y;
            current.y = y;
            output.lineTo(current.x, current.y);
          } else if (upper == 'C') {
            const Vec2 c1 = point(relative);
            const Vec2 c2 = point(relative);
            const Vec2 target = point(relative);
            output.cubicTo(c1.x, c1.y, c2.x, c2.y, target.x, target.y);
            current = target;
            lastCubicControl = c2;
          } else if (upper == 'S') {
            const bool smooth =
                previousCommand == 'C' || previousCommand == 'c' ||
                previousCommand == 'S' || previousCommand == 's';
            const Vec2 c1 = smooth ? current * 2.0f - lastCubicControl : current;
            const Vec2 c2 = point(relative);
            const Vec2 target = point(relative);
            output.cubicTo(c1.x, c1.y, c2.x, c2.y, target.x, target.y);
            current = target;
            lastCubicControl = c2;
          } else if (upper == 'Q') {
            const Vec2 control = point(relative);
            const Vec2 target = point(relative);
            output.quadraticTo(control.x, control.y, target.x, target.y);
            current = target;
            lastQuadraticControl = control;
          } else if (upper == 'T') {
            const bool smooth =
                previousCommand == 'Q' || previousCommand == 'q' ||
                previousCommand == 'T' || previousCommand == 't';
            const Vec2 control =
                smooth ? current * 2.0f - lastQuadraticControl : current;
            const Vec2 target = point(relative);
            output.quadraticTo(control.x, control.y, target.x, target.y);
            current = target;
            lastQuadraticControl = control;
          } else if (upper == 'A') {
            const float rx = number();
            const float ry = number();
            const float rotation = number();
            const bool largeArc = number() != 0.0f;
            const bool sweep = number() != 0.0f;
            const Vec2 target = point(relative);
            arc(rx, ry, invertHeight > 0.0f ? -rotation : rotation, largeArc,
                invertHeight > 0.0f ? !sweep : sweep, target);
          } else {
            fail();
          }
          previousCommand = command;
          first = false;
          separators();
          if (cursor < end &&
              std::isalpha(static_cast<unsigned char>(*cursor))) break;
        }
        if (first) fail();
      }
    }
  } parser(*this, data, viewBoxHeight);
  parser.parse();
  return *this;
}
Path& Path::close() { impl_->value.closePath(); return *this; }
Path& Path::rect(float x, float y, float width, float height) {
  impl_->value.rect(sv(x), sv(y), sv(width), sv(height)); return *this;
}

Path& Path::roundedRect(float x, float y, float width, float height, float radius,
                        float continuousCornersPercent) {
  radius = std::clamp(radius, 0.0f, std::min(width, height) * 0.5f);
  const float continuous = std::clamp(continuousCornersPercent, 0.0f, 100.0f) * 0.01f;
  if (continuous <= 0.001f) {
    impl_->value.roundedRect(sv(x), sv(y), sv(width), sv(height), sv(radius));
    return *this;
  }

  // Apple's continuous corner is close to a superellipse. This cubic construction smoothly
  // blends from the circular kappa to a longer, flatter shoulder without CPU tessellation.
  const float kappa = 0.55228475f + 0.30f * continuous;
  const float right = x + width;
  const float bottom = y + height;
  moveTo(x + radius, y);
  lineTo(right - radius, y);
  cubicTo(right - radius + radius * kappa, y, right, y + radius - radius * kappa, right, y + radius);
  lineTo(right, bottom - radius);
  cubicTo(right, bottom - radius + radius * kappa, right - radius + radius * kappa, bottom, right - radius, bottom);
  lineTo(x + radius, bottom);
  cubicTo(x + radius - radius * kappa, bottom, x, bottom - radius + radius * kappa, x, bottom - radius);
  lineTo(x, y + radius);
  cubicTo(x, y + radius - radius * kappa, x + radius - radius * kappa, y, x + radius, y);
  return close();
}

Path& Path::circle(float centerX, float centerY, float radius) {
  impl_->value.circle(sv(centerX), sv(centerY), sv(radius)); return *this;
}
Path& Path::ellipse(float centerX, float centerY, float radiusX, float radiusY) {
  impl_->value.ellipse(sv(centerX), sv(centerY), sv(radiusX), sv(radiusY)); return *this;
}

Path& Path::polygon(Vec2 center, float radius, std::uint32_t sides, float rotation) {
  sides = std::max(sides, 3U);
  for (std::uint32_t i = 0; i < sides; ++i) {
    const float angle = rotation + (2.0f * pi * static_cast<float>(i) / static_cast<float>(sides));
    const float x = center.x + std::cos(angle) * radius;
    const float y = center.y + std::sin(angle) * radius;
    if (i == 0) moveTo(x, y); else lineTo(x, y);
  }
  return close();
}

Path& Path::star(Vec2 center, float outerRadius, float innerRadius,
                 std::uint32_t points, float rotation) {
  points = std::max(points, 2U);
  for (std::uint32_t i = 0; i < points * 2U; ++i) {
    const float radius = (i & 1U) == 0U ? outerRadius : innerRadius;
    const float angle = rotation + (pi * static_cast<float>(i) / static_cast<float>(points));
    const float x = center.x + std::cos(angle) * radius;
    const float y = center.y + std::sin(angle) * radius;
    if (i == 0) moveTo(x, y); else lineTo(x, y);
  }
  return close();
}

struct VectorAtlas::Impl {
  struct RegisteredFont {
    std::string family;
    std::uint16_t weight;
    bool italic;
    std::uint8_t mask;
  };

  slughorn::Atlas atlas;
  ShapeId nextId = slughorn::KeyIterator::AUTO_KEY_START;
  std::string family;
  std::string style;
  std::uint16_t nextFontMask = 0;
  std::vector<RegisteredFont> fonts;

  Impl() : atlas(1024) {}
};

VectorAtlas::VectorAtlas() : impl_(std::make_unique<Impl>()) {}
VectorAtlas::~VectorAtlas() = default;
VectorAtlas::VectorAtlas(VectorAtlas&&) noexcept = default;
VectorAtlas& VectorAtlas::operator=(VectorAtlas&&) noexcept = default;

ShapeId VectorAtlas::addPath(const Path& path) {
  if (impl_->atlas.isBuilt()) throw std::logic_error("VectorAtlas is already built");
  const ShapeId id = impl_->nextId++;
  slughorn::canvas::Canvas canvas(impl_->atlas);
  if (!canvas.defineShape(path.impl_->value, slughorn::Key(id))) return 0;
  return id;
}

ShapeId VectorAtlas::addStroke(const Path& path, const StrokeStyle& style) {
  if (impl_->atlas.isBuilt()) throw std::logic_error("VectorAtlas is already built");
  slughorn::canvas::Path centerline = path.impl_->value;

  const bool tapered = std::abs(style.startTaper - 1.0f) > 0.0001f ||
                       std::abs(style.endTaper - 1.0f) > 0.0001f;
  if (tapered) {
    // Variable-width strokes are emitted as a compact chain of exact vector quads. Adjacent
    // samples share endpoints derived from the same tangent, so the outline remains watertight.
    // Only authored geometry changes; the runtime still renders one immutable Slug shape.
    slughorn::canvas::Path outline;
    constexpr int samples = 192;
    std::vector<float> pattern;
    pattern.reserve(style.dashLengths.size() * 2U);
    for (float value : style.dashLengths) pattern.push_back(std::max(value, 0.001f));
    if (pattern.size() & 1U) pattern.insert(pattern.end(), pattern.begin(), pattern.end());
    float patternLength = 0.0f;
    for (float value : pattern) patternLength += value;
    float phase = patternLength > 0.0f ? std::fmod(style.dashOffset, patternLength) : 0.0f;
    if (phase < 0.0f) phase += patternLength;
    std::size_t patternIndex = 0;
    while (patternLength > 0.0f && phase >= pattern[patternIndex]) {
      phase -= pattern[patternIndex];
      patternIndex = (patternIndex + 1U) % pattern.size();
    }
    float remaining = patternLength > 0.0f
      ? pattern[patternIndex] - phase
      : std::numeric_limits<float>::max();
    bool drawing = patternLength <= 0.0f || (patternIndex & 1U) == 0U;
    bool runOpen = false;
    std::size_t visibleIndex = 0;
    auto previous = centerline.sample(0.0f);
    for (int i = 1; i <= samples; ++i) {
      const float t0 = static_cast<float>(i - 1) / samples;
      const float t1 = static_cast<float>(i) / samples;
      auto current = centerline.sample(sv(t1));
      const float dx = static_cast<float>(current.x - previous.x);
      const float dy = static_cast<float>(current.y - previous.y);
      const float distance = std::sqrt(dx * dx + dy * dy);
      const float half0 = style.width * 0.5f * std::max(0.0f, style.startTaper +
        (style.endTaper - style.startTaper) * t0);
      const float half1 = style.width * 0.5f * std::max(0.0f, style.startTaper +
        (style.endTaper - style.startTaper) * t1);
      if (drawing && distance > 0.00001f && (half0 > 0.0f || half1 > 0.0f)) {
        if (!runOpen) {
          appendCap(outline, previous, half0,
                    resolvedStartCap(style, visibleIndex, t0 <= 0.00001f,
                                     patternLength > 0.0f), true);
          runOpen = true;
        }
        const float nx0 = -std::sin(static_cast<float>(previous.angle));
        const float ny0 = std::cos(static_cast<float>(previous.angle));
        const float nx1 = -std::sin(static_cast<float>(current.angle));
        const float ny1 = std::cos(static_cast<float>(current.angle));
        outline.moveTo(previous.x + sv(nx0 * half0), previous.y + sv(ny0 * half0));
        outline.lineTo(current.x + sv(nx1 * half1), current.y + sv(ny1 * half1));
        outline.lineTo(current.x - sv(nx1 * half1), current.y - sv(ny1 * half1));
        outline.lineTo(previous.x - sv(nx0 * half0), previous.y - sv(ny0 * half0));
        outline.closePath();
      }
      if (patternLength > 0.0f) {
        remaining -= distance;
        while (remaining <= 0.0f) {
          const bool wasDrawing = drawing;
          patternIndex = (patternIndex + 1U) % pattern.size();
          drawing = (patternIndex & 1U) == 0U;
          remaining += pattern[patternIndex];
          if (wasDrawing && !drawing && runOpen) {
            appendCap(outline, current, half1,
                      resolvedEndCap(style, visibleIndex, i == samples, true), false);
            runOpen = false;
            ++visibleIndex;
          }
        }
      }
      previous = current;
    }
    if (runOpen) {
      const float endHalf = style.width * 0.5f * std::max(0.0f, style.endTaper);
      appendCap(outline, previous, endHalf,
                resolvedEndCap(style, visibleIndex, true, patternLength > 0.0f), false);
    }
    const ShapeId id = impl_->nextId++;
    slughorn::canvas::Canvas canvas(impl_->atlas);
    if (!canvas.defineShape(outline, slughorn::Key(id))) return 0;
    return id;
  }

  // Slughorn expands the centerline once during atlas construction. Runtime transforms therefore
  // update one compact quad instance per shape, while caps/joins remain exact vector curves.
  slughorn::canvas::Path outline;
  if (style.dashLengths.empty()) {
    if (!appendStrokedPath(outline, std::move(centerline), style.width, style.join,
                           resolvedStartCap(style, 0, true, false),
                           resolvedEndCap(style, 0, true, false))) return 0;
  } else {
    auto segments = splitDashes(centerline, style);
    if (segments.empty()) return 0;
    for (auto& segment : segments) {
      if (!appendStrokedPath(outline, std::move(segment.path), style.width, style.join,
                             resolvedStartCap(style, segment.visibleIndex,
                                              segment.atPathStart, true),
                             resolvedEndCap(style, segment.visibleIndex,
                                            segment.atPathEnd, true))) return 0;
    }
  }
  const ShapeId id = impl_->nextId++;
  slughorn::canvas::Canvas canvas(impl_->atlas);
  if (!canvas.defineShape(outline, slughorn::Key(id))) return 0;
  return id;
}

bool VectorAtlas::loadFont(const std::string& fontPath, const std::vector<std::uint32_t>& codepoints) {
  return loadFont(fontPath, FontFace{}, codepoints);
}

bool VectorAtlas::loadFont(const std::string& fontPath, FontFace face,
                           const std::vector<std::uint32_t>& codepoints) {
  if (impl_->atlas.isBuilt()) throw std::logic_error("VectorAtlas is already built");
  FreeTypeFace source;
  if (!source.open(fontPath)) return false;
  if (face.family.empty()) face.family = std::filesystem::path(fontPath).stem().string();
  source.setWeight(face.weight);
  return loadFontFace(source.face, std::move(face), codepoints);
}

bool VectorAtlas::loadFontMemory(std::span<const std::uint8_t> fontData, FontFace face,
                                 const std::vector<std::uint32_t>& codepoints) {
  if (impl_->atlas.isBuilt()) throw std::logic_error("VectorAtlas is already built");
  FreeTypeFace source;
  if (!source.open(fontData)) return false;
  source.setWeight(face.weight);
  return loadFontFace(source.face, std::move(face), codepoints);
}

bool VectorAtlas::loadFontFace(void* nativeFace, FontFace face,
                               const std::vector<std::uint32_t>& codepoints) {
  if (impl_->nextFontMask > std::numeric_limits<std::uint8_t>::max()) return false;
  auto* sourceFace = static_cast<FT_Face>(nativeFace);
  slughorn::freetype::LoadConfig config;
  config.mask = static_cast<std::uint8_t>(impl_->nextFontMask);
  config.metrics = slughorn::freetype::readFontMetrics(sourceFace);
  if (sourceFace->family_name) config.familyName = sourceFace->family_name;
  if (sourceFace->style_name) config.styleName = sourceFace->style_name;
  std::size_t count = 0;
  if (codepoints.empty()) {
    count = slughorn::freetype::loadGlyphRange(
      sourceFace, 32, 126, impl_->atlas, &config);
  } else {
    count = slughorn::freetype::loadGlyphs(
      sourceFace, codepoints, impl_->atlas, &config);
  }
  if (count > 0) {
    const std::uint16_t weight = face.weight ? face.weight : inferredWeight(config.styleName);
    const bool italic = face.italic ||
      normalizedFamily(config.styleName).find("italic") != std::string::npos ||
      normalizedFamily(config.styleName).find("oblique") != std::string::npos;
    const auto addFamily = [&](std::string_view family) {
      const std::string normalized = normalizedFamily(family);
      if (normalized.empty()) return;
      const auto same = [&](const Impl::RegisteredFont& entry) {
        return entry.family == normalized && entry.weight == weight && entry.italic == italic;
      };
      auto found = std::find_if(impl_->fonts.begin(), impl_->fonts.end(), same);
      if (found == impl_->fonts.end())
        impl_->fonts.push_back({normalized, weight, italic, config.mask});
      else
        found->mask = config.mask;
    };
    if (impl_->fonts.empty()) {
      impl_->family = config.familyName;
      impl_->style = config.styleName;
      addFamily("system-ui");
    }
    addFamily(face.family);
    addFamily(config.familyName);
    ++impl_->nextFontMask;
  }
  return count > 0;
}

void VectorAtlas::build() { impl_->atlas.build(); }
bool VectorAtlas::built() const { return impl_->atlas.isBuilt(); }

std::optional<ShapeMetrics> VectorAtlas::metrics(ShapeId id) const {
  const auto shape = impl_->atlas.getShape(slughorn::Key(id));
  if (!shape) return std::nullopt;
  return ShapeMetrics{
    static_cast<float>(shape->bearingX), static_cast<float>(shape->bearingY),
    static_cast<float>(shape->width), static_cast<float>(shape->height),
    static_cast<float>(shape->advance)
  };
}

std::string VectorAtlas::fontFamily() const { return impl_->family; }
std::string VectorAtlas::fontStyle() const { return impl_->style; }
ShapeId VectorAtlas::glyph(std::uint32_t codepoint, std::string_view fontName,
                           std::uint16_t weight, bool italic) const {
  std::uint8_t mask = 0;
  const std::string family = normalizedFamily(fontName);
  unsigned best = std::numeric_limits<unsigned>::max();
  for (const auto& entry : impl_->fonts) {
    if (entry.family != family) continue;
    const unsigned distance = static_cast<unsigned>(
      std::abs(static_cast<int>(entry.weight) - static_cast<int>(weight)));
    const unsigned score = distance + (entry.italic == italic ? 0U : 2000U);
    if (score < best) {
      best = score;
      mask = entry.mask;
    }
  }
  return slughorn::Key(codepoint, mask).codepoint();
}
bool VectorAtlas::hasFontFace(std::string_view fontName, bool italic) const {
  const std::string family = normalizedFamily(fontName);
  return std::any_of(impl_->fonts.begin(), impl_->fonts.end(),
    [&](const Impl::RegisteredFont& entry) {
      return entry.family == family && entry.italic == italic;
    });
}
const slughorn::Atlas& VectorAtlas::native() const { return impl_->atlas; }

std::vector<std::uint32_t> decodeUtf8(std::string_view text) {
  std::vector<std::uint32_t> result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80U) {
      result.push_back(first);
      ++i;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
      length = 2; codepoint = first & 0x1fU; minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
      length = 3; codepoint = first & 0x0fU; minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
      length = 4; codepoint = first & 0x07U; minimum = 0x10000U;
    }

    bool valid = length != 0 && i + length <= text.size();
    for (std::size_t j = 1; valid && j < length; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      valid = (continuation & 0xc0U) == 0x80U;
      if (valid) codepoint = (codepoint << 6U) | (continuation & 0x3fU);
    }
    valid = valid && codepoint >= minimum && codepoint <= 0x10ffffU &&
            !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
    result.push_back(valid ? codepoint : 0xfffdU);
    i += valid ? length : 1U;
  }
  return result;
}

std::string findDefaultSystemFont() {
#ifdef _WIN32
  const std::filesystem::path fonts = "C:/Windows/Fonts";
  for (const char* name : {"segoeui.ttf", "arial.ttf", "calibri.ttf"}) {
    const auto path = fonts / name;
    if (std::filesystem::exists(path)) return path.string();
  }
#elif defined(__APPLE__)
  for (const char* path : {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                           "/Library/Fonts/Arial Unicode.ttf"}) {
    if (std::filesystem::exists(path)) return path;
  }
#endif
  return {};
}

} // namespace slugvk
