#pragma once

#include "nebulakv/raft/types.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv::raft::detail {

inline void append_u8(std::string& output, const std::uint8_t value) {
  output.push_back(static_cast<char>(value));
}

inline void append_u32(std::string& output, const std::uint32_t value) {
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    output.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

inline void append_u64(std::string& output, const std::uint64_t value) {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    output.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

inline void append_bytes(std::string& output, const std::string_view value) {
  if (value.size() > static_cast<std::size_t>(UINT32_MAX)) {
    throw std::length_error{"encoded string is too large"};
  }
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  output.append(value);
}

class Decoder final {
public:
  explicit Decoder(const std::string_view input) : input_{input} {}

  [[nodiscard]] std::uint8_t read_u8() {
    require(1U);
    return static_cast<std::uint8_t>(input_[position_++]);
  }

  [[nodiscard]] std::uint32_t read_u32() {
    require(4U);
    std::uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(input_[position_++])) << shift;
    }
    return value;
  }

  [[nodiscard]] std::uint64_t read_u64() {
    require(8U);
    std::uint64_t value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(input_[position_++])) << shift;
    }
    return value;
  }

  [[nodiscard]] std::string read_bytes() {
    const auto size = static_cast<std::size_t>(read_u32());
    require(size);
    std::string value{input_.substr(position_, size)};
    position_ += size;
    return value;
  }

  [[nodiscard]] bool empty() const noexcept { return position_ == input_.size(); }

private:
  void require(const std::size_t count) const {
    if (count > input_.size() - position_) {
      throw std::runtime_error{"truncated encoded data"};
    }
  }

  std::string_view input_;
  std::size_t position_{0U};
};

inline void encode_command(std::string& output, const Command& command) {
  append_u8(output, static_cast<std::uint8_t>(command.type));
  append_bytes(output, command.key);
  append_bytes(output, command.value);
}

inline Command decode_command(Decoder& decoder) {
  Command command;
  const auto type = decoder.read_u8();
  if (type > static_cast<std::uint8_t>(CommandType::Delete)) {
    throw std::runtime_error{"invalid Raft command type"};
  }
  command.type = static_cast<CommandType>(type);
  command.key = decoder.read_bytes();
  command.value = decoder.read_bytes();
  return command;
}

inline void encode_entry(std::string& output, const LogEntry& entry) {
  append_u64(output, entry.index);
  append_u64(output, entry.term);
  encode_command(output, entry.command);
}

inline LogEntry decode_entry(Decoder& decoder) {
  LogEntry entry;
  entry.index = decoder.read_u64();
  entry.term = decoder.read_u64();
  entry.command = decode_command(decoder);
  return entry;
}

} // namespace nebulakv::raft::detail
