#pragma once

#include <string_view>

namespace nebulakv {

enum class DurabilityMode {
  Sync,
  Batch,
  None,
};

[[nodiscard]] constexpr std::string_view to_string(const DurabilityMode mode) noexcept {
  switch (mode) {
  case DurabilityMode::Sync:
    return "sync";
  case DurabilityMode::Batch:
    return "batch";
  case DurabilityMode::None:
    return "none";
  }

  return "unknown";
}

} // namespace nebulakv
