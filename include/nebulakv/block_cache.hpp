#pragma once

#include "nebulakv/data_block.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nebulakv {

struct BlockCacheStatistics {
  std::size_t capacity_bytes{0};
  std::size_t current_bytes{0};
  std::size_t entry_count{0};
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};

  [[nodiscard]] double hit_ratio() const noexcept;
};

class BlockCache final {
public:
  using BlockPointer = std::shared_ptr<const DataBlock>;

  explicit BlockCache(std::size_t capacity_bytes);

  [[nodiscard]] BlockPointer get(std::string_view table_id, std::uint64_t block_offset);
  void put(std::string table_id, std::uint64_t block_offset, BlockPointer block);

  void clear();
  void reset_statistics();

  [[nodiscard]] BlockCacheStatistics statistics() const;
  [[nodiscard]] std::size_t capacity_bytes() const noexcept;
  [[nodiscard]] static std::size_t estimate_charge(const DataBlock& block) noexcept;

private:
  struct Key {
    std::string table_id;
    std::uint64_t block_offset{0};

    [[nodiscard]] bool operator==(const Key&) const noexcept = default;
  };

  struct KeyHash {
    [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
  };

  struct Node {
    Key key;
    BlockPointer block;
    std::size_t charge{0};
  };

  using List = std::list<Node>;
  using Index = std::unordered_map<Key, List::iterator, KeyHash>;

  void evict_until_fits(std::size_t incoming_charge);

  const std::size_t capacity_bytes_{0};
  mutable std::mutex mutex_;
  List lru_;
  Index index_;
  std::size_t current_bytes_{0};
  std::uint64_t hits_{0};
  std::uint64_t misses_{0};
  std::uint64_t evictions_{0};
};

} // namespace nebulakv
