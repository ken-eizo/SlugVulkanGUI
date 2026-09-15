#pragma once

#include "slugvk/draw_list.hpp"
#include "slugvk/input.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace slugvk::slugui {

using ElementId = WidgetId;
using CallbackId = std::uint64_t;
using PropertyId = std::uint32_t;

enum class LengthUnit : std::uint8_t { Auto, LogicalPixels, PhysicalPixels, Percent };

struct Length {
  float value = 0.0f;
  LengthUnit unit = LengthUnit::Auto;
  static constexpr Length autoSize() { return {}; }
  static constexpr Length logical(float value) { return {value, LengthUnit::LogicalPixels}; }
  static constexpr Length physical(float value) { return {value, LengthUnit::PhysicalPixels}; }
  static constexpr Length percent(float value) { return {value, LengthUnit::Percent}; }
};

enum class PropertyType : std::uint8_t {
  Boolean, Integer, Scalar, String, Color, Paint, Length, CornerRadii, CornerSmoothing,
  BorderWidths
};

using PropertyValue = std::variant<bool, std::int64_t, float, std::string, Color, Paint,
                                   Length, CornerRadii, CornerSmoothing, BorderWidths>;

template <typename T>
struct Property {
  PropertyId id = 0;
  [[nodiscard]] explicit operator bool() const { return id != 0; }
};

class PropertyStore {
public:
  PropertyStore();
  ~PropertyStore();
  PropertyStore(PropertyStore&&) noexcept;
  PropertyStore& operator=(PropertyStore&&) noexcept;
  PropertyStore(const PropertyStore&) = delete;
  PropertyStore& operator=(const PropertyStore&) = delete;

  template <typename T>
  Property<T> define(std::string name, T initialValue) {
    return Property<T>{addProperty(std::move(name), PropertyValue{
      std::in_place_type<T>, std::move(initialValue)})};
  }

  template <typename T>
  [[nodiscard]] const T& get(Property<T> property) const {
    return std::get<T>(value(property.id));
  }

  template <typename T>
  bool set(Property<T> property, T value) {
    return setValue(property.id, PropertyValue{std::in_place_type<T>, std::move(value)});
  }

  template <typename T, typename Function>
  void bind(Property<T> target, std::vector<PropertyId> dependencies, Function&& function) {
    std::function<T(const PropertyStore&)> typed(std::forward<Function>(function));
    addBinding(target.id, std::move(dependencies),
      [typed = std::move(typed)](const PropertyStore& properties) -> PropertyValue {
        return PropertyValue{std::in_place_type<T>, typed(properties)};
      });
  }

  [[nodiscard]] std::optional<PropertyId> find(std::string_view name) const;
  [[nodiscard]] PropertyType type(PropertyId property) const;
  [[nodiscard]] std::string_view name(PropertyId property) const;
  [[nodiscard]] std::uint64_t revision(PropertyId property) const;
  [[nodiscard]] std::uint64_t generation() const;
  bool evaluateBindings();

private:
  using BindingFunction = std::function<PropertyValue(const PropertyStore&)>;
  PropertyId addProperty(std::string name, PropertyValue value);
  void addBinding(PropertyId target, std::vector<PropertyId> dependencies,
                  BindingFunction function);
  [[nodiscard]] const PropertyValue& value(PropertyId property) const;
  bool setValue(PropertyId property, PropertyValue value);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

template <typename T>
class ValueSource {
public:
  ValueSource() = default;
  ValueSource(T value) : source_(std::move(value)) {}
  ValueSource(Property<T> property) : source_(property) {}
  [[nodiscard]] const T& resolve(const PropertyStore& properties) const {
    if (const auto* property = std::get_if<Property<T>>(&source_)) return properties.get(*property);
    return std::get<T>(source_);
  }
  [[nodiscard]] bool bound() const { return std::holds_alternative<Property<T>>(source_); }
  [[nodiscard]] std::optional<PropertyId> propertyId() const {
    if (const auto* property = std::get_if<Property<T>>(&source_)) return property->id;
    return std::nullopt;
  }
private:
  std::variant<T, Property<T>> source_ = T{};
};

struct Insets {
  ValueSource<Length> left = Length::logical(0.0f);
  ValueSource<Length> top = Length::logical(0.0f);
  ValueSource<Length> right = Length::logical(0.0f);
  ValueSource<Length> bottom = Length::logical(0.0f);
  static Insets all(Length value) { return {value, value, value, value}; }
  static Insets symmetric(Length horizontal, Length vertical) {
    return {horizontal, vertical, horizontal, vertical};
  }
};

enum class LayoutKind : std::uint8_t { Absolute, Row, Column, Stack };
enum class Alignment : std::uint8_t { Start, Center, End, Stretch };
enum class Justify : std::uint8_t { Start, Center, End, SpaceBetween };

struct LayoutSpec {
  LayoutKind kind = LayoutKind::Absolute;
  ValueSource<Length> x = Length::autoSize();
  ValueSource<Length> y = Length::autoSize();
  ValueSource<Length> width = Length::autoSize();
  ValueSource<Length> height = Length::autoSize();
  ValueSource<Length> minWidth = Length::autoSize();
  ValueSource<Length> minHeight = Length::autoSize();
  ValueSource<Length> maxWidth = Length::autoSize();
  ValueSource<Length> maxHeight = Length::autoSize();
  ValueSource<Length> preferredWidth = Length::autoSize();
  ValueSource<Length> preferredHeight = Length::autoSize();
  ValueSource<float> grow = 0.0f;
  Insets padding = {};
  ValueSource<Length> spacing = Length::logical(0.0f);
  Alignment crossAlignment = Alignment::Stretch;
  std::optional<Alignment> alignSelf = {};
  Justify mainAlignment = Justify::Start;
};

enum class ImportFidelity : std::uint8_t {
  Native, Approximated, BakedVector, BakedRaster, Unsupported
};

struct SourceMetadata {
  std::string provider;
  std::string documentId;
  std::string nodeId;
  std::string nodeName;
  ImportFidelity fidelity = ImportFidelity::Native;
};

struct StatefulPaint {
  ValueSource<Paint> normal = Paint::solid({});
  std::optional<ValueSource<Paint>> hovered = {};
  std::optional<ValueSource<Paint>> pressed = {};
  std::optional<ValueSource<Paint>> disabled = {};
};
struct EmptyVisual {};
struct StrokeVisual {
  StatefulPaint paint = {Paint::solid(Color::fromRgb8(0x000000), 0.0f)};
  ValueSource<float> width = 0.0f;
  std::optional<ValueSource<BorderWidths>> individualWidths = {};
  StrokeAlign align = StrokeAlign::Center;
};
struct RoundedRectangleVisual {
  StatefulPaint paint = {};
  ValueSource<CornerRadii> radii = CornerRadii{};
  ValueSource<CornerSmoothing> smoothing = CornerSmoothing{};
  StrokeVisual stroke = {};
};
struct ShapeVisual {
  ShapeId shape = 0;
  StatefulPaint paint = {};
  Rect fillPlacement = {0.0f, 0.0f, 1.0f, 1.0f};
  ShapeId strokeShape = 0;
  StatefulPaint strokePaint = {Paint::solid(Color::fromRgb8(0x000000), 0.0f)};
  Rect strokePlacement = {0.0f, 0.0f, 1.0f, 1.0f};
};
struct TextVisual {
  ValueSource<std::string> text = std::string{};
  TextStyle style = {};
  StatefulPaint paint = {};
  std::vector<TextRun> runs = {};
};
using Visual = std::variant<EmptyVisual, RoundedRectangleVisual, ShapeVisual, TextVisual>;

struct InteractionSpec {
  bool interactive = false;
  ValueSource<bool> enabled = true;
  CallbackId callback = 0;
};

struct Element {
  ElementId id = 0;
  std::string name;
  SourceMetadata source = {};
  LayoutSpec layout = {};
  ValueSource<bool> visible = true;
  bool clipsChildren = false;
  bool overlay = false;
  InteractionSpec interaction = {};
  Visual visual = EmptyVisual{};
  std::vector<Element> children;
  Element& add(Element child) {
    children.push_back(std::move(child));
    return children.back();
  }
};

Element absolute(ElementId id, std::string name = {});
Element row(ElementId id, std::string name = {});
Element column(ElementId id, std::string name = {});
Element stack(ElementId id, std::string name = {});
Element roundedRectangle(ElementId id, Paint paint, CornerRadii radii = {},
                         CornerSmoothing smoothing = {}, std::string name = {});
Element shape(ElementId id, ShapeId shape, Paint paint, std::string name = {});
Element text(ElementId id, std::string text, TextStyle style, std::string name = {});

enum class EventType : std::uint8_t { HoverEntered, HoverExited, Pressed, Released, Activated };
struct UiEvent {
  EventType type = EventType::Activated;
  ElementId target = 0;
  Vec2 cursor = {};
  Vec2 cursorDelta = {};
};

class Component {
public:
  using Callback = std::function<void(const UiEvent&)>;
  explicit Component(Element root);
  Component(Element root, PropertyStore properties);
  ~Component();
  Component(Component&&) noexcept;
  Component& operator=(Component&&) noexcept;
  Component(const Component&) = delete;
  Component& operator=(const Component&) = delete;
  [[nodiscard]] Element& root() { ++structureGeneration_; return root_; }
  [[nodiscard]] const Element& root() const { return root_; }
  // Call after mutating an Element reference retained outside root().
  void invalidateLayout() noexcept { ++structureGeneration_; }
  [[nodiscard]] std::uint64_t layoutGeneration() const noexcept { return structureGeneration_; }
  [[nodiscard]] PropertyStore& properties() { return properties_; }
  [[nodiscard]] const PropertyStore& properties() const { return properties_; }
  void on(CallbackId callback, Callback handler);
private:
  friend class Runtime;
  void dispatch(CallbackId callback, const UiEvent& event);
  Element root_;
  PropertyStore properties_;
  std::uint64_t structureGeneration_ = 1;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct FrameInput {
  Vec2 cursor = {};
  Vec2 cursorDelta = {};
  ButtonState primary = {};
  static FrameInput from(const InputState& input);
};

struct LayoutBox {
  ElementId id = 0;
  Rect bounds = {};
  Rect clip = {};
  bool overlay = false;
  bool interactive = false;
};
struct RuntimeStats {
  std::uint32_t elements = 0;
  std::uint32_t visibleElements = 0;
  std::uint32_t layoutPasses = 0;
  std::uint32_t callbacks = 0;
  // Number of spatial-index candidates inspected by hit tests this frame.
  std::uint32_t hitCandidates = 0;
};

class Runtime {
public:
  Runtime();
  ~Runtime();
  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  void layout(Component& component, Rect viewport, float deviceScale = 1.0f);
  RuntimeStats render(Component& component, const FrameInput& input, DrawList& drawList,
                      Rect viewport, float deviceScale = 1.0f);
  RuntimeStats render(Component& component, const InputState& input, DrawList& drawList,
                      Rect viewport, float deviceScale = 1.0f) {
    return render(component, FrameInput::from(input), drawList, viewport, deviceScale);
  }
  [[nodiscard]] std::span<const LayoutBox> boxes() const;
  [[nodiscard]] const LayoutBox* find(ElementId id) const;
  [[nodiscard]] ElementId hovered() const;
  [[nodiscard]] ElementId active() const;
  [[nodiscard]] ElementId focused() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace slugvk::slugui
