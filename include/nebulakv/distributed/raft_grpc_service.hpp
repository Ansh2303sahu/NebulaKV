#pragma once

#include "nebulakv/raft/node.hpp"
#include "nebulakv/v1/key_value_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>

namespace nebulakv::distributed {

class RaftServiceImpl final : public v1::RaftService::Service {
public:
  explicit RaftServiceImpl(raft::RaftNode& node);

  grpc::Status RequestVote(grpc::ServerContext* context, const v1::RequestVoteRequest* request,
                           v1::RequestVoteResponse* response) override;
  grpc::Status AppendEntries(grpc::ServerContext* context, const v1::AppendEntriesRequest* request,
                             v1::AppendEntriesResponse* response) override;
  grpc::Status InstallSnapshot(grpc::ServerContext* context,
                               const v1::InstallSnapshotRequest* request,
                               v1::InstallSnapshotResponse* response) override;

private:
  raft::RaftNode& node_;
};

} // namespace nebulakv::distributed
