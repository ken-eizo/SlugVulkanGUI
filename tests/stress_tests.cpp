#include "slugvk/slugui.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace slugvk;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() try {
  namespace sui = slugui;
  constexpr std::uint32_t columns = 100;
  constexpr std::uint32_t rows = 100;
  constexpr float stride = 20.0f;

  auto root = sui::absolute(1, "stress-root");
  root.children.reserve(columns * rows);
  const Paint paint = Paint::solid(Color::fromRgb8(0x304050));
  for (std::uint32_t y = 0; y < rows; ++y) {
    for (std::uint32_t x = 0; x < columns; ++x) {
      auto cell = sui::roundedRectangle(2 + y * columns + x, paint);
      cell.layout.x = sui::Length::physical(x * stride);
      cell.layout.y = sui::Length::physical(y * stride);
      cell.layout.width = sui::Length::physical(12.0f);
      cell.layout.height = sui::Length::physical(12.0f);
      cell.interaction = {true, true, 0};
      root.add(std::move(cell));
    }
  }

  sui::Component component(std::move(root));
  sui::Runtime runtime;
  DrawList first;
  sui::FrameInput input;
  input.cursor = {1005.0f, 1005.0f};
  const Rect viewport{0, 0, columns * stride, rows * stride};
  const auto initial = runtime.render(component, input, first, viewport);
  require(initial.layoutPasses == 1, "initial stress layout must run exactly once");
  require(initial.elements == columns * rows + 1, "all stress elements must resolve");
  require(initial.hitCandidates < 256, "spatial hit testing regressed toward O(n)");

  DrawList stable;
  input.cursor = {1205.0f, 805.0f};
  const auto steady = runtime.render(component, input, stable, viewport);
  require(steady.layoutPasses == 0, "unchanged large UI must not relayout");
  require(steady.hitCandidates < 256, "steady hit testing must stay spatially bounded");

  std::cout << "SlugUI stress passed: elements=" << steady.elements
            << " candidates=" << steady.hitCandidates << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Stress failure: " << error.what() << '\n';
  return 1;
}
