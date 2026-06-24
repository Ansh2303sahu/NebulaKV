#include "nebulakv/raft/in_memory_transport.hpp"
#include "nebulakv/raft/node.hpp"
#include "nebulakv/raft/state_machine.hpp"
#include "nebulakv/raft/storage.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nebulakv::raft {

namespace {

using namespace std::chrono_literals;

struct TestNode {
  RaftStorage storage;
  DurableKeyValueStateMachine state_machine;
  InMemoryRaftTransport transport;
  RaftNode node;

  TestNode(const std::filesystem::path& root, RaftNodeOptions options, InMemoryRaftNetwork& network)
      : storage{root / options.node_id / "raft"},
        state_machine{DurableStateMachineOptions{root / options.node_id}},
        transport{options.node_id, network},
        node{std::move(options), storage, state_machine, transport} {}
};

[[nodiscard]] std::vector<Peer> peers_for(const std::string& local) {
  std::vector<Peer> peers;
  for (const std::string id : {"node-1", "node-2", "node-3"}) {
    if (id != local) {
      peers.push_back({id, id + ":5001"});
    }
  }
  return peers;
}

[[nodiscard]] RaftNodeOptions options_for(const std::string& id, const std::uint64_t seed,
                                          const std::uint64_t threshold = 100U) {
  RaftNodeOptions options;
  options.node_id = id;
  options.peers = peers_for(id);
  options.election_timeout_min = 150ms;
  options.election_timeout_max = 250ms;
  options.heartbeat_interval = 30ms;
  options.rpc_timeout = 50ms;
  options.random_seed = seed;
  options.snapshot_threshold_entries = threshold;
  return options;
}

} // namespace

TEST(RaftNodeTest, ElectsLeaderAndReplicatesCommittedWrite) {
  test::TemporaryDirectory directory;
  InMemoryRaftNetwork network;
  TestNode first{directory.path(), options_for("node-1", 1U), network};
  TestNode second{directory.path(), options_for("node-2", 2U), network};
  TestNode third{directory.path(), options_for("node-3", 3U), network};
  network.register_endpoint("node-1", first.node);
  network.register_endpoint("node-2", second.node);
  network.register_endpoint("node-3", third.node);

  first.node.trigger_election();
  ASSERT_TRUE(first.node.is_leader());
  const SubmitResult result = first.node.submit({CommandType::Put, "alpha", "one"}, 500ms);
  ASSERT_TRUE(result.committed()) << result.message;
  first.node.trigger_replication();

  EXPECT_EQ(first.state_machine.get("alpha"), std::optional<std::string>{"one"});
  EXPECT_EQ(second.state_machine.get("alpha"), std::optional<std::string>{"one"});
  EXPECT_EQ(third.state_machine.get("alpha"), std::optional<std::string>{"one"});
  EXPECT_GE(first.node.status().commit_index, result.log_index);
}

TEST(RaftNodeTest, RefusesWriteWithoutAQuorum) {
  test::TemporaryDirectory directory;
  InMemoryRaftNetwork network;
  TestNode first{directory.path(), options_for("node-1", 1U), network};
  TestNode second{directory.path(), options_for("node-2", 2U), network};
  TestNode third{directory.path(), options_for("node-3", 3U), network};
  network.register_endpoint("node-1", first.node);
  network.register_endpoint("node-2", second.node);
  network.register_endpoint("node-3", third.node);
  first.node.trigger_election();
  ASSERT_TRUE(first.node.is_leader());

  network.partition("node-1", "node-2");
  network.partition("node-1", "node-3");
  const SubmitResult result = first.node.submit({CommandType::Put, "isolated", "value"}, 80ms);

  EXPECT_EQ(result.status, SubmitStatus::Timeout);
  EXPECT_FALSE(first.state_machine.get("isolated").has_value());
}

TEST(RaftNodeTest, NewLeaderRepairsConflictingUncommittedSuffix) {
  test::TemporaryDirectory directory;
  InMemoryRaftNetwork network;
  TestNode first{directory.path(), options_for("node-1", 1U), network};
  TestNode second{directory.path(), options_for("node-2", 2U), network};
  TestNode third{directory.path(), options_for("node-3", 3U), network};
  network.register_endpoint("node-1", first.node);
  network.register_endpoint("node-2", second.node);
  network.register_endpoint("node-3", third.node);
  first.node.trigger_election();
  ASSERT_TRUE(first.node.is_leader());
  ASSERT_TRUE(first.node.submit({CommandType::Put, "stable", "one"}, 500ms).committed());

  network.partition("node-1", "node-2");
  network.partition("node-1", "node-3");
  EXPECT_EQ(first.node.submit({CommandType::Put, "lost", "old"}, 60ms).status,
            SubmitStatus::Timeout);

  second.node.trigger_election();
  ASSERT_TRUE(second.node.is_leader());
  ASSERT_TRUE(second.node.submit({CommandType::Put, "winner", "new"}, 500ms).committed());
  network.heal_all();
  second.node.trigger_replication();
  second.node.trigger_replication();

  EXPECT_FALSE(first.state_machine.get("lost").has_value());
  EXPECT_EQ(first.state_machine.get("winner"), std::optional<std::string>{"new"});
}

TEST(RaftNodeTest, SendsSnapshotToFollowerThatFallsBehindRetainedLog) {
  test::TemporaryDirectory directory;
  InMemoryRaftNetwork network;
  TestNode first{directory.path(), options_for("node-1", 1U, 3U), network};
  TestNode second{directory.path(), options_for("node-2", 2U, 3U), network};
  TestNode third{directory.path(), options_for("node-3", 3U, 3U), network};
  network.register_endpoint("node-1", first.node);
  network.register_endpoint("node-2", second.node);
  network.register_endpoint("node-3", third.node);
  first.node.trigger_election();
  ASSERT_TRUE(first.node.is_leader());

  network.partition("node-1", "node-3");
  ASSERT_TRUE(first.node.submit({CommandType::Put, "a", "1"}, 500ms).committed());
  ASSERT_TRUE(first.node.submit({CommandType::Put, "b", "2"}, 500ms).committed());
  ASSERT_TRUE(first.node.submit({CommandType::Put, "c", "3"}, 500ms).committed());
  EXPECT_GT(first.node.status().snapshot_index, 0U);

  network.heal("node-1", "node-3");
  for (int attempt = 0; attempt < 5; ++attempt) {
    first.node.trigger_replication();
  }

  EXPECT_EQ(third.state_machine.get("a"), std::optional<std::string>{"1"});
  EXPECT_EQ(third.state_machine.get("c"), std::optional<std::string>{"3"});
  EXPECT_GT(third.node.status().metrics.snapshots_installed, 0U);
}

TEST(RaftNodeTest, QuorumReadBarrierRejectsIsolatedLeader) {
  test::TemporaryDirectory directory;
  InMemoryRaftNetwork network;
  TestNode first{directory.path(), options_for("node-1", 1U), network};
  TestNode second{directory.path(), options_for("node-2", 2U), network};
  TestNode third{directory.path(), options_for("node-3", 3U), network};
  network.register_endpoint("node-1", first.node);
  network.register_endpoint("node-2", second.node);
  network.register_endpoint("node-3", third.node);
  first.node.trigger_election();
  ASSERT_TRUE(first.node.is_leader());

  EXPECT_TRUE(first.node.confirm_leadership(std::chrono::steady_clock::now() + 500ms));

  network.partition("node-1", "node-2");
  network.partition("node-1", "node-3");
  EXPECT_FALSE(first.node.confirm_leadership(std::chrono::steady_clock::now() + 80ms));
}

TEST(RaftNodeTest, AcknowledgedWriteSurvivesNodeRestart) {
  test::TemporaryDirectory directory;
  InMemoryRaftNetwork network;
  const auto options = [] {
    RaftNodeOptions result;
    result.node_id = "single";
    result.election_timeout_min = 150ms;
    result.election_timeout_max = 250ms;
    result.heartbeat_interval = 30ms;
    result.rpc_timeout = 50ms;
    result.random_seed = 11U;
    result.snapshot_threshold_entries = 100U;
    return result;
  }();

  {
    RaftStorage storage{directory.path() / "single" / "raft"};
    DurableKeyValueStateMachine state_machine{
        DurableStateMachineOptions{directory.path() / "single"}};
    InMemoryRaftTransport transport{"single", network};
    RaftNode node{options, storage, state_machine, transport};
    network.register_endpoint("single", node);
    node.trigger_election();
    ASSERT_TRUE(node.submit({CommandType::Put, "durable", "yes"}, 500ms).committed());
    network.unregister_endpoint("single");
  }

  RaftStorage storage{directory.path() / "single" / "raft"};
  DurableKeyValueStateMachine state_machine{
      DurableStateMachineOptions{directory.path() / "single"}};
  InMemoryRaftTransport transport{"single", network};
  RaftNode restarted{options, storage, state_machine, transport};
  EXPECT_EQ(state_machine.get("durable"), std::optional<std::string>{"yes"});
  EXPECT_GE(restarted.status().commit_index, 2U);
}

} // namespace nebulakv::raft
