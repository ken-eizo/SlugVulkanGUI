#pragma once

#include "slugvk/types.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slughorn { class Atlas; }

namespace slugvk {

struct ShapeMetrics {
  float bearingX = 0.0f;
  float bearingY = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float advance = 0.0f;
};

struct FontFace {
  std::string family;
  std::uint16_t weight = 0;
  bool italic = false;
};

class Path {
public:
  Path();
  Path(const Path&);
  Path(Path&&) noexcept;
  Path& operator=(const Path&);
  Path& operator=(Path&&) noexcept;
  ~Path();

  Path& moveTo(float x, float y);
  Path& lineTo(float x, float y);
  Path& quadraticTo(float cx, float cy, float x, float y);
  Path& cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
  // Append SVG path-data commands in the SVG 1.1 coordinate system. This keeps
  // icon sources (for example Lucide's 24x24 paths) as their canonical data
  // instead of flattening them into disconnected runtime line segments.
  Path& svgPath(std::string_view data, float viewBoxHeight = 0.0f);
  Path& close();
  Path& rect(float x, float y, float width, float height);
  Path& roundedRect(float x, float y, float width, float height, float radius,
                    float continuousCornersPercent = 0.0f);
  Path& circle(float centerX, float centerY, float radius);
  Path& ellipse(float centerX, float centerY, float radiusX, float radiusY);
  Path& polygon(Vec2 center, float radius, std::uint32_t sides, float rotationRadians = 0.0f);
  Path& star(Vec2 center, float outerRadius, float innerRadius, std::uint32_t points,
             float rotationRadians = 0.0f);

private:
  friend class VectorAtlas;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class VectorAtlas {
public:
  VectorAtlas();
  ~VectorAtlas();
  VectorAtlas(VectorAtlas&&) noexcept;
  VectorAtlas& operator=(VectorAtlas&&) noexcept;
  VectorAtlas(const VectorAtlas&) = delete;
  VectorAtlas& operator=(const VectorAtlas&) = delete;

  ShapeId addPath(const Path& path);
  ShapeId addStroke(const Path& path, const StrokeStyle& style);
  bool loadFont(const std::string& fontPath, const std::vector<std::uint32_t>& codepoints = {});
  bool loadFont(const std::string& fontPath, FontFace face,
                const std::vector<std::uint32_t>& codepoints = {});
  bool loadFontMemory(std::span<const std::uint8_t> fontData, FontFace face = {},
                      const std::vector<std::uint32_t>& codepoints = {});
  void build();

  [[nodiscard]] bool built() const;
  [[nodiscard]] std::optional<ShapeMetrics> metrics(ShapeId id) const;
  [[nodiscard]] std::string fontFamily() const;
  [[nodiscard]] std::string fontStyle() const;
  [[nodiscard]] ShapeId glyph(std::uint32_t codepoint,
                              std::string_view fontName = "system-ui",
                              std::uint16_t weight = 400,
                              bool italic = false) const;
  [[nodiscard]] bool hasFontFace(std::string_view fontName, bool italic) const;
  [[nodiscard]] const slughorn::Atlas& native() const;

private:
  bool loadFontFace(void* nativeFace, FontFace face,
                    const std::vector<std::uint32_t>& codepoints);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::vector<std::uint32_t> decodeUtf8(std::string_view text);
std::string findDefaultSystemFont();

} // namespace slugvk
