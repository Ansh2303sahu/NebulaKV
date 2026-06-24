#include "nebulakv/durability_mode.hpp"
#include "nebulakv/wal_reader.hpp"
#include "nebulakv/wal_record.hpp"
#include "nebulakv/wal_writer.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using nebulakv::DurabilityMode;
using nebulakv::OperationType;
using nebulakv::WalReadIssueCode;
using nebulakv::WalReader;
using nebulakv::WalRecord;
using nebulakv::WalWriter;
using nebulakv::WalWriterOptions;
using nebulakv::test::TemporaryDirectory;

std::vector<WalRecord> read_records(const std::filesystem::path& path) {
  std::vector<WalRecord> records;
  const WalReader reader{path};
  const auto result = reader.scan([&records](const WalRecord& record) {
    records.push_back(record);
  });
  EXPECT_FALSE(result.issue.has_value());
  return records;
}

TEST(WalWriterReaderTest, RoundTripsPutRecord) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "name", "NebulaKV"});
  }

  const auto records = read_records(path);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].operation, OperationType::Put);
  EXPECT_EQ(records[0].key, "name");
  EXPECT_EQ(records[0].value, "NebulaKV");
}

TEST(WalWriterReaderTest, RoundTripsDeleteRecord) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Delete, "obsolete", {}});
  }

  const auto records = read_records(path);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].operation, OperationType::Delete);
  EXPECT_EQ(records[0].key, "obsolete");
  EXPECT_TRUE(records[0].value.empty());
}

TEST(WalWriterReaderTest, PreservesRecordOrder) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "one"});
    writer.append({OperationType::Put, "key", "two"});
    writer.append({OperationType::Delete, "key", {}});
  }

  const auto records = read_records(path);
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].value, "one");
  EXPECT_EQ(records[1].value, "two");
  EXPECT_EQ(records[2].operation, OperationType::Delete);
}

TEST(WalWriterReaderTest, PreservesEmbeddedNullBytes) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  const std::string key{"key\0suffix", 10};
  const std::string value{"value\0suffix", 12};
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, key, value});
  }

  const auto records = read_records(path);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].key, key);
  EXPECT_EQ(records[0].value, value);
}

TEST(WalWriterReaderTest, ReportsAppendedByteCount) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  WalWriter writer{{path, DurabilityMode::None, std::chrono::milliseconds{100}}};

  writer.append({OperationType::Put, "key", "value"});

  EXPECT_EQ(writer.bytes_appended(), std::filesystem::file_size(path));
}

TEST(WalWriterReaderTest, ExplicitFlushWorksForNoneMode) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  WalWriter writer{{path, DurabilityMode::None, std::chrono::milliseconds{100}}};
  writer.append({OperationType::Put, "key", "value"});

  EXPECT_NO_THROW(writer.flush());
  EXPECT_EQ(read_records(path).size(), 1U);
}

TEST(WalWriterReaderTest, BatchModeFlushesWithoutAdditionalWrites) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  WalWriter writer{{path, DurabilityMode::Batch, std::chrono::milliseconds{10}}};
  writer.append({OperationType::Put, "key", "value"});

  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  EXPECT_NO_THROW(writer.flush());
  EXPECT_EQ(read_records(path).size(), 1U);
}

TEST(WalWriterReaderTest, RejectsEmptyKey) {
  TemporaryDirectory directory;
  WalWriter writer{{directory.file("database.wal"), DurabilityMode::None,
                    std::chrono::milliseconds{100}}};

  EXPECT_THROW(writer.append({OperationType::Put, "", "value"}), std::invalid_argument);
}

TEST(WalWriterReaderTest, RejectsValueOnDeleteRecord) {
  TemporaryDirectory directory;
  WalWriter writer{{directory.file("database.wal"), DurabilityMode::None,
                    std::chrono::milliseconds{100}}};

  EXPECT_THROW(writer.append({OperationType::Delete, "key", "value"}),
               std::invalid_argument);
}

TEST(WalWriterReaderTest, CleanlyHandlesMissingFile) {
  TemporaryDirectory directory;
  const WalReader reader{directory.file("missing.wal")};

  const auto result = reader.scan([](const WalRecord&) {});

  EXPECT_EQ(result.records_read, 0U);
  EXPECT_FALSE(result.issue.has_value());
}

TEST(WalWriterReaderTest, StopsOnPartialHeader) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  nebulakv::test::write_file(path,
                             {std::byte{'N'}, std::byte{'K'}, std::byte{'V'}});

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::IncompleteRecord);
  EXPECT_EQ(result.issue->byte_offset, 0U);
}

TEST(WalWriterReaderTest, DetectsInvalidMagic) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 0, std::byte{'X'});

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::InvalidMagic);
}

TEST(WalWriterReaderTest, DetectsUnsupportedVersion) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 4, std::byte{2});

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::UnsupportedVersion);
}

TEST(WalWriterReaderTest, DetectsInvalidOperation) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 6, std::byte{99});

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::InvalidOperation);
}

TEST(WalWriterReaderTest, DetectsOversizedKeyLengthBeforeAllocation) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 8, std::byte{1});
  nebulakv::test::overwrite_byte(path, 9, std::byte{4});

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::InvalidRecordLength);
}

TEST(WalWriterReaderTest, StopsOnPartialPayload) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 2U);

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::IncompleteRecord);
}

TEST(WalWriterReaderTest, DetectsChecksumMismatch) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 16, std::byte{'x'});

  const WalReader reader{path};
  const auto result = reader.scan([](const WalRecord&) {});

  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::ChecksumMismatch);
  EXPECT_EQ(result.issue->byte_offset, 0U);
}

TEST(WalWriterReaderTest, ReportsOffsetOfCorruptedLaterRecord) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  std::uint64_t first_record_size = 0;
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "first", "one"});
    first_record_size = writer.bytes_appended();
    writer.append({OperationType::Put, "second", "two"});
  }
  nebulakv::test::overwrite_byte(path, first_record_size + 16U, std::byte{'x'});

  std::vector<WalRecord> records;
  const WalReader reader{path};
  const auto result = reader.scan([&records](const WalRecord& record) {
    records.push_back(record);
  });

  ASSERT_EQ(records.size(), 1U);
  ASSERT_TRUE(result.issue.has_value());
  EXPECT_EQ(result.issue->code, WalReadIssueCode::ChecksumMismatch);
  EXPECT_EQ(result.issue->byte_offset, first_record_size);
  EXPECT_EQ(result.valid_bytes, first_record_size);
}

}  // namespace
