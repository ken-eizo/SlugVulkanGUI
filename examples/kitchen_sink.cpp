#include "slugvk/slugvk.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace slugvk;

namespace {

struct Shapes {
  ShapeId rectangle = 0;
  ShapeId circle = 0;
  ShapeId check = 0;
  ShapeId chevron = 0;
  ShapeId polygon = 0;
  ShapeId star = 0;
  ShapeId solidStroke = 0;
  ShapeId dashedStroke = 0;
  ShapeId taperStroke = 0;
};

Shapes buildAtlas(VectorAtlas& atlas) {
  Shapes shapes;
  shapes.rectangle = atlas.addPath(Path{}.rect(0, 0, 200, 40));
  shapes.circle = atlas.addPath(Path{}.circle(50, 50, 50));
  shapes.polygon = atlas.addPath(Path{}.polygon({50, 50}, 48, 6, -0.5f));
  shapes.star = atlas.addPath(Path{}.star({50, 50}, 49, 21, 5, -1.5707963f));

  StrokeStyle iconStroke;
  iconStroke.width = 10;
  iconStroke.cap = LineCap::Round;
  iconStroke.join = LineJoin::Round;
  shapes.check = atlas.addStroke(Path{}.moveTo(8, 50).lineTo(38, 80).lineTo(92, 13), iconStroke);
  shapes.chevron = atlas.addStroke(Path{}.moveTo(20, 28).lineTo(50, 60).lineTo(80, 28), iconStroke);

  Path wave;
  wave.moveTo(0, 50).cubicTo(30, 0, 70, 100, 100, 50).cubicTo(130, 0, 170, 100, 200, 50);
  StrokeStyle solid;
  solid.width = 7;
  solid.cap = LineCap::Round;
  solid.join = LineJoin::Round;
  shapes.solidStroke = atlas.addStroke(wave, solid);
  StrokeStyle dashed = solid;
  dashed.width = 5;
  dashed.dashLengths = {13, 8, 3, 8};
  dashed.dashOffset = 4;
  shapes.dashedStroke = atlas.addStroke(wave, dashed);
  StrokeStyle tapered = solid;
  tapered.startTaper = 0.05f;
  tapered.endTaper = 1.35f;
  shapes.taperStroke = atlas.addStroke(wave, tapered);

  const std::string font = findDefaultSystemFont();
  if (font.empty() || !atlas.loadFont(font))
    throw std::runtime_error("No usable system font was found");
  atlas.build();
  return shapes;
}

TextStyle textStyle(float size = 14.0f) {
  TextStyle style;
  style.size = size;
  style.paint = Paint::solid(Color::fromRgb8(0xe9efff));
  return style;
}

enum class DemoPage { Components, TextZoom };

constexpr std::string_view longText =
  "SLUG VECTOR TEXT - CONTINUOUS SCALE DEMONSTRATION\n"
  "\n"
  "This page renders every glyph from immutable Slug curve and band textures.\n"
  "Only four dynamic vertices are emitted for each visible vector glyph.\n"
  "Use the slider, the minus and plus buttons, or the mouse wheel over this page.\n"
  "\n"
  "A vector renderer should remain stable when typography changes every frame.\n"
  "Tiny captions must keep their counters open and their stems consistent.\n"
  "Large display letters must reveal smooth curves instead of bitmap pixels.\n"
  "The same GPU coverage solver handles both ends of that scale continuously.\n"
  "\n"
  "Vulkan records this complete document together with the surrounding GUI.\n"
  "The curve atlas is retained; positions, clips, colors, and scale are dynamic.\n"
  "Linear, diamond, radial, and procedural paints use the same fragment path.\n"
  "Filled paths, stroked paths, and glyph outlines all consume the same Paint.\n"
  "\n"
  "0123456789  ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
  "abcdefghijklmnopqrstuvwxyz  !?.,:;+-*/=()[]{}\n"
  "\n"
  "Zoom down to inspect dense paragraphs, then zoom in on a single letter.\n"
  "No alternate bitmap font path is selected as the scale changes.\n"
  "This makes animated interfaces predictable and keeps their edges coherent.\n"
  "\n"
  "SlugVulkan is a declarative Windows and macOS vector GUI experiment.\n"
  "Its target is low-latency interaction, stable exact curves, and one GPU batch.\n";

#ifdef NDEBUG
constexpr bool validationEnabled = false;
#else
constexpr bool validationEnabled = true;
#endif

} // namespace

int main(int argc, char** argv) try {
  const bool smokeTest = argc > 1 && std::string_view(argv[1]) == "--smoke";
  VectorAtlas atlas;
  const Shapes shapes = buildAtlas(atlas);

  Window window({1500, 950, "SlugVulkan - Vector Renderer + Declarative GUI", true, !smokeTest});
  VulkanRenderer renderer(window, atlas, {
    .clearColor = Color::fromRgb8(0x090d19),
    .validation = smokeTest && validationEnabled,
    .vsync = false
  });
  std::cout << "SlugVulkan device: " << renderer.deviceName() << '\n';
  std::cout << "Present mode: " << renderer.presentModeName() << " | frames in flight: 1\n";
  std::cout << "Vector font: " << atlas.fontFamily() << " " << atlas.fontStyle() << '\n';

  UiSkin skin;
  skin.rectangle = shapes.rectangle;
  skin.circle = shapes.circle;
  skin.check = shapes.check;
  skin.chevron = shapes.chevron;
  skin.text = textStyle(14);
  UiContext ui(skin);
  DrawList draw;

  DemoPage page = DemoPage::Components;
  float textZoom = 1.0f;
  Vec2 textPan = {};
  int spinValue = 12;
  float sliderValue = 0.42f;
  ListBoxModel list{{"Alpha", "Beta", "Gamma", "Delta"}, 1};
  bool checked = true;
  ComboBoxModel combo{{"Vulkan", "Metal via MoltenVK", "DirectX (future)"}, 0, false};
  ComboBoxModel dropdown{{"Linear", "Diamond", "Radial", "Shader"}, 2, false};
  int radioValue = 0;
  std::string editable = "Editable vector text";
  bool switchValue = true;
  float scrollValue = 0.28f;
  std::vector<TreeNode> tree{
    {"Scene", {{"Vector Shapes", {}, true, false}, {"Text", {}, true, false},
                {"Widgets", {{"Inputs", {}, true, false}, {"Views", {}, true, false}}, true, false}}, true, false},
    {"Renderer", {{"Curve Atlas", {}, true, false}, {"Band Atlas", {}, true, false}}, true, false}
  };
  GridModel grid{{"Component", "State", "Backend"},
    {{"Button", "Interactive", "Slug"}, {"Slider", "0.42", "Slug"},
     {"Text", "Vector", "FreeType"}, {"Present", "Synced", "Vulkan"}}, 1, 0};

  Tween<float> motion = tween(0.0f, 1.0f, 1800.0f, Easing::Spring);
  bool reverseMotion = false;
  int smokeFrames = 0;
  auto previous = std::chrono::steady_clock::now();

  while (!window.shouldClose()) {
    renderer.prepareFrame();
    window.pollEvents();
    const auto now = std::chrono::steady_clock::now();
    const float deltaMs = std::chrono::duration<float, std::milli>(now - previous).count();
    previous = now;
    if (!motion.running()) {
      reverseMotion = !reverseMotion;
      motion.restart(reverseMotion ? 1.0f : 0.0f, reverseMotion ? 0.0f : 1.0f, 1800.0f, Easing::Spring);
    }
    const float animated = motion.update(deltaMs);
    if (smokeTest && smokeFrames >= 6) page = DemoPage::TextZoom;

    draw.clear();
    const Vec2 framebuffer = window.framebufferSize();
    draw.setClip({0, 0, framebuffer.x, framebuffer.y});

    draw.roundedRect({18, 16, framebuffer.x - 36, 74}, 16.0f,
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0x191f38), Color::fromRgb8(0x242d50), {0, 0}, {1, 0}),
      100.0f);
    TextStyle heading = textStyle(25);
    heading.bold = true;
    draw.text("SlugVulkan", {38, 25, 350, 34}, heading);
    TextStyle subtitle = textStyle(13);
    subtitle.paint = Paint::solid(Color::fromRgb8(0x94a3c9));
    draw.text("Exact Slug curves on Vulkan | retained atlas + declarative dynamic batch | Windows / MoltenVK",
              {38, 58, framebuffer.x - 80, 24}, subtitle);

    ui.beginFrame(window.input(), draw, deltaMs);
    const float navigationX = std::max(700.0f, framebuffer.x - 426.0f);
    if (ui.button(hashId("page-components"),
                  page == DemoPage::Components ? "[ Components ]" : "Components",
                  {navigationX, 34, 190, 34}))
      page = DemoPage::Components;
    if (ui.button(hashId("page-text-zoom"),
                  page == DemoPage::TextZoom ? "[ Slug Text Zoom ]" : "Slug Text Zoom",
                  {navigationX + 202.0f, 34, 190, 34}))
      page = DemoPage::TextZoom;

    if (page == DemoPage::Components) {

    // Paint and vector geometry gallery.
    draw.roundedRect({18, 105, 420, 228}, 14.0f, skin.panel, 100.0f);
    draw.text("GPU VECTOR PAINTS", {34, 116, 390, 24}, textStyle(13));
    draw.shape(shapes.rectangle, {35, 149, 180, 37}, Paint::solid(Color::fromRgb8(0x4f67ff), 0.84f));
    draw.roundedRect({235, 149, 180, 37}, 10.0f,
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0xff4d8d), Color::fromRgb8(0xffca55), {0, 0}, {1, 0}),
      100.0f);
    draw.shape(shapes.polygon, {38, 204, 72, 72},
      Paint::gradient(GradientKind::Diamond, Color::fromRgb8(0x86f7d4), Color::fromRgb8(0x116a9c), {.5f, .5f}, {1, 1}));
    draw.shape(shapes.circle, {130, 204, 72, 72},
      Paint::gradient(GradientKind::Radial, Color::fromRgb8(0xffffff), Color::fromRgb8(0x6c3cff), {.5f, .5f}, {1, .5f}));
    Paint shaderPaint = Paint::shader(Color::fromRgb8(0x15e0b8), Color::fromRgb8(0x853cff), animated);
    draw.fill(shapes.star, {222 + animated * 28.0f, 204, 72, 72}, shaderPaint);
    draw.stroke(shapes.solidStroke, {310, 200, 105, 26}, shaderPaint);
    draw.stroke(shapes.dashedStroke, {310, 238, 105, 26},
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0xff5e9c), Color::fromRgb8(0xffd66b)));
    draw.stroke(shapes.taperStroke, {310, 276, 105, 25}, Paint::solid(Color::fromRgb8(0x8cff81)));
    draw.text("solid / opacity", {42, 155, 165, 20}, textStyle(12));
    draw.text("linear", {242, 155, 165, 20}, textStyle(12));
    draw.text("shader fill + line", {220, 294, 190, 18}, textStyle(11));

    // Typography gallery.
    draw.roundedRect({18, 348, 420, 238}, 14.0f, skin.panel, 100.0f);
    draw.text("VECTOR TYPOGRAPHY", {34, 359, 390, 24}, textStyle(13));
    TextStyle display = textStyle(30);
    display.bold = true;
    display.paint = Paint::gradient(GradientKind::Linear, Color::fromRgb8(0x70e1ff), Color::fromRgb8(0xb777ff));
    draw.text("Crisp at every scale", {35, 390, 385, 42}, display);
    TextStyle decorated = textStyle(18);
    decorated.italic = true;
    decorated.underline = true;
    decorated.strikethrough = true;
    decorated.letterSpacing = 1.3f;
    draw.text("Bold  Italic  Underline  Strike", {35, 443, 385, 30}, decorated);
    TextStyle centered = textStyle(15);
    centered.align = HorizontalAlign::Center;
    centered.lineHeight = 1.55f;
    centered.listMarker = ListMarker::Bullet;
    draw.text("Alignment and list state\nLine height and letter spacing", {35, 485, 385, 78}, centered);

    // Input status panel.
    draw.roundedRect({18, 602, 420, 145}, 14.0f, skin.panel, 100.0f);
    draw.text("INPUT STATE (continuous)", {34, 613, 390, 24}, textStyle(13));
    const auto& input = window.input();
    std::ostringstream inputText;
    inputText << std::fixed << std::setprecision(1)
              << "cursor: " << input.cursorPosition().x << ", " << input.cursorPosition().y
              << "   delta/raw: " << input.cursorDelta().x << ", " << input.rawMouseDelta().y << "\n"
              << "left: press=" << input.mouse(MouseButton::Left).pressed
              << " release=" << input.mouse(MouseButton::Left).released
              << " held=" << input.mouse(MouseButton::Left).down << "\n"
              << "wheel: start=" << input.scroll().started << " active=" << input.scroll().active
              << " end=" << input.scroll().ended << " dy=" << input.scroll().delta.y;
    draw.text(inputText.str(), {35, 642, 385, 90}, textStyle(13));

    // Declarative widgets: column 2.
    draw.roundedRect({456, 105, 550, 642}, 14.0f, skin.panel, 100.0f);
    draw.text("DECLARATIVE WIDGETS", {474, 116, 510, 24}, textStyle(13));
    float y = 150.0f;
    const WidgetId buttonId = hashId("primary-button");
    ui.button(buttonId, "Button (hover / press / release)", {475, y, 250, 34});
    ui.tooltip(buttonId, "Tooltip: cursor position and hover are always available", {735, y, 250, 34});
    y += 44;
    ui.spinButton(hashId("spin"), "Spin", {475, y, 250, 34}, spinValue, 0, 100);
    ui.slider(hashId("slider"), "Slider", {735, y, 250, 34}, sliderValue);
    y += 48;
    ui.listBox(hashId("list"), {475, y, 250, 118}, list);
    ui.checkbox(hashId("check"), "Checkbox", {735, y, 250, 32}, checked);
    ui.toggle(hashId("switch"), "Keyboard switch", {735, y + 42, 250, 34}, switchValue);
    ui.radio(hashId("radio-a"), "Radio A", {735, y + 84, 115, 30}, radioValue == 0) ? radioValue = 0 : 0;
    ui.radio(hashId("radio-b"), "Radio B", {860, y + 84, 115, 30}, radioValue == 1) ? radioValue = 1 : 0;
    y += 132;
    TextStyle inputLabel = textStyle(11);
    inputLabel.paint = skin.muted;
    draw.text("TEXT INPUT  |  click here, then type; Backspace / Enter / Escape supported",
              {475, y, 510, 20}, inputLabel);
    y += 22;
    ui.textField(hashId("text-input"), {475, y, 510, 38}, editable, "Type vector text here...");
    y += 48;
    ui.comboBox(hashId("combo"), {475, y, 250, 34}, combo);
    ui.dropdown(hashId("dropdown"), {735, y, 250, 34}, dropdown);
    y += 52;
    ui.treeView(hashId("tree"), {475, y, 300, 172}, tree);
    ui.gridView(hashId("grid-small"), {787, y, 198, 172}, grid);
    ui.scrollBar(hashId("scroll-h"), {475, y + 184, 510, 20}, scrollValue, 0.27f);
    // Views panel: column 3, large grid demonstrates clipping and selection.
    draw.roundedRect({1024, 105, framebuffer.x - 1042, 642}, 14.0f, skin.panel, 100.0f);
    draw.text("TREE / GRID / ANIMATION", {1042, 116, framebuffer.x - 1070, 24}, textStyle(13));
    ui.treeView(hashId("tree-large"), {1042, 150, framebuffer.x - 1080, 196}, tree);
    ui.gridView(hashId("grid-large"), {1042, 360, framebuffer.x - 1080, 220}, grid);
    ui.scrollBar(hashId("scroll-v"), {framebuffer.x - 55, 600, 20, 125}, scrollValue, 0.32f);
    draw.shape(shapes.circle, {1042 + animated * std::max(0.0f, framebuffer.x - 1165), 625, 56, 56}, skin.accent);
    draw.text("Tween: 0 -> 1 / 1800 ms / spring easing", {1042, 690, framebuffer.x - 1100, 28}, textStyle(13));
    } else {
      const Rect document{42.0f, 190.0f, framebuffer.x - 84.0f, std::max(120.0f, framebuffer.y - 254.0f)};
      const auto& zoomInput = window.input();
      const Interaction documentDrag = ui.interaction(hashId("text-document-pan"), document);
      if (documentDrag.held) textPan = textPan + documentDrag.cursorDelta;

      const float previousZoom = textZoom;
      Vec2 zoomAnchor{document.width * 0.5f, document.height * 0.5f};
      if (document.contains(zoomInput.cursorPosition()) && std::abs(zoomInput.scroll().delta.y) > 0.0001f) {
        zoomAnchor = zoomInput.cursorPosition() - Vec2{document.x, document.y};
        textZoom *= std::exp(zoomInput.scroll().delta.y * 0.13f);
      }
      bool resetView = false;

      draw.roundedRect({18, 105, framebuffer.x - 36, framebuffer.y - 167}, 14.0f, skin.panel, 100.0f);
      draw.text("SLUG LONG TEXT / CONTINUOUS ZOOM", {38, 119, 390, 27}, textStyle(15));
      TextStyle help = textStyle(12);
      help.paint = skin.muted;
      draw.text("Wheel to zoom at cursor. Left-drag the document to pan 1:1. Slider and buttons are also live.",
                {38, 145, 610, 22}, help);

      if (ui.button(hashId("zoom-minus"), "-", {framebuffer.x - 630, 126, 48, 36}))
        textZoom = std::max(0.25f, textZoom / 1.25f);
      if (ui.button(hashId("zoom-reset"), "Reset", {framebuffer.x - 570, 126, 78, 36})) {
        textZoom = 1.0f;
        textPan = {};
        resetView = true;
      }
      if (ui.button(hashId("zoom-plus"), "+", {framebuffer.x - 480, 126, 48, 36}))
        textZoom = std::min(8.0f, textZoom * 1.25f);
      ui.slider(hashId("text-zoom-slider"), "Zoom", {framebuffer.x - 418, 126, 380, 36},
                textZoom, 0.25f, 8.0f);
      textZoom = std::clamp(textZoom, 0.25f, 8.0f);
      if (!resetView && std::abs(textZoom - previousZoom) > 0.000001f) {
        const float ratio = textZoom / previousZoom;
        textPan = zoomAnchor - (zoomAnchor - textPan) * ratio;
      }

      draw.roundedRect(document, 12.0f,
        documentDrag.held ? Paint::solid(Color::fromRgb8(0x17213a)) : Paint::solid(Color::fromRgb8(0x10172a)),
        100.0f);
      std::ostringstream zoomLabel;
      zoomLabel << std::fixed << std::setprecision(0) << textZoom * 100.0f << "%  |  "
                << 16.0f * textZoom << " px vector glyphs";
      draw.text(zoomLabel.str(), {document.x + document.width - 270.0f, document.y + 10.0f, 250.0f, 22.0f}, help);

      const Rect oldClip = draw.clip();
      draw.setClip({document.x + 2.0f, document.y + 2.0f, document.width - 4.0f, document.height - 4.0f});
      TextStyle documentText = textStyle(16.0f * textZoom);
      documentText.lineHeight = 1.42f;
      documentText.letterSpacing = 0.1f * textZoom;
      documentText.paint = Paint::gradient(GradientKind::Linear,
        Color::fromRgb8(0xf5f8ff), Color::fromRgb8(0x79d9ff), {0.0f, 0.0f}, {1.0f, 0.7f});
      draw.text(std::string(longText),
                {document.x + 24.0f + textPan.x, document.y + 38.0f + textPan.y,
                 document.width - 48.0f, document.height - 54.0f},
                documentText);
      draw.setClip(oldClip);
    }

    ui.endFrame();

    const auto previousStats = renderer.stats();
    std::ostringstream footer;
    footer << "GPU batch: " << previousStats.drawCalls << " draw | " << previousStats.vertices
           << " vertices | " << previousStats.indices << " indices | atlas immutable, frame data dynamic";
    draw.text(footer.str(), {24, framebuffer.y - 42, framebuffer.x - 48, 25}, textStyle(12));
    renderer.draw(draw);
    if (smokeTest && ++smokeFrames >= 12) window.requestClose();
  }
  renderer.waitIdle();
  if (smokeTest) {
    const auto finalStats = renderer.stats();
    std::cout << "Smoke batch: " << finalStats.drawCalls << " draw, " << finalStats.vertices
              << " vertices, " << finalStats.indices << " indices\n";
    if (finalStats.drawCalls != 1 || finalStats.vertices == 0 || finalStats.indices == 0)
      throw std::runtime_error("Smoke test produced an empty GPU batch");
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "SlugVulkan fatal error: " << error.what() << '\n';
  return 1;
}
