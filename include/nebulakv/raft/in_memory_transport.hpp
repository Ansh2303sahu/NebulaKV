#pragma once

#include "nebulakv/raft/transport.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace nebulakv::raft {

class InMemoryRaftNetwork final {
public:
  void register_endpoint(const NodeId& id, RaftEndpoint& endpoint);
  void unregister_endpoint(const NodeId& id);

  void partition(const NodeId& first, const NodeId& second);
  void heal(const NodeId& first, const NodeId& second);
  void heal_all();
  void set_delay(std::chrono::milliseconds delay);
  void drop_next(std::size_t message_count);

  [[nodiscard]] std::optional<RequestVoteResponse> request_vote(const NodeId& source,
                                                                const NodeId& destination,
                                                                const RequestVoteRequest& request,
                                                                std::chrono::milliseconds timeout);
  [[nodiscard]] std::optional<AppendEntriesResponse>
  append_entries(const NodeId& source, const NodeId& destination,
                 const AppendEntriesRequest& request, std::chrono::milliseconds timeout);
  [[nodiscard]] std::optional<InstallSnapshotResponse>
  install_snapshot(const NodeId& source, const NodeId& destination,
                   const InstallSnapshotRequest& request, std::chrono::milliseconds timeout);

private:
  [[nodiscard]] bool should_deliver_locked(const NodeId& source, const NodeId& destination);
  [[nodiscard]] static std::pair<NodeId, NodeId> link_key(const NodeId& first,
                                                          const NodeId& second);

  mutable std::mutex mutex_;
  std::map<NodeId, RaftEndpoint*, std::less<>> endpoints_;
  std::set<std::pair<NodeId, NodeId>> partitions_;
  std::chrono::milliseconds delay_{0};
  std::size_t drops_remaining_{0};
};

class InMemoryRaftTransport final : public RaftTransport {
public:
  InMemoryRaftTransport(NodeId source, InMemoryRaftNetwork& network);

  [[nodiscard]] std::optional<RequestVoteResponse>
  request_vote(const Peer& peer, const RequestVoteRequest& request,
               std::chrono::milliseconds timeout) override;
  [[nodiscard]] std::optional<AppendEntriesResponse>
  append_entries(const Peer& peer, const AppendEntriesRequest& request,
                 std::chrono::milliseconds timeout) override;
  [[nodiscard]] std::optional<InstallSnapshotResponse>
  install_snapshot(const Peer& peer, const InstallSnapshotRequest& request,
                   std::chrono::milliseconds timeout) override;

private:
  NodeId source_;
  InMemoryRaftNetwork& network_;
};

} // namespace nebulakv::raft
