#include "nebulakv/block_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nebulakv {

double BlockCacheStatistics::hit_ratio() const noexcept {
  const std::uint64_t total = hits + misses;
  if (total == 0U) {
    return 0.0;
  }
  return static_cast<double>(hits) / static_cast<double>(total);
}

BlockCache::BlockCache(const std::size_t capacity_bytes) : capacity_bytes_{capacity_bytes} {
  if (capacity_bytes_ == 0U) {
    throw std::invalid_argument{"block cache capacity must be positive"};
  }
}

BlockCache::BlockPointer BlockCache::get(const std::string_view table_id,
                                         const std::uint64_t block_offset) {
  std::lock_guard lock{mutex_};
  const Key key{std::string{table_id}, block_offset};
  const auto found = index_.find(key);
  if (found == index_.end()) {
    ++misses_;
    return {};
  }

  lru_.splice(lru_.begin(), lru_, found->second);
  ++hits_;
  return found->second->block;
}

void BlockCache::put(std::string table_id, const std::uint64_t block_offset, BlockPointer block) {
  if (!block) {
    throw std::invalid_argument{"cannot cache a null data block"};
  }
  const std::size_t charge = estimate_charge(*block);

  std::lock_guard lock{mutex_};
  const Key key{std::move(table_id), block_offset};
  const auto existing = index_.find(key);
  if (existing != index_.end()) {
    current_bytes_ -= existing->second->charge;
    lru_.erase(existing->second);
    index_.erase(existing);
  }

  if (charge > capacity_bytes_) {
    return;
  }

  evict_until_fits(charge);
  lru_.push_front(Node{key, std::move(block), charge});
  index_.emplace(lru_.front().key, lru_.begin());
  current_bytes_ += charge;
}

void BlockCache::clear() {
  std::lock_guard lock{mutex_};
  index_.clear();
  lru_.clear();
  current_bytes_ = 0U;
}

void BlockCache::reset_statistics() {
  std::lock_guard lock{mutex_};
  hits_ = 0U;
  misses_ = 0U;
  evictions_ = 0U;
}

BlockCacheStatistics BlockCache::statistics() const {
  std::lock_guard lock{mutex_};
  return BlockCacheStatistics{capacity_bytes_, current_bytes_, index_.size(),
                              hits_,           misses_,        evictions_};
}

std::size_t BlockCache::capacity_bytes() const noexcept { return capacity_bytes_; }

std::size_t BlockCache::estimate_charge(const DataBlock& block) noexcept {
  std::size_t result = sizeof(DataBlock);
  result += block.records.size() * sizeof(DataBlock::Record);
  for (const auto& [key, entry] : block.records) {
    result += key.size();
    result += entry.value.size();
  }
  return result;
}

std::size_t BlockCache::KeyHash::operator()(const Key& key) const noexcept {
  const std::size_t first = std::hash<std::string>{}(key.table_id);
  const std::size_t second = std::hash<std::uint64_t>{}(key.block_offset);
  return first ^ (second + 0x9E3779B9U + (first << 6U) + (first >> 2U));
}

void BlockCache::evict_until_fits(const std::size_t incoming_charge) {
  while (!lru_.empty() && current_bytes_ > capacity_bytes_ - incoming_charge) {
    auto last = std::prev(lru_.end());
    current_bytes_ -= last->charge;
    index_.erase(last->key);
    lru_.erase(last);
    ++evictions_;
  }
}

} // namespace nebulakv
