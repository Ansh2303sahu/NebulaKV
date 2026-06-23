#include "nebulakv/durability_mode.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(const int argc, char** argv) {
  const std::filesystem::path wal_path =
      argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"data/nebulakv.wal"};

  nebulakv::PersistentStoreOptions options;
  options.wal_path = wal_path;
  options.durability_mode = nebulakv::DurabilityMode::Sync;

  try {
    nebulakv::PersistentKeyValueStore store{options};
    store.put("project", "NebulaKV");
    store.put("storage", "checksummed write-ahead log");

    const auto project = store.get("project");
    const auto storage = store.get("storage");
    if (!project || !storage) {
      std::cerr << "Failed to read the demonstration keys\n";
      return 1;
    }

    std::cout << *project << ": " << *storage << '\n';
    std::cout << "wal=" << wal_path << '\n';
    std::cout << "durability=" << nebulakv::to_string(store.durability_mode()) << '\n';
    std::cout << "recovered_records=" << store.recovery_report().records_applied << '\n';
    std::cout << "entries=" << store.size() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV startup failed: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
