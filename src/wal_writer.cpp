#include "nebulakv/wal_writer.hpp"

#include "wal_format.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace nebulakv {

namespace {

[[nodiscard]] int open_wal_file(const std::filesystem::path& path) {
  if (path.empty()) {
    throw std::invalid_argument{"WAL path must not be empty"};
  }

  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  const int file_descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (file_descriptor < 0) {
    throw std::system_error{errno, std::generic_category(), "failed to open WAL file"};
  }
  return file_descriptor;
}

void write_all(const int file_descriptor, const std::span<const std::byte> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const std::size_t remaining = bytes.size() - written;
    const ssize_t result = ::write(file_descriptor, bytes.data() + written, remaining);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error{errno, std::generic_category(), "failed to append WAL record"};
    }
    if (result == 0) {
      throw std::runtime_error{"WAL write returned zero bytes"};
    }
    written += static_cast<std::size_t>(result);
  }
}

void sync_file(const int file_descriptor) {
  while (::fsync(file_descriptor) != 0) {
    if (errno == EINTR) {
      continue;
    }
    throw std::system_error{errno, std::generic_category(), "failed to fsync WAL file"};
  }
}

} // namespace

WalWriter::WalWriter(WalWriterOptions options)
    : file_descriptor_{open_wal_file(options.path)}, durability_mode_{options.durability_mode},
      batch_flush_interval_{options.batch_flush_interval} {
  if (batch_flush_interval_ <= std::chrono::milliseconds::zero()) {
    ::close(file_descriptor_);
    file_descriptor_ = -1;
    throw std::invalid_argument{"batch flush interval must be greater than zero"};
  }

  if (durability_mode_ == DurabilityMode::Batch) {
    flush_thread_ = std::thread{[this] { batch_flush_loop(); }};
  }
}

WalWriter::~WalWriter() {
  if (durability_mode_ == DurabilityMode::Batch) {
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
    }
    flush_condition_.notify_all();
    if (flush_thread_.joinable()) {
      flush_thread_.join();
    }
  }

  {
    std::lock_guard lock{mutex_};
    if (dirty_ && durability_mode_ != DurabilityMode::None) {
      try {
        flush_locked();
      } catch (...) {
        // Destructors cannot report I/O failures. Explicit flush() propagates them.
      }
    }
  }

  if (file_descriptor_ >= 0) {
    ::close(file_descriptor_);
  }
}

void WalWriter::append(const WalRecord& record) {
  const std::vector<std::byte> encoded = wal_format::serialize(record);

  std::lock_guard lock{mutex_};
  throw_if_background_flush_failed_locked();
  write_all(file_descriptor_, encoded);
  bytes_appended_ += static_cast<std::uint64_t>(encoded.size());
  dirty_ = true;

  if (durability_mode_ == DurabilityMode::Sync) {
    flush_locked();
  } else if (durability_mode_ == DurabilityMode::Batch) {
    flush_condition_.notify_one();
  }
}

void WalWriter::flush() {
  std::lock_guard lock{mutex_};
  throw_if_background_flush_failed_locked();
  flush_locked();
}

void WalWriter::reset() {
  std::lock_guard lock{mutex_};
  throw_if_background_flush_failed_locked();
  flush_locked();
  while (::ftruncate(file_descriptor_, 0) != 0) {
    if (errno == EINTR) {
      continue;
    }
    throw std::system_error{errno, std::generic_category(), "failed to truncate WAL file"};
  }
  sync_file(file_descriptor_);
  bytes_appended_ = 0;
  dirty_ = false;
}

std::uint64_t WalWriter::bytes_appended() const {
  std::lock_guard lock{mutex_};
  return bytes_appended_;
}

DurabilityMode WalWriter::durability_mode() const noexcept { return durability_mode_; }

void WalWriter::batch_flush_loop() {
  std::unique_lock lock{mutex_};
  while (!stopping_) {
    flush_condition_.wait_for(lock, batch_flush_interval_, [this] { return stopping_; });
    if (!dirty_) {
      continue;
    }

    try {
      flush_locked();
    } catch (...) {
      background_flush_error_ = std::current_exception();
      stopping_ = true;
    }
  }

  if (dirty_ && !background_flush_error_) {
    try {
      flush_locked();
    } catch (...) {
      background_flush_error_ = std::current_exception();
    }
  }
}

void WalWriter::flush_locked() {
  if (!dirty_) {
    return;
  }
  sync_file(file_descriptor_);
  dirty_ = false;
}

void WalWriter::throw_if_background_flush_failed_locked() const {
  if (background_flush_error_) {
    std::rethrow_exception(background_flush_error_);
  }
}

} // namespace nebulakv
