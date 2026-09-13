#include "slugvk/slugui.hpp"

#include "slugvk/vector_atlas.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace slugvk::slugui {
namespace {

bool same(Color a, Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool same(Paint a, Paint b) {
  return a.kind == b.kind && same(a.start, b.start) && same(a.end, b.end) &&
         a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
         a.target.x == b.target.x && a.target.y == b.target.y &&
         a.opacity == b.opacity && a.shaderParameter == b.shaderParameter;
}

bool same(Length a, Length b) { return a.value == b.value && a.unit == b.unit; }

bool same(CornerRadii a, CornerRadii b) {
  return a.topLeft == b.topLeft && a.topRight == b.topRight &&
         a.bottomRight == b.bottomRight && a.bottomLeft == b.bottomLeft;
}

bool same(CornerSmoothing a, CornerSmoothing b) {
  return a.topLeftPercent == b.topLeftPercent && a.topRightPercent == b.topRightPercent &&
         a.bottomRightPercent == b.bottomRightPercent &&
         a.bottomLeftPercent == b.bottomLeftPercent;
}

bool same(BorderWidths a, BorderWidths b) {
  return a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom && a.left == b.left;
}

bool same(const PropertyValue& a, const PropertyValue& b) {
  if (a.index() != b.index()) return false;
  return std::visit([](const auto& left, const auto& right) {
    using Left = std::decay_t<decltype(left)>;
    using Right = std::decay_t<decltype(right)>;
    if constexpr (!std::is_same_v<Left, Right>) {
      return false;
    } else {
      if constexpr (std::is_same_v<Left, Color> || std::is_same_v<Left, Paint> ||
                    std::is_same_v<Left, Length> || std::is_same_v<Left, CornerRadii> ||
                    std::is_same_v<Left, CornerSmoothing> ||
                    std::is_same_v<Left, BorderWidths>) {
        return same(left, right);
      } else {
        return left == right;
      }
    }
  }, a, b);
}

PropertyType propertyType(const PropertyValue& value) {
  return std::visit([](const auto& entry) -> PropertyType {
    using T = std::decay_t<decltype(entry)>;
    if constexpr (std::is_same_v<T, bool>) return PropertyType::Boolean;
    else if constexpr (std::is_same_v<T, std::int64_t>) return PropertyType::Integer;
    else if constexpr (std::is_same_v<T, float>) return PropertyType::Scalar;
    else if constexpr (std::is_same_v<T, std::string>) return PropertyType::String;
    else if constexpr (std::is_same_v<T, Color>) return PropertyType::Color;
    else if constexpr (std::is_same_v<T, Paint>) return PropertyType::Paint;
    else if constexpr (std::is_same_v<T, Length>) return PropertyType::Length;
    else if constexpr (std::is_same_v<T, CornerRadii>) return PropertyType::CornerRadii;
    else if constexpr (std::is_same_v<T, CornerSmoothing>) return PropertyType::CornerSmoothing;
    else if constexpr (std::is_same_v<T, BorderWidths>) return PropertyType::BorderWidths;
    else static_assert(!sizeof(T), "Unhandled SlugUI property type");
  }, value);
}

} // namespace

struct PropertyStore::Impl {
  struct Slot {
    std::string name;
    PropertyValue value;
    std::uint64_t revision = 1;
  };
  struct Binding {
    PropertyId target = 0;
    std::vector<PropertyId> dependencies;
    std::function<PropertyValue(const PropertyStore&)> function;
  };

  std::vector<Slot> slots;
  std::unordered_map<std::string, PropertyId> names;
  std::vector<Binding> bindings;
  std::vector<std::vector<std::size_t>> dependents;
  std::vector<bool> bindingQueued;
  std::deque<std::size_t> dirtyBindings;
  std::uint64_t generation = 0;


  void queueBinding(std::size_t index) {
    if (bindingQueued[index]) return;
    bindingQueued[index] = true;
    dirtyBindings.push_back(index);
  }
  void queueDependents(PropertyId property) {
    if (property == 0 || property > dependents.size()) return;
    for (const auto binding : dependents[property - 1]) queueBinding(binding);
  }
};

PropertyStore::PropertyStore() : impl_(std::make_unique<Impl>()) {}
PropertyStore::~PropertyStore() = default;
PropertyStore::PropertyStore(PropertyStore&&) noexcept = default;
PropertyStore& PropertyStore::operator=(PropertyStore&&) noexcept = default;

PropertyId PropertyStore::addProperty(std::string name, PropertyValue value) {
  if (name.empty()) throw std::invalid_argument("SlugUI property names cannot be empty");
  if (impl_->names.contains(name)) throw std::invalid_argument("Duplicate SlugUI property: " + name);
  const auto id = static_cast<PropertyId>(impl_->slots.size() + 1);
  impl_->slots.push_back({std::move(name), std::move(value), 1});
  impl_->dependents.emplace_back();
  impl_->names.emplace(impl_->slots.back().name, id);
  ++impl_->generation;
  return id;
}

const PropertyValue& PropertyStore::value(PropertyId property) const {
  if (property == 0 || property > impl_->slots.size()) {
    throw std::out_of_range("Invalid SlugUI property id");
  }
  return impl_->slots[property - 1].value;
}

bool PropertyStore::setValue(PropertyId property, PropertyValue value) {
  if (propertyType(this->value(property)) != propertyType(value)) {
    throw std::invalid_argument("SlugUI property type mismatch");
  }
  auto& slot = impl_->slots[property - 1];
  if (same(slot.value, value)) return false;
  slot.value = std::move(value);
  ++slot.revision;
  ++impl_->generation;
  impl_->queueDependents(property);
  return true;
}

void PropertyStore::addBinding(PropertyId target, std::vector<PropertyId> dependencies,
                               BindingFunction function) {
  (void)value(target);
  for (const auto dependency : dependencies) (void)value(dependency);
  const auto index = impl_->bindings.size();
  impl_->bindings.push_back({target, std::move(dependencies), std::move(function)});
  impl_->bindingQueued.push_back(false);
  for (const auto dependency : impl_->bindings.back().dependencies)
    impl_->dependents[dependency - 1].push_back(index);
  impl_->queueBinding(index);
}

std::optional<PropertyId> PropertyStore::find(std::string_view name) const {
  const auto it = impl_->names.find(std::string(name));
  if (it == impl_->names.end()) return std::nullopt;
  return it->second;
}

PropertyType PropertyStore::type(PropertyId property) const { return propertyType(value(property)); }
std::string_view PropertyStore::name(PropertyId property) const {
  (void)value(property);
  return impl_->slots[property - 1].name;
}
std::uint64_t PropertyStore::revision(PropertyId property) const {
  (void)value(property);
  return impl_->slots[property - 1].revision;
}
std::uint64_t PropertyStore::generation() const { return impl_->generation; }

bool PropertyStore::evaluateBindings() {
  bool changed = false;
  std::size_t evaluations = 0;
  const std::size_t maximumEvaluations = std::max<std::size_t>(
    32, impl_->bindings.size() * std::max<std::size_t>(4, impl_->slots.size() + 1));
  while (!impl_->dirtyBindings.empty()) {
    const auto index = impl_->dirtyBindings.front();
    impl_->dirtyBindings.pop_front();
    impl_->bindingQueued[index] = false;
    if (++evaluations > maximumEvaluations)
      throw std::runtime_error("SlugUI property bindings did not converge");
    auto& binding = impl_->bindings[index];
    auto next = binding.function(*this);
    if (propertyType(next) != type(binding.target))
      throw std::invalid_argument("SlugUI binding returned the wrong property type");
    changed |= setValue(binding.target, std::move(next));
  }
  return changed;
}

namespace {

Element container(ElementId id, LayoutKind kind, std::string name) {
  Element result;
  result.id = id;
  result.name = std::move(name);
  result.layout.kind = kind;
  return result;
}

} // namespace

Element absolute(ElementId id, std::string name) {
  return container(id, LayoutKind::Absolute, std::move(name));
}
Element row(ElementId id, std::string name) {
  return container(id, LayoutKind::Row, std::move(name));
}
Element column(ElementId id, std::string name) {
  return container(id, LayoutKind::Column, std::move(name));
}
Element stack(ElementId id, std::string name) {
  return container(id, LayoutKind::Stack, std::move(name));
}

Element roundedRectangle(ElementId id, Paint paint, CornerRadii radii,
                         CornerSmoothing smoothing, std::string name) {
  auto result = stack(id, std::move(name));
  result.visual = RoundedRectangleVisual{StatefulPaint{paint}, radii, smoothing, {}};
  return result;
}

Element shape(ElementId id, ShapeId shapeId, Paint paint, std::string name) {
  auto result = stack(id, std::move(name));
  result.visual = ShapeVisual{shapeId, StatefulPaint{paint}};
  return result;
}

Element text(ElementId id, std::string value, TextStyle style, std::string name) {
  auto result = absolute(id, std::move(name));
  const auto paint = style.paint;
  result.visual = TextVisual{std::move(value), std::move(style), StatefulPaint{paint}};
  return result;
}

struct Component::Impl {
  std::unordered_map<CallbackId, std::vector<Callback>> callbacks;
};

Component::Component(Element root)
    : root_(std::move(root)), properties_(), impl_(std::make_unique<Impl>()) {}
Component::Component(Element root, PropertyStore properties)
    : root_(std::move(root)), properties_(std::move(properties)),
      impl_(std::make_unique<Impl>()) {}
Component::~Component() = default;
Component::Component(Component&&) noexcept = default;
Component& Component::operator=(Component&&) noexcept = default;

void Component::on(CallbackId callback, Callback handler) {
  if (callback == 0 || !handler) return;
  impl_->callbacks[callback].push_back(std::move(handler));
}

void Component::dispatch(CallbackId callback, const UiEvent& event) {
  const auto it = impl_->callbacks.find(callback);
  if (it == impl_->callbacks.end()) return;
  for (const auto& handler : it->second) handler(event);
}

FrameInput FrameInput::from(const InputState& input) {
  return {input.cursorPosition(), input.cursorDelta(), input.mouse(MouseButton::Left)};
}

namespace {

Rect intersect(Rect a, Rect b) {
  const float left = std::max(a.x, b.x);
  const float top = std::max(a.y, b.y);
  const float right = std::min(a.x + a.width, b.x + b.width);
  const float bottom = std::min(a.y + a.height, b.y + b.height);
  return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

struct Measured {
  float width = 0.0f;
  float height = 0.0f;
};

} // namespace

struct Runtime::Impl {
  struct Resolved {
    LayoutBox box;
    const Element* element = nullptr;
    bool enabled = true;
  };
  struct FlowChild {
    const Element* element = nullptr;
    Measured measured;
    float grow = 0.0f;
  };
  struct MeasureKey {
    const Element* element = nullptr;
    std::uint32_t parentWidth = 0;
    std::uint32_t parentHeight = 0;
    bool operator==(const MeasureKey&) const = default;
  };
  struct MeasureKeyHash {
    std::size_t operator()(const MeasureKey& key) const noexcept {
      std::size_t hash = std::hash<const Element*>{}(key.element);
      hash ^= static_cast<std::size_t>(key.parentWidth) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
      hash ^= static_cast<std::size_t>(key.parentHeight) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
      return hash;
    }
  };

  std::vector<Resolved> resolved;
  std::vector<LayoutBox> publicBoxes;
  std::vector<std::vector<FlowChild>> flowScratch;
  std::unordered_map<MeasureKey, Measured, MeasureKeyHash> measureCache;
  std::unordered_map<ElementId, std::size_t> resolvedIndex;
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> hitCells;
  std::vector<std::size_t> hitGlobals;
  ElementId hovered = 0;
  ElementId active = 0;
  ElementId focused = 0;
  float scale = 1.0f;
  RuntimeStats stats = {};
  const Component* layoutComponent = nullptr;
  std::uint64_t layoutStructureGeneration = 0;
  Rect layoutViewport = {};
  float layoutDeviceScale = 0.0f;
  std::vector<PropertyId> layoutDependencies;
  std::vector<std::uint64_t> layoutObserved;

  template <typename T>
  static void addLayoutDependency(std::vector<PropertyId>& result,
                                  const ValueSource<T>& source) {
    if (const auto property = source.propertyId()) result.push_back(*property);
  }

  static void collectLayoutDependencies(const Element& element,
                                        std::vector<PropertyId>& result) {
    addLayoutDependency(result, element.visible);
    addLayoutDependency(result, element.interaction.enabled);
    const auto& layout = element.layout;
    addLayoutDependency(result, layout.x);
    addLayoutDependency(result, layout.y);
    addLayoutDependency(result, layout.width);
    addLayoutDependency(result, layout.height);
    addLayoutDependency(result, layout.minWidth);
    addLayoutDependency(result, layout.minHeight);
    addLayoutDependency(result, layout.maxWidth);
    addLayoutDependency(result, layout.maxHeight);
    addLayoutDependency(result, layout.preferredWidth);
    addLayoutDependency(result, layout.preferredHeight);
    addLayoutDependency(result, layout.grow);
    addLayoutDependency(result, layout.padding.left);
    addLayoutDependency(result, layout.padding.top);
    addLayoutDependency(result, layout.padding.right);
    addLayoutDependency(result, layout.padding.bottom);
    addLayoutDependency(result, layout.spacing);
    if (const auto* textVisual = std::get_if<TextVisual>(&element.visual))
      addLayoutDependency(result, textVisual->text);
    for (const auto& child : element.children)
      collectLayoutDependencies(child, result);
  }

  static bool sameRect(Rect a, Rect b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }
  [[nodiscard]] bool layoutCurrent(const Component& component, Rect viewport,
                                   float deviceScale) const {
    const float normalizedScale = std::max(0.01f, deviceScale);
    if (layoutComponent != &component ||
        layoutStructureGeneration != component.layoutGeneration() ||
        !sameRect(layoutViewport, viewport) || layoutDeviceScale != normalizedScale ||
        layoutDependencies.size() != layoutObserved.size()) return false;
    const auto& properties = component.properties();
    for (std::size_t i = 0; i < layoutDependencies.size(); ++i) {
      if (properties.revision(layoutDependencies[i]) != layoutObserved[i]) return false;
    }
    return true;
  }

  void rememberLayoutState(const Component& component, Rect viewport, float deviceScale) {
    layoutComponent = &component;
    layoutStructureGeneration = component.layoutGeneration();
    layoutViewport = viewport;
    layoutDeviceScale = std::max(0.01f, deviceScale);
    layoutDependencies.clear();
    collectLayoutDependencies(component.root(), layoutDependencies);
    std::sort(layoutDependencies.begin(), layoutDependencies.end());
    layoutDependencies.erase(std::unique(layoutDependencies.begin(), layoutDependencies.end()),
                             layoutDependencies.end());
    layoutObserved.clear();
    layoutObserved.reserve(layoutDependencies.size());
    const auto& properties = component.properties();
    for (const auto property : layoutDependencies)
      layoutObserved.push_back(properties.revision(property));
  }

  [[nodiscard]] std::optional<float> length(const ValueSource<Length>& source,
                                             const PropertyStore& properties,
                                             float parent) const {
    const auto value = source.resolve(properties);
    switch (value.unit) {
      case LengthUnit::LogicalPixels: return value.value * scale;
      case LengthUnit::PhysicalPixels: return value.value;
      case LengthUnit::Percent: return parent * value.value * 0.01f;
      case LengthUnit::Auto: return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] float inset(const ValueSource<Length>& source,
                            const PropertyStore& properties, float parent) const {
    return std::max(0.0f, length(source, properties, parent).value_or(0.0f));
  }

  [[nodiscard]] Measured textSize(const TextVisual& textVisual,
                                  const PropertyStore& properties) const {
    const auto& value = textVisual.text.resolve(properties);
    const float glyphAdvance = textVisual.style.size * scale * 0.6f +
                               textVisual.style.letterSpacing * scale;
    const float lineHeight = textVisual.style.size * scale * textVisual.style.lineHeight;
    std::size_t maxCharacters = 0;
    std::size_t lines = 0;
    std::size_t start = 0;
    do {
      const auto end = value.find('\n', start);
      const auto line = std::string_view(value).substr(
        start, end == std::string::npos ? value.size() - start : end - start);
      maxCharacters = std::max(maxCharacters, decodeUtf8(line).size());
      ++lines;
      if (end == std::string::npos) break;
      start = end + 1;
    } while (start <= value.size());
    return {
      static_cast<float>(maxCharacters) * glyphAdvance +
        textVisual.style.indent * scale,
      static_cast<float>(lines) * lineHeight
    };
  }

  [[nodiscard]] Measured constrain(const Element& element, Measured measured,
                                   const PropertyStore& properties,
                                   float parentWidth, float parentHeight) const {
    const auto width = length(element.layout.width, properties, parentWidth);
    const auto height = length(element.layout.height, properties, parentHeight);
    const auto preferredWidth = length(element.layout.preferredWidth, properties, parentWidth);
    const auto preferredHeight = length(element.layout.preferredHeight, properties, parentHeight);
    if (width) measured.width = *width;
    else if (preferredWidth) measured.width = *preferredWidth;
    if (height) measured.height = *height;
    else if (preferredHeight) measured.height = *preferredHeight;

    if (const auto minimum = length(element.layout.minWidth, properties, parentWidth)) {
      measured.width = std::max(measured.width, *minimum);
    }
    if (const auto minimum = length(element.layout.minHeight, properties, parentHeight)) {
      measured.height = std::max(measured.height, *minimum);
    }
    if (const auto maximum = length(element.layout.maxWidth, properties, parentWidth)) {
      measured.width = std::min(measured.width, *maximum);
    }
    if (const auto maximum = length(element.layout.maxHeight, properties, parentHeight)) {
      measured.height = std::min(measured.height, *maximum);
    }
    measured.width = std::max(0.0f, measured.width);
    measured.height = std::max(0.0f, measured.height);
    return measured;
  }

  [[nodiscard]] Measured measure(const Element& element, const PropertyStore& properties,
                                 float parentWidth, float parentHeight) {
    const MeasureKey key{&element, std::bit_cast<std::uint32_t>(parentWidth),
                         std::bit_cast<std::uint32_t>(parentHeight)};
    if (const auto cached = measureCache.find(key); cached != measureCache.end()) {
      return cached->second;
    }
    if (!element.visible.resolve(properties)) {
      measureCache.emplace(key, Measured{});
      return {};
    }
    const float left = inset(element.layout.padding.left, properties, parentWidth);
    const float right = inset(element.layout.padding.right, properties, parentWidth);
    const float top = inset(element.layout.padding.top, properties, parentHeight);
    const float bottom = inset(element.layout.padding.bottom, properties, parentHeight);
    const float innerWidth = std::max(0.0f, parentWidth - left - right);
    const float innerHeight = std::max(0.0f, parentHeight - top - bottom);
    const float gap = inset(element.layout.spacing, properties,
                            element.layout.kind == LayoutKind::Row ? parentWidth : parentHeight);

    Measured result = {};
    if (const auto* textVisual = std::get_if<TextVisual>(&element.visual)) {
      result = textSize(*textVisual, properties);
    }

    std::size_t visibleChildren = 0;
    float childrenWidth = 0.0f;
    float childrenHeight = 0.0f;
    for (const auto& child : element.children) {
      if (!child.visible.resolve(properties)) continue;
      const auto childSize = measure(child, properties, innerWidth, innerHeight);
      ++visibleChildren;
      if (element.layout.kind == LayoutKind::Row) {
        childrenWidth += childSize.width;
        childrenHeight = std::max(childrenHeight, childSize.height);
      } else if (element.layout.kind == LayoutKind::Column) {
        childrenWidth = std::max(childrenWidth, childSize.width);
        childrenHeight += childSize.height;
      } else {
        const float x = length(child.layout.x, properties, innerWidth).value_or(0.0f);
        const float y = length(child.layout.y, properties, innerHeight).value_or(0.0f);
        childrenWidth = std::max(childrenWidth, x + childSize.width);
        childrenHeight = std::max(childrenHeight, y + childSize.height);
      }
    }
    if (visibleChildren > 1 &&
        (element.layout.kind == LayoutKind::Row ||
         element.layout.kind == LayoutKind::Column)) {
      const float spacing = gap * static_cast<float>(visibleChildren - 1);
      if (element.layout.kind == LayoutKind::Row) childrenWidth += spacing;
      else childrenHeight += spacing;
    }
    result.width = std::max(result.width, childrenWidth + left + right);
    result.height = std::max(result.height, childrenHeight + top + bottom);
    const auto constrained = constrain(element, result, properties, parentWidth, parentHeight);
    measureCache.emplace(key, constrained);
    return constrained;
  }

  void append(const Element& element, Rect bounds, Rect inheritedClip,
              const PropertyStore& properties, bool ancestorEnabled,
              bool ancestorOverlay, std::size_t depth) {
    ++stats.elements;
    if (!element.visible.resolve(properties)) return;
    ++stats.visibleElements;

    const bool enabled = ancestorEnabled && element.interaction.enabled.resolve(properties);
    const bool overlay = ancestorOverlay || element.overlay;
    const std::size_t resolvedPosition = resolved.size();
    resolved.push_back({
      {element.id, bounds, inheritedClip, overlay, element.interaction.interactive},
      &element,
      enabled
    });
    if (element.id != 0) resolvedIndex.try_emplace(element.id, resolvedPosition);

    const float left = inset(element.layout.padding.left, properties, bounds.width);
    const float right = inset(element.layout.padding.right, properties, bounds.width);
    const float top = inset(element.layout.padding.top, properties, bounds.height);
    const float bottom = inset(element.layout.padding.bottom, properties, bounds.height);
    Rect inner = {
      bounds.x + left,
      bounds.y + top,
      std::max(0.0f, bounds.width - left - right),
      std::max(0.0f, bounds.height - top - bottom)
    };
    const Rect childClip = element.clipsChildren ? intersect(inheritedClip, bounds) : inheritedClip;

    if (element.layout.kind == LayoutKind::Row ||
        element.layout.kind == LayoutKind::Column) {
      const bool horizontal = element.layout.kind == LayoutKind::Row;
      const float mainSize = horizontal ? inner.width : inner.height;
      const float crossSize = horizontal ? inner.height : inner.width;
      const float gap = inset(element.layout.spacing, properties, mainSize);
      auto& children = flowScratch[depth];
      children.clear();
      if (children.capacity() < element.children.size()) {
        children.reserve(element.children.size());
      }
      float used = 0.0f;
      float totalGrow = 0.0f;
      for (const auto& child : element.children) {
        if (!child.visible.resolve(properties)) continue;
        auto childSize = measure(child, properties, inner.width, inner.height);
        const float grow = std::max(0.0f, child.layout.grow.resolve(properties));
        used += horizontal ? childSize.width : childSize.height;
        totalGrow += grow;
        children.push_back({&child, childSize, grow});
      }
      if (children.size() > 1) used += gap * static_cast<float>(children.size() - 1);
      const float available = std::max(0.0f, mainSize - used);
      if (totalGrow > 0.0f) {
        for (auto& child : children) {
          const float addition = available * child.grow / totalGrow;
          if (horizontal) child.measured.width += addition;
          else child.measured.height += addition;
        }
      }

      float cursor = 0.0f;
      float actualGap = gap;
      if (totalGrow == 0.0f) {
        switch (element.layout.mainAlignment) {
          case Justify::Center: cursor = available * 0.5f; break;
          case Justify::End: cursor = available; break;
          case Justify::SpaceBetween:
            if (children.size() > 1) {
              actualGap += available / static_cast<float>(children.size() - 1);
            }
            break;
          case Justify::Start: break;
        }
      }

      for (auto& child : children) {
        const auto alignment = child.element->layout.alignSelf.value_or(
          element.layout.crossAlignment);
        float childCross = horizontal ? child.measured.height : child.measured.width;
        const auto explicitCross = horizontal
          ? length(child.element->layout.height, properties, inner.height)
          : length(child.element->layout.width, properties, inner.width);
        if (alignment == Alignment::Stretch && !explicitCross) childCross = crossSize;
        float crossOffset = 0.0f;
        if (alignment == Alignment::Center) crossOffset = (crossSize - childCross) * 0.5f;
        else if (alignment == Alignment::End) crossOffset = crossSize - childCross;
        crossOffset = std::max(0.0f, crossOffset);

        Rect childBounds;
        if (horizontal) {
          childBounds = {inner.x + cursor, inner.y + crossOffset,
                         child.measured.width, childCross};
          cursor += child.measured.width + actualGap;
        } else {
          childBounds = {inner.x + crossOffset, inner.y + cursor,
                         childCross, child.measured.height};
          cursor += child.measured.height + actualGap;
        }
        append(*child.element, childBounds, childClip, properties, enabled, overlay, depth + 1);
      }
      return;
    }

    for (const auto& child : element.children) {
      if (!child.visible.resolve(properties)) {
        ++stats.elements;
        continue;
      }
      auto childSize = measure(child, properties, inner.width, inner.height);
      const float x = length(child.layout.x, properties, inner.width).value_or(0.0f);
      const float y = length(child.layout.y, properties, inner.height).value_or(0.0f);
      if (element.layout.kind == LayoutKind::Stack) {
        if (!length(child.layout.width, properties, inner.width)) childSize.width = inner.width;
        if (!length(child.layout.height, properties, inner.height)) childSize.height = inner.height;
      }
      append(child, {inner.x + x, inner.y + y, childSize.width, childSize.height},
             childClip, properties, enabled, overlay, depth + 1);
    }
  }

  [[nodiscard]] std::size_t treeDepth(const Element& element) const {
    std::size_t result = 1;
    for (const auto& child : element.children) {
      result = std::max(result, treeDepth(child) + 1);
    }
    return result;
  }

  static constexpr float hitCellSize = 64.0f;
  static constexpr std::size_t maximumIndexedCellsPerElement = 256;

  [[nodiscard]] static std::uint64_t hitCellKey(std::int32_t x, std::int32_t y) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
           static_cast<std::uint32_t>(y);
  }

  [[nodiscard]] static std::int32_t hitCellCoordinate(float value) noexcept {
    return static_cast<std::int32_t>(std::floor(value / hitCellSize));
  }

  void buildHitIndex() {
    hitCells.clear();
    hitGlobals.clear();
    hitCells.reserve(resolved.size());
    for (std::size_t index = 0; index < resolved.size(); ++index) {
      const auto& entry = resolved[index];
      if (!entry.box.interactive || !entry.enabled) continue;
      const Rect region = intersect(entry.box.bounds, entry.box.clip);
      if (region.width <= 0.0f || region.height <= 0.0f) continue;
      const auto minX = hitCellCoordinate(region.x);
      const auto minY = hitCellCoordinate(region.y);
      const auto maxX = hitCellCoordinate(std::nextafter(
        region.x + region.width, -std::numeric_limits<float>::infinity()));
      const auto maxY = hitCellCoordinate(std::nextafter(
        region.y + region.height, -std::numeric_limits<float>::infinity()));
      const std::uint64_t columns = static_cast<std::uint64_t>(maxX - minX + 1);
      const std::uint64_t rows = static_cast<std::uint64_t>(maxY - minY + 1);
      if (columns * rows > maximumIndexedCellsPerElement) {
        hitGlobals.push_back(index);
        continue;
      }
      for (auto y = minY; y <= maxY; ++y) {
        for (auto x = minX; x <= maxX; ++x) {
          hitCells[hitCellKey(x, y)].push_back(index);
        }
      }
    }
  }

  void performLayout(Component& component, Rect viewport, float deviceScale) {
    resolved.clear();
    publicBoxes.clear();
    measureCache.clear();
    resolvedIndex.clear();
    scale = std::max(0.01f, deviceScale);
    const auto& readOnly = static_cast<const Component&>(component);
    const auto& root = readOnly.root();
    const auto& properties = readOnly.properties();
    flowScratch.resize(treeDepth(root));
    auto rootSize = measure(root, properties, viewport.width, viewport.height);
    if (!length(root.layout.width, properties, viewport.width)) rootSize.width = viewport.width;
    if (!length(root.layout.height, properties, viewport.height)) rootSize.height = viewport.height;
    append(root, {viewport.x, viewport.y, rootSize.width, rootSize.height},
           viewport, properties, true, false, 0);
    publicBoxes.reserve(resolved.size());
    for (const auto& entry : resolved) publicBoxes.push_back(entry.box);
    buildHitIndex();
    rememberLayoutState(static_cast<const Component&>(component), viewport, deviceScale);
    ++stats.layoutPasses;
  }

  [[nodiscard]] const Resolved* resolvedById(ElementId id) const {
    if (id == 0) return nullptr;
    const auto found = resolvedIndex.find(id);
    if (found == resolvedIndex.end() || found->second >= resolved.size()) return nullptr;
    return &resolved[found->second];
  }

  [[nodiscard]] ElementId hit(Vec2 cursor) const {
    const auto found = hitCells.find(hitCellKey(hitCellCoordinate(cursor.x),
                                               hitCellCoordinate(cursor.y)));
    const std::vector<std::size_t>* local = found == hitCells.end() ? nullptr : &found->second;
    for (const bool overlayPass : {true, false}) {
      std::size_t localCount = local ? local->size() : 0;
      std::size_t globalCount = hitGlobals.size();
      while (localCount != 0 || globalCount != 0) {
        const auto localIndex = localCount != 0 ? (*local)[localCount - 1] : 0;
        const auto globalIndex = globalCount != 0 ? hitGlobals[globalCount - 1] : 0;
        const bool takeLocal = globalCount == 0 ||
          (localCount != 0 && localIndex > globalIndex);
        const auto index = takeLocal ? (*local)[--localCount] : hitGlobals[--globalCount];
        const auto& entry = resolved[index];
        if (entry.box.overlay != overlayPass) continue;
        if (entry.box.bounds.contains(cursor) && entry.box.clip.contains(cursor)) {
          return entry.box.id;
        }
      }
    }
    return 0;
  }

  void dispatch(Component& component, const Resolved* target, EventType type,
                const FrameInput& input) {
    if (!target || target->element->interaction.callback == 0) return;
    component.dispatch(target->element->interaction.callback,
                       {type, target->box.id, input.cursor, input.cursorDelta});
    ++stats.callbacks;
  }

  void processInput(Component& component, const FrameInput& input) {
    const ElementId nextHovered = hit(input.cursor);
    if (nextHovered != hovered) {
      dispatch(component, resolvedById(hovered), EventType::HoverExited, input);
      hovered = nextHovered;
      dispatch(component, resolvedById(hovered), EventType::HoverEntered, input);
    }

    if (input.primary.pressed) {
      active = hovered;
      if (active != 0) focused = active;
      const auto* target = resolvedById(active);
      dispatch(component, target, EventType::Pressed, input);
      // Activation on the press edge keeps direct manipulation and buttons one frame behind
      // neither the OS cursor nor the current Vulkan submission.
      dispatch(component, target, EventType::Activated, input);
    }
    if (input.primary.released) {
      dispatch(component, resolvedById(active), EventType::Released, input);
      active = 0;
    } else if (!input.primary.down && !input.primary.pressed) {
      active = 0;
    }
  }

  [[nodiscard]] Paint paint(const StatefulPaint& state, const Resolved& entry,
                            const PropertyStore& properties,
                            const FrameInput& input) const {
    if (!entry.enabled && state.disabled) return state.disabled->resolve(properties);
    if (entry.box.id == active && input.primary.down && state.pressed) {
      return state.pressed->resolve(properties);
    }
    if (entry.box.id == hovered && state.hovered) return state.hovered->resolve(properties);
    return state.normal.resolve(properties);
  }

  void draw(const Resolved& entry, const PropertyStore& properties,
            const FrameInput& input, DrawList& drawList, Rect callerClip) const {
    drawList.setClip(intersect(entry.box.clip, callerClip));
    if (const auto* rounded = std::get_if<RoundedRectangleVisual>(&entry.element->visual)) {
      auto radii = rounded->radii.resolve(properties);
      radii.topLeft *= scale;
      radii.topRight *= scale;
      radii.bottomRight *= scale;
      radii.bottomLeft *= scale;
      BorderStyle border;
      border.paint = paint(rounded->stroke.paint, entry, properties, input);
      border.width = std::max(0.0f, rounded->stroke.width.resolve(properties) * scale);
      border.align = rounded->stroke.align;
      if (rounded->stroke.individualWidths) {
        auto widths = rounded->stroke.individualWidths->resolve(properties);
        widths.top = std::max(0.0f, widths.top * scale);
        widths.right = std::max(0.0f, widths.right * scale);
        widths.bottom = std::max(0.0f, widths.bottom * scale);
        widths.left = std::max(0.0f, widths.left * scale);
        border.individualWidths = widths;
      }
      drawList.roundedRect(entry.box.bounds, radii,
                           paint(rounded->paint, entry, properties, input),
                           rounded->smoothing.resolve(properties), std::move(border));
    } else if (const auto* shapeVisual = std::get_if<ShapeVisual>(&entry.element->visual)) {
      const auto placed = [&entry](const Rect& placement) {
        return Rect{
          entry.box.bounds.x + placement.x * entry.box.bounds.width,
          entry.box.bounds.y + placement.y * entry.box.bounds.height,
          placement.width * entry.box.bounds.width,
          placement.height * entry.box.bounds.height,
        };
      };
      if (shapeVisual->shape != 0) {
        drawList.shape(shapeVisual->shape, placed(shapeVisual->fillPlacement),
                       paint(shapeVisual->paint, entry, properties, input));
      }
      if (shapeVisual->strokeShape != 0) {
        drawList.shape(shapeVisual->strokeShape, placed(shapeVisual->strokePlacement),
                       paint(shapeVisual->strokePaint, entry, properties, input));
      }
    } else if (const auto* textVisual = std::get_if<TextVisual>(&entry.element->visual)) {
      auto style = textVisual->style;
      style.size *= scale;
      style.letterSpacing *= scale;
      style.indent *= scale;
      style.paint = paint(textVisual->paint, entry, properties, input);
      const auto& value = textVisual->text.resolve(properties);
      if (!textVisual->runs.empty()) {
        drawList.textRunsStatic(textVisual->runs, entry.box.bounds, std::move(style), scale);
      } else if (textVisual->text.bound()) {
        drawList.text(value, entry.box.bounds, std::move(style));
      } else {
        drawList.textStatic(value, entry.box.bounds, std::move(style));
      }
    }
  }

  void emit(const PropertyStore& properties, const FrameInput& input, DrawList& drawList) const {
    const Rect previousClip = drawList.clip();
    const bool previousOverlay = drawList.overlayMode();
    if (previousOverlay) {
      for (const auto& entry : resolved) draw(entry, properties, input, drawList, previousClip);
    } else {
      for (const auto& entry : resolved) {
        if (!entry.box.overlay) draw(entry, properties, input, drawList, previousClip);
      }
      drawList.beginOverlay();
      for (const auto& entry : resolved) {
        if (entry.box.overlay) draw(entry, properties, input, drawList, previousClip);
      }
      drawList.endOverlay();
    }
    drawList.setClip(previousClip);
    if (previousOverlay) drawList.beginOverlay();
  }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::layout(Component& component, Rect viewport, float deviceScale) {
  impl_->stats = {};
  component.properties().evaluateBindings();
  if (!impl_->layoutCurrent(component, viewport, deviceScale))
    impl_->performLayout(component, viewport, deviceScale);
}

RuntimeStats Runtime::render(Component& component, const FrameInput& input,
                             DrawList& drawList, Rect viewport, float deviceScale) {
  impl_->stats = {};
  component.properties().evaluateBindings();
  if (!impl_->layoutCurrent(component, viewport, deviceScale))
    impl_->performLayout(component, viewport, deviceScale);
  impl_->processInput(component, input);
  component.properties().evaluateBindings();
  if (!impl_->layoutCurrent(component, viewport, deviceScale))
    impl_->performLayout(component, viewport, deviceScale);
  impl_->emit(component.properties(), input, drawList);
  return impl_->stats;
}

std::span<const LayoutBox> Runtime::boxes() const { return impl_->publicBoxes; }

const LayoutBox* Runtime::find(ElementId id) const {
  if (id == 0) return nullptr;
  const auto found = impl_->resolvedIndex.find(id);
  if (found == impl_->resolvedIndex.end() || found->second >= impl_->publicBoxes.size()) return nullptr;
  return &impl_->publicBoxes[found->second];
}

ElementId Runtime::hovered() const { return impl_->hovered; }
ElementId Runtime::active() const { return impl_->active; }
ElementId Runtime::focused() const { return impl_->focused; }

} // namespace slugvk::slugui
