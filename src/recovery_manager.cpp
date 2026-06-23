#include "nebulakv/recovery_manager.hpp"

#include "nebulakv/in_memory_key_value_store.hpp"
#include "nebulakv/wal_record.hpp"

#include <filesystem>
#include <ostream>

namespace nebulakv {

RecoveryReport RecoveryManager::recover(const std::filesystem::path& wal_path,
                                        InMemoryKeyValueStore& destination,
                                        const RecoveryOptions options) {
  RecoveryReport report;
  if (!std::filesystem::exists(wal_path)) {
    return report;
  }

  const WalReader reader{wal_path};
  const WalScanResult scan = reader.scan([&destination, &report](const WalRecord& record) {
    if (record.operation == OperationType::Put) {
      destination.put(record.key, record.value);
      ++report.puts_applied;
    } else {
      static_cast<void>(destination.remove(record.key));
      ++report.deletes_applied;
    }
    ++report.records_applied;
  });

  report.valid_wal_bytes = scan.valid_bytes;
  report.issue = scan.issue;

  if (scan.issue && options.diagnostics != nullptr) {
    *options.diagnostics << "{\"level\":\"error\",\"component\":\"wal_recovery\","
                         << "\"offset\":" << scan.issue->byte_offset << ",\"error\":\""
                         << to_string(scan.issue->code) << "\"}\n";
  }

  if (scan.issue && scan.issue->code != WalReadIssueCode::IoError &&
      options.truncate_invalid_tail) {
    std::filesystem::resize_file(wal_path, scan.valid_bytes);
    report.invalid_tail_truncated = true;
  }

  return report;
}

} // namespace nebulakv
