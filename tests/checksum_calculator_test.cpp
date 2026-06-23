#include "nebulakv/checksum_calculator.hpp"

#include <array>
#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

TEST(ChecksumCalculatorTest, MatchesStandardCrc32Vector) {
  EXPECT_EQ(nebulakv::ChecksumCalculator::crc32("123456789"), 0xCBF43926U);
}

TEST(ChecksumCalculatorTest, EmptyInputHasZeroChecksum) {
  EXPECT_EQ(nebulakv::ChecksumCalculator::crc32(std::string_view{}), 0U);
}

TEST(ChecksumCalculatorTest, DetectsDifferentBinaryPayloads) {
  const std::array first{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
  const std::array second{std::byte{0x00}, std::byte{0x01}, std::byte{0x03}};

  EXPECT_NE(nebulakv::ChecksumCalculator::crc32(first),
            nebulakv::ChecksumCalculator::crc32(second));
}
