#pragma once

#include "nebulakv/key_value_store.hpp"

#include <cstddef>
#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nebulakv {

class InMemoryKeyValueStore final : public KeyValueStore {
public:
  static constexpr std::size_t kMaxKeySize = 1024;
  static constexpr std::size_t kMaxValueSize = 1024 * 1024;

  InMemoryKeyValueStore() = default;
  ~InMemoryKeyValueStore() override = default;

  InMemoryKeyValueStore(const InMemoryKeyValueStore&) = delete;
  InMemoryKeyValueStore& operator=(const InMemoryKeyValueStore&) = delete;
  InMemoryKeyValueStore(InMemoryKeyValueStore&&) = delete;
  InMemoryKeyValueStore& operator=(InMemoryKeyValueStore&&) = delete;

  void put(std::string key, std::string value) override;

  [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;

  bool remove(std::string_view key) override;

  [[nodiscard]] bool exists(std::string_view key) const override;

  [[nodiscard]] std::size_t size() const;

private:
  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept;
  };

  using Storage =
      std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>>;

  static void validate_key(std::string_view key);
  static void validate_value(std::string_view value);

  mutable std::shared_mutex mutex_;
  Storage entries_;
};

} // namespace nebulakv
