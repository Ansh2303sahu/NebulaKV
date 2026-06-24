#pragma once

#include "nebulakv/raft/types.hpp"

#include <chrono>
#include <optional>

namespace nebulakv::raft {

class RaftEndpoint {
public:
  virtual ~RaftEndpoint() = default;

  [[nodiscard]] virtual RequestVoteResponse
  handle_request_vote(const RequestVoteRequest& request) = 0;
  [[nodiscard]] virtual AppendEntriesResponse
  handle_append_entries(const AppendEntriesRequest& request) = 0;
  [[nodiscard]] virtual InstallSnapshotResponse
  handle_install_snapshot(const InstallSnapshotRequest& request) = 0;
};

class RaftTransport {
public:
  virtual ~RaftTransport() = default;

  [[nodiscard]] virtual std::optional<RequestVoteResponse>
  request_vote(const Peer& peer, const RequestVoteRequest& request,
               std::chrono::milliseconds timeout) = 0;
  [[nodiscard]] virtual std::optional<AppendEntriesResponse>
  append_entries(const Peer& peer, const AppendEntriesRequest& request,
                 std::chrono::milliseconds timeout) = 0;
  [[nodiscard]] virtual std::optional<InstallSnapshotResponse>
  install_snapshot(const Peer& peer, const InstallSnapshotRequest& request,
                   std::chrono::milliseconds timeout) = 0;
};

} // namespace nebulakv::raft
