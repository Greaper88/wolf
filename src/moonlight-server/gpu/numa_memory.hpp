#pragma once

#include "cpu_topology.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace wolf::numa {

struct MemoryNode {
  int id;
  // Physical package IDs with CPUs on this node. Empty or shared ownership is
  // retained instead of assigning the same memory total to several packages.
  // A partially unknown CPU-to-package mapping has unknown (empty) ownership.
  std::vector<int> cpu_ids;
  std::optional<std::uint64_t> total_bytes;
  std::optional<std::uint64_t> used_bytes;
  std::optional<double> used_percent;
};

// Node MemTotal is OS-usable memory, not installed DIMM capacity. Used is
// MemTotal - MemFree and includes cache/reclaimable memory. It is not a process
// allocation measurement or an estimate of node-local MemAvailable.
std::vector<MemoryNode> read_memory(const Topology &topology, std::filesystem::path nodes = "/sys/devices/system/node");

using MemoryClock = std::function<std::chrono::steady_clock::time_point()>;
using MemorySampler = std::function<std::vector<MemoryNode>()>;

// Creation does no I/O. Copies share a synchronized 250 ms cache. Each refresh
// reads a node's meminfo once and needs no background worker or counter baseline.
MemorySampler make_memory_sampler(Topology topology,
                                  std::filesystem::path nodes = "/sys/devices/system/node",
                                  MemoryClock clock = {});

} // namespace wolf::numa
