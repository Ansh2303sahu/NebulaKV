#pragma once

#include "nebulakv/raft/transport.hpp"
#include "nebulakv/v1/key_value_service.grpc.pb.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

namespace nebulakv::distributed {

class GrpcRaftTransport final : public raft::RaftTransport {
public:
  explicit GrpcRaftTransport(const std::vector<raft::Peer>& peers,
                             std::size_t max_message_bytes = 64U * 1024U * 1024U);

  [[nodiscard]] std::optional<raft::RequestVoteResponse>
  request_vote(const raft::Peer& peer, const raft::RequestVoteRequest& request,
               std::chrono::milliseconds timeout) override;
  [[nodiscard]] std::optional<raft::AppendEntriesResponse>
  append_entries(const raft::Peer& peer, const raft::AppendEntriesRequest& request,
                 std::chrono::milliseconds timeout) override;
  [[nodiscard]] std::optional<raft::InstallSnapshotResponse>
  install_snapshot(const raft::Peer& peer, const raft::InstallSnapshotRequest& request,
                   std::chrono::milliseconds timeout) override;

private:
  [[nodiscard]] v1::RaftService::Stub& stub_for(const raft::Peer& peer);

  std::map<raft::NodeId, std::unique_ptr<v1::RaftService::Stub>, std::less<>> stubs_;
};

} // namespace nebulakv::distributed
