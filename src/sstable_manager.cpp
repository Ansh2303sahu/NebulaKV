#include "nebulakv/sstable_manager.hpp"

#include "nebulakv/sstable_reader.hpp"
#include "nebulakv/sstable_writer.hpp"
#include "nebulakv/validation.hpp"

#include <algorithm>
#include <cerrno>
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
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace nebulakv {
namespace {

[[nodiscard]] bool is_sstable_path(const std::filesystem::path& path) {
  return path.extension() == ".sst";
}

[[nodiscard]] bool is_temporary_storage_path(const std::filesystem::path& path) {
  const std::string filename = path.filename().string();
  return (path.extension() == ".tmp" && path.stem().extension() == ".sst") ||
         filename == "CURRENT.tmp" ||
         (filename.rfind("MANIFEST-", 0U) == 0U && path.extension() == ".tmp");
}

[[nodiscard]] bool ranges_overlap(const std::string_view left_smallest,
                                  const std::string_view left_largest,
                                  const std::string_view right_smallest,
                                  const std::string_view right_largest) {
  return left_smallest <= right_largest && right_smallest <= left_largest;
}

template <typename ManagedTableType> void sort_tables(std::vector<ManagedTableType>& tables) {
  std::sort(tables.begin(), tables.end(), [](const auto& left, const auto& right) {
    const auto& lhs = left.reader->metadata();
    const auto& rhs = right.reader->metadata();
    if (lhs.max_sequence_number != rhs.max_sequence_number) {
      return lhs.max_sequence_number > rhs.max_sequence_number;
    }
    if (lhs.generation != rhs.generation) {
      return lhs.generation > rhs.generation;
    }
    return lhs.path.filename().string() > rhs.path.filename().string();
  });
}

template <typename ManagedTableType>
void validate_level1_ranges(const std::vector<ManagedTableType>& tables) {
  std::vector<SSTableMetadata> level1;
  for (const auto& table : tables) {
    if (table.level == SSTableLevel::Level1) {
      SSTableMetadata metadata = table.reader->metadata();
      metadata.level = table.level;
      level1.push_back(std::move(metadata));
    }
  }
  std::sort(level1.begin(), level1.end(), [](const auto& left, const auto& right) {
    return left.smallest_key < right.smallest_key;
  });
  for (std::size_t index = 1; index < level1.size(); ++index) {
    if (level1[index - 1U].largest_key >= level1[index].smallest_key) {
      throw std::logic_error{"Level-1 SSTable ranges must not overlap"};
    }
  }
}

void sync_directory(const std::filesystem::path& directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::system_error{errno, std::generic_category(),
                            "failed to open SSTable directory for fsync"};
  }
  while (::fsync(descriptor) != 0) {
    if (errno == EINTR) {
      continue;
    }
    const int saved_error = errno;
    static_cast<void>(::close(descriptor));
    throw std::system_error{saved_error, std::generic_category(),
                            "failed to fsync SSTable directory"};
  }
  if (::close(descriptor) != 0) {
    throw std::system_error{errno, std::generic_category(), "failed to close SSTable directory"};
  }
}

[[nodiscard]] bool metadata_matches(const SSTableMetadata& expected,
                                    const SSTableMetadata& actual) {
  return expected.generation == actual.generation && expected.entry_count == actual.entry_count &&
         expected.block_count == actual.block_count &&
         expected.min_sequence_number == actual.min_sequence_number &&
         expected.max_sequence_number == actual.max_sequence_number &&
         expected.smallest_key == actual.smallest_key && expected.largest_key == actual.largest_key;
}

template <typename ManagedTableType>
[[nodiscard]] std::optional<Entry>
newest_entry_from_tables(const std::vector<ManagedTableType>& tables, const std::string_view key) {
  std::optional<Entry> newest;
  for (const auto& table : tables) {
    const SSTableMetadata& metadata = table.reader->metadata();
    if (key < metadata.smallest_key || key > metadata.largest_key) {
      continue;
    }
    const auto candidate = table.reader->get(key);
    if (candidate && (!newest || candidate->sequence_number > newest->sequence_number)) {
      newest = candidate;
    }
  }
  return newest;
}

template <typename ManagedTableType>
[[nodiscard]] std::size_t live_count_after_flush(const std::vector<ManagedTableType>& current,
                                                 const MemTable::Snapshot& incoming,
                                                 std::size_t live_count) {
  for (const auto& [key, entry] : incoming) {
    const auto previous = newest_entry_from_tables(current, key);
    if (previous && previous->sequence_number >= entry.sequence_number) {
      continue;
    }
    const bool was_live = previous && !previous->deleted;
    const bool is_live = !entry.deleted;
    if (!was_live && is_live) {
      ++live_count;
    } else if (was_live && !is_live) {
      --live_count;
    }
  }
  return live_count;
}

template <typename ManagedTableType>
[[nodiscard]] std::size_t compute_live_key_count(const std::vector<ManagedTableType>& tables) {
  std::map<std::string, Entry, std::less<>> latest;
  for (const auto& table : tables) {
    for (auto& [key, entry] : table.reader->read_all()) {
      const auto existing = latest.find(key);
      if (existing == latest.end() || entry.sequence_number > existing->second.sequence_number) {
        latest.insert_or_assign(std::move(key), std::move(entry));
      }
    }
  }
  return static_cast<std::size_t>(std::count_if(
      latest.begin(), latest.end(), [](const auto& item) { return !item.second.deleted; }));
}

} // namespace

SSTableManager::SSTableManager(SSTableManagerOptions options)
    : directory_{std::move(options.directory)},
      target_data_block_bytes_{options.target_data_block_bytes},
      bloom_false_positive_rate_{options.bloom_false_positive_rate},
      level0_compaction_trigger_{options.level0_compaction_trigger},
      level0_compaction_max_tables_{options.level0_compaction_max_tables},
      block_cache_{std::make_shared<BlockCache>(options.block_cache_capacity_bytes)},
      manifest_{directory_} {
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
  if (level0_compaction_trigger_ < 2U || level0_compaction_max_tables_ < 2U ||
      level0_compaction_max_tables_ < level0_compaction_trigger_) {
    throw std::invalid_argument{
        "Level-0 compaction limits must be at least two and max must meet trigger"};
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
  const std::uint64_t file_id = reserve_file_id();
  const std::filesystem::path path =
      table_path(SSTableLevel::Level0, file_id, table.generation(), max_sequence);

  SSTableWriterOptions writer_options;
  writer_options.output_path = path;
  writer_options.target_data_block_bytes = target_data_block_bytes_;
  writer_options.generation = table.generation();
  SSTableMetadata metadata = SSTableWriter::write(snapshot, std::move(writer_options));
  metadata.level = SSTableLevel::Level0;
  auto new_reader =
      std::make_shared<SSTableReader>(metadata.path, block_cache_, build_bloom_filter(snapshot));

  try {
    for (;;) {
      struct StateSnapshot {
        std::vector<ManagedTable> readers;
        std::size_t live_key_count{0};
        std::uint64_t version{0};
      };
      const StateSnapshot state = [this] {
        std::shared_lock lock{mutex_};
        return StateSnapshot{std::vector<ManagedTable>{readers_.cbegin(), readers_.cend()},
                             live_key_count_, state_version_};
      }();
      std::vector<ManagedTable> current{state.readers.cbegin(), state.readers.cend()};
      const std::size_t new_live_count =
          live_count_after_flush(current, snapshot, state.live_key_count);
      current.push_back(ManagedTable{new_reader, SSTableLevel::Level0});

      std::unique_lock lock{mutex_};
      if (state_version_ != state.version) {
        continue;
      }
      publish_tables_locked(std::move(current), new_live_count);
      break;
    }
  } catch (...) {
    throw;
  }
  return metadata;
}

CompactionResult SSTableManager::compact_level0(const bool force_all) {
  std::lock_guard compaction_lock{compaction_mutex_};

  const CompactionSelection selection = [this, force_all] {
    std::shared_lock lock{mutex_};
    return select_level0_compaction_locked(force_all);
  }();
  if (selection.selected.empty()) {
    return {};
  }

  CompactionResult result;
  result.performed = true;
  result.input_tables = selection.selected.size();

  std::map<std::string, Entry, std::less<>> merged;
  std::uint64_t output_generation = 0;
  for (const ManagedTable& table : selection.selected) {
    const SSTableMetadata& metadata = table.reader->metadata();
    result.input_entries += metadata.entry_count;
    output_generation = std::max(output_generation, metadata.generation);
    for (auto& [key, entry] : table.reader->read_all()) {
      const auto existing = merged.find(key);
      if (existing == merged.end() || entry.sequence_number > existing->second.sequence_number) {
        merged.insert_or_assign(std::move(key), std::move(entry));
      }
    }
  }

  for (auto entry = merged.begin(); entry != merged.end();) {
    if (entry->second.deleted &&
        can_drop_tombstone(entry->first, entry->second, selection.unselected)) {
      entry = merged.erase(entry);
      ++result.tombstones_dropped;
    } else {
      ++entry;
    }
  }

  MemTable::Snapshot compacted;
  compacted.reserve(merged.size());
  for (auto& [key, entry] : merged) {
    compacted.emplace_back(std::move(key), std::move(entry));
  }
  result.output_entries = compacted.size();

  std::optional<ManagedTable> output;
  std::filesystem::path output_path;
  if (!compacted.empty()) {
    const std::uint64_t file_id = reserve_file_id();
    result.output_file_id = file_id;
    std::uint64_t max_sequence = 0;
    for (const auto& [key, entry] : compacted) {
      static_cast<void>(key);
      max_sequence = std::max(max_sequence, entry.sequence_number);
    }
    output_path = table_path(SSTableLevel::Level1, file_id, output_generation, max_sequence);
    SSTableWriterOptions writer_options;
    writer_options.output_path = output_path;
    writer_options.target_data_block_bytes = target_data_block_bytes_;
    writer_options.generation = output_generation;
    SSTableMetadata output_metadata = SSTableWriter::write(compacted, std::move(writer_options));
    output_metadata.level = SSTableLevel::Level1;
    output = ManagedTable{std::make_shared<SSTableReader>(output_metadata.path, block_cache_,
                                                          build_bloom_filter(compacted)),
                          SSTableLevel::Level1};
    result.output_tables = 1U;
  }

  std::vector<std::filesystem::path> obsolete_paths;
  try {
    std::unique_lock lock{mutex_};
    std::unordered_set<std::string> selected_paths;
    selected_paths.reserve(selection.selected.size());
    for (const ManagedTable& table : selection.selected) {
      selected_paths.insert(table.reader->metadata().path.lexically_normal().string());
    }

    for (const std::string& selected_path : selected_paths) {
      const bool still_active = std::any_of(
          readers_.begin(), readers_.end(), [&selected_path](const ManagedTable& table) {
            return table.reader->metadata().path.lexically_normal().string() == selected_path;
          });
      if (!still_active) {
        throw std::runtime_error{"compaction input changed before manifest publication"};
      }
    }

    std::vector<ManagedTable> replacement;
    replacement.reserve(readers_.size() - selection.selected.size() + (output ? 1U : 0U));
    for (const ManagedTable& table : readers_) {
      const std::string path = table.reader->metadata().path.lexically_normal().string();
      if (selected_paths.contains(path)) {
        obsolete_paths.push_back(table.reader->metadata().path);
      } else {
        replacement.push_back(table);
      }
    }
    if (output) {
      replacement.push_back(*output);
    }

    const std::size_t unchanged_live_count = live_key_count_;
    publish_tables_locked(std::move(replacement), unchanged_live_count);
    ++compaction_statistics_.runs;
    compaction_statistics_.input_tables += result.input_tables;
    compaction_statistics_.output_tables += result.output_tables;
    compaction_statistics_.input_entries += result.input_entries;
    compaction_statistics_.output_entries += result.output_entries;
    compaction_statistics_.tombstones_dropped += result.tombstones_dropped;
  } catch (...) {
    throw;
  }

  bool removed_any = false;
  for (const auto& path : obsolete_paths) {
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    removed_any = removed_any || removed;
  }
  if (removed_any) {
    sync_directory(directory_);
  }
  return result;
}

CompactionResult SSTableManager::compact_if_needed() {
  if (!needs_compaction()) {
    return {};
  }
  return compact_level0(false);
}

bool SSTableManager::needs_compaction() const {
  return level_table_count(SSTableLevel::Level0) >= level0_compaction_trigger_;
}

std::optional<Entry> SSTableManager::get(const std::string_view key) const {
  validate_key(key);
  const auto readers = [this] {
    std::shared_lock lock{mutex_};
    return readers_;
  }();
  return newest_entry_from_tables(readers, key);
}

std::size_t SSTableManager::table_count() const {
  std::shared_lock lock{mutex_};
  return readers_.size();
}

std::size_t SSTableManager::level_table_count(const SSTableLevel level) const {
  std::shared_lock lock{mutex_};
  return static_cast<std::size_t>(
      std::count_if(readers_.begin(), readers_.end(),
                    [level](const ManagedTable& table) { return table.level == level; }));
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
  return metadata_for(readers_);
}

const std::filesystem::path& SSTableManager::directory() const noexcept { return directory_; }

const std::filesystem::path& SSTableManager::current_path() const noexcept {
  return manifest_.current_path();
}

std::filesystem::path SSTableManager::active_manifest_path() const {
  std::shared_lock lock{mutex_};
  return manifest_.active_manifest_path();
}

CompactionStatistics SSTableManager::compaction_statistics() const {
  std::shared_lock lock{mutex_};
  return compaction_statistics_;
}

void SSTableManager::load_existing() {
  for (const auto& directory_entry : std::filesystem::directory_iterator{directory_}) {
    if (!directory_entry.is_regular_file()) {
      continue;
    }
    if (is_temporary_storage_path(directory_entry.path())) {
      std::error_code error;
      std::filesystem::remove(directory_entry.path(), error);
    }
  }

  if (const auto snapshot = manifest_.load()) {
    load_manifest_snapshot(*snapshot);
  } else {
    migrate_legacy_tables();
  }
  cleanup_abandoned_files(metadata_for(readers_));
  live_key_count_ = compute_live_key_count(readers_);
  recalculate_state_locked();
  block_cache_->clear();
  block_cache_->reset_statistics();
}

void SSTableManager::load_manifest_snapshot(const ManifestSnapshot& snapshot) {
  std::vector<ManagedTable> loaded;
  loaded.reserve(snapshot.tables.size());
  for (const SSTableMetadata& expected : snapshot.tables) {
    auto uncached_reader = std::make_shared<SSTableReader>(expected.path, block_cache_);
    if (!metadata_matches(expected, uncached_reader->metadata())) {
      throw std::runtime_error{"manifest metadata mismatch for SSTable: " + expected.path.string()};
    }
    MemTable::Snapshot contents = uncached_reader->read_all();
    loaded.push_back(ManagedTable{
        std::make_shared<SSTableReader>(expected.path, block_cache_, build_bloom_filter(contents)),
        expected.level});
  }
  readers_ = std::move(loaded);
  sort_tables(readers_);
  validate_level1_ranges(readers_);
  next_file_id_ = snapshot.next_file_id;
}

void SSTableManager::migrate_legacy_tables() {
  std::vector<std::filesystem::path> paths;
  for (const auto& directory_entry : std::filesystem::directory_iterator{directory_}) {
    if (directory_entry.is_regular_file() && is_sstable_path(directory_entry.path())) {
      paths.push_back(directory_entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    auto uncached_reader = std::make_shared<SSTableReader>(path, block_cache_);
    MemTable::Snapshot contents = uncached_reader->read_all();
    readers_.push_back(ManagedTable{
        std::make_shared<SSTableReader>(path, block_cache_, build_bloom_filter(contents)),
        SSTableLevel::Level0});
  }
  sort_tables(readers_);
  next_file_id_ = 1U;
  static_cast<void>(manifest_.commit(metadata_for(readers_), next_file_id_));
}

void SSTableManager::cleanup_abandoned_files(
    const std::vector<SSTableMetadata>& active_tables) const {
  std::set<std::string> active;
  for (const SSTableMetadata& table : active_tables) {
    active.insert(table.path.filename().string());
  }

  bool removed_any = false;
  for (const auto& directory_entry : std::filesystem::directory_iterator{directory_}) {
    if (!directory_entry.is_regular_file() || !is_sstable_path(directory_entry.path())) {
      continue;
    }
    if (!active.contains(directory_entry.path().filename().string())) {
      std::error_code error;
      const bool removed = std::filesystem::remove(directory_entry.path(), error);
      if (error) {
        throw std::system_error{error, "failed to remove abandoned SSTable file"};
      }
      removed_any = removed_any || removed;
    }
  }
  if (removed_any) {
    sync_directory(directory_);
  }
}

void SSTableManager::publish_tables_locked(std::vector<ManagedTable> tables,
                                           const std::size_t live_key_count) {
  sort_tables(tables);

  validate_level1_ranges(tables);

  std::uint64_t new_max_sequence = 0U;
  std::uint64_t maximum_generation = 0U;
  bool has_tables = false;
  for (const ManagedTable& table : tables) {
    const SSTableMetadata& metadata = table.reader->metadata();
    new_max_sequence = std::max(new_max_sequence, metadata.max_sequence_number);
    maximum_generation = std::max(maximum_generation, metadata.generation);
    has_tables = true;
  }
  std::uint64_t new_next_generation = 0U;
  if (has_tables) {
    if (maximum_generation == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error{"SSTable generation space is exhausted"};
    }
    new_next_generation = maximum_generation + 1U;
  }

  const std::vector<SSTableMetadata> manifest_tables = metadata_for(tables);
  static_cast<void>(manifest_.commit(manifest_tables, next_file_id_));
  readers_ = std::move(tables);
  live_key_count_ = live_key_count;
  max_sequence_number_ = new_max_sequence;
  next_generation_ = new_next_generation;
  ++state_version_;
  block_cache_->clear();
  block_cache_->reset_statistics();
}

void SSTableManager::recalculate_state_locked() {
  max_sequence_number_ = 0U;
  std::uint64_t maximum_generation = 0U;
  bool has_tables = false;
  for (const ManagedTable& table : readers_) {
    const SSTableMetadata& metadata = table.reader->metadata();
    max_sequence_number_ = std::max(max_sequence_number_, metadata.max_sequence_number);
    maximum_generation = std::max(maximum_generation, metadata.generation);
    has_tables = true;
  }
  if (has_tables) {
    if (maximum_generation == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error{"SSTable generation space is exhausted"};
    }
    next_generation_ = maximum_generation + 1U;
  } else {
    next_generation_ = 0U;
  }
}

SSTableManager::CompactionSelection
SSTableManager::select_level0_compaction_locked(const bool force_all) const {
  std::vector<ManagedTable> level0;
  for (const ManagedTable& table : readers_) {
    if (table.level == SSTableLevel::Level0) {
      level0.push_back(table);
    }
  }
  if (level0.empty() || (!force_all && level0.size() < level0_compaction_trigger_)) {
    return {};
  }

  std::sort(level0.begin(), level0.end(), [](const auto& left, const auto& right) {
    const auto& lhs = left.reader->metadata();
    const auto& rhs = right.reader->metadata();
    if (lhs.max_sequence_number != rhs.max_sequence_number) {
      return lhs.max_sequence_number < rhs.max_sequence_number;
    }
    return lhs.generation < rhs.generation;
  });
  const std::size_t selected_count =
      force_all ? level0.size() : std::min(level0.size(), level0_compaction_max_tables_);

  CompactionSelection selection;
  selection.selected = std::vector<ManagedTable>{
      level0.cbegin(), level0.cbegin() + static_cast<std::ptrdiff_t>(selected_count)};
  selection.smallest_key = selection.selected.front().reader->metadata().smallest_key;
  selection.largest_key = selection.selected.front().reader->metadata().largest_key;
  for (const ManagedTable& table : selection.selected) {
    selection.smallest_key =
        std::min(selection.smallest_key, table.reader->metadata().smallest_key);
    selection.largest_key = std::max(selection.largest_key, table.reader->metadata().largest_key);
  }

  std::set<std::string> selected_paths;
  for (const ManagedTable& table : selection.selected) {
    selected_paths.insert(table.reader->metadata().path.lexically_normal().string());
  }

  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (const ManagedTable& table : readers_) {
      if (table.level != SSTableLevel::Level1) {
        continue;
      }
      const std::string path = table.reader->metadata().path.lexically_normal().string();
      if (selected_paths.contains(path)) {
        continue;
      }
      const SSTableMetadata& metadata = table.reader->metadata();
      if (ranges_overlap(selection.smallest_key, selection.largest_key, metadata.smallest_key,
                         metadata.largest_key)) {
        selection.selected.push_back(table);
        selected_paths.insert(path);
        selection.smallest_key = std::min(selection.smallest_key, metadata.smallest_key);
        selection.largest_key = std::max(selection.largest_key, metadata.largest_key);
        expanded = true;
      }
    }
  }

  for (const ManagedTable& table : readers_) {
    const std::string path = table.reader->metadata().path.lexically_normal().string();
    if (!selected_paths.contains(path)) {
      selection.unselected.push_back(table);
    }
  }
  return selection;
}

bool SSTableManager::can_drop_tombstone(const std::string_view key, const Entry& tombstone,
                                        const std::vector<ManagedTable>& unselected) const {
  for (const ManagedTable& table : unselected) {
    const SSTableMetadata& metadata = table.reader->metadata();
    if (key < metadata.smallest_key || key > metadata.largest_key) {
      continue;
    }
    const auto entry = table.reader->get(key);
    if (entry && !entry->deleted && entry->sequence_number <= tombstone.sequence_number) {
      return false;
    }
  }
  return true;
}

std::vector<SSTableMetadata>
SSTableManager::metadata_for(const std::vector<ManagedTable>& tables) const {
  std::vector<SSTableMetadata> result;
  result.reserve(tables.size());
  for (const ManagedTable& table : tables) {
    SSTableMetadata metadata = table.reader->metadata();
    metadata.level = table.level;
    result.push_back(std::move(metadata));
  }
  return result;
}

std::uint64_t SSTableManager::reserve_file_id() {
  std::unique_lock lock{mutex_};
  if (next_file_id_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error{"SSTable file identifier space is exhausted"};
  }
  const std::uint64_t result = next_file_id_;
  ++next_file_id_;
  return result;
}

std::filesystem::path SSTableManager::table_path(const SSTableLevel level,
                                                 const std::uint64_t file_id,
                                                 const std::uint64_t generation,
                                                 const std::uint64_t max_sequence) const {
  std::ostringstream name;
  name << "sstable-" << to_string(level) << "-f" << std::setw(20) << std::setfill('0') << file_id
       << "-g" << std::setw(20) << std::setfill('0') << generation << "-s" << std::setw(20)
       << std::setfill('0') << max_sequence << ".sst";
  return directory_ / name.str();
}

BlockCacheStatistics SSTableManager::block_cache_statistics() const {
  return block_cache_->statistics();
}

BloomFilterAggregateStatistics SSTableManager::bloom_filter_statistics() const {
  const auto readers = [this] {
    std::shared_lock lock{mutex_};
    return readers_;
  }();

  BloomFilterAggregateStatistics aggregate;
  for (const ManagedTable& table : readers) {
    if (const auto filter = table.reader->bloom_filter_statistics()) {
      ++aggregate.filter_count;
      aggregate.inserted_keys += filter->inserted_keys;
      aggregate.memory_bytes += filter->memory_bytes;
    }
    const SSTableLookupStatistics lookup = table.reader->lookup_statistics();
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
    return readers_;
  }();
  for (const ManagedTable& table : readers) {
    table.reader->reset_lookup_statistics();
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

} // namespace nebulakv
