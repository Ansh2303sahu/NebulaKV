#include "nebulakv/network/bounded_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

namespace {

using nebulakv::network::BoundedExecutor;

TEST(BoundedExecutorTest, RejectsInvalidConfiguration) {
  EXPECT_THROW(BoundedExecutor(0U, 1U), std::invalid_argument);
  EXPECT_THROW(BoundedExecutor(1U, 0U), std::invalid_argument);
}

TEST(BoundedExecutorTest, ExecutesSubmittedTask) {
  BoundedExecutor executor{2U, 4U};
  auto future = executor.try_submit([] { return 42; });
  ASSERT_TRUE(future.has_value());
  EXPECT_EQ(future->get(), 42);

  const auto statistics = executor.statistics();
  EXPECT_EQ(statistics.submitted_tasks, 1U);
  EXPECT_EQ(statistics.completed_tasks, 1U);
  EXPECT_EQ(statistics.rejected_tasks, 0U);
}

TEST(BoundedExecutorTest, AppliesBackpressureWhenQueueIsFull) {
  BoundedExecutor executor{1U, 1U};
  std::promise<void> release_worker;
  std::shared_future<void> release = release_worker.get_future().share();
  std::promise<void> worker_started;

  auto running = executor.try_submit([&worker_started, release] {
    worker_started.set_value();
    release.wait();
    return 1;
  });
  ASSERT_TRUE(running.has_value());
  worker_started.get_future().wait();

  auto queued = executor.try_submit([] { return 2; });
  ASSERT_TRUE(queued.has_value());
  auto rejected = executor.try_submit([] { return 3; });
  EXPECT_FALSE(rejected.has_value());

  release_worker.set_value();
  EXPECT_EQ(running->get(), 1);
  EXPECT_EQ(queued->get(), 2);
  EXPECT_EQ(executor.statistics().rejected_tasks, 1U);
}

TEST(BoundedExecutorTest, ShutdownDrainsAcceptedTasks) {
  std::atomic<int> completed{0};
  BoundedExecutor executor{2U, 16U};
  for (int index = 0; index < 10; ++index) {
    auto future =
        executor.try_submit([&completed] { completed.fetch_add(1, std::memory_order_relaxed); });
    ASSERT_TRUE(future.has_value());
  }

  executor.shutdown(true);
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 10);
  EXPECT_FALSE(executor.try_submit([] {}).has_value());
}

} // namespace
