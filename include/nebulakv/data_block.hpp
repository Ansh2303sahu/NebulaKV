#pragma once

#include "nebulakv/entry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace nebulakv {

struct DataBlock {
  using Record = std::pair<std::string, Entry>;

  std::vector<Record> records;

  [[nodiscard]] bool empty() const noexcept { return records.empty(); }
  [[nodiscard]] const std::string& first_key() const { return records.front().first; }
  [[nodiscard]] const std::string& last_key() const { return records.back().first; }
};

}  // namespace nebulakv
