#pragma once

#include "nebulakv/wal_record.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace nebulakv {

enum class WalReadIssueCode {
  IncompleteRecord,
  InvalidMagic,
  UnsupportedVersion,
  InvalidOperation,
  InvalidRecordLength,
  InvalidDeleteRecord,
  ChecksumMismatch,
  IoError,
};

[[nodiscard]] std::string_view to_string(WalReadIssueCode code) noexcept;

struct WalReadIssue {
  WalReadIssueCode code{WalReadIssueCode::IoError};
  std::uint64_t byte_offset{0};
};

struct WalScanResult {
  std::size_t records_read{0};
  std::uint64_t valid_bytes{0};
  std::optional<WalReadIssue> issue;
};

class WalReader final {
 public:
  using RecordVisitor = std::function<void(const WalRecord&)>;

  explicit WalReader(std::filesystem::path path);

  [[nodiscard]] WalScanResult scan(const RecordVisitor& visitor) const;

 private:
  std::filesystem::path path_;
};

}  // namespace nebulakv
