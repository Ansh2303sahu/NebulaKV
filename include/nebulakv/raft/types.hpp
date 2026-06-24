#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv::raft {

using NodeId = std::string;

struct Peer {
  NodeId id;
  std::string address;
};

enum class Role : std::uint8_t {
  Follower = 0,
  Candidate = 1,
  Leader = 2,
};

[[nodiscard]] std::string_view to_string(Role role) noexcept;

enum class CommandType : std::uint8_t {
  Noop = 0,
  Put = 1,
  Delete = 2,
};

struct Command {
  CommandType type{CommandType::Noop};
  std::string key;
  std::string value;

  [[nodiscard]] bool operator==(const Command&) const = default;
};

struct LogEntry {
  std::uint64_t index{0};
  std::uint64_t term{0};
  Command command;

  [[nodiscard]] bool operator==(const LogEntry&) const = default;
};

struct RequestVoteRequest {
  std::uint64_t term{0};
  NodeId candidate_id;
  std::uint64_t last_log_index{0};
  std::uint64_t last_log_term{0};
};

struct RequestVoteResponse {
  std::uint64_t term{0};
  bool vote_granted{false};
};

struct AppendEntriesRequest {
  std::uint64_t term{0};
  NodeId leader_id;
  std::uint64_t previous_log_index{0};
  std::uint64_t previous_log_term{0};
  std::vector<LogEntry> entries;
  std::uint64_t leader_commit{0};
};

struct AppendEntriesResponse {
  std::uint64_t term{0};
  bool success{false};
  std::uint64_t match_index{0};
  std::uint64_t conflict_index{1};
};

struct InstallSnapshotRequest {
  std::uint64_t term{0};
  NodeId leader_id;
  std::uint64_t last_included_index{0};
  std::uint64_t last_included_term{0};
  std::string payload;
};

struct InstallSnapshotResponse {
  std::uint64_t term{0};
  bool success{false};
  std::uint64_t installed_index{0};
};

enum class SubmitStatus : std::uint8_t {
  Committed = 0,
  NotLeader = 1,
  Timeout = 2,
  Unavailable = 3,
  InvalidCommand = 4,
};

struct SubmitResult {
  SubmitStatus status{SubmitStatus::Unavailable};
  std::uint64_t log_index{0};
  std::uint64_t term{0};
  NodeId leader_id;
  std::string message;

  [[nodiscard]] bool committed() const noexcept { return status == SubmitStatus::Committed; }
};

struct Snapshot {
  std::uint64_t last_included_index{0};
  std::uint64_t last_included_term{0};
  std::string payload;

  [[nodiscard]] bool empty() const noexcept { return last_included_index == 0U; }
};

struct HardState {
  std::uint64_t current_term{0};
  NodeId voted_for;
  std::uint64_t commit_index{0};
  std::uint64_t last_applied{0};
};

struct PersistentState {
  HardState hard_state;
  std::vector<LogEntry> log;
  std::optional<Snapshot> snapshot;
};

struct RaftMetrics {
  std::uint64_t elections_started{0};
  std::uint64_t leadership_changes{0};
  std::uint64_t append_entries_sent{0};
  std::uint64_t append_entries_received{0};
  std::uint64_t vote_requests_sent{0};
  std::uint64_t vote_requests_received{0};
  std::uint64_t snapshots_created{0};
  std::uint64_t snapshots_installed{0};
  std::uint64_t replication_failures{0};
  std::uint64_t committed_commands{0};
};

struct NodeStatus {
  NodeId node_id;
  Role role{Role::Follower};
  std::uint64_t term{0};
  NodeId leader_id;
  std::uint64_t commit_index{0};
  std::uint64_t last_applied{0};
  std::uint64_t last_log_index{0};
  std::uint64_t last_log_term{0};
  std::uint64_t snapshot_index{0};
  std::size_t peer_count{0};
  RaftMetrics metrics;
};

struct RaftNodeOptions {
  NodeId node_id;
  std::vector<Peer> peers;
  std::chrono::milliseconds election_timeout_min{300};
  std::chrono::milliseconds election_timeout_max{600};
  std::chrono::milliseconds heartbeat_interval{75};
  std::chrono::milliseconds rpc_timeout{200};
  std::size_t max_append_entries{128U};
  std::uint64_t snapshot_threshold_entries{10'000U};
  std::uint64_t random_seed{0U};
};

} // namespace nebulakv::raft
