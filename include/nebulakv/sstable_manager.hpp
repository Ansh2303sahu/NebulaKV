#pragma once

#include "nebulakv/entry.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace nebulakv {

class SSTableReader;

struct SSTableManagerOptions {
  std::filesystem::path directory;
  std::size_t target_data_block_bytes{32U * 1024U};
};

class SSTableManager final {
public:
  explicit SSTableManager(SSTableManagerOptions options);

  [[nodiscard]] SSTableMetadata flush(const MemTable& table);
  [[nodiscard]] std::optional<Entry> get(std::string_view key) const;
  [[nodiscard]] std::size_t table_count() const;
  [[nodiscard]] std::size_t live_key_count() const;
  [[nodiscard]] std::uint64_t max_sequence_number() const;
  [[nodiscard]] std::uint64_t next_generation() const;
  [[nodiscard]] std::vector<SSTableMetadata> metadata() const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept;

private:
  void load_existing();
  void add_reader(std::shared_ptr<SSTableReader> reader);
  [[nodiscard]] std::filesystem::path table_path(std::uint64_t generation,
                                                 std::uint64_t max_sequence) const;

  const std::filesystem::path directory_;
  const std::size_t target_data_block_bytes_{0};
  mutable std::shared_mutex mutex_;
  std::vector<std::shared_ptr<SSTableReader>> readers_;
  std::size_t live_key_count_{0};
  std::uint64_t max_sequence_number_{0};
  std::uint64_t next_generation_{0};
};

} // namespace nebulakv
