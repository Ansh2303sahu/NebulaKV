#include "nebulakv/raft/fault_injecting_transport.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nebulakv::raft {

namespace {

void validate_options(const FaultInjectionOptions& options) {
  if (options.drop_probability < 0.0 || options.drop_probability > 1.0) {
    throw std::invalid_argument{"drop probability must be between zero and one"};
  }
  if (options.fixed_delay < std::chrono::milliseconds{0}) {
    throw std::invalid_argument{"fault injection delay must not be negative"};
  }
}

} // namespace

FaultInjectingTransport::FaultInjectingTransport(RaftTransport& delegate,
                                                 FaultInjectionOptions options)
    : delegate_{delegate}, options_{options}, random_{options.random_seed} {
  validate_options(options_);
}

void FaultInjectingTransport::configure(FaultInjectionOptions options) {
  validate_options(options);
  std::lock_guard lock{mutex_};
  options_ = options;
  random_.seed(options.random_seed);
}

std::optional<RequestVoteResponse>
FaultInjectingTransport::request_vote(const Peer& peer, const RequestVoteRequest& request,
                                      const std::chrono::milliseconds timeout) {
  const auto injected_delay = delay();
  if (should_drop() || injected_delay > timeout) {
    std::this_thread::sleep_for(std::min(injected_delay, timeout));
    return std::nullopt;
  }
  std::this_thread::sleep_for(injected_delay);
  return delegate_.request_vote(peer, request, timeout - injected_delay);
}

std::optional<AppendEntriesResponse>
FaultInjectingTransport::append_entries(const Peer& peer, const AppendEntriesRequest& request,
                                        const std::chrono::milliseconds timeout) {
  const auto injected_delay = delay();
  if (should_drop() || injected_delay > timeout) {
    std::this_thread::sleep_for(std::min(injected_delay, timeout));
    return std::nullopt;
  }
  std::this_thread::sleep_for(injected_delay);
  return delegate_.append_entries(peer, request, timeout - injected_delay);
}

std::optional<InstallSnapshotResponse>
FaultInjectingTransport::install_snapshot(const Peer& peer, const InstallSnapshotRequest& request,
                                          const std::chrono::milliseconds timeout) {
  const auto injected_delay = delay();
  if (should_drop() || injected_delay > timeout) {
    std::this_thread::sleep_for(std::min(injected_delay, timeout));
    return std::nullopt;
  }
  std::this_thread::sleep_for(injected_delay);
  return delegate_.install_snapshot(peer, request, timeout - injected_delay);
}

bool FaultInjectingTransport::should_drop() {
  std::lock_guard lock{mutex_};
  std::bernoulli_distribution distribution{options_.drop_probability};
  return distribution(random_);
}

std::chrono::milliseconds FaultInjectingTransport::delay() const {
  std::lock_guard lock{mutex_};
  return options_.fixed_delay;
}

} // namespace nebulakv::raft
