#include "nebulakv/distributed/grpc_raft_transport.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace nebulakv::distributed {

namespace {

[[nodiscard]] int checked_message_size(const std::size_t bytes) {
  if (bytes == 0U || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument{"maximum gRPC message size is invalid"};
  }
  return static_cast<int>(bytes);
}

void fill_command(v1::RaftCommand* target, const raft::Command& source) {
  switch (source.type) {
  case raft::CommandType::Noop:
    target->set_type(v1::RaftCommand::NOOP);
    break;
  case raft::CommandType::Put:
    target->set_type(v1::RaftCommand::PUT);
    break;
  case raft::CommandType::Delete:
    target->set_type(v1::RaftCommand::DELETE);
    break;
  }
  target->set_key(source.key);
  target->set_value(source.value);
}

} // namespace

GrpcRaftTransport::GrpcRaftTransport(const std::vector<raft::Peer>& peers,
                                     const std::size_t max_message_bytes) {
  grpc::ChannelArguments arguments;
  const int maximum = checked_message_size(max_message_bytes);
  arguments.SetMaxReceiveMessageSize(maximum);
  arguments.SetMaxSendMessageSize(maximum);
  for (const auto& peer : peers) {
    auto channel =
        grpc::CreateCustomChannel(peer.address, grpc::InsecureChannelCredentials(), arguments);
    stubs_.emplace(peer.id, v1::RaftService::NewStub(std::move(channel)));
  }
}

std::optional<raft::RequestVoteResponse>
GrpcRaftTransport::request_vote(const raft::Peer& peer, const raft::RequestVoteRequest& request,
                                const std::chrono::milliseconds timeout) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  v1::RequestVoteRequest rpc_request;
  rpc_request.set_term(request.term);
  rpc_request.set_candidate_id(request.candidate_id);
  rpc_request.set_last_log_index(request.last_log_index);
  rpc_request.set_last_log_term(request.last_log_term);
  v1::RequestVoteResponse rpc_response;
  const grpc::Status status = stub_for(peer).RequestVote(&context, rpc_request, &rpc_response);
  if (!status.ok()) {
    return std::nullopt;
  }
  return raft::RequestVoteResponse{rpc_response.term(), rpc_response.vote_granted()};
}

std::optional<raft::AppendEntriesResponse>
GrpcRaftTransport::append_entries(const raft::Peer& peer, const raft::AppendEntriesRequest& request,
                                  const std::chrono::milliseconds timeout) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  v1::AppendEntriesRequest rpc_request;
  rpc_request.set_term(request.term);
  rpc_request.set_leader_id(request.leader_id);
  rpc_request.set_previous_log_index(request.previous_log_index);
  rpc_request.set_previous_log_term(request.previous_log_term);
  rpc_request.set_leader_commit(request.leader_commit);
  for (const auto& entry : request.entries) {
    auto* rpc_entry = rpc_request.add_entries();
    rpc_entry->set_index(entry.index);
    rpc_entry->set_term(entry.term);
    fill_command(rpc_entry->mutable_command(), entry.command);
  }
  v1::AppendEntriesResponse rpc_response;
  const grpc::Status status = stub_for(peer).AppendEntries(&context, rpc_request, &rpc_response);
  if (!status.ok()) {
    return std::nullopt;
  }
  return raft::AppendEntriesResponse{rpc_response.term(), rpc_response.success(),
                                     rpc_response.match_index(), rpc_response.conflict_index()};
}

std::optional<raft::InstallSnapshotResponse>
GrpcRaftTransport::install_snapshot(const raft::Peer& peer,
                                    const raft::InstallSnapshotRequest& request,
                                    const std::chrono::milliseconds timeout) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  v1::InstallSnapshotRequest rpc_request;
  rpc_request.set_term(request.term);
  rpc_request.set_leader_id(request.leader_id);
  rpc_request.set_last_included_index(request.last_included_index);
  rpc_request.set_last_included_term(request.last_included_term);
  rpc_request.set_payload(request.payload);
  v1::InstallSnapshotResponse rpc_response;
  const grpc::Status status = stub_for(peer).InstallSnapshot(&context, rpc_request, &rpc_response);
  if (!status.ok()) {
    return std::nullopt;
  }
  return raft::InstallSnapshotResponse{rpc_response.term(), rpc_response.success(),
                                       rpc_response.installed_index()};
}

v1::RaftService::Stub& GrpcRaftTransport::stub_for(const raft::Peer& peer) {
  const auto iterator = stubs_.find(peer.id);
  if (iterator == stubs_.end()) {
    throw std::invalid_argument{"unknown Raft peer: " + peer.id};
  }
  return *iterator->second;
}

} // namespace nebulakv::distributed
