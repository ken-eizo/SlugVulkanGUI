#pragma once

#include "slugvk/types.hpp"

#include <span>
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
  float opacity = 1.0f;
};

struct TextCommand {
  std::string utf8;
  std::string_view borrowedUtf8 = {};
  std::span<const TextRun> runs = {};
  Rect bounds = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  TextStyle style = {};
  float runScale = 1.0f;
  float opacity = 1.0f;

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
  BorderStyle border = {};
  float opacity = 1.0f;
};

// A screen-space cubic stroke. Unlike an atlas ShapeId, its control points stay in the
// destination coordinate system, so resizing a panel never anisotropically stretches one
// pre-rasterized curve. VulkanRenderer adaptively tessellates the cubic into analytic AA
// segments at frame-build time.
struct CubicBezierCommand {
  Vec2 from = {};
  Vec2 control1 = {};
  Vec2 control2 = {};
  Vec2 to = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  StrokeStyle style = {};
  float opacity = 1.0f;
};

struct RetainedTextCommand {
  RetainedTextId text = 0;
  Vec2 position = {};
  float scale = 1.0f;
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  float opacity = 1.0f;
};

// Analytic circular stroke: one coverage evaluation, without tessellation seams.
struct ArcCommand {
  Vec2 center = {};
  float radius = 0.0f;
  float startRadians = 0.0f;
  float sweepRadians = 0.0f;
  Rect clip = {};
  StrokeStyle style = {};
  float opacity = 1.0f;
};

// A linear RGBA32F pixel buffer owned by the VulkanRenderer's device. The renderer samples the
// buffer directly in the vector fragment pass, so compute clients can publish an image without a
// CPU readback or a second graphics device. Pixel storage is A,R,G,B float words, matching the
// CompNode/After Effects 32-bpc field contract.
struct PixelBufferCommand {
  Rect destination = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  float opacity = 1.0f;
};

using DisplayCommand = std::variant<DrawCommand, TextCommand, RoundedRectCommand,
                                    CubicBezierCommand, RetainedTextCommand, ArcCommand,
                                    PixelBufferCommand>;

class DrawList {
public:
  void clear();
  void setOpacity(float opacity) { opacity_ = std::clamp(opacity, 0.0f, 1.0f); }
  [[nodiscard]] float opacity() const { return opacity_; }
  void setClip(Rect clip) {
    clip_ = clip;
  }
  [[nodiscard]] Rect clip() const {
    return clip_;
  }

  // Overlay commands are emitted after all regular commands while staying in the same GPU batch.
  void beginOverlay() {
    overlayMode_ = true;
  }
  void endOverlay() {
    overlayMode_ = false;
  }
  [[nodiscard]] bool overlayMode() const {
    return overlayMode_;
  }

  void shape(ShapeId shape, Rect destination, Paint paint);
  void shape(ShapeId shape, Rect destination, Paint paint, Rect clip);
  void fill(ShapeId shape, Rect destination, Paint paint) {
    this->shape(shape, destination, paint);
  }
  void stroke(ShapeId shape, Rect destination, Paint paint) {
    this->shape(shape, destination, paint);
  }
  void stroke(ShapeId shape, Rect destination, const StrokeStyle& style) {
    this->shape(shape, destination, style.paint);
  }
  // Applies radius after destination sizing. radiusPx is absolute framebuffer pixels.
  void roundedRect(Rect destination, float radiusPx, Paint paint,
                   float continuousCornersPercent = 0.0f, BorderStyle border = {});
  // Individual values are optional at the API level: use this overload only where a corner
  // differs. Radii remain absolute framebuffer pixels after destination sizing.
  void roundedRect(Rect destination, CornerRadii radiiPx, Paint paint,
                   CornerSmoothing continuousCorners = {}, BorderStyle border = {});
  void cubicBezier(Vec2 from, Vec2 control1, Vec2 control2, Vec2 to, StrokeStyle style);
  void arc(Vec2 center, float radius, float startRadians, float sweepRadians, StrokeStyle style);
  // Draws the buffer most recently supplied to VulkanRenderer::setExternalPixelBuffer().
  void pixelBuffer(Rect destination, std::uint32_t width, std::uint32_t height);
  void text(std::string utf8, Rect bounds, TextStyle style);
  // The referenced bytes must remain alive until VulkanRenderer::draw() returns.
  void textStatic(std::string_view utf8, Rect bounds, TextStyle style);
  // The referenced runs and their strings must remain alive until VulkanRenderer::draw() returns.
  void textRunsStatic(std::span<const TextRun> runs, Rect bounds, TextStyle paragraphStyle,
                      float runScale = 1.0f);
  // Draws text whose glyph instances were retained by VulkanRenderer. Only this transform and
  // clip are dynamic, so zoom/pan does not relayout or upload the document.
  void retainedText(RetainedTextId text, Vec2 position, float scale);

  [[nodiscard]] const std::vector<DisplayCommand>& commands() const {
    return commands_;
  }
  [[nodiscard]] const std::vector<DisplayCommand>& overlayCommands() const {
    return overlayCommands_;
  }

private:
  Rect clip_ = {0.0f, 0.0f, 100000.0f, 100000.0f};
  std::vector<DisplayCommand> commands_{};
  std::vector<DisplayCommand> overlayCommands_{};
  bool overlayMode_ = false;
  float opacity_ = 1.0f;
};

} // namespace slugvk
