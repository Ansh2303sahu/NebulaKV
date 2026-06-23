#include "nebulakv/sstable_manager.hpp"

#include "nebulakv/sstable_reader.hpp"
#include "nebulakv/sstable_writer.hpp"
#include "nebulakv/validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace nebulakv {

namespace {

[[nodiscard]] bool is_sstable_path(const std::filesystem::path& path) {
  return path.extension() == ".sst";
}

[[nodiscard]] bool is_temporary_sstable_path(const std::filesystem::path& path) {
  return path.extension() == ".tmp" && path.stem().extension() == ".sst";
}

} // namespace

SSTableManager::SSTableManager(SSTableManagerOptions options)
    : directory_{std::move(options.directory)},
      target_data_block_bytes_{options.target_data_block_bytes},
      bloom_false_positive_rate_{options.bloom_false_positive_rate},
      block_cache_{std::make_shared<BlockCache>(options.block_cache_capacity_bytes)} {
  if (directory_.empty()) {
    throw std::invalid_argument{"SSTable directory must not be empty"};
  }
  if (target_data_block_bytes_ == 0U ||
      target_data_block_bytes_ > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument{"SSTable target data block size is invalid"};
  }
  if (!std::isfinite(bloom_false_positive_rate_) || bloom_false_positive_rate_ <= 0.0 ||
      bloom_false_positive_rate_ >= 1.0) {
    throw std::invalid_argument{
        "SSTable Bloom filter false-positive rate must be between zero and one"};
  }
  std::filesystem::create_directories(directory_);
  load_existing();
}

SSTableMetadata SSTableManager::flush(const MemTable& table) {
  if (!table.is_immutable()) {
    throw std::invalid_argument{"only immutable MemTables can be flushed to SSTables"};
  }
  const MemTable::Snapshot snapshot = table.snapshot();
  if (snapshot.empty()) {
    throw std::invalid_argument{"cannot flush an empty MemTable"};
  }

  std::uint64_t max_sequence = 0;
  for (const auto& [key, entry] : snapshot) {
    static_cast<void>(key);
    max_sequence = std::max(max_sequence, entry.sequence_number);
  }
  const std::filesystem::path path = table_path(table.generation(), max_sequence);
  if (std::filesystem::exists(path)) {
    throw std::runtime_error{"SSTable output path already exists: " + path.string()};
  }

  SSTableWriterOptions writer_options;
  writer_options.output_path = path;
  writer_options.target_data_block_bytes = target_data_block_bytes_;
  writer_options.generation = table.generation();
  SSTableMetadata metadata = SSTableWriter::write(snapshot, std::move(writer_options));
  add_reader(
      std::make_shared<SSTableReader>(metadata.path, block_cache_, build_bloom_filter(snapshot)));
  return metadata;
}

std::optional<Entry> SSTableManager::get(const std::string_view key) const {
  validate_key(key);
  const auto readers = [this] {
    std::shared_lock lock{mutex_};
    return std::vector<std::shared_ptr<SSTableReader>>{readers_.cbegin(), readers_.cend()};
  }();

  std::optional<Entry> newest;
  for (const auto& reader : readers) {
    const SSTableMetadata& table = reader->metadata();
    if (key < table.smallest_key || key > table.largest_key) {
      continue;
    }
    const auto candidate = reader->get(key);
    if (candidate && (!newest || candidate->sequence_number > newest->sequence_number)) {
      newest = candidate;
    }
  }
  return newest;
}

std::size_t SSTableManager::table_count() const {
  std::shared_lock lock{mutex_};
  return readers_.size();
}

std::size_t SSTableManager::live_key_count() const {
  std::shared_lock lock{mutex_};
  return live_key_count_;
}

std::uint64_t SSTableManager::max_sequence_number() const {
  std::shared_lock lock{mutex_};
  return max_sequence_number_;
}

std::uint64_t SSTableManager::next_generation() const {
  std::shared_lock lock{mutex_};
  return next_generation_;
}

std::vector<SSTableMetadata> SSTableManager::metadata() const {
  std::shared_lock lock{mutex_};
  std::vector<SSTableMetadata> result;
  result.reserve(readers_.size());
  for (const auto& reader : readers_) {
    result.push_back(reader->metadata());
  }
  return result;
}

const std::filesystem::path& SSTableManager::directory() const noexcept { return directory_; }

void SSTableManager::load_existing() {
  std::vector<std::filesystem::path> paths;
  for (const auto& directory_entry : std::filesystem::directory_iterator{directory_}) {
    if (!directory_entry.is_regular_file()) {
      continue;
    }
    if (is_temporary_sstable_path(directory_entry.path())) {
      std::error_code ignored;
      std::filesystem::remove(directory_entry.path(), ignored);
      continue;
    }
    if (is_sstable_path(directory_entry.path())) {
      paths.push_back(directory_entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());

  std::map<std::string, Entry, std::less<>> latest;
  for (const auto& path : paths) {
    const auto uncached_reader = std::make_shared<SSTableReader>(path, block_cache_);
    MemTable::Snapshot snapshot = uncached_reader->read_all();
    for (auto& [key, entry] : snapshot) {
      const auto existing = latest.find(key);
      if (existing == latest.end() || entry.sequence_number > existing->second.sequence_number) {
        latest.insert_or_assign(key, entry);
      }
    }
    readers_.push_back(
        std::make_shared<SSTableReader>(path, block_cache_, build_bloom_filter(snapshot)));
  }

  std::sort(readers_.begin(), readers_.end(), [](const auto& left, const auto& right) {
    const auto& lhs = left->metadata();
    const auto& rhs = right->metadata();
    if (lhs.max_sequence_number != rhs.max_sequence_number) {
      return lhs.max_sequence_number > rhs.max_sequence_number;
    }
    return lhs.generation > rhs.generation;
  });

  live_key_count_ = static_cast<std::size_t>(std::count_if(
      latest.begin(), latest.end(), [](const auto& item) { return !item.second.deleted; }));
  std::uint64_t maximum_generation = 0;
  bool has_tables = false;
  for (const auto& reader : readers_) {
    max_sequence_number_ = std::max(max_sequence_number_, reader->metadata().max_sequence_number);
    maximum_generation = std::max(maximum_generation, reader->metadata().generation);
    has_tables = true;
  }
  if (has_tables) {
    if (maximum_generation == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error{"SSTable generation space is exhausted"};
    }
    next_generation_ = maximum_generation + 1U;
  }
  block_cache_->clear();
  block_cache_->reset_statistics();
}

void SSTableManager::add_reader(std::shared_ptr<SSTableReader> reader) {
  std::unique_lock lock{mutex_};
  readers_.push_back(std::move(reader));
  std::sort(readers_.begin(), readers_.end(), [](const auto& left, const auto& right) {
    const auto& lhs = left->metadata();
    const auto& rhs = right->metadata();
    if (lhs.max_sequence_number != rhs.max_sequence_number) {
      return lhs.max_sequence_number > rhs.max_sequence_number;
    }
    return lhs.generation > rhs.generation;
  });

  std::uint64_t maximum_generation = 0;
  for (const auto& table : readers_) {
    max_sequence_number_ = std::max(max_sequence_number_, table->metadata().max_sequence_number);
    maximum_generation = std::max(maximum_generation, table->metadata().generation);
  }
  if (maximum_generation == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error{"SSTable generation space is exhausted"};
  }
  next_generation_ = maximum_generation + 1U;

  std::map<std::string, Entry, std::less<>> latest;
  for (const auto& table : readers_) {
    for (auto& [key, entry] : table->read_all()) {
      const auto existing = latest.find(key);
      if (existing == latest.end() || entry.sequence_number > existing->second.sequence_number) {
        latest.insert_or_assign(std::move(key), std::move(entry));
      }
    }
  }
  live_key_count_ = static_cast<std::size_t>(std::count_if(
      latest.begin(), latest.end(), [](const auto& item) { return !item.second.deleted; }));
  block_cache_->clear();
  block_cache_->reset_statistics();
}

BlockCacheStatistics SSTableManager::block_cache_statistics() const {
  return block_cache_->statistics();
}

BloomFilterAggregateStatistics SSTableManager::bloom_filter_statistics() const {
  const auto readers = [this] {
    std::shared_lock lock{mutex_};
    return std::vector<std::shared_ptr<SSTableReader>>{readers_.cbegin(), readers_.cend()};
  }();

  BloomFilterAggregateStatistics aggregate;
  for (const auto& reader : readers) {
    if (const auto filter = reader->bloom_filter_statistics()) {
      ++aggregate.filter_count;
      aggregate.inserted_keys += filter->inserted_keys;
      aggregate.memory_bytes += filter->memory_bytes;
    }
    const SSTableLookupStatistics lookup = reader->lookup_statistics();
    aggregate.checks += lookup.bloom_checks;
    aggregate.negative_results += lookup.bloom_negatives;
  }
  return aggregate;
}

void SSTableManager::clear_block_cache() { block_cache_->clear(); }

void SSTableManager::reset_read_statistics() {
  block_cache_->reset_statistics();
  const auto readers = [this] {
    std::shared_lock lock{mutex_};
    return std::vector<std::shared_ptr<SSTableReader>>{readers_.cbegin(), readers_.cend()};
  }();
  for (const auto& reader : readers) {
    reader->reset_lookup_statistics();
  }
}

std::shared_ptr<const BloomFilter>
SSTableManager::build_bloom_filter(const MemTable::Snapshot& snapshot) const {
  auto filter = std::make_shared<BloomFilter>(snapshot.size(), bloom_false_positive_rate_);
  for (const auto& [key, entry] : snapshot) {
    static_cast<void>(entry);
    filter->add(key);
  }
  return filter;
}

std::filesystem::path SSTableManager::table_path(const std::uint64_t generation,
                                                 const std::uint64_t max_sequence) const {
  std::ostringstream name;
  name << "sstable-" << std::setw(20) << std::setfill('0') << generation << '-' << std::setw(20)
       << std::setfill('0') << max_sequence << ".sst";
  return directory_ / name.str();
}

} // namespace nebulakv
