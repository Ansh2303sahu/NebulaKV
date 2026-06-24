#include "nebulakv/raft/storage.hpp"

#include "nebulakv/checksum_calculator.hpp"
#include "serialization.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nebulakv::raft {

namespace {

constexpr std::string_view kHardMagic{"NBHR"};
constexpr std::string_view kLogMagic{"NBRL"};
constexpr std::string_view kSnapshotMagic{"NBRS"};
constexpr std::uint32_t kVersion = 1U;

[[noreturn]] void throw_system_error(const std::string& operation,
                                     const std::filesystem::path& path) {
  throw std::system_error{errno, std::generic_category(), operation + ": " + path.string()};
}

void write_all(const int descriptor, const std::string_view data,
               const std::filesystem::path& path) {
  std::size_t written = 0U;
  while (written < data.size()) {
    const auto result = ::write(descriptor, data.data() + written, data.size() - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_system_error("failed to write Raft file", path);
    }
    written += static_cast<std::size_t>(result);
  }
}

void sync_directory(const std::filesystem::path& directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    throw_system_error("failed to open Raft directory", directory);
  }
  if (::fsync(descriptor) != 0) {
    const int saved_errno = errno;
    ::close(descriptor);
    errno = saved_errno;
    throw_system_error("failed to synchronize Raft directory", directory);
  }
  if (::close(descriptor) != 0) {
    throw_system_error("failed to close Raft directory", directory);
  }
}

void atomic_write(const std::filesystem::path& path, const std::string_view data) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  const int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP);
  if (descriptor < 0) {
    throw_system_error("failed to create temporary Raft file", temporary);
  }

  try {
    write_all(descriptor, data, temporary);
    if (::fsync(descriptor) != 0) {
      throw_system_error("failed to synchronize Raft file", temporary);
    }
    if (::close(descriptor) != 0) {
      throw_system_error("failed to close Raft file", temporary);
    }
  } catch (...) {
    ::close(descriptor);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    std::filesystem::remove(temporary);
    throw std::filesystem::filesystem_error{"failed to publish Raft file", temporary, path,
                                            rename_error};
  }
  sync_directory(path.parent_path());
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  const auto file_size = std::filesystem::file_size(path);
  if (file_size > static_cast<std::uintmax_t>(SIZE_MAX)) {
    throw std::length_error{"Raft file is too large: " + path.string()};
  }
  std::string contents(static_cast<std::size_t>(file_size), '\0');
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw std::runtime_error{"failed to open Raft file: " + path.string()};
  }
  if (!contents.empty()) {
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
      throw std::runtime_error{"failed to read complete Raft file: " + path.string()};
    }
  }
  return contents;
}

[[nodiscard]] std::string wrap_payload(const std::string_view magic,
                                       const std::string_view payload) {
  std::string encoded;
  encoded.reserve(magic.size() + 4U + payload.size() + 4U);
  encoded.append(magic);
  detail::append_u32(encoded, kVersion);
  encoded.append(payload);
  detail::append_u32(encoded, ChecksumCalculator::crc32(payload));
  return encoded;
}

[[nodiscard]] std::string unwrap_payload(const std::filesystem::path& path,
                                         const std::string_view expected_magic) {
  const std::string encoded = read_file(path);
  constexpr std::size_t kEnvelopeBytes = 4U + 4U + 4U;
  if (encoded.size() < kEnvelopeBytes) {
    throw std::runtime_error{"truncated Raft file: " + path.string()};
  }
  if (std::string_view{encoded}.substr(0U, 4U) != expected_magic) {
    throw std::runtime_error{"invalid Raft file magic: " + path.string()};
  }
  detail::Decoder header{std::string_view{encoded}.substr(4U, 4U)};
  if (header.read_u32() != kVersion) {
    throw std::runtime_error{"unsupported Raft file version: " + path.string()};
  }
  const std::string_view payload{encoded.data() + 8U, encoded.size() - kEnvelopeBytes};
  detail::Decoder checksum_decoder{std::string_view{encoded}.substr(encoded.size() - 4U)};
  const std::uint32_t stored_checksum = checksum_decoder.read_u32();
  if (stored_checksum != ChecksumCalculator::crc32(payload)) {
    throw std::runtime_error{"Raft file checksum mismatch: " + path.string()};
  }
  return std::string{payload};
}

[[nodiscard]] HardState decode_hard_state(const std::string_view payload) {
  detail::Decoder decoder{payload};
  HardState state;
  state.current_term = decoder.read_u64();
  state.voted_for = decoder.read_bytes();
  state.commit_index = decoder.read_u64();
  state.last_applied = decoder.read_u64();
  if (!decoder.empty()) {
    throw std::runtime_error{"unexpected bytes in Raft hard state"};
  }
  return state;
}

[[nodiscard]] std::vector<LogEntry> decode_log(const std::string_view payload) {
  detail::Decoder decoder{payload};
  const auto count = static_cast<std::size_t>(decoder.read_u64());
  std::vector<LogEntry> entries;
  entries.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    entries.push_back(detail::decode_entry(decoder));
  }
  if (!decoder.empty()) {
    throw std::runtime_error{"unexpected bytes in Raft log"};
  }
  for (std::size_t index = 1U; index < entries.size(); ++index) {
    if (entries[index].index != entries[index - 1U].index + 1U) {
      throw std::runtime_error{"Raft log indices are not contiguous"};
    }
  }
  return entries;
}

[[nodiscard]] Snapshot decode_snapshot(const std::string_view payload) {
  detail::Decoder decoder{payload};
  Snapshot snapshot;
  snapshot.last_included_index = decoder.read_u64();
  snapshot.last_included_term = decoder.read_u64();
  snapshot.payload = decoder.read_bytes();
  if (!decoder.empty()) {
    throw std::runtime_error{"unexpected bytes in Raft snapshot"};
  }
  return snapshot;
}

} // namespace

RaftStorage::RaftStorage(std::filesystem::path directory)
    : directory_{std::move(directory)}, hard_state_path_{directory_ / "hard-state.bin"},
      log_path_{directory_ / "raft-log.bin"}, snapshot_path_{directory_ / "snapshot.bin"} {
  if (directory_.empty()) {
    throw std::invalid_argument{"Raft storage directory must not be empty"};
  }
  std::filesystem::create_directories(directory_);
}

PersistentState RaftStorage::load() const {
  std::lock_guard lock{mutex_};
  PersistentState state;
  if (std::filesystem::exists(hard_state_path_)) {
    state.hard_state = decode_hard_state(unwrap_payload(hard_state_path_, kHardMagic));
  }
  if (std::filesystem::exists(log_path_)) {
    state.log = decode_log(unwrap_payload(log_path_, kLogMagic));
  }
  if (std::filesystem::exists(snapshot_path_)) {
    state.snapshot = decode_snapshot(unwrap_payload(snapshot_path_, kSnapshotMagic));
  }
  return state;
}

void RaftStorage::save_hard_state(const HardState& state) {
  std::lock_guard lock{mutex_};
  std::string payload;
  detail::append_u64(payload, state.current_term);
  detail::append_bytes(payload, state.voted_for);
  detail::append_u64(payload, state.commit_index);
  detail::append_u64(payload, state.last_applied);
  atomic_write(hard_state_path_, wrap_payload(kHardMagic, payload));
}

void RaftStorage::replace_log(const std::vector<LogEntry>& entries) {
  std::lock_guard lock{mutex_};
  std::string payload;
  detail::append_u64(payload, static_cast<std::uint64_t>(entries.size()));
  for (const auto& entry : entries) {
    detail::encode_entry(payload, entry);
  }
  atomic_write(log_path_, wrap_payload(kLogMagic, payload));
}

void RaftStorage::save_snapshot(const Snapshot& snapshot) {
  if (snapshot.last_included_index == 0U) {
    throw std::invalid_argument{"snapshot index must be greater than zero"};
  }
  std::lock_guard lock{mutex_};
  std::string payload;
  detail::append_u64(payload, snapshot.last_included_index);
  detail::append_u64(payload, snapshot.last_included_term);
  detail::append_bytes(payload, snapshot.payload);
  atomic_write(snapshot_path_, wrap_payload(kSnapshotMagic, payload));
}

const std::filesystem::path& RaftStorage::directory() const noexcept { return directory_; }

const std::filesystem::path& RaftStorage::hard_state_path() const noexcept {
  return hard_state_path_;
}

const std::filesystem::path& RaftStorage::log_path() const noexcept { return log_path_; }

const std::filesystem::path& RaftStorage::snapshot_path() const noexcept { return snapshot_path_; }

} // namespace nebulakv::raft
