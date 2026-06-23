#pragma once

#include "nebulakv/wal_reader.hpp"

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>

namespace nebulakv {

class InMemoryKeyValueStore;

struct RecoveryOptions {
  bool truncate_invalid_tail{true};
  std::ostream* diagnostics{nullptr};
};

struct RecoveryReport {
  std::size_t records_applied{0};
  std::size_t puts_applied{0};
  std::size_t deletes_applied{0};
  std::uint64_t valid_wal_bytes{0};
  bool invalid_tail_truncated{false};
  std::optional<WalReadIssue> issue;

  [[nodiscard]] bool completed_cleanly() const noexcept { return !issue.has_value(); }
};

class RecoveryManager final {
public:
  [[nodiscard]] static RecoveryReport recover(const std::filesystem::path& wal_path,
                                              InMemoryKeyValueStore& destination,
                                              RecoveryOptions options = {});
};

} // namespace nebulakv
