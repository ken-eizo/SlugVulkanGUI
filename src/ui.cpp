#include "slugvk/ui.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace slugvk {

namespace {
std::string utf8(char32_t cp) {
  std::string out;
  if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
  else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
  return out;
}

WidgetId childId(WidgetId parent, std::uint64_t child) {
  return parent ^ (child + 0x9e3779b97f4a7c15ULL + (parent << 6U) + (parent >> 2U));
}
}

UiContext::UiContext(UiSkin skin) : skin_(std::move(skin)) {
  skin_.text.paint = Paint::solid(Color::fromRgb8(0xeaf0ff));
}

void UiContext::beginFrame(const InputState& inputState, DrawList& drawList, float deltaMs) {
  input_ = &inputState;
  draw_ = &drawList;
  deltaMs_ = deltaMs;
  rootClip_ = drawList.clip();
  overlayClaimed_ = false;
  hovered_ = 0;
  ++frame_;
}

void UiContext::endFrame() {
  if (input_ && input_->mouse(MouseButton::Left).released) active_ = 0;
  if (!overlayClaimed_) {
    overlayOwner_ = 0;
    overlayRect_ = {};
  }
  input_ = nullptr;
  draw_ = nullptr;
}

Interaction UiContext::interaction(WidgetId id, Rect bounds) {
  return interactionImpl(id, bounds, false);
}

Interaction UiContext::interactionImpl(WidgetId id, Rect bounds, bool overlay) {
  Interaction state;
  if (!input_) return state;
  state.cursor = input_->cursorPosition();
  state.cursorDelta = input_->cursorDelta();
  if (!overlay && overlayOwner_ != 0 && overlayRect_.contains(state.cursor)) return state;
  state.hovered = bounds.contains(state.cursor);
  if (state.hovered) hovered_ = id;
  const auto& left = input_->mouse(MouseButton::Left);
  if (state.hovered && left.pressed) {
    active_ = id;
    focused_ = id;
    state.pressed = true;
  }
  state.held = active_ == id && left.down;
  state.released = active_ == id && left.released;
  // Declarative state changes happen on the press edge, so their visual response is in this frame.
  // Release remains independently observable through Interaction::released.
  state.clicked = state.pressed;
  return state;
}

void UiContext::rounded(Rect bounds, Paint paint, float radiusPx) {
  draw_->roundedRect(bounds, radiusPx >= 0.0f ? radiusPx : skin_.cornerRadius,
                     paint, skin_.continuousCornersPercent);
}

void UiContext::controlBackground(Rect bounds, const Interaction& state, bool selected) {
  Paint paint = selected ? skin_.active : skin_.control;
  if (state.held) paint = skin_.active;
  else if (state.hovered) paint = skin_.hovered;
  rounded(bounds, paint);
}

void UiContext::label(std::string_view value, Rect bounds, HorizontalAlign align, Paint* overridePaint) {
  TextStyle style = skin_.text;
  style.align = align;
  if (overridePaint) style.paint = *overridePaint;
  bounds.x += 8.0f;
  bounds.width = std::max(0.0f, bounds.width - 16.0f);
  draw_->text(std::string(value), bounds, style);
}

bool UiContext::button(WidgetId id, std::string_view text, Rect bounds) {
  const auto state = interaction(id, bounds);
  controlBackground(bounds, state);
  label(text, bounds, HorizontalAlign::Center);
  return state.clicked;
}

bool UiContext::spinButton(WidgetId id, std::string_view text, Rect bounds, int& value,
                           int minimum, int maximum, int step) {
  const float buttonWidth = std::min(bounds.height, bounds.width * 0.25f);
  const Rect minus{bounds.x, bounds.y, buttonWidth, bounds.height};
  const Rect plus{bounds.x + bounds.width - buttonWidth, bounds.y, buttonWidth, bounds.height};
  const auto base = interaction(id, bounds);
  const auto minusState = interaction(childId(id, 1), minus);
  const auto plusState = interaction(childId(id, 2), plus);
  const int old = value;
  if (minusState.clicked) value = std::max(minimum, value - step);
  if (plusState.clicked) value = std::min(maximum, value + step);
  controlBackground(bounds, base);
  if (minusState.hovered || minusState.held) rounded(minus, minusState.held ? skin_.active : skin_.hovered);
  if (plusState.hovered || plusState.held) rounded(plus, plusState.held ? skin_.active : skin_.hovered);
  label("-", minus, HorizontalAlign::Center);
  label("+", plus, HorizontalAlign::Center);
  label(std::string(text) + "  " + std::to_string(value),
        {bounds.x + buttonWidth, bounds.y, bounds.width - buttonWidth * 2.0f, bounds.height},
        HorizontalAlign::Center);
  return old != value;
}

bool UiContext::slider(WidgetId id, std::string_view text, Rect bounds, float& value,
                       float minimum, float maximum) {
  const auto state = interaction(id, bounds);
  const float labelWidth = text.empty() ? 8.0f : std::min(100.0f, bounds.width * 0.35f);
  Rect track{bounds.x + labelWidth, bounds.y + bounds.height * 0.43f,
             bounds.width - labelWidth - 12.0f, bounds.height * 0.14f};
  const float range = std::max(maximum - minimum, 0.0001f);
  const float old = value;
  if (state.held && track.width > 0.0f)
    value = std::clamp(minimum + (state.cursor.x - track.x) / track.width * range, minimum, maximum);
  controlBackground(bounds, state);
  if (!text.empty()) label(text, {bounds.x, bounds.y, labelWidth, bounds.height});
  rounded(track, state.hovered ? skin_.hovered : skin_.muted, track.height * 0.5f);
  const float t = std::clamp((value - minimum) / range, 0.0f, 1.0f);
  rounded({track.x, track.y, track.width * t, track.height}, skin_.accent, track.height * 0.5f);
  const float diameter = bounds.height * (state.hovered ? 0.64f : 0.55f);
  draw_->shape(skin_.circle, {track.x + track.width * t - diameter * 0.5f,
                              bounds.y + (bounds.height - diameter) * 0.5f, diameter, diameter},
               state.held ? skin_.active : skin_.accent);
  return std::abs(old - value) > 0.000001f;
}

bool UiContext::listBox(WidgetId id, Rect bounds, ListBoxModel& model, float rowHeight) {
  const auto outer = interaction(id, bounds);
  controlBackground(bounds, outer);
  const Rect oldClip = draw_->clip();
  draw_->setClip(bounds);
  bool changed = false;
  for (std::size_t i = 0; i < model.items.size(); ++i) {
    Rect row{bounds.x + 3.0f, bounds.y + 3.0f + rowHeight * static_cast<float>(i), bounds.width - 6.0f, rowHeight};
    if (row.y >= bounds.y + bounds.height) break;
    const auto state = interaction(childId(id, i + 1U), row);
    if (state.clicked) { model.selected = static_cast<int>(i); changed = true; }
    if (state.hovered || static_cast<int>(i) == model.selected)
      rounded(row, static_cast<int>(i) == model.selected ? skin_.active : skin_.hovered);
    label(model.items[i], row);
  }
  draw_->setClip(oldClip);
  return changed;
}

bool UiContext::checkbox(WidgetId id, std::string_view text, Rect bounds, bool& value) {
  const auto state = interaction(id, bounds);
  if (state.clicked) value = !value;
  const float boxSize = std::min(bounds.height, 24.0f);
  Rect box{bounds.x, bounds.y + (bounds.height - boxSize) * 0.5f, boxSize, boxSize};
  rounded(box, state.hovered ? skin_.hovered : skin_.control);
  if (value) {
    const Rect mark{box.x + 4.0f, box.y + 4.0f, box.width - 8.0f, box.height - 8.0f};
    if (skin_.check) draw_->shape(skin_.check, mark, skin_.accent);
    else rounded(mark, skin_.accent, 3.0f);
  }
  label(text, {bounds.x + boxSize + 4.0f, bounds.y, bounds.width - boxSize - 4.0f, bounds.height});
  return state.clicked;
}

bool UiContext::comboBox(WidgetId id, Rect bounds, ComboBoxModel& model) {
  const auto state = interaction(id, bounds);
  if (state.clicked) model.open = !model.open;
  if (model.open && overlayOwner_ != 0 && overlayOwner_ != id) model.open = false;

  bool changed = false;
  const float rowHeight = bounds.height;
  const Rect menu{bounds.x, bounds.y + bounds.height + 2.0f, bounds.width,
                  rowHeight * static_cast<float>(model.items.size())};
  std::vector<Interaction> rowStates(model.items.size());
  if (model.open) {
    overlayOwner_ = id;
    overlayRect_ = menu;
    overlayClaimed_ = true;
    if (input_ && input_->mouse(MouseButton::Left).pressed &&
        !bounds.contains(input_->cursorPosition()) && !menu.contains(input_->cursorPosition())) {
      model.open = false;
      overlayClaimed_ = false;
    }
    for (std::size_t i = 0; model.open && i < model.items.size(); ++i) {
      const Rect row{menu.x + 2.0f, menu.y + static_cast<float>(i) * rowHeight,
                     menu.width - 4.0f, rowHeight};
      rowStates[i] = interactionImpl(childId(id, i + 17U), row, true);
      if (rowStates[i].clicked) {
        model.selected = static_cast<int>(i);
        model.open = false;
        overlayClaimed_ = false;
        changed = true;
      }
    }
  }

  controlBackground(bounds, state, model.open);
  const std::string value = model.selected >= 0 && model.selected < static_cast<int>(model.items.size())
    ? model.items[static_cast<std::size_t>(model.selected)] : "Select...";
  label(value, {bounds.x, bounds.y, bounds.width - bounds.height, bounds.height});
  label(model.open ? "^" : "v", {bounds.x + bounds.width - bounds.height, bounds.y, bounds.height, bounds.height},
        HorizontalAlign::Center);
  if (model.open) {
    const Rect oldClip = draw_->clip();
    draw_->setClip(rootClip_);
    draw_->beginOverlay();
    rounded({menu.x - 3.0f, menu.y - 3.0f, menu.width + 6.0f, menu.height + 6.0f},
            Paint::solid(Color::fromRgb8(0x080c17), 0.72f), skin_.cornerRadius + 3.0f);
    rounded(menu, skin_.panel);
    for (std::size_t i = 0; i < model.items.size(); ++i) {
      Rect row{menu.x + 2.0f, menu.y + static_cast<float>(i) * rowHeight, menu.width - 4.0f, rowHeight};
      if (rowStates[i].hovered) rounded(row, skin_.hovered);
      label(model.items[i], row);
    }
    draw_->endOverlay();
    draw_->setClip(oldClip);
  }
  return changed;
}

bool UiContext::dropdown(WidgetId id, Rect bounds, ComboBoxModel& model) {
  return comboBox(id, bounds, model);
}

bool UiContext::radio(WidgetId id, std::string_view text, Rect bounds, bool selected) {
  const auto state = interaction(id, bounds);
  const float size = std::min(bounds.height, 22.0f);
  Rect circle{bounds.x, bounds.y + (bounds.height - size) * 0.5f, size, size};
  draw_->shape(skin_.circle, circle, state.hovered ? skin_.hovered : skin_.control);
  if (selected || state.clicked)
    draw_->shape(skin_.circle, {circle.x + 5.0f, circle.y + 5.0f, size - 10.0f, size - 10.0f}, skin_.accent);
  label(text, {bounds.x + size + 4.0f, bounds.y, bounds.width - size - 4.0f, bounds.height});
  return state.clicked;
}

bool UiContext::textField(WidgetId id, Rect bounds, std::string& value, std::string_view placeholder) {
  const auto state = interaction(id, bounds);
  controlBackground(bounds, state, focused_ == id);
  bool changed = false;
  if (focused_ == id && input_) {
    for (char32_t cp : input_->textInput()) { value += utf8(cp); changed = true; }
    if (input_->key(GLFW_KEY_BACKSPACE).pressed && !value.empty()) {
      do { value.pop_back(); } while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U);
      changed = true;
    }
    if (input_->key(GLFW_KEY_ENTER).pressed || input_->key(GLFW_KEY_ESCAPE).pressed) focused_ = 0;
  }
  if (value.empty()) {
    Paint faded = skin_.muted;
    label(placeholder, bounds, HorizontalAlign::Left, &faded);
  } else label(value, bounds);
  if (focused_ == id && (frame_ / 30U) % 2U == 0U) {
    const float approximateX = bounds.x + 9.0f + static_cast<float>(value.size()) * skin_.text.size * 0.52f;
    draw_->shape(skin_.rectangle, {std::min(approximateX, bounds.x + bounds.width - 5.0f), bounds.y + 6.0f,
                                   1.5f, bounds.height - 12.0f}, skin_.accent);
  }
  return changed;
}

bool UiContext::toggle(WidgetId id, std::string_view text, Rect bounds, bool& value) {
  const auto state = interaction(id, bounds);
  bool changed = state.clicked;
  if (state.clicked) value = !value;
  if (focused_ == id && input_ && input_->key(GLFW_KEY_SPACE).pressed) {
    value = !value;
    changed = true;
  }
  const float switchWidth = std::min(48.0f, bounds.width * 0.35f);
  Rect track{bounds.x + bounds.width - switchWidth, bounds.y + 4.0f, switchWidth, bounds.height - 8.0f};
  label(text, {bounds.x, bounds.y, bounds.width - switchWidth - 6.0f, bounds.height});
  rounded(track, value ? skin_.accent : (state.hovered ? skin_.hovered : skin_.control), track.height * 0.5f);
  const float knob = track.height - 6.0f;
  const float knobX = value ? track.x + track.width - knob - 3.0f : track.x + 3.0f;
  draw_->shape(skin_.circle, {knobX, track.y + 3.0f, knob, knob}, Paint::solid(Color::fromRgb8(0xffffff)));
  return changed;
}

bool UiContext::scrollBar(WidgetId id, Rect bounds, float& value, float pageRatio) {
  const auto state = interaction(id, bounds);
  const float old = value;
  value = std::clamp(value, 0.0f, 1.0f);
  pageRatio = std::clamp(pageRatio, 0.05f, 1.0f);
  const bool vertical = bounds.height > bounds.width;
  const float length = vertical ? bounds.height : bounds.width;
  const float thumbLength = std::max(length * pageRatio, vertical ? bounds.width : bounds.height);
  if (state.held) {
    const float p = vertical ? state.cursor.y - bounds.y : state.cursor.x - bounds.x;
    value = std::clamp((p - thumbLength * 0.5f) / std::max(length - thumbLength, 1.0f), 0.0f, 1.0f);
  }
  rounded(bounds, state.hovered ? skin_.hovered : skin_.control);
  Rect thumb = bounds;
  if (vertical) { thumb.y += (length - thumbLength) * value; thumb.height = thumbLength; }
  else { thumb.x += (length - thumbLength) * value; thumb.width = thumbLength; }
  rounded(thumb, state.held ? skin_.active : skin_.accent);
  return std::abs(old - value) > 0.000001f;
}

void UiContext::tooltip(WidgetId anchor, std::string_view text, Rect bounds) {
  if (hovered_ != anchor) return;
  const Rect oldClip = draw_->clip();
  draw_->setClip(rootClip_);
  draw_->beginOverlay();
  rounded(bounds, skin_.panel);
  label(text, bounds);
  draw_->endOverlay();
  draw_->setClip(oldClip);
}

bool UiContext::treeNode(WidgetId id, TreeNode& node, float x, float& y, float width,
                         float rowHeight, int depth, const Rect& clip) {
  Rect row{x, y, width, rowHeight};
  y += rowHeight;
  if (row.y + row.height < clip.y || row.y > clip.y + clip.height) return false;
  const WidgetId nodeId = childId(id, hashId(node.label));
  const auto state = interaction(nodeId, row);
  const float indent = 16.0f * static_cast<float>(depth);
  bool changed = false;
  if (state.clicked) {
    if (!node.children.empty() && state.cursor.x < row.x + indent + 22.0f) node.expanded = !node.expanded;
    else node.selected = !node.selected;
    changed = true;
  }
  if (node.selected || state.hovered)
    rounded(row, node.selected ? skin_.active : skin_.hovered);
  if (!node.children.empty())
    label(node.expanded ? "v" : ">", {row.x + indent, row.y, 18.0f, row.height}, HorizontalAlign::Center);
  label(node.label, {row.x + indent + 18.0f, row.y, row.width - indent - 18.0f, row.height});
  if (node.expanded) {
    for (auto& child : node.children) changed = treeNode(nodeId, child, x, y, width, rowHeight, depth + 1, clip) || changed;
  }
  return changed;
}

bool UiContext::treeView(WidgetId id, Rect bounds, std::vector<TreeNode>& roots, float rowHeight) {
  const auto state = interaction(id, bounds);
  controlBackground(bounds, state);
  const Rect old = draw_->clip();
  draw_->setClip(bounds);
  float y = bounds.y + 3.0f;
  bool changed = false;
  for (auto& root : roots) changed = treeNode(id, root, bounds.x + 3.0f, y, bounds.width - 6.0f, rowHeight, 0, bounds) || changed;
  draw_->setClip(old);
  return changed;
}

bool UiContext::gridView(WidgetId id, Rect bounds, GridModel& model, float rowHeight) {
  const auto outer = interaction(id, bounds);
  controlBackground(bounds, outer);
  if (model.headers.empty()) return false;
  const Rect old = draw_->clip();
  draw_->setClip(bounds);
  const float columnWidth = bounds.width / static_cast<float>(model.headers.size());
  for (std::size_t c = 0; c < model.headers.size(); ++c) {
    Rect cell{bounds.x + columnWidth * static_cast<float>(c), bounds.y, columnWidth, rowHeight};
    draw_->shape(skin_.rectangle, cell, skin_.panel);
    label(model.headers[c], cell, HorizontalAlign::Center);
  }
  bool changed = false;
  for (std::size_t r = 0; r < model.rows.size(); ++r) {
    const float y = bounds.y + rowHeight * static_cast<float>(r + 1U);
    if (y >= bounds.y + bounds.height) break;
    for (std::size_t c = 0; c < model.headers.size(); ++c) {
      Rect cell{bounds.x + columnWidth * static_cast<float>(c), y, columnWidth, rowHeight};
      const auto state = interaction(childId(id, r * model.headers.size() + c + 1U), cell);
      if (state.clicked) {
        model.selectedRow = static_cast<int>(r);
        model.selectedColumn = static_cast<int>(c);
        changed = true;
      }
      if (state.hovered || (model.selectedRow == static_cast<int>(r) && model.selectedColumn == static_cast<int>(c)))
        draw_->shape(skin_.rectangle, cell, state.hovered ? skin_.hovered : skin_.active);
      if (c < model.rows[r].size()) label(model.rows[r][c], cell);
    }
  }
  draw_->setClip(old);
  return changed;
}

} // namespace slugvk
