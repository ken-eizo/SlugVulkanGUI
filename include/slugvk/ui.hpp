#pragma once

#include "slugvk/draw_list.hpp"
#include "slugvk/input.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slugvk {

struct Interaction {
  bool hovered = false;
  bool pressed = false;
  bool released = false;
  bool held = false;
  bool clicked = false;
  Vec2 cursor = {};
  Vec2 cursorDelta = {};
};

struct UiSkin {
  ShapeId rectangle = 0;
  ShapeId circle = 0;
  ShapeId check = 0;
  Paint panel = Paint::solid(Color::fromRgb8(0x151c30));
  Paint control = Paint::solid(Color::fromRgb8(0x27314d));
  Paint hovered = Paint::solid(Color::fromRgb8(0x354363));
  Paint active = Paint::solid(Color::fromRgb8(0x5268a2));
  Paint accent = Paint::gradient(GradientKind::Linear,
    Color::fromRgb8(0x6d5dfc), Color::fromRgb8(0x28c7fa));
  Paint muted = Paint::solid(Color::fromRgb8(0x78839b));
  TextStyle text = {};
  float cornerRadius = 8.0f;
  float continuousCornersPercent = 100.0f;
};

struct ListBoxModel {
  std::vector<std::string> items;
  int selected = -1;
};

struct ComboBoxModel {
  std::vector<std::string> items;
  int selected = -1;
  bool open = false;
};

struct TreeNode {
  std::string label;
  std::vector<TreeNode> children;
  bool expanded = true;
  bool selected = false;
};

struct GridModel {
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
  int selectedRow = -1;
  int selectedColumn = -1;
};

class UiContext {
public:
  explicit UiContext(UiSkin skin);

  void beginFrame(const InputState& input, DrawList& drawList);
  void endFrame();

  Interaction interaction(WidgetId id, Rect bounds);
  bool button(WidgetId id, std::string_view label, Rect bounds);
  bool spinButton(WidgetId id, std::string_view label, Rect bounds, int& value,
                  int minimum, int maximum, int step = 1);
  bool slider(WidgetId id, std::string_view label, Rect bounds, float& value,
              float minimum = 0.0f, float maximum = 1.0f);
  bool listBox(WidgetId id, Rect bounds, ListBoxModel& model, float rowHeight = 26.0f);
  bool checkbox(WidgetId id, std::string_view label, Rect bounds, bool& value);
  bool comboBox(WidgetId id, Rect bounds, ComboBoxModel& model);
  bool dropdown(WidgetId id, Rect bounds, ComboBoxModel& model);
  bool radio(WidgetId id, std::string_view label, Rect bounds, bool selected);
  bool textField(WidgetId id, Rect bounds, std::string& value, std::string_view placeholder = {});
  bool toggle(WidgetId id, std::string_view label, Rect bounds, bool& value);
  bool scrollBar(WidgetId id, Rect bounds, float& value, float pageRatio = 0.25f);
  void tooltip(WidgetId anchor, std::string_view text, Rect bounds);
  bool treeView(WidgetId id, Rect bounds, std::vector<TreeNode>& roots, float rowHeight = 24.0f);
  bool gridView(WidgetId id, Rect bounds, GridModel& model, float rowHeight = 25.0f);

  [[nodiscard]] WidgetId hovered() const { return hovered_; }
  [[nodiscard]] WidgetId focused() const { return focused_; }

private:
  Interaction interactionImpl(WidgetId id, Rect bounds, bool overlay);
  void rounded(Rect bounds, Paint paint, float radiusPx = -1.0f);
  void controlBackground(Rect bounds, const Interaction& state, bool selected = false);
  void label(std::string_view value, Rect bounds, HorizontalAlign align = HorizontalAlign::Left,
             Paint* overridePaint = nullptr);
  bool treeNode(WidgetId id, TreeNode& node, float x, float& y, float width,
                float rowHeight, int depth, const Rect& clip);

  UiSkin skin_;
  const InputState* input_ = nullptr;
  DrawList* draw_ = nullptr;
  WidgetId hovered_ = 0;
  WidgetId active_ = 0;
  WidgetId focused_ = 0;
  std::uint64_t frame_ = 0;
  Rect rootClip_ = {};
  Rect overlayRect_ = {};
  WidgetId overlayOwner_ = 0;
  bool overlayClaimed_ = false;
};

} // namespace slugvk
