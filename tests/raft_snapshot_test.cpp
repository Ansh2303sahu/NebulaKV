#include "nebulakv/raft/state_machine.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace nebulakv::raft {

TEST(RaftSnapshotTest, RestoresLogicalStateIntoFreshLsmStorage) {
  test::TemporaryDirectory directory;
  DurableStateMachineOptions options;
  options.directory = directory.path() / "node";
  DurableKeyValueStateMachine state_machine{options};

  state_machine.recover(std::nullopt, {{1U, 1U, {CommandType::Put, "alpha", "one"}},
                                       {2U, 1U, {CommandType::Put, "beta", "two"}},
                                       {3U, 1U, {CommandType::Delete, "alpha", {}}}});
  const std::string snapshot = state_machine.create_snapshot();

  DurableStateMachineOptions restored_options;
  restored_options.directory = directory.path() / "restored";
  DurableKeyValueStateMachine restored{restored_options};
  restored.recover(Snapshot{3U, 1U, snapshot}, {});

  EXPECT_FALSE(restored.get("alpha").has_value());
  ASSERT_TRUE(restored.get("beta").has_value());
  EXPECT_EQ(*restored.get("beta"), "two");
  EXPECT_EQ(restored.size(), 1U);
}

TEST(RaftSnapshotTest, RejectsTruncatedStateSnapshot) {
  test::TemporaryDirectory directory;
  DurableStateMachineOptions options;
  options.directory = directory.path() / "node";
  DurableKeyValueStateMachine state_machine{options};
  state_machine.recover(std::nullopt, {});

  EXPECT_THROW(state_machine.install_snapshot("short"), std::runtime_error);
}

} // namespace nebulakv::raft
