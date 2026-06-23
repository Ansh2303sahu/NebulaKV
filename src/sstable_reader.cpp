#include "nebulakv/sstable_reader.hpp"

#include "nebulakv/sstable_error.hpp"
#include "nebulakv/validation.hpp"
#include "sstable_format.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace nebulakv {

namespace {

class FileDescriptor final {
public:
  explicit FileDescriptor(const std::filesystem::path& path)
      : value_{::open(path.c_str(), O_RDONLY | O_CLOEXEC)} {
    if (value_ < 0) {
      throw std::system_error{errno, std::generic_category(), "failed to open SSTable file"};
    }
  }

  ~FileDescriptor() { ::close(value_); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_{-1};
};

[[nodiscard]] std::vector<std::byte> read_exact(const int descriptor, const std::uint64_t offset,
                                                const std::size_t size) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    throw std::overflow_error{"SSTable offset exceeds the platform file range"};
  }

  std::vector<std::byte> bytes(size);
  std::size_t completed = 0;
  while (completed < size) {
    const std::uint64_t absolute_offset = offset + completed;
    if (absolute_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      throw std::overflow_error{"SSTable read offset exceeds the platform file range"};
    }
    const ssize_t result = ::pread(descriptor, bytes.data() + completed, size - completed,
                                   static_cast<off_t>(absolute_offset));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error{errno, std::generic_category(), "failed to read SSTable file"};
    }
    if (result == 0) {
      throw SSTableCorruptionError{absolute_offset, "SSTable file is truncated"};
    }
    completed += static_cast<std::size_t>(result);
  }
  return bytes;
}

[[nodiscard]] std::uint64_t checked_file_size(const std::filesystem::path& path) {
  const auto size = std::filesystem::file_size(path);
  if (size > std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error{"SSTable file is too large"};
  }
  return static_cast<std::uint64_t>(size);
}

} // namespace

SSTableReader::SSTableReader(std::filesystem::path path, std::shared_ptr<BlockCache> block_cache,
                             std::shared_ptr<const BloomFilter> bloom_filter)
    : path_{std::move(path)}, cache_table_id_{path_.lexically_normal().string()},
      block_cache_{std::move(block_cache)}, bloom_filter_{std::move(bloom_filter)} {
  if (path_.empty()) {
    throw std::invalid_argument{"SSTable path must not be empty"};
  }

  const std::uint64_t size = checked_file_size(path_);
  const std::uint64_t minimum_size = sstable_format::kHeaderSize + sstable_format::kFooterSize;
  if (size < minimum_size) {
    throw SSTableCorruptionError{0, "SSTable file is too small"};
  }

  FileDescriptor descriptor{path_};
  const auto header_bytes = read_exact(descriptor.get(), 0, sstable_format::kHeaderSize);
  const sstable_format::Header header = sstable_format::parse_header(header_bytes, 0);

  const std::uint64_t footer_offset = size - sstable_format::kFooterSize;
  const auto footer_bytes =
      read_exact(descriptor.get(), footer_offset, sstable_format::kFooterSize);
  footer_ = sstable_format::parse_footer(footer_bytes, footer_offset);

  if (footer_.index_offset < sstable_format::kHeaderSize || footer_.index_offset > footer_offset ||
      footer_.index_size > footer_offset - footer_.index_offset) {
    throw SSTableCorruptionError{footer_offset, "SSTable footer references an invalid index range"};
  }
  if (header.entry_count != footer_.entry_count || header.block_count != footer_.block_count ||
      header.min_sequence_number != footer_.min_sequence_number ||
      header.max_sequence_number != footer_.max_sequence_number) {
    throw SSTableCorruptionError{footer_offset, "SSTable header and footer metadata disagree"};
  }
  if (footer_.index_size > std::numeric_limits<std::size_t>::max()) {
    throw SSTableCorruptionError{footer_.index_offset,
                                 "SSTable index is too large for this process"};
  }

  const auto index_bytes = read_exact(descriptor.get(), footer_.index_offset,
                                      static_cast<std::size_t>(footer_.index_size));
  index_ = sstable_format::parse_index_block(index_bytes, footer_.index_offset);
  if (index_.entries.size() != footer_.block_count) {
    throw SSTableCorruptionError{footer_.index_offset,
                                 "SSTable block count does not match the index"};
  }

  std::uint64_t previous_end = sstable_format::kHeaderSize;
  for (const IndexEntry& entry : index_.entries) {
    if (entry.block_offset < sstable_format::kHeaderSize || entry.block_offset < previous_end ||
        entry.block_offset > footer_.index_offset ||
        entry.block_size > footer_.index_offset - entry.block_offset) {
      throw SSTableCorruptionError{footer_.index_offset,
                                   "SSTable index references an invalid data block"};
    }
    previous_end = entry.block_offset + entry.block_size;
  }
  if (previous_end != footer_.index_offset) {
    throw SSTableCorruptionError{footer_.index_offset,
                                 "SSTable data blocks do not end at the index"};
  }

  metadata_.path = path_;
  metadata_.generation = header.generation;
  metadata_.entry_count = header.entry_count;
  metadata_.block_count = header.block_count;
  metadata_.min_sequence_number = header.min_sequence_number;
  metadata_.max_sequence_number = header.max_sequence_number;
  metadata_.smallest_key = index_.entries.front().first_key;
  metadata_.largest_key = index_.entries.back().last_key;
}

std::optional<Entry> SSTableReader::get(const std::string_view key) const {
  validate_key(key);
  if (key < metadata_.smallest_key || key > metadata_.largest_key) {
    return std::nullopt;
  }

  const auto candidate =
      std::lower_bound(index_.entries.begin(), index_.entries.end(), key,
                       [](const IndexEntry& entry, const std::string_view sought) {
                         return entry.last_key < sought;
                       });
  if (candidate == index_.entries.end() || key < candidate->first_key) {
    return std::nullopt;
  }

  if (bloom_filter_) {
    bloom_checks_.fetch_add(1U, std::memory_order_relaxed);
    if (!bloom_filter_->may_contain(key)) {
      bloom_negatives_.fetch_add(1U, std::memory_order_relaxed);
      return std::nullopt;
    }
  }

  const BlockCache::BlockPointer block = read_data_block(*candidate);
  const auto entry =
      std::lower_bound(block->records.begin(), block->records.end(), key,
                       [](const DataBlock::Record& record, const std::string_view sought) {
                         return record.first < sought;
                       });
  if (entry == block->records.end() || entry->first != key) {
    return std::nullopt;
  }
  return entry->second;
}

MemTable::Snapshot SSTableReader::read_all() const {
  MemTable::Snapshot result;
  if (metadata_.entry_count > std::numeric_limits<std::size_t>::max()) {
    throw std::length_error{"SSTable entry count exceeds process limits"};
  }
  result.reserve(static_cast<std::size_t>(metadata_.entry_count));
  for (const IndexEntry& entry : index_.entries) {
    const BlockCache::BlockPointer block = read_data_block(entry);
    result.insert(result.end(), block->records.begin(), block->records.end());
  }
  if (result.size() != metadata_.entry_count) {
    throw SSTableCorruptionError{0, "SSTable entry count does not match its metadata"};
  }
  return result;
}

const SSTableMetadata& SSTableReader::metadata() const noexcept { return metadata_; }

const IndexBlock& SSTableReader::index() const noexcept { return index_; }

const SSTableFooter& SSTableReader::footer() const noexcept { return footer_; }

bool SSTableReader::uses_bloom_filter() const noexcept { return static_cast<bool>(bloom_filter_); }

std::optional<BloomFilterStatistics> SSTableReader::bloom_filter_statistics() const noexcept {
  if (!bloom_filter_) {
    return std::nullopt;
  }
  return bloom_filter_->statistics();
}

SSTableLookupStatistics SSTableReader::lookup_statistics() const noexcept {
  return SSTableLookupStatistics{bloom_checks_.load(std::memory_order_relaxed),
                                 bloom_negatives_.load(std::memory_order_relaxed)};
}

void SSTableReader::reset_lookup_statistics() noexcept {
  bloom_checks_.store(0U, std::memory_order_relaxed);
  bloom_negatives_.store(0U, std::memory_order_relaxed);
}

BlockCache::BlockPointer SSTableReader::read_data_block(const IndexEntry& index_entry) const {
  if (index_entry.block_size > std::numeric_limits<std::size_t>::max()) {
    throw SSTableCorruptionError{index_entry.block_offset,
                                 "SSTable data block is too large for this process"};
  }
  if (block_cache_) {
    if (auto cached = block_cache_->get(cache_table_id_, index_entry.block_offset)) {
      return cached;
    }
  }

  FileDescriptor descriptor{path_};
  const auto bytes = read_exact(descriptor.get(), index_entry.block_offset,
                                static_cast<std::size_t>(index_entry.block_size));
  auto block = std::make_shared<DataBlock>(
      sstable_format::parse_data_block(bytes, index_entry.block_offset));
  if (block->first_key() != index_entry.first_key || block->last_key() != index_entry.last_key) {
    throw SSTableCorruptionError{index_entry.block_offset,
                                 "SSTable data block does not match its index range"};
  }
  if (block_cache_) {
    block_cache_->put(cache_table_id_, index_entry.block_offset, block);
  }
  return block;
}

} // namespace nebulakv
