#include "figma_import_bridge.hpp"
#include "figma_group_31.generated.hpp"

#include <utility>

namespace slugvk::example {

std::optional<slugui::Component> buildFigmaImport(VectorAtlas& atlas, bool enabled) {
  if (!enabled) return std::nullopt;
  Group_31Generated generated(atlas);
  return std::optional<slugui::Component>{std::move(generated.component)};
}

} // namespace slugvk::example
