#include "slugvk/slugvk.hpp"
#include "slughorn/slughorn.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() try {
  using namespace slugvk;
  require(hashId("stable") == hashId("stable"), "stable widget ids");
  require(hashId("stable") != hashId("different"), "distinct widget ids");
  require(std::abs(ease(Easing::Linear, 0.25f) - 0.25f) < 0.0001f, "linear easing");
  require(std::abs(ease(Easing::SmoothStep, 0.5f) - 0.5f) < 0.0001f, "smoothstep easing");
  auto decoded = decodeUtf8("A\xE3\x81\x82\xF0\x9F\x98\x80");
  require(decoded.size() == 3 && decoded[0] == 'A' && decoded[1] == 0x3042 && decoded[2] == 0x1f600,
          "UTF-8 decoding");
  const auto overlong = decodeUtf8(std::string_view{"\xC0\xAF", 2});
  require(overlong.size() == 2 && overlong[0] == 0xfffdU && overlong[1] == 0xfffdU,
          "overlong UTF-8 rejection");
  const auto surrogate = decodeUtf8(std::string_view{"\xED\xA0\x80", 3});
  require(!surrogate.empty() && surrogate[0] == 0xfffdU,
          "UTF-8 surrogate rejection");
  const auto truncated = decodeUtf8(std::string_view{"\xE3\x81", 2});
  require(truncated.size() == 2 && truncated[0] == 0xfffdU && truncated[1] == 0xfffdU,
          "truncated UTF-8 rejection");

  const Paint sharedShader = Paint::shader(Color::fromRgb8(0x10e0b0), Color::fromRgb8(0x8040ff), 0.75f);
  require(sharedShader.kind == GradientKind::Shader && std::abs(sharedShader.shaderParameter - 0.75f) < 0.0001f,
          "procedural paint factory");
  DrawList paintList;
  paintList.fill(1, {0, 0, 100, 40}, sharedShader);
  StrokeStyle paintedStroke;
  paintedStroke.paint = sharedShader;
  paintList.beginOverlay();
  paintList.stroke(2, {0, 50, 100, 20}, paintedStroke);
  paintList.endOverlay();
  require(paintList.commands().size() == 1 && paintList.overlayCommands().size() == 1,
          "overlay command ordering");
  require(std::get<DrawCommand>(paintList.commands().front()).paint.kind == GradientKind::Shader &&
          std::get<DrawCommand>(paintList.overlayCommands().front()).paint.kind == GradientKind::Shader,
          "fill and line share Paint");

  constexpr std::string_view persistentText = "persistent text without a frame copy";
  DrawList staticTextList;
  staticTextList.textStatic(persistentText, {0, 0, 300, 30}, {});
  require(std::get<TextCommand>(staticTextList.commands().front()).text() == persistentText,
          "borrowed static text command");

  DrawList cornerList;
  cornerList.roundedRect({20, 30, 360, 72}, 12.0f, sharedShader, 100.0f);
  require(cornerList.commands().size() == 1, "analytic rounded rectangle command");
  const auto& corner = std::get<RoundedRectCommand>(cornerList.commands().front());
  require(std::abs(corner.destination.width - 360.0f) < 0.0001f &&
          std::abs(corner.destination.height - 72.0f) < 0.0001f &&
          std::abs(corner.radiiPx.topLeft - 12.0f) < 0.0001f &&
          std::abs(corner.radiiPx.bottomRight - 12.0f) < 0.0001f,
          "rounded rectangle keeps absolute pixel radius after sizing");
  DrawList individualCornerList;
  individualCornerList.roundedRect({0, 0, 200, 80}, {4, 12, 20, 28}, sharedShader,
                                   {0, 25, 50, 100});
  const auto& individualCorner = std::get<RoundedRectCommand>(individualCornerList.commands().front());
  require(individualCorner.radiiPx.topLeft == 4 && individualCorner.radiiPx.bottomLeft == 28 &&
          individualCorner.continuousCorners.topRightPercent == 25 &&
          individualCorner.continuousCorners.bottomLeftPercent == 100,
          "individual corner radius and smoothing overrides");

  VectorAtlas atlas;
  const ShapeId rectangle = atlas.addPath(Path{}.rect(0, 0, 100, 40));
  const ShapeId rounded = atlas.addPath(Path{}.roundedRect(0, 0, 100, 40, 8, 100));
  StrokeStyle dashed;
  dashed.width = 3;
  dashed.cap = LineCap::Round;
  dashed.dashLengths = {8, 4};
  dashed.startCap = LineCap::Square;
  dashed.endCap = LineCap::Butt;
  dashed.dashStartCap = LineCap::Butt;
  dashed.dashEndCap = LineCap::Square;
  dashed.dashCaps.resize(2);
  dashed.dashCaps[1] = {LineCap::Round, LineCap::Butt};
  const ShapeId stroke = atlas.addStroke(Path{}.moveTo(0, 0).cubicTo(30, 50, 70, -50, 100, 0), dashed);
  StrokeStyle tapered;
  tapered.width = 6;
  tapered.startTaper = 0.0f;
  tapered.endTaper = 1.0f;
  const ShapeId taperStroke = atlas.addStroke(Path{}.moveTo(0, 0).quadraticTo(50, 30, 100, 0), tapered);
  require(rectangle && rounded && stroke && taperStroke, "shape registration");
  atlas.build();
  require(atlas.built(), "atlas build");
  const auto metric = atlas.metrics(rounded);
  require(metric && metric->width > 0 && metric->height > 0, "shape metrics");
  require(!atlas.native().getCurveTextureData().empty(), "curve texture");
  require(!atlas.native().getBandTextureData().empty(), "band texture");
  std::cout << "SlugVulkan core tests passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Test failure: " << error.what() << '\n';
  return 1;
}
