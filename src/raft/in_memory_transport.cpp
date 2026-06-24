#include "nebulakv/raft/in_memory_transport.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nebulakv::raft {

void InMemoryRaftNetwork::register_endpoint(const NodeId& id, RaftEndpoint& endpoint) {
  if (id.empty()) {
    throw std::invalid_argument{"in-memory Raft endpoint id must not be empty"};
  }
  std::lock_guard lock{mutex_};
  const auto [iterator, inserted] = endpoints_.emplace(id, &endpoint);
  static_cast<void>(iterator);
  if (!inserted) {
    throw std::invalid_argument{"duplicate in-memory Raft endpoint: " + id};
  }
}

void InMemoryRaftNetwork::unregister_endpoint(const NodeId& id) {
  std::lock_guard lock{mutex_};
  endpoints_.erase(id);
}

void InMemoryRaftNetwork::partition(const NodeId& first, const NodeId& second) {
  std::lock_guard lock{mutex_};
  partitions_.insert(link_key(first, second));
}

void InMemoryRaftNetwork::heal(const NodeId& first, const NodeId& second) {
  std::lock_guard lock{mutex_};
  partitions_.erase(link_key(first, second));
}

void InMemoryRaftNetwork::heal_all() {
  std::lock_guard lock{mutex_};
  partitions_.clear();
}

void InMemoryRaftNetwork::set_delay(const std::chrono::milliseconds delay) {
  if (delay < std::chrono::milliseconds{0}) {
    throw std::invalid_argument{"network delay must not be negative"};
  }
  std::lock_guard lock{mutex_};
  delay_ = delay;
}

void InMemoryRaftNetwork::drop_next(const std::size_t message_count) {
  std::lock_guard lock{mutex_};
  drops_remaining_ = message_count;
}

std::optional<RequestVoteResponse>
InMemoryRaftNetwork::request_vote(const NodeId& source, const NodeId& destination,
                                  const RequestVoteRequest& request,
                                  const std::chrono::milliseconds timeout) {
  RaftEndpoint* endpoint = nullptr;
  std::chrono::milliseconds delay{0};
  {
    std::lock_guard lock{mutex_};
    if (!should_deliver_locked(source, destination)) {
      return std::nullopt;
    }
    const auto iterator = endpoints_.find(destination);
    if (iterator == endpoints_.end()) {
      return std::nullopt;
    }
    endpoint = iterator->second;
    delay = delay_;
  }
  if (delay > timeout) {
    std::this_thread::sleep_for(timeout);
    return std::nullopt;
  }
  std::this_thread::sleep_for(delay);
  return endpoint->handle_request_vote(request);
}

std::optional<AppendEntriesResponse>
InMemoryRaftNetwork::append_entries(const NodeId& source, const NodeId& destination,
                                    const AppendEntriesRequest& request,
                                    const std::chrono::milliseconds timeout) {
  RaftEndpoint* endpoint = nullptr;
  std::chrono::milliseconds delay{0};
  {
    std::lock_guard lock{mutex_};
    if (!should_deliver_locked(source, destination)) {
      return std::nullopt;
    }
    const auto iterator = endpoints_.find(destination);
    if (iterator == endpoints_.end()) {
      return std::nullopt;
    }
    endpoint = iterator->second;
    delay = delay_;
  }
  if (delay > timeout) {
    std::this_thread::sleep_for(timeout);
    return std::nullopt;
  }
  std::this_thread::sleep_for(delay);
  return endpoint->handle_append_entries(request);
}

std::optional<InstallSnapshotResponse>
InMemoryRaftNetwork::install_snapshot(const NodeId& source, const NodeId& destination,
                                      const InstallSnapshotRequest& request,
                                      const std::chrono::milliseconds timeout) {
  RaftEndpoint* endpoint = nullptr;
  std::chrono::milliseconds delay{0};
  {
    std::lock_guard lock{mutex_};
    if (!should_deliver_locked(source, destination)) {
      return std::nullopt;
    }
    const auto iterator = endpoints_.find(destination);
    if (iterator == endpoints_.end()) {
      return std::nullopt;
    }
    endpoint = iterator->second;
    delay = delay_;
  }
  if (delay > timeout) {
    std::this_thread::sleep_for(timeout);
    return std::nullopt;
  }
  std::this_thread::sleep_for(delay);
  return endpoint->handle_install_snapshot(request);
}

bool InMemoryRaftNetwork::should_deliver_locked(const NodeId& source, const NodeId& destination) {
  if (partitions_.contains(link_key(source, destination))) {
    return false;
  }
  if (drops_remaining_ != 0U) {
    --drops_remaining_;
    return false;
  }
  return true;
}

std::pair<NodeId, NodeId> InMemoryRaftNetwork::link_key(const NodeId& first, const NodeId& second) {
  return std::minmax(first, second);
}

InMemoryRaftTransport::InMemoryRaftTransport(NodeId source, InMemoryRaftNetwork& network)
    : source_{std::move(source)}, network_{network} {
  if (source_.empty()) {
    throw std::invalid_argument{"in-memory Raft transport source must not be empty"};
  }
}

std::optional<RequestVoteResponse>
InMemoryRaftTransport::request_vote(const Peer& peer, const RequestVoteRequest& request,
                                    const std::chrono::milliseconds timeout) {
  return network_.request_vote(source_, peer.id, request, timeout);
}

std::optional<AppendEntriesResponse>
InMemoryRaftTransport::append_entries(const Peer& peer, const AppendEntriesRequest& request,
                                      const std::chrono::milliseconds timeout) {
  return network_.append_entries(source_, peer.id, request, timeout);
}

std::optional<InstallSnapshotResponse>
InMemoryRaftTransport::install_snapshot(const Peer& peer, const InstallSnapshotRequest& request,
                                        const std::chrono::milliseconds timeout) {
  return network_.install_snapshot(source_, peer.id, request, timeout);
}

} // namespace nebulakv::raft
