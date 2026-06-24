#pragma once

#include "nebulakv/raft/state_machine.hpp"
#include "nebulakv/raft/storage.hpp"
#include "nebulakv/raft/transport.hpp"
#include "nebulakv/raft/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

namespace nebulakv::raft {

class RaftNode final : public RaftEndpoint {
public:
  RaftNode(RaftNodeOptions options, RaftStorage& storage, StateMachine& state_machine,
           RaftTransport& transport);
  ~RaftNode() override;

  RaftNode(const RaftNode&) = delete;
  RaftNode& operator=(const RaftNode&) = delete;
  RaftNode(RaftNode&&) = delete;
  RaftNode& operator=(RaftNode&&) = delete;

  void start();
  void stop();

  [[nodiscard]] SubmitResult submit(Command command,
                                    std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] SubmitResult submit(Command command, std::chrono::milliseconds timeout);

  [[nodiscard]] RequestVoteResponse handle_request_vote(const RequestVoteRequest& request) override;
  [[nodiscard]] AppendEntriesResponse
  handle_append_entries(const AppendEntriesRequest& request) override;
  [[nodiscard]] InstallSnapshotResponse
  handle_install_snapshot(const InstallSnapshotRequest& request) override;

  [[nodiscard]] NodeStatus status() const;
  [[nodiscard]] bool is_leader() const;
  [[nodiscard]] bool confirm_leadership(std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] NodeId leader_id() const;
  [[nodiscard]] std::optional<std::string> leader_address() const;
  [[nodiscard]] bool wait_for_role(Role role, std::chrono::milliseconds timeout) const;
  void trigger_election();
  void trigger_replication();
  void trigger_snapshot();

private:
  void ticker_loop();
  void begin_election();
  void become_follower_locked(std::uint64_t term, NodeId leader_id = {});
  void become_leader_locked();
  void reset_election_deadline_locked();
  void persist_hard_state_locked();
  void persist_log_locked();

  [[nodiscard]] std::uint64_t last_log_index_locked() const noexcept;
  [[nodiscard]] std::uint64_t last_log_term_locked() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> term_at_locked(std::uint64_t index) const noexcept;
  [[nodiscard]] std::vector<LogEntry> entries_from_locked(std::uint64_t index) const;
  void truncate_log_from_locked(std::uint64_t index);
  void append_entries_locked(const std::vector<LogEntry>& entries);
  [[nodiscard]] LogEntry append_local_entry_locked(Command command);

  [[nodiscard]] bool replicate_all(bool heartbeat_only = false);
  [[nodiscard]] bool replicate_peer(const Peer& peer, bool heartbeat_only);
  [[nodiscard]] bool advance_commit_locked();
  void apply_committed_locked();
  void maybe_create_snapshot_locked();
  [[nodiscard]] std::size_t quorum_size() const noexcept;
  [[nodiscard]] const Peer* find_peer(std::string_view id) const noexcept;

  RaftNodeOptions options_;
  RaftStorage& storage_;
  StateMachine& state_machine_;
  RaftTransport& transport_;

  mutable std::mutex mutex_;
  mutable std::condition_variable state_changed_;
  std::mutex campaign_mutex_;
  std::mutex replication_mutex_;
  std::mutex submit_mutex_;
  std::thread ticker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  Role role_{Role::Follower};
  HardState hard_state_;
  std::vector<LogEntry> log_;
  std::optional<Snapshot> snapshot_;
  NodeId leader_id_;
  std::map<NodeId, std::uint64_t, std::less<>> next_index_;
  std::map<NodeId, std::uint64_t, std::less<>> match_index_;
  std::chrono::steady_clock::time_point election_deadline_;
  std::chrono::steady_clock::time_point heartbeat_deadline_;
  mutable std::mt19937_64 random_;
  RaftMetrics metrics_;
};

} // namespace nebulakv::raft
