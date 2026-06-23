#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace nebulakv {

struct BloomFilterStatistics {
  std::size_t bit_count{0};
  std::size_t hash_count{0};
  std::size_t inserted_keys{0};
  std::size_t memory_bytes{0};
  double target_false_positive_rate{0.0};
};

class BloomFilter final {
public:
  BloomFilter(std::size_t expected_entries, double false_positive_rate);

  void add(std::string_view key);

  [[nodiscard]] bool may_contain(std::string_view key) const noexcept;
  [[nodiscard]] BloomFilterStatistics statistics() const noexcept;

private:
  [[nodiscard]] static std::uint64_t primary_hash(std::string_view key) noexcept;
  [[nodiscard]] static std::uint64_t secondary_hash(std::string_view key) noexcept;
  [[nodiscard]] std::size_t bit_index(std::uint64_t first, std::uint64_t second,
                                      std::size_t hash_index) const noexcept;

  std::vector<std::uint64_t> words_;
  std::size_t bit_count_{0};
  std::size_t hash_count_{0};
  std::size_t inserted_keys_{0};
  double target_false_positive_rate_{0.0};
};

} // namespace nebulakv
