#pragma once

#include <cstdint>
#include <string>

namespace nebulakv {

struct Entry {
  std::string value;
  std::uint64_t sequence_number{0};
  bool deleted{false};

  friend bool operator==(const Entry&, const Entry&) = default;
};

}  // namespace nebulakv
