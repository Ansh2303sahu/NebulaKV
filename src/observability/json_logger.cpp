#include "nebulakv/observability/json_logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace nebulakv::observability {

JsonLogger::JsonLogger(std::ostream& output) : output_{output} {}

void JsonLogger::log(const std::string_view level, const std::string_view event,
                     const std::map<std::string, std::string, std::less<>>& fields) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream timestamp;
  timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");

  std::lock_guard lock{mutex_};
  output_ << "{\"timestamp\":\"" << timestamp.str() << "\","
          << "\"level\":\"" << escape(level) << "\","
          << "\"event\":\"" << escape(event) << '"';
  for (const auto& [key, value] : fields) {
    output_ << ",\"" << escape(key) << "\":\"" << escape(value) << '"';
  }
  output_ << "}\n";
  output_.flush();
}

std::string JsonLogger::escape(const std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped.push_back(character);
      break;
    }
  }
  return escaped;
}

} // namespace nebulakv::observability
