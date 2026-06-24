#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace nebulakv::test {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("nebulakv-" + std::to_string(::getpid()) + "-" + std::to_string(timestamp));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  TemporaryDirectory(TemporaryDirectory&&) = delete;
  TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  [[nodiscard]] std::filesystem::path file(const std::string& name) const {
    return path_ / name;
  }

 private:
  std::filesystem::path path_;
};

inline std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    throw std::runtime_error{"failed to open test file"};
  }
  const std::streamsize size = input.tellg();
  if (size < 0) {
    throw std::runtime_error{"failed to determine test file size"};
  }
  input.seekg(0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) {
      throw std::runtime_error{"failed to read test file"};
    }
  }
  return bytes;
}

inline void write_file(const std::filesystem::path& path,
                       const std::vector<std::byte>& bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw std::runtime_error{"failed to create test file"};
  }
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  if (!output) {
    throw std::runtime_error{"failed to write test file"};
  }
}

inline void overwrite_byte(const std::filesystem::path& path,
                           const std::uint64_t offset,
                           const std::byte value) {
  std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
  if (!file) {
    throw std::runtime_error{"failed to open test file for modification"};
  }
  file.seekp(static_cast<std::streamoff>(offset));
  const char byte = static_cast<char>(std::to_integer<unsigned char>(value));
  file.write(&byte, 1);
  if (!file) {
    throw std::runtime_error{"failed to modify test file"};
  }
}

}  // namespace nebulakv::test
