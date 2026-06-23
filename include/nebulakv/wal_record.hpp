#pragma once

#include <cstdint>
#include <string>

namespace nebulakv {

enum class OperationType : std::uint8_t {
  Put = 1,
  Delete = 2,
};

struct WalRecord {
  OperationType operation{OperationType::Put};
  std::string key;
  std::string value;
};

} // namespace nebulakv
