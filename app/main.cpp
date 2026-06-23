#include "industry_starter/core.hpp"

#include <array>
#include <iostream>

int main() {
  constexpr std::array values{1, 2, 3, 4, 5};
  std::cout << "sum=" << industry_starter::sum(values) << '\n';
  return 0;
}
