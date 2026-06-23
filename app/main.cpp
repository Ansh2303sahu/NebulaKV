#include "nebulakv/durability_mode.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

int main(const int argc, char** argv) {
  const std::filesystem::path wal_path =
      argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"data/nebulakv.wal"};
  const bool create_checkpoint = argc > 2 && std::string_view{argv[2]} == "--checkpoint";

  nebulakv::PersistentStoreOptions options;
  options.wal_path = wal_path;
  options.durability_mode = nebulakv::DurabilityMode::Sync;

  try {
    nebulakv::PersistentKeyValueStore store{options};
    store.put("project", "NebulaKV");
    store.put("storage", "WAL-backed MemTables, indexed SSTables, Bloom filters, and LRU cache");
    if (create_checkpoint) {
      store.checkpoint();
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
    std::cout << "durability=" << nebulakv::to_string(store.durability_mode()) << '\n';
    std::cout << "recovered_records=" << store.recovery_report().records_applied << '\n';
    std::cout << "last_sequence=" << store.last_sequence_number() << '\n';
    std::cout << "immutable_memtables=" << store.immutable_memtable_count() << '\n';
    const auto cache = store.block_cache_statistics();
    const auto bloom = store.bloom_filter_statistics();
    std::cout << "sstables=" << store.sstable_count() << '\n';
    std::cout << "bloom_filters=" << bloom.filter_count << '\n';
    std::cout << "bloom_memory_bytes=" << bloom.memory_bytes << '\n';
    std::cout << "cache_entries=" << cache.entry_count << '\n';
    std::cout << "cache_hit_ratio=" << cache.hit_ratio() << '\n';
    std::cout << "entries=" << store.size() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV startup failed: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
