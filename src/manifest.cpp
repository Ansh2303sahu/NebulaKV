#include "nebulakv/manifest.hpp"

#include "nebulakv/checksum_calculator.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace nebulakv {
namespace {

constexpr std::array<std::byte, 8> kManifestMagic{std::byte{'N'}, std::byte{'B'}, std::byte{'L'},
                                                  std::byte{'M'}, std::byte{'A'}, std::byte{'N'},
                                                  std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::byte, 8> kCurrentMagic{std::byte{'N'}, std::byte{'B'}, std::byte{'L'},
                                                 std::byte{'C'}, std::byte{'U'}, std::byte{'R'},
                                                 std::byte{'0'}, std::byte{'1'}};
constexpr std::uint16_t kManifestVersion = 1;
constexpr std::size_t kMaximumMetadataFileBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumManifestTables = 1U << 20U;
constexpr std::size_t kMaximumFilenameBytes = 4096U;

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value) noexcept : value_{value} {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] int release() noexcept {
    const int value = value_;
    value_ = -1;
    return value;
  }

private:
  int value_{-1};
};

void append_uint8(std::vector<std::byte>& output, const std::uint8_t value) {
  output.push_back(static_cast<std::byte>(value));
}

void append_uint16(std::vector<std::byte>& output, const std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value & 0xFFU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_uint32(std::vector<std::byte>& output, const std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_uint64(std::vector<std::byte>& output, const std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_bytes(std::vector<std::byte>& output, const std::string_view value) {
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  output.insert(output.end(), begin, begin + value.size());
}

class Decoder final {
public:
  explicit Decoder(const std::span<const std::byte> bytes) : bytes_{bytes} {}

  [[nodiscard]] std::uint8_t read_uint8() {
    require(1U);
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
  }

  [[nodiscard]] std::uint16_t read_uint16() {
    require(2U);
    const std::uint16_t value =
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes_[offset_])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 1U])) << 8U);
    offset_ += 2U;
    return value;
  }

  [[nodiscard]] std::uint32_t read_uint32() {
    require(4U);
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
               << (index * 8U);
    }
    offset_ += 4U;
    return value;
  }

  [[nodiscard]] std::uint64_t read_uint64() {
    require(8U);
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
               << (index * 8U);
    }
    offset_ += 8U;
    return value;
  }

  [[nodiscard]] std::string read_string(const std::size_t size) {
    require(size);
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
    std::string result{begin, size};
    offset_ += size;
    return result;
  }

  void expect_magic(const std::span<const std::byte> magic) {
    require(magic.size());
    if (!std::equal(magic.begin(), magic.end(),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset_))) {
      throw std::runtime_error{"metadata file has an invalid magic value"};
    }
    offset_ += magic.size();
  }

  [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

private:
  void require(const std::size_t count) const {
    if (count > bytes_.size() - offset_) {
      throw std::runtime_error{"metadata file is truncated"};
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_{0};
};

[[nodiscard]] std::uint32_t checked_uint32(const std::size_t value, const char* context) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error{context};
  }
  return static_cast<std::uint32_t>(value);
}

void append_checksum(std::vector<std::byte>& bytes) {
  append_uint32(bytes, ChecksumCalculator::crc32(std::span<const std::byte>{bytes}));
}

[[nodiscard]] std::span<const std::byte> verified_payload(const std::vector<std::byte>& bytes) {
  if (bytes.size() < sizeof(std::uint32_t)) {
    throw std::runtime_error{"metadata file is too small"};
  }
  const std::size_t payload_size = bytes.size() - sizeof(std::uint32_t);
  Decoder checksum_decoder{std::span<const std::byte>{bytes}.subspan(payload_size)};
  const std::uint32_t expected = checksum_decoder.read_uint32();
  const auto payload = std::span<const std::byte>{bytes}.first(payload_size);
  if (ChecksumCalculator::crc32(payload) != expected) {
    throw std::runtime_error{"metadata checksum mismatch"};
  }
  return payload;
}

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    throw std::runtime_error{"failed to open metadata file: " + path.string()};
  }
  const std::streamsize length = input.tellg();
  if (length < 0) {
    throw std::runtime_error{"failed to determine metadata file size"};
  }
  const auto unsigned_length = static_cast<std::uintmax_t>(length);
  if (unsigned_length > kMaximumMetadataFileBytes) {
    throw std::runtime_error{"metadata file exceeds the supported size limit"};
  }
  input.seekg(0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) {
      throw std::runtime_error{"failed to read metadata file"};
    }
  }
  return bytes;
}

void write_all(const int descriptor, const std::span<const std::byte> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error{errno, std::generic_category(), "failed to write metadata file"};
    }
    if (result == 0) {
      throw std::runtime_error{"metadata write returned zero bytes"};
    }
    written += static_cast<std::size_t>(result);
  }
}

void sync_descriptor(const int descriptor, const char* context) {
  while (::fsync(descriptor) != 0) {
    if (errno == EINTR) {
      continue;
    }
    throw std::system_error{errno, std::generic_category(), context};
  }
}

void sync_directory(const std::filesystem::path& directory) {
  FileDescriptor descriptor{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (descriptor.get() < 0) {
    throw std::system_error{errno, std::generic_category(),
                            "failed to open metadata directory for fsync"};
  }
  sync_descriptor(descriptor.get(), "failed to fsync metadata directory");
}

void atomic_write(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);

  FileDescriptor descriptor{
      ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644)};
  if (descriptor.get() < 0) {
    throw std::system_error{errno, std::generic_category(),
                            "failed to create temporary metadata file"};
  }

  try {
    write_all(descriptor.get(), bytes);
    sync_descriptor(descriptor.get(), "failed to fsync metadata file");
    const int raw_descriptor = descriptor.release();
    if (::close(raw_descriptor) != 0) {
      throw std::system_error{errno, std::generic_category(), "failed to close metadata file"};
    }
    std::filesystem::rename(temporary, path);
    const auto parent =
        path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
    sync_directory(parent);
  } catch (...) {
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

[[nodiscard]] std::vector<std::byte> serialize_current(const std::string_view manifest_filename) {
  if (manifest_filename.empty() || manifest_filename.size() > kMaximumFilenameBytes) {
    throw std::invalid_argument{"manifest filename is invalid"};
  }
  std::vector<std::byte> bytes;
  bytes.reserve(20U + manifest_filename.size());
  bytes.insert(bytes.end(), kCurrentMagic.begin(), kCurrentMagic.end());
  append_uint16(bytes, kManifestVersion);
  append_uint16(bytes, 0U);
  append_uint32(bytes, checked_uint32(manifest_filename.size(), "manifest filename is too large"));
  append_bytes(bytes, manifest_filename);
  append_checksum(bytes);
  return bytes;
}

[[nodiscard]] std::string parse_current(const std::vector<std::byte>& bytes) {
  Decoder decoder{verified_payload(bytes)};
  decoder.expect_magic(kCurrentMagic);
  if (decoder.read_uint16() != kManifestVersion) {
    throw std::runtime_error{"CURRENT references an unsupported metadata version"};
  }
  if (decoder.read_uint16() != 0U) {
    throw std::runtime_error{"CURRENT contains unsupported flags"};
  }
  const std::uint32_t size = decoder.read_uint32();
  if (size == 0U || size > kMaximumFilenameBytes) {
    throw std::runtime_error{"CURRENT contains an invalid manifest filename length"};
  }
  std::string filename = decoder.read_string(size);
  if (!decoder.at_end()) {
    throw std::runtime_error{"CURRENT contains trailing data"};
  }
  const std::filesystem::path path{filename};
  if (path.is_absolute() || path.has_parent_path() || filename.rfind("MANIFEST-", 0U) != 0U) {
    throw std::runtime_error{"CURRENT contains an unsafe manifest filename"};
  }
  return filename;
}

[[nodiscard]] std::vector<std::byte> serialize_manifest(const ManifestSnapshot& snapshot) {
  if (snapshot.generation == 0U || snapshot.next_file_id == 0U) {
    throw std::invalid_argument{"manifest counters must be non-zero"};
  }
  if (snapshot.tables.size() > kMaximumManifestTables) {
    throw std::length_error{"manifest contains too many SSTables"};
  }

  std::set<std::string> filenames;
  std::vector<std::byte> bytes;
  bytes.insert(bytes.end(), kManifestMagic.begin(), kManifestMagic.end());
  append_uint16(bytes, kManifestVersion);
  append_uint16(bytes, 0U);
  append_uint64(bytes, snapshot.generation);
  append_uint64(bytes, snapshot.next_file_id);
  append_uint32(bytes, checked_uint32(snapshot.tables.size(), "manifest table count is too large"));
  append_uint32(bytes, 0U);

  for (const SSTableMetadata& table : snapshot.tables) {
    const std::string filename = table.path.filename().string();
    if (filename.empty() || filename.size() > kMaximumFilenameBytes || table.path.is_absolute() ||
        table.path.has_parent_path()) {
      throw std::invalid_argument{"manifest SSTable path must be a filename"};
    }
    if (!filenames.insert(filename).second) {
      throw std::invalid_argument{"manifest contains duplicate SSTable filenames"};
    }
    if (table.smallest_key.empty() || table.largest_key.empty() ||
        table.smallest_key > table.largest_key) {
      throw std::invalid_argument{"manifest contains an invalid SSTable key range"};
    }

    append_uint8(bytes, static_cast<std::uint8_t>(table.level));
    append_uint8(bytes, 0U);
    append_uint16(bytes, 0U);
    append_uint64(bytes, table.generation);
    append_uint64(bytes, table.entry_count);
    append_uint32(bytes, table.block_count);
    append_uint64(bytes, table.min_sequence_number);
    append_uint64(bytes, table.max_sequence_number);
    append_uint32(bytes, checked_uint32(filename.size(), "SSTable filename is too large"));
    append_uint32(bytes,
                  checked_uint32(table.smallest_key.size(), "SSTable smallest key is too large"));
    append_uint32(bytes,
                  checked_uint32(table.largest_key.size(), "SSTable largest key is too large"));
    append_bytes(bytes, filename);
    append_bytes(bytes, table.smallest_key);
    append_bytes(bytes, table.largest_key);
  }
  append_checksum(bytes);
  return bytes;
}

[[nodiscard]] ManifestSnapshot parse_manifest(const std::vector<std::byte>& bytes,
                                              const std::filesystem::path& directory) {
  Decoder decoder{verified_payload(bytes)};
  decoder.expect_magic(kManifestMagic);
  if (decoder.read_uint16() != kManifestVersion) {
    throw std::runtime_error{"manifest uses an unsupported version"};
  }
  if (decoder.read_uint16() != 0U) {
    throw std::runtime_error{"manifest contains unsupported flags"};
  }

  ManifestSnapshot snapshot;
  snapshot.generation = decoder.read_uint64();
  snapshot.next_file_id = decoder.read_uint64();
  const std::uint32_t table_count = decoder.read_uint32();
  if (decoder.read_uint32() != 0U) {
    throw std::runtime_error{"manifest header contains unsupported flags"};
  }
  if (snapshot.generation == 0U || snapshot.next_file_id == 0U ||
      table_count > kMaximumManifestTables) {
    throw std::runtime_error{"manifest header contains invalid counters"};
  }

  snapshot.tables.reserve(table_count);
  std::set<std::string> filenames;
  for (std::uint32_t index = 0; index < table_count; ++index) {
    const std::uint8_t raw_level = decoder.read_uint8();
    if (decoder.read_uint8() != 0U || decoder.read_uint16() != 0U) {
      throw std::runtime_error{"manifest table record contains unsupported flags"};
    }
    SSTableLevel level;
    if (raw_level == static_cast<std::uint8_t>(SSTableLevel::Level0)) {
      level = SSTableLevel::Level0;
    } else if (raw_level == static_cast<std::uint8_t>(SSTableLevel::Level1)) {
      level = SSTableLevel::Level1;
    } else {
      throw std::runtime_error{"manifest contains an unsupported SSTable level"};
    }

    SSTableMetadata table;
    table.level = level;
    table.generation = decoder.read_uint64();
    table.entry_count = decoder.read_uint64();
    table.block_count = decoder.read_uint32();
    table.min_sequence_number = decoder.read_uint64();
    table.max_sequence_number = decoder.read_uint64();
    const std::uint32_t filename_size = decoder.read_uint32();
    const std::uint32_t smallest_size = decoder.read_uint32();
    const std::uint32_t largest_size = decoder.read_uint32();
    if (filename_size == 0U || filename_size > kMaximumFilenameBytes || smallest_size == 0U ||
        largest_size == 0U) {
      throw std::runtime_error{"manifest contains invalid variable-length fields"};
    }
    const std::string filename = decoder.read_string(filename_size);
    const std::filesystem::path filename_path{filename};
    if (filename_path.is_absolute() || filename_path.has_parent_path() ||
        !filenames.insert(filename).second) {
      throw std::runtime_error{"manifest contains an unsafe or duplicate SSTable filename"};
    }
    table.path = directory / filename;
    table.smallest_key = decoder.read_string(smallest_size);
    table.largest_key = decoder.read_string(largest_size);
    if (table.smallest_key > table.largest_key || table.entry_count == 0U ||
        table.block_count == 0U || table.min_sequence_number == 0U ||
        table.min_sequence_number > table.max_sequence_number) {
      throw std::runtime_error{"manifest contains invalid SSTable metadata"};
    }
    snapshot.tables.push_back(std::move(table));
  }
  if (!decoder.at_end()) {
    throw std::runtime_error{"manifest contains trailing data"};
  }
  return snapshot;
}

} // namespace

ManifestManager::ManifestManager(std::filesystem::path directory)
    : directory_{std::move(directory)}, current_path_{directory_ / "CURRENT"} {
  if (directory_.empty()) {
    throw std::invalid_argument{"manifest directory must not be empty"};
  }
  std::filesystem::create_directories(directory_);
}

std::optional<ManifestSnapshot> ManifestManager::load() {
  if (!std::filesystem::exists(current_path_)) {
    for (const auto& entry : std::filesystem::directory_iterator{directory_}) {
      const std::string filename = entry.path().filename().string();
      if (entry.is_regular_file() && filename.rfind("MANIFEST-", 0U) == 0U &&
          entry.path().extension() != ".tmp") {
        throw std::runtime_error{"CURRENT is missing while versioned manifests are present"};
      }
    }
    return std::nullopt;
  }
  const std::string filename = parse_current(read_file(current_path_));
  const std::filesystem::path path = directory_ / filename;
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error{"CURRENT references a missing manifest: " + path.string()};
  }
  ManifestSnapshot snapshot = parse_manifest(read_file(path), directory_);

  std::ostringstream expected_name;
  expected_name << "MANIFEST-" << std::setw(20) << std::setfill('0') << snapshot.generation;
  if (filename != expected_name.str()) {
    throw std::runtime_error{"manifest filename does not match its generation"};
  }

  for (const SSTableMetadata& expected : snapshot.tables) {
    if (!std::filesystem::exists(expected.path)) {
      throw std::runtime_error{"manifest references a missing SSTable: " + expected.path.string()};
    }
  }

  current_generation_ = snapshot.generation;
  active_manifest_path_ = path;
  return snapshot;
}

ManifestSnapshot ManifestManager::commit(const std::vector<SSTableMetadata>& tables,
                                         const std::uint64_t next_file_id) {
  if (current_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error{"manifest generation space is exhausted"};
  }
  ManifestSnapshot snapshot{current_generation_ + 1U, next_file_id,
                            std::vector<SSTableMetadata>{tables.cbegin(), tables.cend()}};
  for (SSTableMetadata& table : snapshot.tables) {
    table.path = table.path.filename();
  }

  std::filesystem::path path = manifest_path(snapshot.generation);
  atomic_write(path, serialize_manifest(snapshot));
  const std::string filename = path.filename().string();
  atomic_write(current_path_, serialize_current(filename));

  current_generation_ = snapshot.generation;
  active_manifest_path_ = std::move(path);
  return snapshot;
}

const std::filesystem::path& ManifestManager::current_path() const noexcept {
  return current_path_;
}

std::filesystem::path ManifestManager::active_manifest_path() const {
  return active_manifest_path_;
}

std::uint64_t ManifestManager::current_generation() const noexcept { return current_generation_; }

std::filesystem::path ManifestManager::manifest_path(const std::uint64_t generation) const {
  std::ostringstream filename;
  filename << "MANIFEST-" << std::setw(20) << std::setfill('0') << generation;
  return directory_ / filename.str();
}

} // namespace nebulakv
