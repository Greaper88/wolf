#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wolf::numa {

struct Placement {
  int node = -1;
  std::vector<unsigned> cpus;
  std::vector<unsigned> memory_nodes;
  // Full leader allowances let loose guests borrow remote resources without
  // escaping the operator's limits when Docker creates a sibling cgroup.
  std::vector<unsigned> allowed_cpus = {};
  std::vector<unsigned> allowed_memory_nodes = {};
};

enum class Mode {
  loose,
  strict
};
Mode read_mode();
const char *mode_name(Mode mode);

struct Paths {
  std::filesystem::path drm_class = "/sys/class/drm";
  std::filesystem::path nodes = "/sys/devices/system/node";
  // Use the leader's original allowance, not an already scoped caller's affinity.
  // A stream worker may switch from one GPU's node to another when joining a lobby.
  std::filesystem::path status = "/proc/self/status";
};

struct Resolution {
  std::optional<Placement> placement;
  std::string reason;
  // A known local resource is excluded; silently using remote resources would violate placement.
  bool conflict = false;
};

// Linux list syntax, for example "0-3,8,12-15". Invalid input throws invalid_argument.
std::vector<unsigned> parse_list(std::string_view value);
std::string format_list(const std::vector<unsigned> &values);
std::vector<unsigned> intersect_lists(const std::vector<unsigned> &left, const std::vector<unsigned> &right);

// WOLF_NUMA_AWARE defaults to true. resolve() itself is independent of that switch.
bool enabled();
Resolution resolve(std::string_view render_node, const Paths &paths = {});

// Applies only to the current thread. Loose mode seeds execution on a local CPU,
// immediately restores the eligible CPU mask, and prefers local RAM with fallback.
// Strict mode keeps a local CPU mask and binds RAM until scope exit. New children
// inherit the policy; existing shared buffers and driver threads are unaffected.
// Always restores the previous policy on exit.
class ThreadScope {
public:
  // A reused task thread may have inherited another GPU's narrower affinity.
  // By default use the resolved leader allowance, then restore the caller's exact mask.
  // Opt in to intersection only for an intentional additional per-thread restriction.
  explicit ThreadScope(const std::optional<Placement> &placement,
                       Mode mode = read_mode(),
                       bool respect_current_affinity = false);
  explicit ThreadScope(const Placement &placement, Mode mode = read_mode(), bool respect_current_affinity = false);
  ~ThreadScope() noexcept;
  ThreadScope(const ThreadScope &) = delete;
  ThreadScope &operator=(const ThreadScope &) = delete;

  bool cpu_applied() const;
  bool memory_applied() const;
  const std::string &cpu_error() const;
  const std::string &memory_error() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace wolf::numa
