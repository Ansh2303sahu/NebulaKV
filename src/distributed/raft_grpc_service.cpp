#include "nebulakv/distributed/raft_grpc_service.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nebulakv::distributed {

namespace {

[[nodiscard]] raft::Command decode_command(const v1::RaftCommand& command) {
  raft::Command decoded;
  switch (command.type()) {
  case v1::RaftCommand::NOOP:
    decoded.type = raft::CommandType::Noop;
    break;
  case v1::RaftCommand::PUT:
    decoded.type = raft::CommandType::Put;
    break;
  case v1::RaftCommand::DELETE:
    decoded.type = raft::CommandType::Delete;
    break;
  default:
    throw std::invalid_argument{"invalid replicated command type"};
  }
  decoded.key = command.key();
  decoded.value = command.value();
  return decoded;
}

} // namespace

RaftServiceImpl::RaftServiceImpl(raft::RaftNode& node) : node_{node} {}

grpc::Status RaftServiceImpl::RequestVote(grpc::ServerContext* context,
                                          const v1::RequestVoteRequest* request,
                                          v1::RequestVoteResponse* response) {
  if (context->IsCancelled()) {
    return {grpc::StatusCode::CANCELLED, "vote request cancelled"};
  }
  const auto result =
      node_.handle_request_vote({request->term(), request->candidate_id(),
                                 request->last_log_index(), request->last_log_term()});
  response->set_term(result.term);
  response->set_vote_granted(result.vote_granted);
  return grpc::Status::OK;
}

grpc::Status RaftServiceImpl::AppendEntries(grpc::ServerContext* context,
                                            const v1::AppendEntriesRequest* request,
                                            v1::AppendEntriesResponse* response) {
  if (context->IsCancelled()) {
    return {grpc::StatusCode::CANCELLED, "append request cancelled"};
  }
  raft::AppendEntriesRequest decoded;
  decoded.term = request->term();
  decoded.leader_id = request->leader_id();
  decoded.previous_log_index = request->previous_log_index();
  decoded.previous_log_term = request->previous_log_term();
  decoded.leader_commit = request->leader_commit();
  decoded.entries.reserve(static_cast<std::size_t>(request->entries_size()));
  try {
    for (const auto& entry : request->entries()) {
      decoded.entries.push_back({entry.index(), entry.term(), decode_command(entry.command())});
    }
  } catch (const std::exception& error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  }
  const auto result = node_.handle_append_entries(decoded);
  response->set_term(result.term);
  response->set_success(result.success);
  response->set_match_index(result.match_index);
  response->set_conflict_index(result.conflict_index);
  return grpc::Status::OK;
}

grpc::Status RaftServiceImpl::InstallSnapshot(grpc::ServerContext* context,
                                              const v1::InstallSnapshotRequest* request,
                                              v1::InstallSnapshotResponse* response) {
  if (context->IsCancelled()) {
    return {grpc::StatusCode::CANCELLED, "snapshot request cancelled"};
  }
  const auto result = node_.handle_install_snapshot(
      {request->term(), request->leader_id(), request->last_included_index(),
       request->last_included_term(), request->payload()});
  response->set_term(result.term);
  response->set_success(result.success);
  response->set_installed_index(result.installed_index);
  return grpc::Status::OK;
}

} // namespace nebulakv::distributed
