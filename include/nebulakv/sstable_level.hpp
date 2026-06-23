#pragma once

#include <cstdint>
#include <string_view>

namespace nebulakv {

enum class SSTableLevel : std::uint8_t {
  Level0 = 0,
  Level1 = 1,
};

[[nodiscard]] constexpr std::string_view to_string(const SSTableLevel level) noexcept {
  switch (level) {
  case SSTableLevel::Level0:
    return "L0";
  case SSTableLevel::Level1:
    return "L1";
  }
  return "unknown";
}

} // namespace nebulakv
