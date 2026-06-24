#include "nebulakv/raft/node.hpp"

#include "nebulakv/validation.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace nebulakv::raft {

namespace {

[[nodiscard]] std::uint64_t seed_for(const RaftNodeOptions& options) {
  if (options.random_seed != 0U) {
    return options.random_seed;
  }
  return static_cast<std::uint64_t>(std::hash<std::string>{}(options.node_id)) ^
         static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

void validate_options(const RaftNodeOptions& options) {
  if (options.node_id.empty()) {
    throw std::invalid_argument{"Raft node id must not be empty"};
  }
  if (options.election_timeout_min <= std::chrono::milliseconds{0} ||
      options.election_timeout_max < options.election_timeout_min) {
    throw std::invalid_argument{"invalid Raft election timeout range"};
  }
  if (options.heartbeat_interval <= std::chrono::milliseconds{0} ||
      options.heartbeat_interval >= options.election_timeout_min) {
    throw std::invalid_argument{"heartbeat interval must be positive and below election timeout"};
  }
  if (options.rpc_timeout <= std::chrono::milliseconds{0}) {
    throw std::invalid_argument{"Raft RPC timeout must be positive"};
  }
  if (options.max_append_entries == 0U) {
    throw std::invalid_argument{"maximum AppendEntries batch must be positive"};
  }

  std::vector<NodeId> ids;
  ids.reserve(options.peers.size());
  for (const auto& peer : options.peers) {
    if (peer.id.empty() || peer.address.empty()) {
      throw std::invalid_argument{"Raft peer id and address must not be empty"};
    }
    if (peer.id == options.node_id) {
      throw std::invalid_argument{"Raft peer list must not contain the local node"};
    }
    ids.push_back(peer.id);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
    throw std::invalid_argument{"Raft peer ids must be unique"};
  }
}

[[nodiscard]] bool valid_client_command(const Command& command) {
  if (command.type == CommandType::Noop) {
    return false;
  }
  validate_key(command.key);
  if (command.type == CommandType::Put) {
    validate_value(command.value);
  } else if (!command.value.empty()) {
    throw std::invalid_argument{"delete command must not contain a value"};
  }
  return true;
}

} // namespace

RaftNode::RaftNode(RaftNodeOptions options, RaftStorage& storage, StateMachine& state_machine,
                   RaftTransport& transport)
    : options_{std::move(options)}, storage_{storage}, state_machine_{state_machine},
      transport_{transport}, random_{seed_for(options_)} {
  validate_options(options_);

  PersistentState persisted = storage_.load();
  hard_state_ = std::move(persisted.hard_state);
  log_ = std::move(persisted.log);
  snapshot_ = std::move(persisted.snapshot);

  const std::uint64_t snapshot_index = snapshot_ ? snapshot_->last_included_index : 0U;
  if (!log_.empty()) {
    if (log_.front().index != snapshot_index + 1U) {
      throw std::runtime_error{"persisted Raft log does not follow its snapshot"};
    }
    for (std::size_t index = 1U; index < log_.size(); ++index) {
      if (log_[index].index != log_[index - 1U].index + 1U) {
        throw std::runtime_error{"persisted Raft log indices are not contiguous"};
      }
    }
  }
  if (hard_state_.commit_index < snapshot_index) {
    hard_state_.commit_index = snapshot_index;
  }
  if (hard_state_.commit_index > last_log_index_locked()) {
    throw std::runtime_error{"persisted Raft commit index exceeds the log"};
  }

  std::vector<LogEntry> committed_entries;
  for (const auto& entry : log_) {
    if (entry.index <= hard_state_.commit_index) {
      committed_entries.push_back(entry);
    }
  }
  state_machine_.recover(snapshot_, committed_entries);
  hard_state_.last_applied = hard_state_.commit_index;
  persist_hard_state_locked();
  reset_election_deadline_locked();
  heartbeat_deadline_ = std::chrono::steady_clock::now();
}

RaftNode::~RaftNode() { stop(); }

void RaftNode::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  stop_requested_.store(false, std::memory_order_release);
  ticker_ = std::thread{&RaftNode::ticker_loop, this};
}

void RaftNode::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  state_changed_.notify_all();
  if (ticker_.joinable()) {
    ticker_.join();
  }
  state_machine_.flush();
}

SubmitResult RaftNode::submit(Command command,
                              const std::chrono::steady_clock::time_point deadline) {
  try {
    static_cast<void>(valid_client_command(command));
  } catch (const std::exception& error) {
    return {SubmitStatus::InvalidCommand, 0U, 0U, {}, error.what()};
  }

  std::lock_guard submission_lock{submit_mutex_};
  std::uint64_t entry_index = 0U;
  std::uint64_t entry_term = 0U;
  {
    std::lock_guard lock{mutex_};
    if (role_ != Role::Leader) {
      return {SubmitStatus::NotLeader, 0U, hard_state_.current_term, leader_id_,
              "request must be sent to the current leader"};
    }
    const LogEntry entry = append_local_entry_locked(std::move(command));
    entry_index = entry.index;
    entry_term = entry.term;
    persist_log_locked();

    if (quorum_size() == 1U) {
      hard_state_.commit_index = entry_index;
      apply_committed_locked();
      persist_hard_state_locked();
      return {SubmitStatus::Committed, entry_index, entry_term, options_.node_id, {}};
    }
  }

  while (std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(replicate_all(false));
    {
      std::lock_guard lock{mutex_};
      if (hard_state_.commit_index >= entry_index) {
        return {SubmitStatus::Committed, entry_index, entry_term, options_.node_id, {}};
      }
      if (role_ != Role::Leader || hard_state_.current_term != entry_term) {
        return {SubmitStatus::NotLeader, entry_index, hard_state_.current_term, leader_id_,
                "leadership changed before the command committed"};
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }

  std::lock_guard lock{mutex_};
  return {SubmitStatus::Timeout, entry_index, hard_state_.current_term, leader_id_,
          "command did not reach a quorum before its deadline"};
}

SubmitResult RaftNode::submit(Command command, const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds{0}) {
    return {SubmitStatus::Timeout, 0U, 0U, {}, "command timeout must be positive"};
  }
  return submit(std::move(command), std::chrono::steady_clock::now() + timeout);
}

RequestVoteResponse RaftNode::handle_request_vote(const RequestVoteRequest& request) {
  std::lock_guard lock{mutex_};
  ++metrics_.vote_requests_received;
  if (request.term < hard_state_.current_term) {
    return {hard_state_.current_term, false};
  }
  if (request.term > hard_state_.current_term) {
    become_follower_locked(request.term);
  }

  const bool candidate_is_current = request.last_log_term > last_log_term_locked() ||
                                    (request.last_log_term == last_log_term_locked() &&
                                     request.last_log_index >= last_log_index_locked());
  const bool can_vote =
      hard_state_.voted_for.empty() || hard_state_.voted_for == request.candidate_id;
  const bool grant = can_vote && candidate_is_current;
  if (grant) {
    hard_state_.voted_for = request.candidate_id;
    persist_hard_state_locked();
    reset_election_deadline_locked();
  }
  return {hard_state_.current_term, grant};
}

AppendEntriesResponse RaftNode::handle_append_entries(const AppendEntriesRequest& request) {
  std::lock_guard lock{mutex_};
  ++metrics_.append_entries_received;
  if (request.term < hard_state_.current_term) {
    return {hard_state_.current_term, false, last_log_index_locked(), last_log_index_locked() + 1U};
  }
  if (request.term > hard_state_.current_term || role_ != Role::Follower) {
    become_follower_locked(request.term, request.leader_id);
  }
  leader_id_ = request.leader_id;
  reset_election_deadline_locked();

  const std::uint64_t snapshot_index = snapshot_ ? snapshot_->last_included_index : 0U;
  if (request.previous_log_index < snapshot_index) {
    return {hard_state_.current_term, false, snapshot_index, snapshot_index + 1U};
  }

  const auto previous_term = term_at_locked(request.previous_log_index);
  if (!previous_term) {
    return {hard_state_.current_term, false, last_log_index_locked(), last_log_index_locked() + 1U};
  }
  if (*previous_term != request.previous_log_term) {
    std::uint64_t conflict_index = request.previous_log_index;
    while (conflict_index > snapshot_index + 1U) {
      const auto earlier_term = term_at_locked(conflict_index - 1U);
      if (!earlier_term || *earlier_term != *previous_term) {
        break;
      }
      --conflict_index;
    }
    return {hard_state_.current_term, false, 0U, conflict_index};
  }

  bool log_changed = false;
  for (std::size_t position = 0U; position < request.entries.size(); ++position) {
    const LogEntry& incoming = request.entries[position];
    if (incoming.index != request.previous_log_index + position + 1U) {
      return {hard_state_.current_term, false, last_log_index_locked(),
              last_log_index_locked() + 1U};
    }
    if (incoming.index <= snapshot_index) {
      continue;
    }
    const auto existing_term = term_at_locked(incoming.index);
    if (existing_term && *existing_term != incoming.term) {
      truncate_log_from_locked(incoming.index);
      log_changed = true;
    }
    if (!term_at_locked(incoming.index)) {
      const std::vector<LogEntry> remaining{
          request.entries.begin() + static_cast<std::ptrdiff_t>(position), request.entries.end()};
      append_entries_locked(remaining);
      log_changed = true;
      break;
    }
  }
  if (log_changed) {
    persist_log_locked();
  }

  if (request.leader_commit > hard_state_.commit_index) {
    hard_state_.commit_index = std::min(request.leader_commit, last_log_index_locked());
    apply_committed_locked();
    persist_hard_state_locked();
  }

  const std::uint64_t match_index =
      request.entries.empty() ? request.previous_log_index : request.entries.back().index;
  return {hard_state_.current_term, true, std::min(match_index, last_log_index_locked()), 0U};
}

InstallSnapshotResponse RaftNode::handle_install_snapshot(const InstallSnapshotRequest& request) {
  std::lock_guard lock{mutex_};
  if (request.term < hard_state_.current_term) {
    return {hard_state_.current_term, false, snapshot_ ? snapshot_->last_included_index : 0U};
  }
  if (request.term > hard_state_.current_term || role_ != Role::Follower) {
    become_follower_locked(request.term, request.leader_id);
  }
  leader_id_ = request.leader_id;
  reset_election_deadline_locked();

  if (snapshot_ && request.last_included_index <= snapshot_->last_included_index) {
    return {hard_state_.current_term, true, snapshot_->last_included_index};
  }

  const auto matching_term = term_at_locked(request.last_included_index);
  if (matching_term && *matching_term == request.last_included_term) {
    const auto first_to_keep = std::upper_bound(
        log_.begin(), log_.end(), request.last_included_index,
        [](const std::uint64_t index, const LogEntry& entry) { return index < entry.index; });
    log_.erase(log_.begin(), first_to_keep);
  } else {
    log_.clear();
  }

  Snapshot installed{request.last_included_index, request.last_included_term, request.payload};
  storage_.save_snapshot(installed);
  snapshot_ = std::move(installed);
  persist_log_locked();
  state_machine_.install_snapshot(snapshot_->payload);
  hard_state_.commit_index = std::max(hard_state_.commit_index, snapshot_->last_included_index);
  hard_state_.last_applied = std::max(hard_state_.last_applied, snapshot_->last_included_index);
  persist_hard_state_locked();
  ++metrics_.snapshots_installed;
  state_changed_.notify_all();
  return {hard_state_.current_term, true, snapshot_->last_included_index};
}

NodeStatus RaftNode::status() const {
  std::lock_guard lock{mutex_};
  return {options_.node_id,
          role_,
          hard_state_.current_term,
          leader_id_,
          hard_state_.commit_index,
          hard_state_.last_applied,
          last_log_index_locked(),
          last_log_term_locked(),
          snapshot_ ? snapshot_->last_included_index : 0U,
          options_.peers.size(),
          metrics_};
}

bool RaftNode::is_leader() const {
  std::lock_guard lock{mutex_};
  return role_ == Role::Leader;
}

bool RaftNode::confirm_leadership(const std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const bool contacted_quorum = replicate_all(false);
    {
      std::lock_guard lock{mutex_};
      if (role_ != Role::Leader) {
        return false;
      }
      const auto committed_term = term_at_locked(hard_state_.commit_index);
      if (contacted_quorum && committed_term && *committed_term == hard_state_.current_term) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return false;
}

NodeId RaftNode::leader_id() const {
  std::lock_guard lock{mutex_};
  return leader_id_;
}

std::optional<std::string> RaftNode::leader_address() const {
  std::lock_guard lock{mutex_};
  if (leader_id_.empty() || leader_id_ == options_.node_id) {
    return std::nullopt;
  }
  const Peer* peer = find_peer(leader_id_);
  if (peer == nullptr) {
    return std::nullopt;
  }
  return peer->address;
}

bool RaftNode::wait_for_role(const Role role, const std::chrono::milliseconds timeout) const {
  std::unique_lock lock{mutex_};
  return state_changed_.wait_for(lock, timeout, [this, role] { return role_ == role; });
}

void RaftNode::trigger_election() { begin_election(); }

void RaftNode::trigger_replication() { static_cast<void>(replicate_all(false)); }

void RaftNode::trigger_snapshot() {
  std::lock_guard lock{mutex_};
  if (hard_state_.last_applied == 0U) {
    return;
  }
  const auto original_threshold = options_.snapshot_threshold_entries;
  options_.snapshot_threshold_entries = 1U;
  maybe_create_snapshot_locked();
  options_.snapshot_threshold_entries = original_threshold;
}

void RaftNode::ticker_loop() {
  while (!stop_requested_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    bool election_due = false;
    bool heartbeat_due = false;
    {
      std::lock_guard lock{mutex_};
      const auto now = std::chrono::steady_clock::now();
      if (role_ == Role::Leader) {
        heartbeat_due = now >= heartbeat_deadline_;
        if (heartbeat_due) {
          heartbeat_deadline_ = now + options_.heartbeat_interval;
        }
      } else {
        election_due = now >= election_deadline_;
      }
    }
    if (election_due) {
      begin_election();
    } else if (heartbeat_due) {
      static_cast<void>(replicate_all(true));
    }
  }
}

void RaftNode::begin_election() {
  std::lock_guard campaign_lock{campaign_mutex_};
  RequestVoteRequest request;
  std::uint64_t election_term = 0U;
  {
    std::lock_guard lock{mutex_};
    if (role_ == Role::Leader) {
      return;
    }
    role_ = Role::Candidate;
    leader_id_.clear();
    ++hard_state_.current_term;
    election_term = hard_state_.current_term;
    hard_state_.voted_for = options_.node_id;
    ++metrics_.elections_started;
    persist_hard_state_locked();
    reset_election_deadline_locked();
    request = {election_term, options_.node_id, last_log_index_locked(), last_log_term_locked()};
    state_changed_.notify_all();
  }

  std::size_t votes = 1U;
  for (const auto& peer : options_.peers) {
    {
      std::lock_guard lock{mutex_};
      ++metrics_.vote_requests_sent;
    }
    const auto response = transport_.request_vote(peer, request, options_.rpc_timeout);
    if (!response) {
      continue;
    }
    std::lock_guard lock{mutex_};
    if (response->term > hard_state_.current_term) {
      become_follower_locked(response->term);
      return;
    }
    if (role_ != Role::Candidate || hard_state_.current_term != election_term) {
      return;
    }
    if (response->vote_granted) {
      ++votes;
    }
  }

  bool won = false;
  {
    std::lock_guard lock{mutex_};
    if (role_ == Role::Candidate && hard_state_.current_term == election_term &&
        votes >= quorum_size()) {
      become_leader_locked();
      static_cast<void>(append_local_entry_locked(Command{}));
      persist_log_locked();
      if (quorum_size() == 1U) {
        hard_state_.commit_index = last_log_index_locked();
        apply_committed_locked();
        persist_hard_state_locked();
      }
      won = true;
    }
  }
  if (won && quorum_size() > 1U) {
    static_cast<void>(replicate_all(false));
  }
}

void RaftNode::become_follower_locked(const std::uint64_t term, NodeId leader_id) {
  if (term < hard_state_.current_term) {
    return;
  }
  if (term > hard_state_.current_term) {
    hard_state_.current_term = term;
    hard_state_.voted_for.clear();
  }
  role_ = Role::Follower;
  leader_id_ = std::move(leader_id);
  next_index_.clear();
  match_index_.clear();
  reset_election_deadline_locked();
  persist_hard_state_locked();
  state_changed_.notify_all();
}

void RaftNode::become_leader_locked() {
  role_ = Role::Leader;
  leader_id_ = options_.node_id;
  ++metrics_.leadership_changes;
  const std::uint64_t next = last_log_index_locked() + 1U;
  next_index_.clear();
  match_index_.clear();
  for (const auto& peer : options_.peers) {
    next_index_.emplace(peer.id, next);
    match_index_.emplace(peer.id, 0U);
  }
  heartbeat_deadline_ = std::chrono::steady_clock::now();
  state_changed_.notify_all();
}

void RaftNode::reset_election_deadline_locked() {
  const auto minimum = options_.election_timeout_min.count();
  const auto maximum = options_.election_timeout_max.count();
  std::uniform_int_distribution<std::int64_t> distribution{minimum, maximum};
  election_deadline_ =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{distribution(random_)};
}

void RaftNode::persist_hard_state_locked() { storage_.save_hard_state(hard_state_); }

void RaftNode::persist_log_locked() { storage_.replace_log(log_); }

std::uint64_t RaftNode::last_log_index_locked() const noexcept {
  if (!log_.empty()) {
    return log_.back().index;
  }
  return snapshot_ ? snapshot_->last_included_index : 0U;
}

std::uint64_t RaftNode::last_log_term_locked() const noexcept {
  if (!log_.empty()) {
    return log_.back().term;
  }
  return snapshot_ ? snapshot_->last_included_term : 0U;
}

std::optional<std::uint64_t> RaftNode::term_at_locked(const std::uint64_t index) const noexcept {
  if (index == 0U) {
    return 0U;
  }
  if (snapshot_ && index == snapshot_->last_included_index) {
    return snapshot_->last_included_term;
  }
  const std::uint64_t first_index = snapshot_ ? snapshot_->last_included_index + 1U : 1U;
  if (index < first_index || index > last_log_index_locked()) {
    return std::nullopt;
  }
  const auto offset = static_cast<std::size_t>(index - first_index);
  if (offset >= log_.size()) {
    return std::nullopt;
  }
  return log_[offset].term;
}

std::vector<LogEntry> RaftNode::entries_from_locked(const std::uint64_t index) const {
  if (index > last_log_index_locked()) {
    return {};
  }
  const std::uint64_t first_index = snapshot_ ? snapshot_->last_included_index + 1U : 1U;
  if (index < first_index) {
    return {};
  }
  const auto offset = static_cast<std::size_t>(index - first_index);
  const auto count = std::min(options_.max_append_entries, log_.size() - offset);
  return {log_.begin() + static_cast<std::ptrdiff_t>(offset),
          log_.begin() + static_cast<std::ptrdiff_t>(offset + count)};
}

void RaftNode::truncate_log_from_locked(const std::uint64_t index) {
  const std::uint64_t first_index = snapshot_ ? snapshot_->last_included_index + 1U : 1U;
  if (index <= first_index) {
    log_.clear();
    return;
  }
  if (index > last_log_index_locked()) {
    return;
  }
  const auto offset = static_cast<std::size_t>(index - first_index);
  log_.erase(log_.begin() + static_cast<std::ptrdiff_t>(offset), log_.end());
}

void RaftNode::append_entries_locked(const std::vector<LogEntry>& entries) {
  for (const auto& entry : entries) {
    if (entry.index != last_log_index_locked() + 1U || entry.term == 0U) {
      throw std::runtime_error{"invalid Raft log append"};
    }
    log_.push_back(entry);
  }
}

LogEntry RaftNode::append_local_entry_locked(Command command) {
  if (role_ != Role::Leader || hard_state_.current_term == 0U) {
    throw std::logic_error{"only a leader may append a local Raft entry"};
  }
  LogEntry entry{last_log_index_locked() + 1U, hard_state_.current_term, std::move(command)};
  log_.push_back(entry);
  return entry;
}

bool RaftNode::replicate_all(const bool heartbeat_only) {
  std::lock_guard replication_lock{replication_mutex_};
  std::uint64_t request_term = 0U;
  {
    std::lock_guard lock{mutex_};
    if (role_ != Role::Leader) {
      return false;
    }
    request_term = hard_state_.current_term;
  }

  std::size_t successful_nodes = 1U;
  for (const auto& peer : options_.peers) {
    if (replicate_peer(peer, heartbeat_only)) {
      ++successful_nodes;
    }
  }

  bool commit_advanced = false;
  bool current_leader = false;
  {
    std::lock_guard lock{mutex_};
    current_leader = role_ == Role::Leader && hard_state_.current_term == request_term;
    if (current_leader) {
      commit_advanced = advance_commit_locked();
      if (commit_advanced) {
        apply_committed_locked();
        persist_hard_state_locked();
      }
    }
  }

  if (commit_advanced) {
    for (const auto& peer : options_.peers) {
      static_cast<void>(replicate_peer(peer, true));
    }
  }
  return current_leader && successful_nodes >= quorum_size();
}

bool RaftNode::replicate_peer(const Peer& peer, const bool heartbeat_only) {
  for (std::size_t attempt = 0U; attempt < 256U; ++attempt) {
    std::optional<InstallSnapshotRequest> snapshot_request;
    std::optional<AppendEntriesRequest> append_request;
    std::uint64_t request_term = 0U;
    {
      std::lock_guard lock{mutex_};
      if (role_ != Role::Leader) {
        return false;
      }
      request_term = hard_state_.current_term;
      auto next_iterator = next_index_.find(peer.id);
      if (next_iterator == next_index_.end()) {
        next_iterator = next_index_.emplace(peer.id, last_log_index_locked() + 1U).first;
      }
      const std::uint64_t next = next_iterator->second;
      if (snapshot_ && next <= snapshot_->last_included_index) {
        snapshot_request =
            InstallSnapshotRequest{request_term, options_.node_id, snapshot_->last_included_index,
                                   snapshot_->last_included_term, snapshot_->payload};
      } else {
        const std::uint64_t previous = next == 0U ? 0U : next - 1U;
        const auto previous_term = term_at_locked(previous);
        if (!previous_term) {
          next_iterator->second =
              snapshot_ ? snapshot_->last_included_index : std::max<std::uint64_t>(1U, next - 1U);
          continue;
        }
        append_request = AppendEntriesRequest{request_term,
                                              options_.node_id,
                                              previous,
                                              *previous_term,
                                              heartbeat_only ? std::vector<LogEntry>{}
                                                             : entries_from_locked(next),
                                              hard_state_.commit_index};
        ++metrics_.append_entries_sent;
      }
    }

    if (snapshot_request) {
      const auto response =
          transport_.install_snapshot(peer, *snapshot_request, options_.rpc_timeout);
      std::lock_guard lock{mutex_};
      if (!response) {
        ++metrics_.replication_failures;
        return false;
      }
      if (response->term > hard_state_.current_term) {
        become_follower_locked(response->term);
        return false;
      }
      if (role_ != Role::Leader || hard_state_.current_term != request_term) {
        return false;
      }
      if (response->success) {
        match_index_[peer.id] = response->installed_index;
        next_index_[peer.id] = response->installed_index + 1U;
        return true;
      }
      ++metrics_.replication_failures;
      return false;
    }

    const auto response = transport_.append_entries(peer, *append_request, options_.rpc_timeout);
    std::lock_guard lock{mutex_};
    if (!response) {
      ++metrics_.replication_failures;
      return false;
    }
    if (response->term > hard_state_.current_term) {
      become_follower_locked(response->term);
      return false;
    }
    if (role_ != Role::Leader || hard_state_.current_term != request_term) {
      return false;
    }
    if (response->success) {
      match_index_[peer.id] = response->match_index;
      next_index_[peer.id] = response->match_index + 1U;
      return true;
    }
    ++metrics_.replication_failures;
    const std::uint64_t current_next = next_index_[peer.id];
    const std::uint64_t fallback = response->conflict_index == 0U
                                       ? std::max<std::uint64_t>(1U, current_next - 1U)
                                       : response->conflict_index;
    next_index_[peer.id] = std::min(current_next, fallback);
  }
  return false;
}

bool RaftNode::advance_commit_locked() {
  std::vector<std::uint64_t> replicated;
  replicated.reserve(options_.peers.size() + 1U);
  replicated.push_back(last_log_index_locked());
  for (const auto& peer : options_.peers) {
    const auto iterator = match_index_.find(peer.id);
    replicated.push_back(iterator == match_index_.end() ? 0U : iterator->second);
  }
  std::sort(replicated.begin(), replicated.end());
  const std::uint64_t candidate = replicated[replicated.size() - quorum_size()];
  if (candidate <= hard_state_.commit_index) {
    return false;
  }
  const auto candidate_term = term_at_locked(candidate);
  if (!candidate_term || *candidate_term != hard_state_.current_term) {
    return false;
  }
  hard_state_.commit_index = candidate;
  return true;
}

void RaftNode::apply_committed_locked() {
  while (hard_state_.last_applied < hard_state_.commit_index) {
    const std::uint64_t index = hard_state_.last_applied + 1U;
    if (snapshot_ && index <= snapshot_->last_included_index) {
      hard_state_.last_applied = snapshot_->last_included_index;
      continue;
    }
    const std::uint64_t first_index = snapshot_ ? snapshot_->last_included_index + 1U : 1U;
    if (index < first_index) {
      throw std::runtime_error{"committed Raft entry is no longer available"};
    }
    const auto offset = static_cast<std::size_t>(index - first_index);
    if (offset >= log_.size()) {
      throw std::runtime_error{"committed Raft entry is missing"};
    }
    state_machine_.apply(log_[offset]);
    hard_state_.last_applied = index;
    if (log_[offset].command.type != CommandType::Noop) {
      ++metrics_.committed_commands;
    }
  }
  maybe_create_snapshot_locked();
  state_changed_.notify_all();
}

void RaftNode::maybe_create_snapshot_locked() {
  if (options_.snapshot_threshold_entries == 0U || hard_state_.last_applied == 0U) {
    return;
  }
  const std::uint64_t previous_snapshot = snapshot_ ? snapshot_->last_included_index : 0U;
  if (hard_state_.last_applied - previous_snapshot < options_.snapshot_threshold_entries) {
    return;
  }
  const auto term = term_at_locked(hard_state_.last_applied);
  if (!term) {
    throw std::runtime_error{"cannot determine term for Raft snapshot"};
  }
  Snapshot created{hard_state_.last_applied, *term, state_machine_.create_snapshot()};
  storage_.save_snapshot(created);
  snapshot_ = std::move(created);
  if (!log_.empty()) {
    const auto first_to_keep = std::upper_bound(
        log_.begin(), log_.end(), snapshot_->last_included_index,
        [](const std::uint64_t index, const LogEntry& entry) { return index < entry.index; });
    log_.erase(log_.begin(), first_to_keep);
  }
  persist_log_locked();
  ++metrics_.snapshots_created;
}

std::size_t RaftNode::quorum_size() const noexcept {
  const std::size_t cluster_size = options_.peers.size() + 1U;
  return (cluster_size / 2U) + 1U;
}

const Peer* RaftNode::find_peer(const std::string_view id) const noexcept {
  const auto iterator = std::find_if(options_.peers.begin(), options_.peers.end(),
                                     [id](const Peer& peer) { return peer.id == id; });
  return iterator == options_.peers.end() ? nullptr : &*iterator;
}

} // namespace nebulakv::raft
