# C++ Industry Project Foundation

A production-style C++20 starter repository for Linux and WSL2. It includes CMake presets,
GCC/Clang CI builds, GoogleTest, Google Benchmark, clang-format, clang-tidy, AddressSanitizer,
UndefinedBehaviorSanitizer, and ThreadSanitizer.

## Prerequisites

Ubuntu/WSL2:

```bash
sudo apt update
sudo apt install -y build-essential clang clang-tidy clang-format cmake ninja-build git
```

Required: CMake 3.22+, a C++20 compiler, Ninja, and Git. The first configure downloads pinned
GoogleTest or Google Benchmark sources with CMake FetchContent.

## Required Phase 1 commands

```bash
git clone <your-repository-url>
cd <your-repository-name>
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The `debug` preset runs clang-tidy during compilation. To use Clang explicitly:

```bash
CC=clang CXX=clang++ cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

To switch compiler after configuring, delete the relevant build directory first:

```bash
rm -rf build/debug
```

## Other presets

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan

cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/benchmarks/project_benchmarks
```

## Formatting

```bash
cmake --build --preset debug --target format
cmake --build --preset debug --target format-check
```

## Run the sample executable

```bash
./build/debug/project_cli
```

## Repository layout

```text
.
├── .github/workflows/ci.yml
├── app/
├── benchmarks/
├── cmake/
├── include/industry_starter/
├── src/
├── tests/
├── .clang-format
├── .clang-tidy
├── CMakeLists.txt
└── CMakePresets.json
```

## Phase 1 completion checklist

- [ ] Debug builds on Linux or WSL2
- [ ] Release builds
- [ ] GCC and Clang pass in GitHub Actions
- [ ] Unit tests run with CTest
- [ ] Pull requests trigger CI
- [ ] clang-format check passes
- [ ] clang-tidy runs in the debug build
- [ ] ASan/UBSan tests pass
- [ ] TSan tests pass
- [ ] Benchmark target builds and runs
