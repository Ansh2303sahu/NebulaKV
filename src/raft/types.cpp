#include "nebulakv/raft/types.hpp"

namespace nebulakv::raft {

std::string_view to_string(const Role role) noexcept {
  switch (role) {
  case Role::Follower:
    return "follower";
  case Role::Candidate:
    return "candidate";
  case Role::Leader:
    return "leader";
  }
  return "unknown";
}

} // namespace nebulakv::raft
