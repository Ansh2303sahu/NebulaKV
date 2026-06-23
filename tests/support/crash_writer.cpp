#include "nebulakv/durability_mode.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>

int main(const int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }

  nebulakv::PersistentStoreOptions options;
  options.wal_path = std::filesystem::path{argv[1]};
  options.durability_mode = nebulakv::DurabilityMode::Sync;
  options.emit_recovery_diagnostics = false;
  nebulakv::PersistentKeyValueStore store{options};

  for (std::size_t index = 0; index < 1000U; ++index) {
    store.put("key-" + std::to_string(index), "value-" + std::to_string(index));
  }

  std::_Exit(EXIT_SUCCESS);
}
