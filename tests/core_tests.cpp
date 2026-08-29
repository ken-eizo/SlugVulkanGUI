#include "slughorn/slughorn.hpp"
#include "slugvk/slugvk.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int main() try {
  using namespace slugvk;
  require(hashId("stable") == hashId("stable"), "stable widget ids");
  require(hashId("stable") != hashId("different"), "distinct widget ids");
  const auto padded = inset({10, 20, 100, 60}, Insets::symmetric(4, 8));
  require(padded.x == 18 && padded.y == 24 && padded.width == 84 && padded.height == 52,
          "Insets define deterministic padding geometry");
  const float columnSpecs[] = {24.0f, -1.0f, -2.0f};
  const auto columns = gridColumns({0, 0, 100, 20}, columnSpecs, 2.0f);
  require(columns.size() == 3 && columns[0].width == 24.0f &&
              std::abs(columns[1].width * 2.0f - columns[2].width) < 0.0001f,
          "fixed and flex columns share one geometry source");
  LinearLayout rows({0, 0, 80, 100}, Axis::Vertical, 4.0f);
  require(rows.take(20).height == 20 && rows.take(30).y == 24 && rows.remaining().y == 58,
          "linear layout advances by extent and gap");
  TextEditState editor;
  editor.reset("12.5", true);
  require(editor.insert("7") && editor.value == "7", "typing replaces a selected value");
  editor.insert("\xE3\x81\x82");
  require(editor.backspace() && editor.value == "7", "UTF-8 backspace erases one codepoint");
  require(std::abs(ease(Easing::Linear, 0.25f) - 0.25f) < 0.0001f, "linear easing");
  require(std::abs(ease(Easing::SmoothStep, 0.5f) - 0.5f) < 0.0001f, "smoothstep easing");
  auto decoded = decodeUtf8("A\xE3\x81\x82\xF0\x9F\x98\x80");
  require(decoded.size() == 3 && decoded[0] == 'A' && decoded[1] == 0x3042 && decoded[2] == 0x1f600,
          "UTF-8 decoding");
  const auto overlong = decodeUtf8(std::string_view{"\xC0\xAF", 2});
  require(overlong.size() == 2 && overlong[0] == 0xfffdU && overlong[1] == 0xfffdU,
          "overlong UTF-8 rejection");
  const auto surrogate = decodeUtf8(std::string_view{"\xED\xA0\x80", 3});
  require(!surrogate.empty() && surrogate[0] == 0xfffdU, "UTF-8 surrogate rejection");
  const auto truncated = decodeUtf8(std::string_view{"\xE3\x81", 2});
  require(truncated.size() == 2 && truncated[0] == 0xfffdU && truncated[1] == 0xfffdU,
          "truncated UTF-8 rejection");

  InputState input;
  InputWriter inputWriter(input);
  inputWriter.beginFrame();
  inputWriter.cursor({10.0f, 20.0f});
  inputWriter.cursor({13.0f, 24.0f});
  inputWriter.cursor({12.0f, 30.0f});
  inputWriter.finishFrame(1.0);
  require(input.rawMouseDeltas().size() == 3 && input.rawMouseDeltas()[1].x == 3.0f &&
              input.rawMouseDeltas()[1].y == 4.0f && input.rawMouseDelta().x == 12.0f &&
              input.rawMouseDelta().y == 30.0f,
          "per-event raw mouse deltas preserve the frame trajectory");
  inputWriter.beginFrame();
  require(input.rawMouseDeltas().empty() && input.rawMouseDelta().x == 0.0f &&
              input.rawMouseDelta().y == 0.0f,
          "raw mouse delta samples reset at frame start");
  inputWriter.key(16, InputAction::Press);
  inputWriter.key(16, InputAction::Press);
  require(input.key(16).pressed && input.key(16).down,
          "duplicate modifier samples preserve the press edge");
  inputWriter.beginFrame();
  inputWriter.key(16, InputAction::Release);
  inputWriter.key(16, InputAction::Release);
  require(input.key(16).released && !input.key(16).down,
          "duplicate modifier samples preserve the release edge");

  const Paint sharedShader =
      Paint::shader(Color::fromRgb8(0x10e0b0), Color::fromRgb8(0x8040ff), 0.75f);
  require(sharedShader.kind == GradientKind::Shader &&
              std::abs(sharedShader.shaderParameter - 0.75f) < 0.0001f,
          "procedural paint factory");
  const Paint conic = Paint::hsvConic(0.8f);
  require(conic.kind == GradientKind::HsvConic && std::abs(conic.opacity - 0.8f) < 0.0001f &&
              std::abs(conic.origin.x - 0.5f) < 0.0001f &&
              std::abs(conic.origin.y - 0.5f) < 0.0001f,
          "HSV conic paint factory");
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
              std::get<DrawCommand>(paintList.overlayCommands().front()).paint.kind ==
                  GradientKind::Shader,
          "fill and line share Paint");

  constexpr std::string_view persistentText = "persistent text without a frame copy";
  DrawList staticTextList;
  staticTextList.textStatic(persistentText, {0, 0, 300, 30}, {});
  require(std::get<TextCommand>(staticTextList.commands().front()).text() == persistentText,
          "borrowed static text command");
  std::array<TextRun, 2> persistentRuns{{
    {"mixed ", TextStyle{.fontName = "Geist", .weight = 300}},
    {"text", TextStyle{.fontName = "Geist", .weight = 900,
                       .underline = true,
                       .paint = Paint::solid(Color::fromRgb8(0xff4080))}},
  }};
  DrawList richTextList;
  richTextList.textRunsStatic(persistentRuns, {0, 0, 300, 30}, {}, 2.0f);
  const auto& richText = std::get<TextCommand>(richTextList.commands().front());
  require(richText.runs.size() == 2 && richText.runs[1].style.weight == 900 &&
          richText.runs[1].style.underline && richText.runScale == 2.0f,
          "borrowed rich text runs preserve per-range style");

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
                                   {0, 25, 50, 100},
                                   {Paint::solid(Color::fromRgb8(0xff4080)), 3.0f,
                                    StrokeAlign::Inside});
  const auto& individualCorner = std::get<RoundedRectCommand>(individualCornerList.commands().front());
  require(individualCorner.radiiPx.topLeft == 4 && individualCorner.radiiPx.bottomLeft == 28 &&
          individualCorner.continuousCorners.topRightPercent == 25 &&
          individualCorner.continuousCorners.bottomLeftPercent == 100 &&
          individualCorner.border.width == 3.0f &&
          individualCorner.border.align == StrokeAlign::Inside &&
          individualCorner.border.paint.start.r == Color::fromRgb8(0xff4080).r,
          "individual corner radius and smoothing overrides");
  BorderStyle sideBorder;
  sideBorder.width = 9.0f;
  sideBorder.individualWidths = BorderWidths{1.0f, 2.0f, 3.0f, 4.0f};
  const auto resolvedSides = sideBorder.resolvedWidths();
  require(resolvedSides.top == 1.0f && resolvedSides.right == 2.0f &&
          resolvedSides.bottom == 3.0f && resolvedSides.left == 4.0f,
          "individual border widths override the uniform compatibility field");

  DrawList cubicList;
  StrokeStyle cubicStyle;
  cubicStyle.width = 3.25f;
  cubicStyle.cap = LineCap::Round;
  cubicStyle.join = LineJoin::Round;
  cubicStyle.paint = sharedShader;
  cubicList.cubicBezier({10, 20}, {80, 20}, {120, 140}, {220, 140}, cubicStyle);
  require(cubicList.commands().size() == 1, "screen-space cubic stroke command");
  const auto& cubic = std::get<CubicBezierCommand>(cubicList.commands().front());
  require(cubic.from.x == 10 && cubic.control1.x == 80 && cubic.control2.y == 140 &&
              cubic.to.x == 220 && std::abs(cubic.style.width - 3.25f) < 0.0001f,
          "cubic command preserves authored control points and width");

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
  const ShapeId stroke =
      atlas.addStroke(Path{}.moveTo(0, 0).cubicTo(30, 50, 70, -50, 100, 0), dashed);
  StrokeStyle tapered;
  tapered.width = 6;
  tapered.startTaper = 0.0f;
  tapered.endTaper = 1.0f;
  const ShapeId taperStroke =
      atlas.addStroke(Path{}.moveTo(0, 0).quadraticTo(50, 30, 100, 0), tapered);
  StrokeStyle svgStroke;
  svgStroke.width = 2;
  svgStroke.cap = LineCap::Round;
  svgStroke.join = LineJoin::Round;
  const ShapeId relativeSvg =
      atlas.addStroke(Path{}.svgPath("m15 14 5-5-5-5 M20 9H9.5a5.5 5.5 0 0 0 0 11H13",
                                    24.0f),
                      svgStroke);
  require(rectangle && rounded && stroke && taperStroke && relativeSvg,
          "shape registration");
  const auto svgMetric = atlas.metrics(relativeSvg);
  require(svgMetric && svgMetric->bearingY <= 25.1f &&
              svgMetric->bearingY - svgMetric->height >= -1.1f,
          "relative SVG path stays inside reflected viewBox");
  atlas.build();
  require(atlas.built(), "atlas build");
  const auto metric = atlas.metrics(rounded);
  require(metric && metric->width > 0 && metric->height > 0, "shape metrics");
  require(!atlas.native().getCurveTextureData().empty(), "curve texture");
  require(!atlas.native().getBandTextureData().empty(), "band texture");

  const std::string fontPath = findDefaultSystemFont();
  if (!fontPath.empty()) {
    VectorAtlas fontAtlas;
    require(fontAtlas.loadFont(
              fontPath, FontFace{.family = "TestFace", .weight = 400}, {'A'}) &&
            fontAtlas.loadFont(
              fontPath, FontFace{.family = "TestFace", .weight = 700}, {'A'}),
            "explicit font face registration");
    require(fontAtlas.glyph('A', "TestFace", 400) !=
            fontAtlas.glyph('A', "TestFace", 700),
            "font weight selects distinct glyph namespaces");
    require(fontAtlas.hasFontFace("TestFace", false) &&
            !fontAtlas.hasFontFace("TestFace", true),
            "font face style availability");
  }

  {
    namespace sui = slugui;
    sui::Component component(sui::absolute(hashId("property-root")));
    auto& properties = component.properties();
    const auto source = properties.define<float>("source", 3.0f);
    const auto doubled = properties.define<float>("doubled", 0.0f);
    const auto borderWidths = properties.define<BorderWidths>(
      "border-widths", BorderWidths{1.0f, 2.0f, 3.0f, 4.0f});
    properties.bind<float>(doubled, {source.id}, [source](const sui::PropertyStore& values) {
      return values.get(source) * 2.0f;
    });
    require(properties.evaluateBindings() && properties.get(doubled) == 6.0f,
            "SlugUI typed binding initial evaluation");
    require(properties.set(source, 5.0f) && properties.evaluateBindings() &&
            properties.get(doubled) == 10.0f,
            "SlugUI dependency revision evaluation");
    require(properties.find("doubled") == doubled.id &&
            properties.type(doubled.id) == sui::PropertyType::Scalar,
            "SlugUI construction-time property metadata");
    require(properties.type(borderWidths.id) == sui::PropertyType::BorderWidths &&
            properties.get(borderWidths).left == 4.0f,
            "SlugUI typed per-side border width property");
  }

  {
    namespace sui = slugui;
    auto icon = sui::shape(hashId("dual-shape"), 1,
                           Paint::solid(Color::fromRgb8(0x80d8ff)));
    icon.layout.width = sui::Length::physical(32.0f);
    icon.layout.height = sui::Length::physical(32.0f);
    auto& visual = std::get<sui::ShapeVisual>(icon.visual);
    visual.strokeShape = 2;
    visual.strokePaint.normal = Paint::solid(Color::fromRgb8(0xff4080));
    visual.fillPlacement = {0.125f, 0.25f, 0.5f, 0.5f};
    visual.strokePlacement = {0.0625f, 0.1875f, 0.625f, 0.625f};
    sui::Component component(std::move(icon));
    sui::Runtime runtime;
    DrawList list;
    runtime.render(component, sui::FrameInput{}, list, {0, 0, 32, 32});
    require(list.commands().size() == 2,
            "SlugUI vector fill and expanded stroke lower into one ordered batch");
    require(std::get<DrawCommand>(list.commands()[0]).shape == 1 &&
            std::get<DrawCommand>(list.commands()[1]).shape == 2,
            "SlugUI vector stroke draws after its fill");
    const auto fillDestination = std::get<DrawCommand>(list.commands()[0]).destination;
    const auto strokeDestination = std::get<DrawCommand>(list.commands()[1]).destination;
    require(fillDestination.x == 4.0f && fillDestination.y == 8.0f &&
            fillDestination.width == 16.0f && fillDestination.height == 16.0f,
            "SlugUI vector fill preserves its source-coordinate placement");
    require(strokeDestination.x == 2.0f && strokeDestination.y == 6.0f &&
            strokeDestination.width == 20.0f && strokeDestination.height == 20.0f,
            "SlugUI vector stroke preserves its source-coordinate placement");
  }

  {
    namespace sui = slugui;
    auto root = sui::column(hashId("layout-root"));
    root.layout.padding = sui::Insets::all(sui::Length::logical(5.0f));
    root.layout.spacing = sui::Length::logical(2.0f);

    auto fixed = sui::roundedRectangle(hashId("layout-fixed"), sharedShader);
    fixed.layout.width = sui::Length::percent(50.0f);
    fixed.layout.height = sui::Length::logical(10.0f);
    root.add(std::move(fixed));

    auto growing = sui::roundedRectangle(hashId("layout-growing"), sharedShader);
    growing.layout.grow = 1.0f;
    root.add(std::move(growing));

    sui::Component component(std::move(root));
    sui::Runtime runtime;
    runtime.layout(component, {0, 0, 100, 100}, 2.0f);
    const auto* fixedBox = runtime.find(hashId("layout-fixed"));
    const auto* growingBox = runtime.find(hashId("layout-growing"));
    require(fixedBox && growingBox, "SlugUI layout produces stable element boxes");
    require(fixedBox->bounds.x == 10.0f && fixedBox->bounds.y == 10.0f &&
            fixedBox->bounds.width == 40.0f && fixedBox->bounds.height == 20.0f,
            "SlugUI resolves logical pixels and percent once");
    require(growingBox->bounds.x == 10.0f && growingBox->bounds.y == 34.0f &&
            growingBox->bounds.width == 80.0f && growingBox->bounds.height == 56.0f,
            "SlugUI column grow and stretch layout");
  }

  {
    namespace sui = slugui;
    auto root = sui::absolute(hashId("interactive-root"));
    auto button = sui::roundedRectangle(
      hashId("interactive-button"), Paint::solid(Color::fromRgb8(0x203050)),
      CornerRadii::all(6.0f), CornerSmoothing::all(100.0f));
    button.layout.x = sui::Length::physical(10.0f);
    button.layout.y = sui::Length::physical(10.0f);
    button.layout.height = sui::Length::physical(40.0f);
    button.interaction = {true, true, 1};
    auto& buttonVisual = std::get<sui::RoundedRectangleVisual>(button.visual);
    buttonVisual.paint.hovered =
      sui::ValueSource<Paint>{Paint::solid(Color::fromRgb8(0x30a060))};
    buttonVisual.paint.pressed =
      sui::ValueSource<Paint>{Paint::solid(Color::fromRgb8(0x3060d0))};
    buttonVisual.stroke.paint.normal =
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0xff4080),
                      Color::fromRgb8(0x40e0ff));
    buttonVisual.stroke.width = 2.5f;
    buttonVisual.stroke.align = StrokeAlign::Inside;
    root.add(std::move(button));

    auto popup = sui::roundedRectangle(
      hashId("overlay-popup"), Paint::solid(Color::fromRgb8(0xf09030)));
    popup.layout.x = sui::Length::physical(4.0f);
    popup.layout.y = sui::Length::physical(4.0f);
    popup.layout.width = sui::Length::physical(60.0f);
    popup.layout.height = sui::Length::physical(24.0f);
    popup.overlay = true;
    root.add(std::move(popup));

    sui::Component component(std::move(root));
    const auto buttonWidth =
      component.properties().define<sui::Length>("button-width", sui::Length::physical(100.0f));
    component.root().children[0].layout.width = buttonWidth;
    int pressed = 0;
    component.on(1, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Pressed) ++pressed;
      if (event.type == sui::EventType::Activated) {
        component.properties().set(buttonWidth, sui::Length::physical(120.0f));
      }
    });

    sui::Runtime runtime;
    DrawList uiList;
    sui::FrameInput input;
    input.cursor = {20.0f, 20.0f};
    input.primary = {.pressed = true, .released = false, .down = true};
    const auto stats = runtime.render(component, input, uiList, {0, 0, 200, 100});
    const auto* buttonBox = runtime.find(hashId("interactive-button"));
    require(pressed == 1 && stats.callbacks == 3 && stats.layoutPasses == 2,
            "SlugUI press-edge activation updates the current frame");
    require(buttonBox && buttonBox->bounds.width == 120.0f,
            "SlugUI callback property change relayouts without a frame delay");
    require(uiList.commands().size() == 1 && uiList.overlayCommands().size() == 1,
            "SlugUI overlay lowers after regular commands");
    const auto pressedColor =
      std::get<RoundedRectCommand>(uiList.commands().front()).paint.start;
    const auto resolvedBorder =
      std::get<RoundedRectCommand>(uiList.commands().front()).border;
    require(std::abs(pressedColor.r - Color::fromRgb8(0x3060d0).r) < 0.0001f &&
            std::abs(pressedColor.g - Color::fromRgb8(0x3060d0).g) < 0.0001f &&
            std::abs(pressedColor.b - Color::fromRgb8(0x3060d0).b) < 0.0001f,
            "SlugUI pressed paint resolves on the input frame");
    require(resolvedBorder.width == 2.5f &&
            resolvedBorder.align == StrokeAlign::Inside &&
            resolvedBorder.paint.kind == GradientKind::Linear,
            "SlugUI stroke paint and width lower into the current frame");
  }

  std::cout << "SlugVulkan core tests passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Test failure: " << error.what() << '\n';
  return 1;
}
