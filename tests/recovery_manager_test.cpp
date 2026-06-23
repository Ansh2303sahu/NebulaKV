#include "nebulakv/durability_mode.hpp"
#include "nebulakv/in_memory_key_value_store.hpp"
#include "nebulakv/recovery_manager.hpp"
#include "nebulakv/wal_record.hpp"
#include "nebulakv/wal_writer.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

using nebulakv::DurabilityMode;
using nebulakv::InMemoryKeyValueStore;
using nebulakv::OperationType;
using nebulakv::RecoveryManager;
using nebulakv::RecoveryOptions;
using nebulakv::WalReadIssueCode;
using nebulakv::WalWriter;
using nebulakv::test::TemporaryDirectory;

TEST(RecoveryManagerTest, ReplaysPutsUpdatesAndDeletes) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "first", "one"});
    writer.append({OperationType::Put, "first", "updated"});
    writer.append({OperationType::Put, "second", "two"});
    writer.append({OperationType::Delete, "second", {}});
  }

  InMemoryKeyValueStore destination;
  const auto report = RecoveryManager::recover(path, destination);

  EXPECT_TRUE(report.completed_cleanly());
  EXPECT_EQ(report.records_applied, 4U);
  EXPECT_EQ(report.puts_applied, 3U);
  EXPECT_EQ(report.deletes_applied, 1U);
  EXPECT_EQ(destination.get("first"), "updated");
  EXPECT_FALSE(destination.exists("second"));
}

TEST(RecoveryManagerTest, StopsAtIncompleteTailAndKeepsValidRecords) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  std::uint64_t valid_size = 0;
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "first", "one"});
    valid_size = writer.bytes_appended();
    writer.append({OperationType::Put, "second", "two"});
  }
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3U);

  InMemoryKeyValueStore destination;
  const auto report = RecoveryManager::recover(path, destination);

  ASSERT_TRUE(report.issue.has_value());
  EXPECT_EQ(report.issue->code, WalReadIssueCode::IncompleteRecord);
  EXPECT_EQ(report.issue->byte_offset, valid_size);
  EXPECT_EQ(destination.get("first"), "one");
  EXPECT_FALSE(destination.exists("second"));
  EXPECT_TRUE(report.invalid_tail_truncated);
  EXPECT_EQ(std::filesystem::file_size(path), valid_size);
}

TEST(RecoveryManagerTest, DetectsChecksumMismatchAndTruncatesInvalidTail) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  std::uint64_t valid_size = 0;
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "first", "one"});
    valid_size = writer.bytes_appended();
    writer.append({OperationType::Put, "second", "two"});
  }
  nebulakv::test::overwrite_byte(path, valid_size + 16U, std::byte{'x'});

  InMemoryKeyValueStore destination;
  const auto report = RecoveryManager::recover(path, destination);

  ASSERT_TRUE(report.issue.has_value());
  EXPECT_EQ(report.issue->code, WalReadIssueCode::ChecksumMismatch);
  EXPECT_EQ(report.issue->byte_offset, valid_size);
  EXPECT_EQ(destination.get("first"), "one");
  EXPECT_FALSE(destination.exists("second"));
  EXPECT_TRUE(report.invalid_tail_truncated);
  EXPECT_EQ(std::filesystem::file_size(path), valid_size);
}

TEST(RecoveryManagerTest, CanPreserveInvalidTailForForensics) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 16, std::byte{'x'});
  const auto original_size = std::filesystem::file_size(path);

  InMemoryKeyValueStore destination;
  RecoveryOptions options;
  options.truncate_invalid_tail = false;
  const auto report = RecoveryManager::recover(path, destination, options);

  EXPECT_TRUE(report.issue.has_value());
  EXPECT_FALSE(report.invalid_tail_truncated);
  EXPECT_EQ(std::filesystem::file_size(path), original_size);
}

TEST(RecoveryManagerTest, StructuredDiagnosticIncludesCorruptedOffset) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    WalWriter writer{{path, DurabilityMode::Sync, std::chrono::milliseconds{100}}};
    writer.append({OperationType::Put, "key", "value"});
  }
  nebulakv::test::overwrite_byte(path, 16, std::byte{'x'});

  std::ostringstream diagnostics;
  InMemoryKeyValueStore destination;
  RecoveryOptions options;
  options.diagnostics = &diagnostics;
  static_cast<void>(RecoveryManager::recover(path, destination, options));

  EXPECT_NE(diagnostics.str().find("\"offset\":0"), std::string::npos);
  EXPECT_NE(diagnostics.str().find("checksum_mismatch"), std::string::npos);
}

TEST(RecoveryManagerTest, MissingWalProducesEmptyCleanReport) {
  TemporaryDirectory directory;
  InMemoryKeyValueStore destination;

  const auto report = RecoveryManager::recover(directory.file("missing.wal"), destination);

  EXPECT_TRUE(report.completed_cleanly());
  EXPECT_EQ(report.records_applied, 0U);
  EXPECT_EQ(destination.size(), 0U);
}

} // namespace
