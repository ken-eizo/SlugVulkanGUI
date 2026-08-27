#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace slugvk {

class TextEditState {
public:
  std::string value;
  std::size_t caret = 0;
  std::size_t anchor = 0;

  void reset(std::string_view text, bool select_all = false) {
    value.assign(text);
    caret = value.size();
    anchor = select_all ? 0U : caret;
  }

  [[nodiscard]] bool hasSelection() const noexcept { return caret != anchor; }
  [[nodiscard]] std::size_t selectionBegin() const noexcept { return std::min(caret, anchor); }
  [[nodiscard]] std::size_t selectionEnd() const noexcept { return std::max(caret, anchor); }
  void selectAll() noexcept { anchor = 0; caret = value.size(); }
  void collapseSelection() noexcept { anchor = caret; }

  bool insert(std::string_view text) {
    eraseSelection();
    value.insert(caret, text);
    caret += text.size();
    anchor = caret;
    return !text.empty();
  }

  bool backspace() {
    if (eraseSelection()) return true;
    if (caret == 0) return false;
    const auto begin = previousBoundary(caret);
    value.erase(begin, caret - begin);
    caret = anchor = begin;
    return true;
  }

  bool deleteForward() {
    if (eraseSelection()) return true;
    if (caret >= value.size()) return false;
    const auto end = nextBoundary(caret);
    value.erase(caret, end - caret);
    anchor = caret;
    return true;
  }

  void moveLeft(bool extend = false) noexcept {
    if (!extend && hasSelection()) caret = selectionBegin();
    else caret = previousBoundary(caret);
    if (!extend) anchor = caret;
  }

  void moveRight(bool extend = false) noexcept {
    if (!extend && hasSelection()) caret = selectionEnd();
    else caret = nextBoundary(caret);
    if (!extend) anchor = caret;
  }

  void moveHome(bool extend = false) noexcept { caret = 0; if (!extend) anchor = caret; }
  void moveEnd(bool extend = false) noexcept { caret = value.size(); if (!extend) anchor = caret; }

private:
  [[nodiscard]] std::size_t previousBoundary(std::size_t position) const noexcept {
    position = std::min(position, value.size());
    if (position == 0) return 0;
    --position;
    while (position > 0 && (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
      --position;
    return position;
  }

  [[nodiscard]] std::size_t nextBoundary(std::size_t position) const noexcept {
    position = std::min(position, value.size());
    if (position >= value.size()) return value.size();
    ++position;
    while (position < value.size() &&
           (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
      ++position;
    return position;
  }

  bool eraseSelection() {
    if (!hasSelection()) return false;
    const auto begin = selectionBegin();
    value.erase(begin, selectionEnd() - begin);
    caret = anchor = begin;
    return true;
  }
};

} // namespace slugvk
