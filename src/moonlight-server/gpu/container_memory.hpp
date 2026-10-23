#pragma once

#include "cpu_topology.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wolf::numa {

struct ContainerMemoryBytes {
  std::uint64_t local_bytes = 0;
  std::uint64_t remote_bytes = 0;
};
struct ContainerMemory {
  std::vector<int> local_nodes;
  ContainerMemoryBytes anon;
  // Kernel file accounting includes shmem/tmpfs. Do not add shmem again.
  ContainerMemoryBytes file;
};

using ContainerCpuSets = std::map<std::string, std::vector<unsigned>>;
using ContainerMemories = std::map<std::string, ContainerMemory>;
using ContainerMemoryClock = std::function<std::chrono::steady_clock::time_point()>;
using ContainerMemorySampler = std::function<ContainerMemories(const ContainerCpuSets &)>;

// Every eligible logical CPU must map to exactly one known NUMA node. An empty
// mask means unknown/unrestricted, not locality inferred from its selected GPU.
std::optional<std::vector<int>> container_memory_nodes(const std::vector<unsigned> &cpus, const Topology &topology);

// cgroup v2 memory.numa_stat counters are bytes, not cgroup v1 page counts.
// Require complete, unique anon/file node sets; unrelated (overlapping) rows are
// ignored. Shared pages are charged to their owning cgroup, not every user.
std::optional<ContainerMemory> parse_container_memory(std::istream &input, const std::vector<int> &local_nodes);

// Request-driven, copies share a synchronized 250 ms sample cache. A separate
// host cgroup v2 bind may be required when Wolf has a private cgroup namespace.
// Resolve only exact full Docker IDs in cgroupfs (systemd and cgroupfs drivers),
// including custom parent groups. Discovery is bounded and retried at most every 5 seconds;
// inaccessible, ambiguous, unsupported and malformed inputs remain unavailable.
// Factory creation performs no I/O and never starts background work.
ContainerMemorySampler make_container_memory_sampler(Topology topology,
                                                     std::filesystem::path root = "/host/sys/fs/cgroup",
                                                     ContainerMemoryClock clock = {});

} // namespace wolf::numa
