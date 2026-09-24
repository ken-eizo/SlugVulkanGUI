#include "slugvk/slugvk.hpp"
#include "slugvk/vulkan_interop.hpp"
#include "figma_import_bridge.hpp"
#include "geist_font.generated.hpp"
#include "slugui_demo.generated.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace slugvk;

namespace {

struct Shapes {
  ShapeId rectangle = 0;
  ShapeId circle = 0;
  ShapeId check = 0;
  ShapeId polygon = 0;
  ShapeId star = 0;
  ShapeId solidStroke = 0;
  ShapeId mixedDashStroke = 0;
  ShapeId taperStroke = 0;
};

using FontRequest = std::tuple<std::string, std::uint16_t, bool>;

void collectComponentFonts(const slugui::Element& element, const slugui::PropertyStore& properties,
                           std::map<FontRequest, std::vector<std::uint32_t>>& requests) {
  const auto append = [&](const TextStyle& style, std::string_view text) {
    if (style.fontName.empty() || style.fontName == "system-ui" || style.fontName == "Geist") return;
    auto& codepoints = requests[{style.fontName, style.weight, style.italic}];
    const auto decoded = decodeUtf8(text);
    codepoints.insert(codepoints.end(), decoded.begin(), decoded.end());
  };
  if (const auto* visual = std::get_if<slugui::TextVisual>(&element.visual)) {
    if (visual->runs.empty()) {
      append(visual->style, visual->text.resolve(properties));
    } else {
      for (const auto& run : visual->runs) append(run.style, run.text);
    }
  }
  for (const auto& child : element.children) collectComponentFonts(child, properties, requests);
}

void loadComponentSystemFonts(VectorAtlas& atlas, const slugui::Component& component) {
  std::map<FontRequest, std::vector<std::uint32_t>> requests;
  collectComponentFonts(component.root(), component.properties(), requests);
  for (auto& [key, codepoints] : requests) {
    auto& [family, weight, italic] = key;
    for (std::uint32_t codepoint = 32; codepoint <= 126; ++codepoint)
      codepoints.push_back(codepoint);
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    const std::string path = findSystemFont(family, weight, italic);
    if (path.empty()) {
      std::cerr << "Figma font not installed locally: " << family << " "
                << weight << (italic ? " italic" : "") << '\n';
      continue;
    }
    if (!atlas.loadFont(path, FontFace{family, weight, italic}, codepoints)) {
      std::cerr << "Could not load Figma font: " << family << " from " << path << '\n';
    }
  }
}

Shapes buildAtlas(VectorAtlas& atlas, const slugui::Component* figmaComponent = nullptr) {
  Shapes shapes;
  shapes.rectangle = atlas.addPath(Path{}.rect(0, 0, 200, 40));
  shapes.circle = atlas.addPath(Path{}.circle(50, 50, 50));
  shapes.polygon = atlas.addPath(Path{}.polygon({50, 50}, 48, 6, -0.5f));
  shapes.star = atlas.addPath(Path{}.star({50, 50}, 49, 21, 5, -1.5707963f));

  StrokeStyle iconStroke;
  iconStroke.width = 10;
  iconStroke.cap = LineCap::Round;
  iconStroke.join = LineJoin::Round;
  shapes.check = atlas.addStroke(Path{}.moveTo(8, 50).lineTo(38, 20).lineTo(92, 87), iconStroke);

  Path wave;
  wave.moveTo(0, 50).cubicTo(30, 0, 70, 100, 100, 50).cubicTo(130, 0, 170, 100, 200, 50);
  StrokeStyle solid;
  solid.width = 7;
  solid.cap = LineCap::Round;
  solid.join = LineJoin::Round;
  shapes.solidStroke = atlas.addStroke(wave, solid);
  StrokeStyle mixedDash = solid;
  mixedDash.dashLengths = {18.0f, 10.0f};
  mixedDash.cap = LineCap::Butt;
  mixedDash.dashEndCap = LineCap::Square;
  mixedDash.dashCaps.resize(3);
  mixedDash.dashCaps[1] = {LineCap::Round, LineCap::Round};
  mixedDash.dashCaps[2] = {LineCap::Square, LineCap::Butt};
  shapes.mixedDashStroke = atlas.addStroke(Path{}.moveTo(0, 0).lineTo(200, 0), mixedDash);
  StrokeStyle tapered = solid;
  tapered.startTaper = 0.05f;
  tapered.endTaper = 1.35f;
  shapes.taperStroke = atlas.addStroke(wave, tapered);

  constexpr std::array<std::uint16_t, 9> geistWeights{
    400, 100, 200, 300, 500, 600, 700, 800, 900,
  };
  for (const auto weight : geistWeights) {
    if (!atlas.loadFontMemory(
          slugvk::example::assets::geistFont, FontFace{"Geist", weight, false})) {
      throw std::runtime_error("Embedded Geist variable font could not be loaded");
    }
  }
  if (figmaComponent) loadComponentSystemFonts(atlas, *figmaComponent);
  atlas.build();
  return shapes;
}

TextStyle textStyle(float size = 14.0f) {
  TextStyle style;
  style.fontName = "Geist";
  style.size = size;
  style.paint = Paint::solid(Color::fromRgb8(0xe9efff));
  return style;
}

void horizontalCap(DrawList& draw, float x, Rect bounds, LineCap cap, bool start,
                   const Paint& paint) {
  const float half = bounds.height * 0.5f;
  if (cap == LineCap::Round) {
    draw.roundedRect({x - half, bounds.y, bounds.height, bounds.height}, half, paint);
  } else if (cap == LineCap::Square) {
    draw.roundedRect({start ? x - half : x, bounds.y, half, bounds.height}, 0.0f, paint);
  }
}

void dynamicDashedLine(DrawList& draw, Rect bounds, float dashLength, float gapLength,
                       float offset, LineCap startCap, LineCap endCap, const Paint& paint) {
  dashLength = std::max(dashLength, 0.5f);
  gapLength = std::max(gapLength, 0.5f);
  const float period = dashLength + gapLength;
  float phase = std::fmod(offset, period);
  if (phase < 0.0f) phase += period;
  const float right = bounds.x + bounds.width;
  for (float x = bounds.x - phase; x < right; x += period) {
    const float start = std::max(x, bounds.x);
    const float end = std::min(x + dashLength, right);
    if (end > start) {
      draw.roundedRect({start, bounds.y, end - start, bounds.height}, 0.0f, paint);
      horizontalCap(draw, start, bounds, startCap, true, paint);
      horizontalCap(draw, end, bounds, endCap, false, paint);
    }
  }
}

const char* capName(LineCap cap) {
  switch (cap) {
    case LineCap::Round: return "Round";
    case LineCap::Square: return "Square";
    default: return "Butt";
  }
}

LineCap nextCap(LineCap cap) {
  return cap == LineCap::Butt ? LineCap::Round
       : cap == LineCap::Round ? LineCap::Square : LineCap::Butt;
}

const char* alignName(StrokeAlign align) {
  switch (align) {
    case StrokeAlign::Inside: return "Inside";
    case StrokeAlign::Outside: return "Outside";
    default: return "Center";
  }
}

StrokeAlign nextAlign(StrokeAlign align) {
  return align == StrokeAlign::Inside ? StrokeAlign::Center
       : align == StrokeAlign::Center ? StrokeAlign::Outside : StrokeAlign::Inside;
}

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

#ifdef _WIN32
std::wstring quoteWindowsArgument(const std::wstring& value) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(character);
    } else {
      result.append(backslashes, L'\\');
      result.push_back(character);
    }
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}
#endif

bool spawnDetached(const std::filesystem::path& executable,
                   const std::vector<std::string>& arguments) {
#ifdef _WIN32
  std::wstring commandLine = quoteWindowsArgument(executable.wstring());
  for (const auto& argument : arguments) {
    commandLine.push_back(L' ');
    commandLine += quoteWindowsArgument(std::filesystem::path(argument).wstring());
  }
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(
    executable.wstring().c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  if (!started) return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#elif defined(__APPLE__)
  std::vector<std::string> owned;
  owned.reserve(arguments.size() + 1);
  owned.push_back(executable.string());
  owned.insert(owned.end(), arguments.begin(), arguments.end());
  std::vector<char*> pointers;
  pointers.reserve(owned.size() + 1);
  for (auto& argument : owned) pointers.push_back(argument.data());
  pointers.push_back(nullptr);
  pid_t process = 0;
  return posix_spawn(&process, executable.c_str(), nullptr, nullptr,
                     pointers.data(), environ) == 0;
#else
  (void)executable;
  (void)arguments;
  return false;
#endif
}

int runProcess(const std::filesystem::path& executable,
               const std::vector<std::string>& arguments) {
#ifdef _WIN32
  std::wstring commandLine = quoteWindowsArgument(executable.wstring());
  for (const auto& argument : arguments) {
    commandLine.push_back(L' ');
    commandLine += quoteWindowsArgument(std::filesystem::path(argument).wstring());
  }
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.wstring().c_str(), mutableCommand.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return -1;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  return static_cast<int>(exitCode);
#elif defined(__APPLE__)
  std::vector<std::string> owned;
  owned.reserve(arguments.size() + 1);
  owned.push_back(executable.string());
  owned.insert(owned.end(), arguments.begin(), arguments.end());
  std::vector<char*> pointers;
  pointers.reserve(owned.size() + 1);
  for (auto& argument : owned) pointers.push_back(argument.data());
  pointers.push_back(nullptr);
  pid_t process = 0;
  if (posix_spawn(&process, executable.c_str(), nullptr, nullptr,
                  pointers.data(), environ) != 0) {
    return -1;
  }
  int status = 0;
  if (waitpid(process, &status, 0) < 0 || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
#else
  (void)executable;
  (void)arguments;
  return -1;
#endif
}

bool sameFileContents(const std::filesystem::path& first,
                      const std::filesystem::path& second) {
  std::error_code error;
  if (!std::filesystem::exists(first, error) || !std::filesystem::exists(second, error) ||
      std::filesystem::file_size(first, error) != std::filesystem::file_size(second, error)) {
    return false;
  }
  std::ifstream left(first, std::ios::binary);
  std::ifstream right(second, std::ios::binary);
  if (!left || !right) return false;
  std::array<char, 65536> leftBytes{};
  std::array<char, 65536> rightBytes{};
  do {
    left.read(leftBytes.data(), static_cast<std::streamsize>(leftBytes.size()));
    right.read(rightBytes.data(), static_cast<std::streamsize>(rightBytes.size()));
    const auto count = left.gcount();
    if (count != right.gcount() ||
        !std::equal(leftBytes.begin(), leftBytes.begin() + count, rightBytes.begin())) {
      return false;
    }
  } while (left);
  return left.eof() && right.eof();
}

bool stageFigmaSource(const std::filesystem::path& source, bool importLoaded, std::string& status) {
  if (lowerAscii(source.extension().string()) != ".slugui") {
    status = "Drop rejected: expected a .slugui file";
    return false;
  }
  std::error_code sourceError;
  if (!std::filesystem::is_regular_file(source, sourceError)) {
    status = "Drop rejected: file could not be opened";
    return false;
  }
  const std::filesystem::path destination =
    std::filesystem::path(SLUGVK_DEVELOPMENT_SOURCE_DIR) /
    "examples" / "figma_group_31.slugui";
  const std::filesystem::path backup = destination.string() + ".drop-backup";
  std::error_code equivalentError;
  const bool identical = std::filesystem::equivalent(source, destination, equivalentError) ||
                         sameFileContents(source, destination);
  if (importLoaded && identical) {
    status = "Already loaded: the dropped .slugui is identical";
    return false;
  }
  if (identical) {
    // The running executable was already built from this exact source. A fresh preview session
    // only needs the lightweight relaunch path; do not parse/generate the same file again.
    status = "Import staged: source is unchanged; reopening the existing preview...";
    return true;
  }

  std::error_code cleanupError;
  std::filesystem::remove(backup, cleanupError);
  std::filesystem::copy_file(
    destination, backup, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(
    source, destination, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::last_write_time(
    destination, std::filesystem::file_time_type::clock::now());

  const std::filesystem::path compiler =
    std::filesystem::path(SLUGVK_DEVELOPMENT_SOURCE_DIR) /
    "tools" / "slugui_compiler.py";
  const std::filesystem::path generated = SLUGVK_FIGMA_GENERATED_HEADER;
  const int generation = runProcess(SLUGVK_PYTHON_EXECUTABLE, {
    compiler.string(), destination.string(), "--output", generated.string(),
    "--namespace", "slugvk::example",
    "--class-name", "Group_31Generated",
  });
  if (generation != 0) {
    std::filesystem::copy_file(
      backup, destination, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(backup, cleanupError);
    status = generation < 0
      ? "Drop rejected: the AOT compiler could not be started"
      : "Drop rejected: .slugui syntax or types are invalid";
    return false;
  }

  // Mark this exact source/compiler revision as checked for CMake's STAMPED SlugUI rule.
  // The header itself remains content-stable, so an unchanged generated result never forces a
  // C++ rebuild just because the exporter/source timestamp changed.
  const std::filesystem::path generatedStamp = generated.string() + ".stamp";
  {
    std::ofstream stamp(generatedStamp, std::ios::binary | std::ios::app);
  }
  std::error_code stampError;
  std::filesystem::last_write_time(
    generatedStamp, std::filesystem::file_time_type::clock::now(), stampError);

  status = "Import staged: generated; compiling the fast preview bridge...";
  return true;
}

bool launchExampleRebuild(const std::filesystem::path& currentExecutable) {
  const std::filesystem::path cmake = SLUGVK_CMAKE_COMMAND;
  const std::filesystem::path script =
    std::filesystem::path(SLUGVK_DEVELOPMENT_SOURCE_DIR) /
    "tools" / "rebuild_example.cmake";
  return spawnDetached(cmake, {
    std::string("-DBUILD_DIR=") + SLUGVK_DEVELOPMENT_BUILD_DIR,
    std::string("-DSOURCE_DIR=") + SLUGVK_DEVELOPMENT_SOURCE_DIR,
    std::string("-DCONFIG=") + SLUGVK_DEVELOPMENT_CONFIG,
    std::string("-DEXECUTABLE_PATH=") + currentExecutable.string(),
    "-P", script.string(),
  });
}

Vec2 componentSize(const slugui::Component& component, Vec2 fallback) {
  const auto width = component.root().layout.width.resolve(component.properties());
  const auto height = component.root().layout.height.resolve(component.properties());
  if ((width.unit == slugui::LengthUnit::LogicalPixels ||
       width.unit == slugui::LengthUnit::PhysicalPixels) && width.value > 0.0f) {
    fallback.x = width.value;
  }
  if ((height.unit == slugui::LengthUnit::LogicalPixels ||
       height.unit == slugui::LengthUnit::PhysicalPixels) && height.value > 0.0f) {
    fallback.y = height.value;
  }
  return fallback;
}

enum class DemoPage { Components, TextZoom, SlugUi, FigmaImport };

constexpr std::string_view longText =
  "SLUG VECTOR TEXT - CONTINUOUS SCALE DEMONSTRATION\n"
  "\n"
  "This page renders every glyph from immutable Slug curve and band textures.\n"
  "The complete document is retained in device-local memory; six quad vertices are generated on GPU.\n"
  "Use the slider, the minus and plus buttons, or the mouse wheel over this page.\n"
  "\n"
  "A vector renderer should remain stable when typography changes every frame.\n"
  "Tiny captions must keep their counters open and their stems consistent.\n"
  "Large display letters must reveal smooth curves instead of bitmap pixels.\n"
  "The same GPU coverage solver handles both ends of that scale continuously.\n"
  "\n"
  "Vulkan records this retained document together with the surrounding dynamic GUI.\n"
  "Zoom and pan update one GPU transform; glyph layout and instances remain immutable.\n"
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
  "Its target is low-latency interaction, stable exact curves, and minimal GPU batches.\n";

#ifdef NDEBUG
constexpr bool validationEnabled = false;
#else
constexpr bool validationEnabled = true;
#endif

} // namespace

int main(int argc, char** argv) try {
  const auto hasArgument = [argc, argv](std::string_view value) {
    for (int i = 1; i < argc; ++i) if (std::string_view(argv[i]) == value) return true;
    return false;
  };
  const bool smokeTest = hasArgument("--smoke");
  const bool figmaPreview = hasArgument("--figma");
  const bool importFailed = hasArgument("--import-failed");
  const bool importSucceeded = hasArgument("--import-success") && !importFailed;
  const bool lowestLatency = !hasArgument("--mailbox");
  VectorAtlas atlas;
  slugvk::example::SlugUiDemoGenerated slugUiDemo(atlas);
  auto figmaImportDemo = slugvk::example::buildFigmaImport(atlas, importSucceeded);
  const Vec2 figmaDesignSize = figmaImportDemo
    ? componentSize(*figmaImportDemo, {236.0f, 339.0f}) : Vec2{};
  const Shapes shapes = buildAtlas(atlas, figmaImportDemo ? &*figmaImportDemo : nullptr);

  Window window({1500, 950, "SlugVulkan - Vector Renderer + Declarative GUI", true, !smokeTest});
  if (smokeTest) {
    // Destroy one of two live wrappers before creating the renderer. The surviving Window must
    // keep GLFW initialized; this guards the process-wide lifetime reference contract.
    Window lifecycleProbe({64, 64, "SlugVulkan GLFW lifetime probe", false, false});
  }
  VulkanRenderer renderer(window, atlas, {
    .clearColor = Color::fromRgb8(0x090d19),
    .validation = smokeTest && validationEnabled,
    .vsync = false,
    .allowTearing = lowestLatency,
    .gpuTimingInterval = smokeTest ? 1U : 0U
  });
  std::cout << "SlugVulkan device: " << renderer.deviceName() << '\n';
  std::cout << "Present mode: " << renderer.presentModeName() << " | frames in flight: 1\n";
  std::cout << "Vector font: " << atlas.fontFamily() << " " << atlas.fontStyle() << '\n';
  std::cout << "Figma import: " << (figmaImportDemo ? "loaded (drop session)" : "none") << '\n';

  ExternalImageId smokeExternalImage = 0;
  if (smokeTest) {
    auto& interop = renderer.vulkanInterop();
    smokeExternalImage = interop.createOwnedRgba32fImage(2, 2);
    const std::array<float, 16> pixels{
      1.0f, 1.0f, 0.1f, 0.1f,
      1.0f, 0.1f, 1.0f, 0.1f,
      1.0f, 0.1f, 0.1f, 1.0f,
      1.0f, 1.0f, 1.0f, 1.0f,
    };
    if (!interop.updateOwnedRgba32fImage(smokeExternalImage, pixels))
      throw std::runtime_error("Could not initialize the VulkanInterop smoke image");
  }

  UiSkin skin;
  skin.rectangle = shapes.rectangle;
  skin.circle = shapes.circle;
  skin.check = shapes.check;
  skin.text = textStyle(14);
  UiContext ui(skin);
  DrawList draw;
  std::array<TextRun, 3> mixedTextRuns;
  mixedTextRuns[0] = {"Geist 300  ", textStyle(18)};
  mixedTextRuns[0].style.weight = 300;
  mixedTextRuns[0].style.paint = Paint::solid(Color::fromRgb8(0x70e1ff));
  mixedTextRuns[1] = {"700 red  ", textStyle(18)};
  mixedTextRuns[1].style.weight = 700;
  mixedTextRuns[1].style.underline = true;
  mixedTextRuns[1].style.paint = Paint::solid(Color::fromRgb8(0xff5f76));
  mixedTextRuns[2] = {"900 blue", textStyle(18)};
  mixedTextRuns[2].style.weight = 900;
  mixedTextRuns[2].style.strikethrough = true;
  mixedTextRuns[2].style.paint = Paint::solid(Color::fromRgb8(0x687dff));
  TextStyle retainedDocumentStyle = textStyle(16.0f);
  retainedDocumentStyle.lineHeight = 1.42f;
  retainedDocumentStyle.letterSpacing = 0.1f;
  retainedDocumentStyle.paint = Paint::gradient(GradientKind::Linear,
    Color::fromRgb8(0xf5f8ff), Color::fromRgb8(0x79d9ff), {0.0f, 0.0f}, {1.0f, 0.7f});
  const RetainedTextId retainedDocument = renderer.createRetainedText(
    longText, {0.0f, 0.0f, 1400.0f, 5000.0f}, retainedDocumentStyle);
  if (retainedDocument == 0) throw std::runtime_error("Could not retain the long text document");

  DemoPage page = figmaPreview ? DemoPage::FigmaImport : DemoPage::Components;
  float textZoom = 1.0f;
  Vec2 textPan = {};
  float figmaZoom = 1.0f;
  Vec2 figmaPan = {};
  int spinValue = 12;
  float sliderValue = 0.42f;
  float continuousCorners = 50.0f;
  float dashLength = 13.0f;
  float dashGap = 8.0f;
  float dashOffset = 4.0f;
  LineCap dashStartCap = LineCap::Butt;
  LineCap dashEndCap = LineCap::Butt;
  StrokeAlign borderAlign = StrokeAlign::Inside;
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

  namespace sui = slugui;
  auto& slugUiComponent = slugUiDemo.component;
  slugUiComponent.properties().bind<std::string>(
    slugUiDemo.counter_label, {slugUiDemo.clicks.id},
    [&slugUiDemo](const sui::PropertyStore& values) {
      return "Immediate clicks: " + std::to_string(values.get(slugUiDemo.clicks));
    });
  slugUiComponent.on(slugvk::example::SlugUiDemoGenerated::callback_increment,
    [&slugUiDemo](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) {
        slugUiDemo.component.properties().set(
          slugUiDemo.clicks,
          slugUiDemo.component.properties().get(slugUiDemo.clicks) + 1);
      }
    });
  slugUiComponent.on(slugvk::example::SlugUiDemoGenerated::callback_toggle_popup,
    [&slugUiDemo](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) {
        slugUiDemo.component.properties().set(
          slugUiDemo.popup_visible,
          !slugUiDemo.component.properties().get(slugUiDemo.popup_visible));
      }
    });
  sui::Runtime slugUiRuntime;
  sui::Runtime figmaRuntime;
  RetainedDrawListId figmaRetainedDrawList = 0;
  if (figmaImportDemo) {
    // Imported Figma documents are static for the lifetime of this preview process. Compile the
    // full scene graph once, keep its Slug instances in the device-local retained arena, and use
    // only a translation/scale/clip transform while panning and zooming. Rebuilding thousands of
    // glyph/shape instances every frame defeats Slug's resolution-independent retained geometry.
    DrawList retainedSource;
    figmaRuntime.render(*figmaImportDemo, sui::FrameInput{}, retainedSource,
                        {0.0f, 0.0f, figmaDesignSize.x, figmaDesignSize.y}, 1.0f);
    figmaRetainedDrawList = renderer.createRetainedDrawList(retainedSource);
    if (figmaRetainedDrawList == 0)
      throw std::runtime_error("Could not retain the imported Figma DrawList");
  }

  Tween<float> motion = tween(0.0f, 1.0f, 1800.0f, Easing::Spring);
  bool reverseMotion = false;
  int smokeFrames = 0;
  auto previous = std::chrono::steady_clock::now();
  float smoothedFrameMs = 1000.0f / 60.0f;
  bool refreshRendered = false;
  Vec2 lastRefreshSize = {};
  std::uint32_t liveRefreshCount = 0;
  std::optional<std::filesystem::path> pendingDrop;
  bool rebuildAfterExit = false;
  std::string dropStatus = importSucceeded
    ? "Drop import succeeded: generated C++ and Slug atlas were rebuilt"
    : importFailed
      ? "Drop import failed: see build/slugui_drop_rebuild.log"
      : "No file imported. Drop a .slugui to preview it for this session only.";

  window.setDropCallback([&](const std::vector<std::string>& paths) {
    const auto found = std::find_if(paths.rbegin(), paths.rend(), [](const std::string& path) {
      return lowerAscii(std::filesystem::path(path).extension().string()) == ".slugui";
    });
    if (found == paths.rend()) {
      dropStatus = "Drop rejected: expected a .slugui file";
      return;
    }
    pendingDrop = std::filesystem::path(*found);
  });

  const auto drawFrame = [&] {
    const auto now = std::chrono::steady_clock::now();
    const float deltaMs = std::chrono::duration<float, std::milli>(now - previous).count();
    previous = now;
    if (deltaMs > 0.0f && deltaMs < 100.0f)
      smoothedFrameMs += (deltaMs - smoothedFrameMs) * 0.08f;
    if (!motion.running()) {
      reverseMotion = !reverseMotion;
      motion.restart(reverseMotion ? 1.0f : 0.0f, reverseMotion ? 0.0f : 1.0f, 1800.0f, Easing::Spring);
    }
    const float animated = motion.update(deltaMs);
    if (smokeTest) {
      if (figmaPreview) page = DemoPage::FigmaImport;
      else if (smokeFrames >= 9) page = DemoPage::TextZoom;
      else if (smokeFrames >= 6) page = DemoPage::FigmaImport;
      else if (smokeFrames >= 3) page = DemoPage::SlugUi;
    }

    draw.clear();
    const Vec2 framebuffer = window.framebufferSize();
    draw.setClip({0, 0, framebuffer.x, framebuffer.y});
    if (smokeExternalImage != 0)
      draw.externalImage(smokeExternalImage, {2, 2, 12, 12}, 2, 2);

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

    // Sample after the static header is declared, immediately before latency-critical interactions.
    window.resampleCursor();
    ui.beginFrame(window.input(), draw);
    const float navigationX = std::max(430.0f, framebuffer.x - 628.0f);
    if (ui.button(hashId("page-components"),
                  page == DemoPage::Components ? "[ Components ]" : "Components",
                  {navigationX, 34, 145, 34}))
      page = DemoPage::Components;
    if (ui.button(hashId("page-text-zoom"),
                  page == DemoPage::TextZoom ? "[ Text Zoom ]" : "Text Zoom",
                  {navigationX + 153.0f, 34, 145, 34}))
      page = DemoPage::TextZoom;
    if (ui.button(hashId("page-slugui"),
                  page == DemoPage::SlugUi ? "[ SlugUI IR ]" : "SlugUI IR",
                  {navigationX + 306.0f, 34, 145, 34}))
      page = DemoPage::SlugUi;
    if (ui.button(hashId("page-figma-import"),
                  page == DemoPage::FigmaImport ? "[ Figma Import ]" : "Figma Import",
                  {navigationX + 459.0f, 34, 145, 34}))
      page = DemoPage::FigmaImport;

    if (page == DemoPage::Components) {

    // Paint and vector geometry gallery.
    draw.roundedRect({18, 105, 420, 228}, 14.0f, skin.panel, 100.0f);
    draw.text("GPU VECTOR PAINTS", {34, 116, 390, 24}, textStyle(13));
    draw.roundedRect({35, 149, 180, 37}, {3.0f, 8.0f, 14.0f, 18.0f},
      Paint::solid(Color::fromRgb8(0x4f67ff), 0.84f), {0.0f, 35.0f, 70.0f, 100.0f});
    ui.slider(hashId("continuous-corners"), "Corner %", {235, 184, 180, 20},
              continuousCorners, 0.0f, 100.0f);
    BorderStyle alignedBorder;
    alignedBorder.paint = Paint::gradient(
      GradientKind::Linear, Color::fromRgb8(0x70e1ff),
      Color::fromRgb8(0x8cff81), {0, 0}, {1, 0});
    alignedBorder.align = borderAlign;
    alignedBorder.individualWidths = BorderWidths{1.0f, 3.0f, 5.0f, 7.0f};
    draw.roundedRect({235, 149, 180, 37}, 10.0f,
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0xff4d8d), Color::fromRgb8(0xffca55), {0, 0}, {1, 0}),
      continuousCorners, alignedBorder);
    if (ui.button(hashId("stroke-align"), std::string("Stroke: ") + alignName(borderAlign),
                  {275, 112, 140, 26}))
      borderAlign = nextAlign(borderAlign);
    draw.shape(shapes.polygon, {38, 204, 72, 72},
      Paint::gradient(GradientKind::Diamond, Color::fromRgb8(0x86f7d4), Color::fromRgb8(0x116a9c), {.5f, .5f}, {1, 1}));
    draw.shape(shapes.circle, {130, 204, 72, 72},
      Paint::gradient(GradientKind::Radial, Color::fromRgb8(0xffffff), Color::fromRgb8(0x6c3cff), {.5f, .5f}, {1, .5f}));
    Paint shaderPaint = Paint::shader(Color::fromRgb8(0x15e0b8), Color::fromRgb8(0x853cff), animated);
    draw.fill(shapes.star, {222 + animated * 28.0f, 204, 72, 72}, shaderPaint);
    draw.stroke(shapes.solidStroke, {310, 200, 105, 26}, shaderPaint);
    ui.slider(hashId("dash-length"), "Dash " + std::to_string(static_cast<int>(dashLength + 0.5f)),
              {35, 312, 120, 18}, dashLength, 2.0f, 32.0f);
    ui.slider(hashId("dash-gap"), "Gap " + std::to_string(static_cast<int>(dashGap + 0.5f)),
              {165, 312, 120, 18}, dashGap, 1.0f, 24.0f);
    ui.slider(hashId("dash-offset"), "Off " + std::to_string(static_cast<int>(dashOffset + 0.5f)),
              {295, 312, 120, 18}, dashOffset, 0.0f, 64.0f);
    dynamicDashedLine(draw, {310, 243, 105, 7}, dashLength, dashGap, dashOffset,
      dashStartCap, dashEndCap,
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0xff5e9c), Color::fromRgb8(0xffd66b)));
    draw.stroke(shapes.mixedDashStroke, {310, 256, 105, 10}, shaderPaint);
    draw.stroke(shapes.taperStroke, {310, 276, 105, 25}, Paint::solid(Color::fromRgb8(0x8cff81)));
    if (ui.button(hashId("dash-start-cap"), std::string("Start: ") + capName(dashStartCap),
                  {35, 282, 120, 24}))
      dashStartCap = nextCap(dashStartCap);
    if (ui.button(hashId("dash-end-cap"), std::string("End: ") + capName(dashEndCap),
                  {165, 282, 120, 24}))
      dashEndCap = nextCap(dashEndCap);
    draw.text("per-corner / opacity", {42, 155, 165, 20}, textStyle(12));
    draw.text("Corners " + std::to_string(static_cast<int>(continuousCorners + 0.5f)) +
              "% / T1 R3 B5 L7",
              {242, 155, 165, 20}, textStyle(12));
    draw.text("mixed per-dash overrides", {292, 294, 125, 18}, textStyle(10));

    // Typography gallery.
    draw.roundedRect({18, 348, 420, 238}, 14.0f, skin.panel, 100.0f);
    draw.text("VECTOR TYPOGRAPHY", {34, 359, 390, 24}, textStyle(13));
    TextStyle display = textStyle(30);
    display.bold = true;
    display.paint = Paint::gradient(GradientKind::Linear, Color::fromRgb8(0x70e1ff), Color::fromRgb8(0xb777ff));
    draw.text("Crisp at every scale", {35, 390, 385, 42}, display);
    draw.textRunsStatic(mixedTextRuns, {35, 443, 385, 30}, textStyle(18));
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
    } else if (page == DemoPage::TextZoom) {
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
      draw.retainedText(retainedDocument,
                        {document.x + 24.0f + textPan.x, document.y + 38.0f + textPan.y},
                        textZoom);
      draw.setClip(oldClip);
    } else if (page == DemoPage::SlugUi) {
      const Rect slugUiViewport{
        18.0f, 105.0f, framebuffer.x - 36.0f,
        std::max(160.0f, framebuffer.y - 167.0f)};
      slugUiRuntime.render(slugUiDemo.component, window.input(), draw, slugUiViewport,
                           window.contentScale());
    } else {
      draw.roundedRect({18, 105, framebuffer.x - 36, framebuffer.y - 167},
                       14.0f, skin.panel, 100.0f);
      draw.text("FIGMA -> SLUGUI -> AOT -> SLUG/VULKAN",
                {38, 119, 520, 27}, textStyle(15));
      TextStyle help = textStyle(12);
      help.paint = skin.muted;
      draw.text("Figma .slugui | wheel to zoom | left-drag to pan | SVG fill/stroke geometry uses Slug",
                {38, 145, framebuffer.x - 280, 22}, help);
      TextStyle dropHelp = textStyle(11);
      dropHelp.paint = importFailed ? Paint::solid(Color::fromRgb8(0xff6b7a)) : skin.muted;
      draw.text(dropStatus, {38, 163, framebuffer.x - 280, 18}, dropHelp);
      if (ui.button(hashId("figma-reset"), "Reset view",
                    {framebuffer.x - 150, 126, 112, 36})) {
        figmaZoom = 1.0f;
        figmaPan = {};
      }

      const Rect canvas{38.0f, 178.0f, framebuffer.x - 76.0f,
                        std::max(100.0f, framebuffer.y - 242.0f)};
      const Interaction drag = figmaImportDemo
        ? ui.interaction(hashId("figma-import-pan"), canvas) : Interaction{};
      draw.roundedRect(canvas, 12.0f,
        drag.held ? Paint::solid(Color::fromRgb8(0x151d31))
                  : Paint::solid(Color::fromRgb8(0x0d1323)), 100.0f);
      if (figmaImportDemo) {
        if (drag.held) figmaPan = figmaPan + drag.cursorDelta;
        const Vec2 canvasCenter{canvas.x + canvas.width * 0.5f,
                                canvas.y + canvas.height * 0.5f};
        const Vec2 designSize = figmaDesignSize;
        const float fitScale = std::max(0.1f, std::min(
          (canvas.width - 40.0f) / designSize.x,
          (canvas.height - 40.0f) / designSize.y));
        const float previousZoom = figmaZoom;
        const Vec2 cursor = window.input().cursorPosition();
        if (canvas.contains(cursor) && std::abs(window.input().scroll().delta.y) > 0.0001f)
          figmaZoom *= std::exp(window.input().scroll().delta.y * 0.13f);
        figmaZoom = std::clamp(figmaZoom, 0.2f, 8.0f);
        if (std::abs(figmaZoom - previousZoom) > 0.000001f) {
          const float oldScale = fitScale * previousZoom;
          const float newScale = fitScale * figmaZoom;
          const Vec2 oldOrigin = canvasCenter + figmaPan - designSize * (oldScale * 0.5f);
          const Vec2 localAnchor = (cursor - oldOrigin) * (1.0f / oldScale);
          figmaPan = cursor - canvasCenter + designSize * (newScale * 0.5f) -
                     localAnchor * newScale;
        }

        const float renderScale = fitScale * figmaZoom;
        const Vec2 origin = canvasCenter + figmaPan - designSize * (renderScale * 0.5f);
        if (figmaRetainedDrawList != 0) {
          const Rect previousClip = draw.clip();
          draw.setClip(canvas);
          draw.retainedDrawList(figmaRetainedDrawList, origin, renderScale);
          draw.setClip(previousClip);
        } else {
          figmaRuntime.render(*figmaImportDemo, window.input(), draw,
                              {origin.x, origin.y, designSize.x * renderScale,
                               designSize.y * renderScale}, renderScale);
        }
      } else {
        TextStyle empty = textStyle(18);
        empty.align = HorizontalAlign::Center;
        empty.paint = skin.muted;
        draw.text("No .slugui imported - drop a file to preview",
                  {canvas.x, canvas.y + canvas.height * 0.5f - 18, canvas.width, 36}, empty);
      }
    }

    ui.endFrame();

    const auto previousStats = renderer.stats();
    std::ostringstream footer;
    footer << std::fixed << std::setprecision(1)
           << 1000.0f / std::max(smoothedFrameMs, 0.001f) << " fps | "
           << previousStats.cpuBuildMilliseconds << " ms build + "
           << previousStats.cpuUploadMilliseconds << " ms upload + "
           << previousStats.cpuSubmitMilliseconds << " ms submit / "
           << previousStats.gpuMilliseconds << " ms GPU | "
           << previousStats.uploadedBytes / 1024.0f << " KiB upload | "
           << previousStats.drawCalls << " draw | " << previousStats.quads << " quads ("
           << previousStats.retainedQuads << " retained)";
    draw.text(footer.str(), {24, framebuffer.y - 42, framebuffer.x - 48, 25}, textStyle(12));
    renderer.draw(draw);
  };

  window.setRefreshCallback([&] {
    if (window.shouldClose()) return;
    const Vec2 size = window.framebufferSize();
    if (refreshRendered && size.x == lastRefreshSize.x && size.y == lastRefreshSize.y) return;
    const bool hadPreparedFrame = !refreshRendered;
    renderer.prepareFrame();
    drawFrame();
    // The normal loop may have acquired the old-size image before entering the Win32 modal loop.
    // Submit it once, then immediately render the first exact-size frame. Later WM_SIZE callbacks
    // arrive with no pre-acquired image and need only one draw.
    if (hadPreparedFrame) {
      renderer.prepareFrame();
      drawFrame();
    }
    lastRefreshSize = size;
    refreshRendered = true;
    ++liveRefreshCount;
  });

  while (!window.shouldClose()) {
    refreshRendered = false;
    renderer.prepareFrame();
    window.pollEvents();
    if (pendingDrop) {
      try {
        if (stageFigmaSource(*pendingDrop, figmaImportDemo.has_value(), dropStatus)) {
          rebuildAfterExit = true;
          window.requestClose();
        }
      } catch (const std::exception& error) {
        dropStatus = std::string("Drop import failed: ") + error.what();
      }
      pendingDrop.reset();
    }
    if (!refreshRendered) drawFrame();
    if (smokeTest) {
      ++smokeFrames;
      if (smokeFrames == 3) window.setSize(1320, 820);
      if (smokeFrames >= 12) window.requestClose();
    }
  }
  window.setRefreshCallback({});
  window.setDropCallback({});
  renderer.waitIdle();
  if (smokeExternalImage != 0)
    renderer.vulkanInterop().unregisterExternalImage(smokeExternalImage);
  if (rebuildAfterExit) {
    const auto executable = std::filesystem::absolute(argv[0]);
    if (!launchExampleRebuild(executable))
      throw std::runtime_error("Could not start the detached .slugui rebuild helper");
  }
  if (smokeTest) {
    const auto finalStats = renderer.stats();
    std::cout << "Smoke batch: " << finalStats.drawCalls << " draw, " << finalStats.quads << " quads, "
              << std::fixed << std::setprecision(3) << finalStats.cpuBuildMilliseconds
              << " ms build, " << finalStats.cpuUploadMilliseconds << " ms upload, "
              << finalStats.cpuSubmitMilliseconds << " ms submit, "
              << finalStats.gpuMilliseconds << " ms GPU, "
              << finalStats.uploadedBytes / 1024.0f << " KiB upload, "
              << liveRefreshCount << " live resize redraws\n";
    if (finalStats.drawCalls == 0 || finalStats.quads == 0 ||
        (!figmaPreview && finalStats.retainedQuads == 0) || liveRefreshCount == 0)
      throw std::runtime_error("Smoke test produced an empty GPU batch");
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "SlugVulkan fatal error: " << error.what() << '\n';
  return 1;
}
