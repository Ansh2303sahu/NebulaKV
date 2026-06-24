#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_error.hpp"
#include "nebulakv/sstable_reader.hpp"
#include "nebulakv/sstable_writer.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace nebulakv {
namespace {

MemTable::Snapshot make_snapshot(const std::size_t count) {
  MemTable::Snapshot entries;
  entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::string key = "key-" + std::to_string(1000U + index);
    entries.emplace_back(std::move(key),
                         Entry{"value-" + std::to_string(index), index + 1U, false});
  }
  return entries;
}

SSTableMetadata write_table(const std::filesystem::path& path, const MemTable::Snapshot& entries,
                            const std::size_t block_size = 128U) {
  return SSTableWriter::write(entries, SSTableWriterOptions{path, block_size, 7U});
}

TEST(SSTableWriterReaderTest, RoundTripsSortedEntries) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("records.sst");
  const auto entries = make_snapshot(25U);

  write_table(path, entries);
  const SSTableReader reader{path};

  EXPECT_EQ(reader.read_all(), entries);
  EXPECT_EQ(reader.metadata().entry_count, entries.size());
  EXPECT_EQ(reader.metadata().generation, 7U);
}

TEST(SSTableWriterReaderTest, CreatesMultipleIndexedDataBlocks) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("blocks.sst");

  write_table(path, make_snapshot(50U), 96U);
  const SSTableReader reader{path};

  EXPECT_GT(reader.metadata().block_count, 1U);
  EXPECT_EQ(reader.index().entries.size(), reader.metadata().block_count);
  EXPECT_EQ(reader.index().entries.front().first_key, "key-1000");
  EXPECT_EQ(reader.index().entries.back().last_key, "key-1049");
}

TEST(SSTableWriterReaderTest, FindsFirstMiddleAndLastKeys) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("lookup.sst");
  write_table(path, make_snapshot(30U), 100U);
  const SSTableReader reader{path};

  EXPECT_EQ(reader.get("key-1000")->value, "value-0");
  EXPECT_EQ(reader.get("key-1015")->value, "value-15");
  EXPECT_EQ(reader.get("key-1029")->value, "value-29");
}

TEST(SSTableWriterReaderTest, ReturnsNulloptForMissingKey) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("missing.sst");
  write_table(path, make_snapshot(10U));
  const SSTableReader reader{path};

  EXPECT_FALSE(reader.get("key-0999").has_value());
  EXPECT_FALSE(reader.get("key-1005-extra").has_value());
  EXPECT_FALSE(reader.get("key-9999").has_value());
}

TEST(SSTableWriterReaderTest, PreservesTombstones) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("tombstone.sst");
  MemTable::Snapshot entries{{"deleted", Entry{{}, 9U, true}},
                             {"live", Entry{"value", 10U, false}}};

  write_table(path, entries);
  const SSTableReader reader{path};

  const auto tombstone = reader.get("deleted");
  ASSERT_TRUE(tombstone.has_value());
  EXPECT_TRUE(tombstone->deleted);
  EXPECT_TRUE(tombstone->value.empty());
  EXPECT_FALSE(reader.get("live")->deleted);
}

TEST(SSTableWriterReaderTest, PreservesEmbeddedNullBytesInValues) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("binary.sst");
  const std::string value{"a\0b\0c", 5U};
  MemTable::Snapshot entries{{"binary", Entry{value, 1U, false}}};

  write_table(path, entries);
  const SSTableReader reader{path};

  ASSERT_TRUE(reader.get("binary").has_value());
  EXPECT_EQ(reader.get("binary")->value, value);
}

TEST(SSTableWriterReaderTest, ReopensPersistedFile) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("restart.sst");
  write_table(path, make_snapshot(20U));

  const SSTableReader first{path};
  const auto metadata = first.metadata();
  const SSTableReader second{path};

  EXPECT_EQ(second.metadata().entry_count, metadata.entry_count);
  EXPECT_EQ(second.get("key-1010")->value, "value-10");
}

TEST(SSTableWriterReaderTest, RejectsEmptyInput) {
  test::TemporaryDirectory directory;

  EXPECT_THROW(write_table(directory.file("empty.sst"), {}), std::invalid_argument);
}

TEST(SSTableWriterReaderTest, RejectsUnsortedInput) {
  test::TemporaryDirectory directory;
  MemTable::Snapshot entries{{"z", Entry{"1", 1U, false}}, {"a", Entry{"2", 2U, false}}};

  EXPECT_THROW(write_table(directory.file("unsorted.sst"), entries), std::invalid_argument);
}

TEST(SSTableWriterReaderTest, PointLookupReadsOnlyTheIndexedBlock) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("targeted-read.sst");
  write_table(path, make_snapshot(40U), 96U);
  const SSTableReader original{path};
  ASSERT_TRUE(original.index().entries.size() > 1U);
  const auto& damaged = original.index().entries.back();
  test::overwrite_byte(path, damaged.block_offset + 16U, std::byte{0xEE});

  const SSTableReader reader{path};
  EXPECT_EQ(reader.get("key-1000")->value, "value-0");
  EXPECT_THROW(static_cast<void>(reader.get("key-1039")), SSTableCorruptionError);
}

TEST(SSTableWriterReaderTest, DetectsCorruptedDataBlockChecksum) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("corrupt-data.sst");
  write_table(path, make_snapshot(20U), 128U);
  const SSTableReader reader{path};
  const std::uint64_t offset = reader.index().entries.front().block_offset + 16U;

  test::overwrite_byte(path, offset, std::byte{0xFF});

  try {
    const SSTableReader corrupted{path};
    static_cast<void>(corrupted.read_all());
    FAIL() << "corruption was not detected";
  } catch (const SSTableCorruptionError& error) {
    EXPECT_GE(error.byte_offset(), reader.index().entries.front().block_offset);
  }
}

TEST(SSTableWriterReaderTest, DetectsCorruptedIndexChecksum) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("corrupt-index.sst");
  write_table(path, make_snapshot(20U));
  const SSTableReader reader{path};
  const std::uint64_t offset = reader.footer().index_offset + 12U;

  test::overwrite_byte(path, offset, std::byte{0xAA});

  EXPECT_THROW(SSTableReader{path}, SSTableCorruptionError);
}

TEST(SSTableWriterReaderTest, DetectsTruncatedFooter) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("truncated.sst");
  write_table(path, make_snapshot(5U));
  const auto size = std::filesystem::file_size(path);

  std::filesystem::resize_file(path, size - 8U);

  EXPECT_THROW(SSTableReader{path}, SSTableCorruptionError);
}

TEST(SSTableWriterReaderTest, FooterAndHeaderSequenceRangesMatch) {
  test::TemporaryDirectory directory;
  const auto path = directory.file("sequence.sst");
  MemTable::Snapshot entries{
      {"a", Entry{"1", 10U, false}}, {"b", Entry{"2", 20U, false}}, {"c", Entry{"3", 15U, false}}};

  write_table(path, entries);
  const SSTableReader reader{path};

  EXPECT_EQ(reader.metadata().min_sequence_number, 10U);
  EXPECT_EQ(reader.metadata().max_sequence_number, 20U);
  EXPECT_EQ(reader.footer().min_sequence_number, 10U);
  EXPECT_EQ(reader.footer().max_sequence_number, 20U);
}

} // namespace
} // namespace nebulakv
