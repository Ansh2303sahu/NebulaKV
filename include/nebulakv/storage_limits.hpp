#pragma once

#include <cstddef>

namespace nebulakv::storage_limits {

inline constexpr std::size_t kMaxKeySize = 1024;
inline constexpr std::size_t kMaxValueSize = 1024 * 1024;

} // namespace nebulakv::storage_limits
