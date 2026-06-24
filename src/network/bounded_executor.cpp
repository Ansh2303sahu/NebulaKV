#include "nebulakv/network/bounded_executor.hpp"

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>

namespace nebulakv::network {

BoundedExecutor::BoundedExecutor(const std::size_t worker_count,
                                 const std::size_t queue_capacity)
    : worker_count_{worker_count}, queue_capacity_{queue_capacity} {
  if (worker_count == 0U) {
    throw std::invalid_argument{"worker count must be greater than zero"};
  }
  if (queue_capacity == 0U) {
    throw std::invalid_argument{"queue capacity must be greater than zero"};
  }

  workers_.reserve(worker_count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

BoundedExecutor::~BoundedExecutor() { shutdown(true); }

void BoundedExecutor::shutdown(const bool drain) {
  {
    std::lock_guard lock{mutex_};
    if (stopping_) {
      return;
    }

    accepting_ = false;
    stopping_ = true;
    drain_on_shutdown_ = drain;
    if (!drain_on_shutdown_) {
      tasks_.clear();
    }
  }

  task_available_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

ExecutorStatistics BoundedExecutor::statistics() const {
  ExecutorStatistics result;
  result.worker_count = worker_count_;
  result.queue_capacity = queue_capacity_;
  {
    std::lock_guard lock{mutex_};
    result.queued_tasks = tasks_.size();
  }
  result.active_tasks = active_tasks_.load(std::memory_order_relaxed);
  result.submitted_tasks = submitted_tasks_.load(std::memory_order_relaxed);
  result.completed_tasks = completed_tasks_.load(std::memory_order_relaxed);
  result.rejected_tasks = rejected_tasks_.load(std::memory_order_relaxed);
  return result;
}

void BoundedExecutor::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock{mutex_};
      task_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

      if (tasks_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }

      if (stopping_ && !drain_on_shutdown_) {
        return;
      }

      task = std::move(tasks_.front());
      tasks_.pop_front();
      active_tasks_.fetch_add(1U, std::memory_order_relaxed);
    }

    task();
    active_tasks_.fetch_sub(1U, std::memory_order_relaxed);
    completed_tasks_.fetch_add(1U, std::memory_order_relaxed);
  }
}

}  // namespace nebulakv::network
