#ifndef INDUSTRY_STARTER_CORE_HPP
#define INDUSTRY_STARTER_CORE_HPP
#include <span>

#include <cstdint>
#include <span>

namespace industry_starter {

[[nodiscard]] std::int64_t sum(std::span<const int> values) noexcept;

} // namespace industry_starter
#endif // INDUSTRY_STARTER_CORE_HPP
