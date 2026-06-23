#include "nebulakv/durability_mode.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

int main(const int argc, char** argv) {
  std::filesystem::path wal_path{"data/nebulakv.wal"};
  bool create_checkpoint = false;
  bool compact_storage = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--checkpoint") {
      create_checkpoint = true;
    } else if (argument == "--compact") {
      compact_storage = true;
    } else {
      wal_path = std::filesystem::path{argument};
    }
  }

  nebulakv::PersistentStoreOptions options;
  options.wal_path = wal_path;
  options.durability_mode = nebulakv::DurabilityMode::Sync;

  try {
    nebulakv::PersistentKeyValueStore store{options};
    store.put("project", "NebulaKV");
    store.put("storage", "WAL, sorted MemTables, indexed SSTables, Bloom filters, LRU cache, "
                         "atomic manifests, and leveled compaction");
    if (create_checkpoint) {
      store.checkpoint();
    }

    std::optional<nebulakv::CompactionResult> compaction;
    if (compact_storage) {
      compaction = store.compact();
    }

    const auto project = store.get("project");
    const auto storage = store.get("storage");
    if (!project || !storage) {
      std::cerr << "Failed to read the demonstration keys\n";
      return 1;
    }

    std::cout << *project << ": " << *storage << '\n';
    std::cout << "wal=" << wal_path << '\n';
    std::cout << "sstable_directory=" << store.sstable_directory() << '\n';
    std::cout << "current=" << store.current_path() << '\n';
    std::cout << "manifest=" << store.active_manifest_path() << '\n';
    std::cout << "durability=" << nebulakv::to_string(store.durability_mode()) << '\n';
    std::cout << "recovered_records=" << store.recovery_report().records_applied << '\n';
    std::cout << "last_sequence=" << store.last_sequence_number() << '\n';
    std::cout << "immutable_memtables=" << store.immutable_memtable_count() << '\n';
    const auto cache = store.block_cache_statistics();
    const auto bloom = store.bloom_filter_statistics();
    const auto compaction_statistics = store.compaction_statistics();
    std::cout << "sstables=" << store.sstable_count() << '\n';
    std::cout << "level0_sstables=" << store.level0_sstable_count() << '\n';
    std::cout << "level1_sstables=" << store.level1_sstable_count() << '\n';
    std::cout << "bloom_filters=" << bloom.filter_count << '\n';
    std::cout << "bloom_memory_bytes=" << bloom.memory_bytes << '\n';
    std::cout << "cache_entries=" << cache.entry_count << '\n';
    std::cout << "cache_hit_ratio=" << cache.hit_ratio() << '\n';
    std::cout << "compaction_runs=" << compaction_statistics.runs << '\n';
    if (compaction) {
      std::cout << "compaction_performed=" << compaction->performed << '\n';
      std::cout << "compaction_input_tables=" << compaction->input_tables << '\n';
      std::cout << "compaction_output_tables=" << compaction->output_tables << '\n';
      std::cout << "compaction_tombstones_dropped=" << compaction->tombstones_dropped << '\n';
    }
    std::cout << "entries=" << store.size() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV startup failed: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
