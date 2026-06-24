#include "nebulakv/validation.hpp"

#include "nebulakv/storage_limits.hpp"

#include <stdexcept>

namespace nebulakv {

void validate_key(const std::string_view key) {
  if (key.empty()) {
    throw std::invalid_argument{"NebulaKV keys must not be empty"};
  }

  if (key.size() > storage_limits::kMaxKeySize) {
    throw std::length_error{"NebulaKV key exceeds the maximum size"};
  }
}

void validate_value(const std::string_view value) {
  if (value.size() > storage_limits::kMaxValueSize) {
    throw std::length_error{"NebulaKV value exceeds the maximum size"};
  }
}

}  // namespace nebulakv
