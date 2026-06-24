#pragma once

#include <string_view>

namespace nebulakv {

void validate_key(std::string_view key);
void validate_value(std::string_view value);

} // namespace nebulakv
