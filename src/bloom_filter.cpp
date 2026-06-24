#include "nebulakv/bloom_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace nebulakv {
namespace {

inline constexpr double kNaturalLogOfTwo = 0.69314718055994530942;
inline constexpr std::size_t kBitsPerWord = 64U;
inline constexpr std::size_t kMaximumHashCount = 32U;

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value ^= value >> 30U;
  value *= 0xBF58476D1CE4E5B9ULL;
  value ^= value >> 27U;
  value *= 0x94D049BB133111EBULL;
  value ^= value >> 31U;
  return value;
}

[[nodiscard]] std::size_t checked_bit_count(const std::size_t expected_entries,
                                            const double false_positive_rate) {
  const double numerator =
      -static_cast<double>(expected_entries) * std::log(false_positive_rate);
  const double denominator = kNaturalLogOfTwo * kNaturalLogOfTwo;
  const double calculated = std::ceil(numerator / denominator);
  if (!std::isfinite(calculated) || calculated <= 0.0 ||
      calculated > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
    throw std::length_error{"Bloom filter size exceeds process limits"};
  }
  const auto bits = static_cast<std::size_t>(calculated);
  return std::max(bits, kBitsPerWord);
}

[[nodiscard]] std::size_t checked_hash_count(const std::size_t bit_count,
                                             const std::size_t expected_entries) {
  const double calculated = std::round(
      (static_cast<double>(bit_count) / static_cast<double>(expected_entries)) *
      kNaturalLogOfTwo);
  const auto hashes = static_cast<std::size_t>(std::max(1.0, calculated));
  return std::min(hashes, kMaximumHashCount);
}

}  // namespace

BloomFilter::BloomFilter(const std::size_t expected_entries,
                         const double false_positive_rate)
    : target_false_positive_rate_{false_positive_rate} {
  if (expected_entries == 0U) {
    throw std::invalid_argument{"Bloom filter expected entry count must be positive"};
  }
  if (!std::isfinite(false_positive_rate) || false_positive_rate <= 0.0 ||
      false_positive_rate >= 1.0) {
    throw std::invalid_argument{
        "Bloom filter false-positive rate must be between zero and one"};
  }

  bit_count_ = checked_bit_count(expected_entries, false_positive_rate);
  hash_count_ = checked_hash_count(bit_count_, expected_entries);
  const std::size_t word_count = (bit_count_ + kBitsPerWord - 1U) / kBitsPerWord;
  words_.assign(word_count, 0U);
  bit_count_ = word_count * kBitsPerWord;
}

void BloomFilter::add(const std::string_view key) {
  const std::uint64_t first = primary_hash(key);
  const std::uint64_t second = secondary_hash(key);
  for (std::size_t index = 0; index < hash_count_; ++index) {
    const std::size_t bit = bit_index(first, second, index);
    words_[bit / kBitsPerWord] |= std::uint64_t{1} << (bit % kBitsPerWord);
  }
  ++inserted_keys_;
}

bool BloomFilter::may_contain(const std::string_view key) const noexcept {
  const std::uint64_t first = primary_hash(key);
  const std::uint64_t second = secondary_hash(key);
  for (std::size_t index = 0; index < hash_count_; ++index) {
    const std::size_t bit = bit_index(first, second, index);
    if ((words_[bit / kBitsPerWord] &
         (std::uint64_t{1} << (bit % kBitsPerWord))) == 0U) {
      return false;
    }
  }
  return true;
}

BloomFilterStatistics BloomFilter::statistics() const noexcept {
  return BloomFilterStatistics{bit_count_, hash_count_, inserted_keys_,
                               words_.size() * sizeof(std::uint64_t),
                               target_false_positive_rate_};
}

std::uint64_t BloomFilter::primary_hash(const std::string_view key) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char character : key) {
    const auto byte = static_cast<unsigned char>(character);
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ULL;
  }
  return mix64(hash);
}

std::uint64_t BloomFilter::secondary_hash(const std::string_view key) noexcept {
  std::uint64_t hash = 0x9E3779B97F4A7C15ULL;
  for (const char character : key) {
    const auto byte = static_cast<unsigned char>(character);
    hash ^= static_cast<std::uint64_t>(byte) + 0x9E3779B97F4A7C15ULL +
            (hash << 6U) + (hash >> 2U);
  }
  hash = mix64(hash ^ 0xD6E8FEB86659FD93ULL);
  return hash == 0U ? 0xA0761D6478BD642FULL : hash;
}

std::size_t BloomFilter::bit_index(const std::uint64_t first,
                                   const std::uint64_t second,
                                   const std::size_t hash_index) const noexcept {
  const std::uint64_t combined =
      first + static_cast<std::uint64_t>(hash_index) * second;
  return static_cast<std::size_t>(combined % bit_count_);
}

}  // namespace nebulakv
