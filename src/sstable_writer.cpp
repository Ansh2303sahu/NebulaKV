#include "nebulakv/sstable_writer.hpp"

#include "nebulakv/data_block.hpp"
#include "nebulakv/index_block.hpp"
#include "nebulakv/sstable_footer.hpp"
#include "sstable_format.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace nebulakv {

namespace {

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

  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] int release() noexcept {
    const int result = value_;
    value_ = -1;
    return result;
  }

private:
  int value_{-1};
};

void write_all(const int descriptor, const std::span<const std::byte> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error{errno, std::generic_category(), "failed to write SSTable file"};
    }
    if (result == 0) {
      throw std::runtime_error{"SSTable write returned zero bytes"};
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
                            "failed to open SSTable directory for fsync"};
  }
  sync_descriptor(descriptor.get(), "failed to fsync SSTable directory");
}

[[nodiscard]] std::filesystem::path temporary_path_for(const std::filesystem::path& output_path) {
  return output_path.string() + ".tmp";
}

[[nodiscard]] std::uint32_t narrow_block_size(const std::size_t value) {
  if (value == 0U || value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument{"SSTable target data block size must fit in a 32-bit field"};
  }
  return static_cast<std::uint32_t>(value);
}

} // namespace

SSTableMetadata SSTableWriter::write(const MemTable::Snapshot& entries,
                                     SSTableWriterOptions options) {
  if (options.output_path.empty()) {
    throw std::invalid_argument{"SSTable output path must not be empty"};
  }
  if (entries.empty()) {
    throw std::invalid_argument{"cannot write an empty SSTable"};
  }
  const std::uint32_t target_block_size = narrow_block_size(options.target_data_block_bytes);

  std::string_view previous_key;
  std::uint64_t min_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_sequence = 0;
  for (const auto& [key, entry] : entries) {
    if (!previous_key.empty() && key <= previous_key) {
      throw std::invalid_argument{"SSTable input keys must be strictly sorted"};
    }
    if (entry.sequence_number == 0U) {
      throw std::invalid_argument{"SSTable entries require non-zero sequence numbers"};
    }
    previous_key = key;
    min_sequence = std::min(min_sequence, entry.sequence_number);
    max_sequence = std::max(max_sequence, entry.sequence_number);
  }

  std::vector<DataBlock> blocks;
  DataBlock current;
  std::size_t current_size = 12U + sstable_format::kChecksumSize;
  for (const auto& record : entries) {
    const std::size_t record_size = sstable_format::encoded_record_size(record);
    if (!current.empty() && current_size + record_size > options.target_data_block_bytes) {
      blocks.push_back(std::move(current));
      current = DataBlock{};
      current_size = 12U + sstable_format::kChecksumSize;
    }
    current.records.push_back(record);
    current_size += record_size;
  }
  if (!current.empty()) {
    blocks.push_back(std::move(current));
  }
  if (blocks.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error{"SSTable contains too many data blocks"};
  }

  const auto parent = options.output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  const std::filesystem::path temporary_path = temporary_path_for(options.output_path);
  std::error_code ignored;
  std::filesystem::remove(temporary_path, ignored);

  FileDescriptor descriptor{
      ::open(temporary_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644)};
  if (descriptor.get() < 0) {
    throw std::system_error{errno, std::generic_category(), "failed to create temporary SSTable"};
  }

  try {
    sstable_format::Header header;
    header.generation = options.generation;
    header.entry_count = entries.size();
    header.block_count = static_cast<std::uint32_t>(blocks.size());
    header.target_data_block_bytes = target_block_size;
    header.min_sequence_number = min_sequence;
    header.max_sequence_number = max_sequence;
    write_all(descriptor.get(), sstable_format::serialize_header(header));

    IndexBlock index;
    index.entries.reserve(blocks.size());
    std::uint64_t current_offset = sstable_format::kHeaderSize;
    for (const DataBlock& block : blocks) {
      const std::vector<std::byte> encoded = sstable_format::serialize_data_block(block);
      index.entries.push_back(
          IndexEntry{block.first_key(), block.last_key(), current_offset, encoded.size()});
      write_all(descriptor.get(), encoded);
      current_offset += encoded.size();
    }

    const std::vector<std::byte> encoded_index = sstable_format::serialize_index_block(index);
    const std::uint64_t index_offset = current_offset;
    write_all(descriptor.get(), encoded_index);
    current_offset += encoded_index.size();

    SSTableFooter footer;
    footer.index_offset = index_offset;
    footer.index_size = encoded_index.size();
    footer.entry_count = entries.size();
    footer.block_count = static_cast<std::uint32_t>(blocks.size());
    footer.min_sequence_number = min_sequence;
    footer.max_sequence_number = max_sequence;
    write_all(descriptor.get(), sstable_format::serialize_footer(footer));

    sync_descriptor(descriptor.get(), "failed to fsync temporary SSTable");
    const int raw_descriptor = descriptor.release();
    if (::close(raw_descriptor) != 0) {
      throw std::system_error{errno, std::generic_category(), "failed to close temporary SSTable"};
    }

    std::filesystem::rename(temporary_path, options.output_path);
    sync_directory(parent.empty() ? std::filesystem::path{"."} : parent);
  } catch (...) {
    std::filesystem::remove(temporary_path, ignored);
    throw;
  }

  SSTableMetadata metadata;
  metadata.path = std::move(options.output_path);
  metadata.generation = options.generation;
  metadata.entry_count = entries.size();
  metadata.block_count = static_cast<std::uint32_t>(blocks.size());
  metadata.min_sequence_number = min_sequence;
  metadata.max_sequence_number = max_sequence;
  metadata.smallest_key = entries.front().first;
  metadata.largest_key = entries.back().first;
  return metadata;
}

} // namespace nebulakv
