#pragma once

#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace nebulakv::observability {

class JsonLogger final {
public:
  explicit JsonLogger(std::ostream& output);

  void log(std::string_view level, std::string_view event,
           const std::map<std::string, std::string, std::less<>>& fields = {});

private:
  [[nodiscard]] static std::string escape(std::string_view value);

  std::ostream& output_;
  std::mutex mutex_;
};

} // namespace nebulakv::observability
