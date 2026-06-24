#pragma once

#include "nebulakv/sstable_metadata.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace nebulakv {

struct ManifestSnapshot {
  std::uint64_t generation{0};
  std::uint64_t next_file_id{1};
  std::vector<SSTableMetadata> tables;
};

class ManifestManager final {
 public:
  explicit ManifestManager(std::filesystem::path directory);

  [[nodiscard]] std::optional<ManifestSnapshot> load();
  [[nodiscard]] ManifestSnapshot commit(
      const std::vector<SSTableMetadata>& tables,
      std::uint64_t next_file_id);

  [[nodiscard]] const std::filesystem::path& current_path() const noexcept;
  [[nodiscard]] std::filesystem::path active_manifest_path() const;
  [[nodiscard]] std::uint64_t current_generation() const noexcept;

 private:
  [[nodiscard]] std::filesystem::path manifest_path(
      std::uint64_t generation) const;

  std::filesystem::path directory_;
  std::filesystem::path current_path_;
  std::filesystem::path active_manifest_path_;
  std::uint64_t current_generation_{0};
};

}  // namespace nebulakv
