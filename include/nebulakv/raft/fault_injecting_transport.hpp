#pragma once

#include "nebulakv/raft/transport.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>

namespace nebulakv::raft {

struct FaultInjectionOptions {
  double drop_probability{0.0};
  std::chrono::milliseconds fixed_delay{0};
  std::uint64_t random_seed{1U};
};

class FaultInjectingTransport final : public RaftTransport {
public:
  FaultInjectingTransport(RaftTransport& delegate, FaultInjectionOptions options = {});

  void configure(FaultInjectionOptions options);

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
  [[nodiscard]] bool should_drop();
  [[nodiscard]] std::chrono::milliseconds delay() const;

  RaftTransport& delegate_;
  mutable std::mutex mutex_;
  FaultInjectionOptions options_;
  std::mt19937_64 random_;
};

} // namespace nebulakv::raft
