#include "nebulakv/wal_reader.hpp"

#include "nebulakv/checksum_calculator.hpp"
#include "nebulakv/storage_limits.hpp"
#include "wal_format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace nebulakv {

namespace {

struct ReadResult {
  std::size_t bytes_read{0};
  bool io_error{false};
};

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value) : value_{value} {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&&) = delete;
  FileDescriptor& operator=(FileDescriptor&&) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_{-1};
};

[[nodiscard]] ReadResult read_exact(const int file_descriptor,
                                    const std::span<std::byte> destination) {
  ReadResult result;
  while (result.bytes_read < destination.size()) {
    const std::size_t remaining = destination.size() - result.bytes_read;
    const ssize_t count =
        ::read(file_descriptor, destination.data() + result.bytes_read, remaining);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.io_error = true;
      return result;
    }
    if (count == 0) {
      return result;
    }
    result.bytes_read += static_cast<std::size_t>(count);
  }
  return result;
}

[[nodiscard]] WalScanResult make_issue(const WalScanResult& current, const WalReadIssueCode code,
                                       const std::uint64_t offset) {
  WalScanResult result = current;
  result.issue = WalReadIssue{code, offset};
  return result;
}

[[nodiscard]] bool has_valid_magic(const std::array<std::byte, wal_format::kHeaderSize>& header) {
  return std::equal(wal_format::kMagic.begin(), wal_format::kMagic.end(), header.begin());
}

} // namespace

std::string_view to_string(const WalReadIssueCode code) noexcept {
  switch (code) {
  case WalReadIssueCode::IncompleteRecord:
    return "incomplete_record";
  case WalReadIssueCode::InvalidMagic:
    return "invalid_magic";
  case WalReadIssueCode::UnsupportedVersion:
    return "unsupported_version";
  case WalReadIssueCode::InvalidOperation:
    return "invalid_operation";
  case WalReadIssueCode::InvalidRecordLength:
    return "invalid_record_length";
  case WalReadIssueCode::InvalidDeleteRecord:
    return "invalid_delete_record";
  case WalReadIssueCode::ChecksumMismatch:
    return "checksum_mismatch";
  case WalReadIssueCode::IoError:
    return "io_error";
  }

  return "unknown";
}

WalReader::WalReader(std::filesystem::path path) : path_{std::move(path)} {}

WalScanResult WalReader::scan(const RecordVisitor& visitor) const {
  WalScanResult result;
  const int raw_file_descriptor = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (raw_file_descriptor < 0) {
    if (errno == ENOENT) {
      return result;
    }
    return make_issue(result, WalReadIssueCode::IoError, 0);
  }
  const FileDescriptor file_descriptor{raw_file_descriptor};

  while (true) {
    const std::uint64_t record_offset = result.valid_bytes;
    std::array<std::byte, wal_format::kHeaderSize> header{};
    const ReadResult header_read = read_exact(file_descriptor.get(), header);
    if (header_read.io_error) {
      return make_issue(result, WalReadIssueCode::IoError, record_offset);
    }
    if (header_read.bytes_read == 0U) {
      return result;
    }
    if (header_read.bytes_read != header.size()) {
      return make_issue(result, WalReadIssueCode::IncompleteRecord, record_offset);
    }

    if (!has_valid_magic(header)) {
      return make_issue(result, WalReadIssueCode::InvalidMagic, record_offset);
    }

    const std::uint16_t version =
        wal_format::read_uint16_le(std::span<const std::byte>{header}.subspan(4U, 2U));
    if (version != wal_format::kVersion) {
      return make_issue(result, WalReadIssueCode::UnsupportedVersion, record_offset);
    }

    const auto operation_value = std::to_integer<std::uint8_t>(header[6]);
    OperationType operation{};
    if (operation_value == static_cast<std::uint8_t>(OperationType::Put)) {
      operation = OperationType::Put;
    } else if (operation_value == static_cast<std::uint8_t>(OperationType::Delete)) {
      operation = OperationType::Delete;
    } else {
      return make_issue(result, WalReadIssueCode::InvalidOperation, record_offset);
    }

    if (header[7] != std::byte{0}) {
      return make_issue(result, WalReadIssueCode::InvalidRecordLength, record_offset);
    }

    const std::uint32_t key_size_value =
        wal_format::read_uint32_le(std::span<const std::byte>{header}.subspan(8U, 4U));
    const std::uint32_t value_size_value =
        wal_format::read_uint32_le(std::span<const std::byte>{header}.subspan(12U, 4U));
    const std::size_t key_size = static_cast<std::size_t>(key_size_value);
    const std::size_t value_size = static_cast<std::size_t>(value_size_value);

    if (key_size == 0U || key_size > storage_limits::kMaxKeySize ||
        value_size > storage_limits::kMaxValueSize) {
      return make_issue(result, WalReadIssueCode::InvalidRecordLength, record_offset);
    }
    if (operation == OperationType::Delete && value_size != 0U) {
      return make_issue(result, WalReadIssueCode::InvalidDeleteRecord, record_offset);
    }

    std::vector<std::byte> payload(key_size + value_size);
    const ReadResult payload_read = read_exact(file_descriptor.get(), payload);
    if (payload_read.io_error) {
      return make_issue(result, WalReadIssueCode::IoError, record_offset);
    }
    if (payload_read.bytes_read != payload.size()) {
      return make_issue(result, WalReadIssueCode::IncompleteRecord, record_offset);
    }

    std::array<std::byte, wal_format::kChecksumSize> checksum_bytes{};
    const ReadResult checksum_read = read_exact(file_descriptor.get(), checksum_bytes);
    if (checksum_read.io_error) {
      return make_issue(result, WalReadIssueCode::IoError, record_offset);
    }
    if (checksum_read.bytes_read != checksum_bytes.size()) {
      return make_issue(result, WalReadIssueCode::IncompleteRecord, record_offset);
    }

    std::vector<std::byte> checksum_input;
    checksum_input.reserve(header.size() + payload.size());
    checksum_input.insert(checksum_input.end(), header.begin(), header.end());
    checksum_input.insert(checksum_input.end(), payload.begin(), payload.end());

    const std::uint32_t expected_checksum =
        wal_format::read_uint32_le(std::span<const std::byte>{checksum_bytes});
    const std::uint32_t actual_checksum = ChecksumCalculator::crc32(checksum_input);
    if (actual_checksum != expected_checksum) {
      return make_issue(result, WalReadIssueCode::ChecksumMismatch, record_offset);
    }

    const auto* payload_characters = reinterpret_cast<const char*>(payload.data());
    WalRecord record;
    record.operation = operation;
    record.key.assign(payload_characters, key_size);
    record.value.assign(payload_characters + key_size, value_size);
    visitor(record);

    ++result.records_read;
    result.valid_bytes +=
        static_cast<std::uint64_t>(header.size() + payload.size() + checksum_bytes.size());
  }
}

} // namespace nebulakv
