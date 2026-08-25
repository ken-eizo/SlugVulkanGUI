#include "slugvk/vector_atlas.hpp"

#include "slughorn/canvas.hpp"
#include "slughorn/freetype.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace slugvk {

namespace {
constexpr float pi = 3.14159265358979323846f;
slughorn::slug_t sv(float value) { return static_cast<slughorn::slug_t>(value); }

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
  slughorn::Atlas atlas;
  ShapeId nextId = slughorn::KeyIterator::AUTO_KEY_START;
  std::string family;
  std::string style;
  std::uint8_t nextFontMask = 0;
  std::unordered_map<std::string, std::uint8_t> fontMasks;

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
    float patternLength = 0.0f;
    for (float value : style.dashLengths) patternLength += std::max(value, 0.001f);
    float phase = patternLength > 0.0f ? std::fmod(style.dashOffset, patternLength) : 0.0f;
    if (phase < 0.0f) phase += patternLength;
    std::size_t patternIndex = 0;
    while (patternLength > 0.0f && phase > style.dashLengths[patternIndex]) {
      phase -= style.dashLengths[patternIndex];
      patternIndex = (patternIndex + 1U) % style.dashLengths.size();
    }
    float remaining = patternLength > 0.0f
      ? std::max(style.dashLengths[patternIndex] - phase, 0.001f)
      : std::numeric_limits<float>::max();
    bool drawing = patternLength <= 0.0f || (patternIndex & 1U) == 0U;
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
        if (remaining <= 0.0f) {
          patternIndex = (patternIndex + 1U) % style.dashLengths.size();
          drawing = (patternIndex & 1U) == 0U;
          remaining += std::max(style.dashLengths[patternIndex], 0.001f);
        }
      }
      previous = current;
    }
    const ShapeId id = impl_->nextId++;
    slughorn::canvas::Canvas canvas(impl_->atlas);
    if (!canvas.defineShape(outline, slughorn::Key(id))) return 0;
    return id;
  }

  if (!style.dashLengths.empty()) {
    float patternLength = 0.0f;
    for (float value : style.dashLengths) patternLength += std::max(value, 0.001f);
    if (patternLength > 0.0f) {
      slughorn::canvas::Path dashed;
      constexpr int samples = 1024;
      float phase = std::fmod(style.dashOffset, patternLength);
      if (phase < 0.0f) phase += patternLength;
      std::size_t patternIndex = 0;
      while (phase > style.dashLengths[patternIndex]) {
        phase -= style.dashLengths[patternIndex];
        patternIndex = (patternIndex + 1U) % style.dashLengths.size();
      }
      auto previous = centerline.sample(0.0f);
      bool drawing = (patternIndex & 1U) == 0U;
      float remaining = std::max(style.dashLengths[patternIndex] - phase, 0.001f);
      for (int i = 1; i <= samples; ++i) {
        auto current = centerline.sample(sv(static_cast<float>(i) / samples));
        const float dx = static_cast<float>(current.x - previous.x);
        const float dy = static_cast<float>(current.y - previous.y);
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (drawing) {
          dashed.moveTo(previous.x, previous.y);
          dashed.lineTo(current.x, current.y);
        }
        remaining -= distance;
        if (remaining <= 0.0f) {
          patternIndex = (patternIndex + 1U) % style.dashLengths.size();
          drawing = (patternIndex & 1U) == 0U;
          remaining += std::max(style.dashLengths[patternIndex], 0.001f);
        }
        previous = current;
      }
      centerline = std::move(dashed);
    }
  }

  // Slughorn expands the centerline once during atlas construction. Runtime transforms therefore
  // update only four vertices per shape, while caps/joins remain exact vector curves.
  if (!centerline.strokePath(sv(style.width), false, join(style.join), cap(style.cap), sv(4.0f))) return 0;
  const ShapeId id = impl_->nextId++;
  slughorn::canvas::Canvas canvas(impl_->atlas);
  if (!canvas.defineShape(centerline, slughorn::Key(id))) return 0;
  return id;
}

bool VectorAtlas::loadFont(const std::string& fontPath, const std::vector<std::uint32_t>& codepoints) {
  if (impl_->atlas.isBuilt()) throw std::logic_error("VectorAtlas is already built");
  slughorn::freetype::LoadConfig config;
  config.mask = impl_->nextFontMask;
  std::size_t count = 0;
  if (codepoints.empty()) {
    count = slughorn::freetype::loadAsciiFont(fontPath, impl_->atlas, &config) ? 95U : 0U;
  } else {
    count = slughorn::freetype::loadFontGlyphs(fontPath, codepoints, impl_->atlas, &config);
  }
  if (count > 0) {
    if (impl_->fontMasks.empty()) {
      impl_->family = config.familyName;
      impl_->style = config.styleName;
      impl_->fontMasks.emplace("system-ui", config.mask);
    }
    if (!config.familyName.empty()) impl_->fontMasks[config.familyName] = config.mask;
    impl_->fontMasks[std::filesystem::path(fontPath).stem().string()] = config.mask;
    if (impl_->nextFontMask < 255) ++impl_->nextFontMask;
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
ShapeId VectorAtlas::glyph(std::uint32_t codepoint, std::string_view fontName) const {
  std::uint8_t mask = 0;
  const auto found = impl_->fontMasks.find(std::string(fontName));
  if (found != impl_->fontMasks.end()) mask = found->second;
  return slughorn::Key(codepoint, mask).codepoint();
}
const slughorn::Atlas& VectorAtlas::native() const { return impl_->atlas; }

std::vector<std::uint32_t> decodeUtf8(std::string_view text) {
  std::vector<std::uint32_t> result;
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    std::uint32_t codepoint = 0xfffdU;
    std::size_t length = 1;
    if (first < 0x80U) codepoint = first;
    else if ((first & 0xe0U) == 0xc0U && i + 1 < text.size()) {
      codepoint = ((first & 0x1fU) << 6U) | (static_cast<unsigned char>(text[i + 1]) & 0x3fU); length = 2;
    } else if ((first & 0xf0U) == 0xe0U && i + 2 < text.size()) {
      codepoint = ((first & 0x0fU) << 12U) | ((static_cast<unsigned char>(text[i + 1]) & 0x3fU) << 6U)
                | (static_cast<unsigned char>(text[i + 2]) & 0x3fU); length = 3;
    } else if ((first & 0xf8U) == 0xf0U && i + 3 < text.size()) {
      codepoint = ((first & 0x07U) << 18U) | ((static_cast<unsigned char>(text[i + 1]) & 0x3fU) << 12U)
                | ((static_cast<unsigned char>(text[i + 2]) & 0x3fU) << 6U)
                | (static_cast<unsigned char>(text[i + 3]) & 0x3fU); length = 4;
    }
    result.push_back(codepoint <= 0x10ffffU ? codepoint : 0xfffdU);
    i += length;
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
