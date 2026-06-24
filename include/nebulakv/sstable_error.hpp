#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace nebulakv {

class SSTableCorruptionError final : public std::runtime_error {
 public:
  SSTableCorruptionError(std::uint64_t byte_offset, std::string message)
      : std::runtime_error{std::move(message)}, byte_offset_{byte_offset} {}

  [[nodiscard]] std::uint64_t byte_offset() const noexcept { return byte_offset_; }

 private:
  std::uint64_t byte_offset_{0};
};

}  // namespace nebulakv
