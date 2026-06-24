#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nebulakv {

class KeyValueStore {
public:
  virtual ~KeyValueStore() = default;

  virtual void put(std::string key, std::string value) = 0;

  [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) const = 0;

  virtual bool remove(std::string_view key) = 0;

  [[nodiscard]] virtual bool exists(std::string_view key) const = 0;
};

} // namespace nebulakv
