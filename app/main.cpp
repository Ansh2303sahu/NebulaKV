#include "nebulakv/in_memory_key_value_store.hpp"

#include <iostream>
#include <string>

int main() {
  nebulakv::InMemoryKeyValueStore store;
  store.put("project", "NebulaKV");
  store.put("phase", "in-memory engine");

  const auto project = store.get("project");
  const auto phase = store.get("phase");

  if (!project || !phase) {
    std::cerr << "Failed to read the demonstration keys\n";
    return 1;
  }

  std::cout << *project << ": " << *phase << '\n';
  std::cout << "entries=" << store.size() << '\n';
  return 0;
}
