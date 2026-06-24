#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace nebulakv::network {

struct ExecutorStatistics {
  std::size_t worker_count{0};
  std::size_t queue_capacity{0};
  std::size_t queued_tasks{0};
  std::size_t active_tasks{0};
  std::uint64_t submitted_tasks{0};
  std::uint64_t completed_tasks{0};
  std::uint64_t rejected_tasks{0};
};

class BoundedExecutor final {
public:
  BoundedExecutor(std::size_t worker_count, std::size_t queue_capacity);
  ~BoundedExecutor();

  BoundedExecutor(const BoundedExecutor&) = delete;
  BoundedExecutor& operator=(const BoundedExecutor&) = delete;
  BoundedExecutor(BoundedExecutor&&) = delete;
  BoundedExecutor& operator=(BoundedExecutor&&) = delete;

  template <typename Function>
  [[nodiscard]] auto
  try_submit(Function&& function) -> std::optional<std::future<std::invoke_result_t<Function>>> {
    using Result = std::invoke_result_t<Function>;

    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
    std::future<Result> future = task->get_future();

    {
      std::lock_guard lock{mutex_};
      if (!accepting_ || tasks_.size() >= queue_capacity_) {
        rejected_tasks_.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
      }

      tasks_.emplace_back([task] { (*task)(); });
      submitted_tasks_.fetch_add(1U, std::memory_order_relaxed);
    }

    task_available_.notify_one();
    return std::optional<std::future<Result>>{std::move(future)};
  }

  void shutdown(bool drain = true);
  [[nodiscard]] ExecutorStatistics statistics() const;

private:
  void worker_loop();

  const std::size_t worker_count_{0};
  const std::size_t queue_capacity_{0};
  mutable std::mutex mutex_;
  std::condition_variable task_available_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool accepting_{true};
  bool stopping_{false};
  bool drain_on_shutdown_{true};
  std::atomic<std::size_t> active_tasks_{0};
  std::atomic<std::uint64_t> submitted_tasks_{0};
  std::atomic<std::uint64_t> completed_tasks_{0};
  std::atomic<std::uint64_t> rejected_tasks_{0};
};

} // namespace nebulakv::network
