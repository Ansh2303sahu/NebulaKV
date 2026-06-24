#include "nebulakv/network/request_processor.hpp"
#include "nebulakv/persistent_key_value_store.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using nebulakv::PersistentKeyValueStore;
using nebulakv::PersistentStoreOptions;
using nebulakv::network::ApiErrorCode;
using nebulakv::network::ExecutorStatistics;
using nebulakv::network::RequestProcessor;
using nebulakv::network::RequestProcessorOptions;
using nebulakv::network::ServiceStatistics;
using nebulakv::test::TemporaryDirectory;

[[nodiscard]] PersistentStoreOptions store_options(const TemporaryDirectory& directory) {
  PersistentStoreOptions options;
  options.wal_path = directory.file("nebulakv.wal");
  options.sstable_directory = directory.file("sstables");
  options.enable_automatic_compaction = false;
  return options;
}

TEST(RequestProcessorTest, SupportsPutGetAndDelete) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  RequestProcessor processor{store};

  const auto put = processor.put("user:1", "Ansh");
  ASSERT_TRUE(put.success);
  EXPECT_GT(put.sequence_number, 0U);

  const auto get = processor.get("user:1");
  ASSERT_TRUE(get.found);
  EXPECT_EQ(get.value, "Ansh");

  const auto removed = processor.remove("user:1");
  EXPECT_TRUE(removed.deleted);
  EXPECT_FALSE(processor.get("user:1").found);
}

TEST(RequestProcessorTest, MapsValidationErrors) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  RequestProcessor processor{store};

  const auto put = processor.put("", "value");
  EXPECT_FALSE(put.success);
  EXPECT_EQ(put.error.code, ApiErrorCode::InvalidArgument);

  const auto get = processor.get("");
  EXPECT_EQ(get.error.code, ApiErrorCode::InvalidArgument);

  const auto removed = processor.remove("");
  EXPECT_EQ(removed.error.code, ApiErrorCode::InvalidArgument);
}

TEST(RequestProcessorTest, ValidatesEntireBatchBeforeWriting) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  RequestProcessor processor{store};

  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back("valid", "value");
  entries.emplace_back("", "invalid");
  const auto result = processor.batch_put(std::move(entries));

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.writes_applied, 0U);
  EXPECT_EQ(result.error.code, ApiErrorCode::InvalidArgument);
  EXPECT_FALSE(store.exists("valid"));
}

TEST(RequestProcessorTest, AppliesValidBatch) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  RequestProcessor processor{store};

  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back("a", "1");
  entries.emplace_back("b", "2");
  entries.emplace_back("c", "3");
  const auto result = processor.batch_put(std::move(entries));

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.writes_applied, 3U);
  EXPECT_EQ(store.size(), 3U);
  EXPECT_EQ(store.get("b"), "2");
}

TEST(RequestProcessorTest, EnforcesBatchLimits) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  RequestProcessor processor{
      store, RequestProcessorOptions{.max_batch_entries = 2U, .max_batch_bytes = 8U}};

  std::vector<std::pair<std::string, std::string>> too_many{{"a", "1"}, {"b", "2"}, {"c", "3"}};
  EXPECT_EQ(processor.batch_put(std::move(too_many)).error.code, ApiErrorCode::InvalidArgument);

  std::vector<std::pair<std::string, std::string>> too_large{{"key", "123456"}};
  EXPECT_EQ(processor.batch_put(std::move(too_large)).error.code, ApiErrorCode::InvalidArgument);
}

TEST(RequestProcessorTest, ReportsStorageAndQueueStatus) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  RequestProcessor processor{store};
  ASSERT_TRUE(processor.put("key", "value").success);

  ExecutorStatistics executor;
  executor.queued_tasks = 3U;
  executor.active_tasks = 2U;
  executor.rejected_tasks = 5U;
  const ServiceStatistics service{11U, 1U};
  const auto status = processor.status(executor, service);

  EXPECT_TRUE(status.ready);
  EXPECT_EQ(status.live_keys, 1U);
  EXPECT_EQ(status.queued_requests, 3U);
  EXPECT_EQ(status.active_requests, 2U);
  EXPECT_EQ(status.rejected_requests, 5U);
  EXPECT_EQ(status.requests_total, 11U);
  EXPECT_EQ(status.failed_requests_total, 1U);
}

} // namespace
