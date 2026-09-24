#pragma once

#include "slugvk/slugui.hpp"

#include <optional>

namespace slugvk {
class VectorAtlas;
}

namespace slugvk::example {

// Keep the generated Figma translation unit out of kitchen_sink.cpp.
// This lets a .slugui change rebuild only the small bridge instead of the whole demo.
std::optional<slugui::Component> buildFigmaImport(VectorAtlas& atlas, bool enabled);

} // namespace slugvk::example
