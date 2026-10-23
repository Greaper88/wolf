#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wolf::numa {

struct CpuSocket {
  int id;
  std::string name;
  std::vector<unsigned> cpus;
  std::vector<unsigned> numa_nodes;
};

struct TopologyPaths {
  std::filesystem::path cpus = "/sys/devices/system/cpu";
  std::filesystem::path nodes = "/sys/devices/system/node";
  std::filesystem::path cpuinfo = "/proc/cpuinfo";
  std::filesystem::path drm_class = "/sys/class/drm";
};

struct Topology {
  std::vector<CpuSocket> cpus;
  std::map<unsigned, std::vector<unsigned>> node_cpus;
};

struct GpuLocality {
  std::optional<int> numa_node;
  std::vector<unsigned> cpus;
  std::vector<int> cpu_ids;
};

// Physical package IDs and logical CPU IDs are distinct from NUMA node IDs.
// Discovery reports online host topology, independently of Wolf's placement mode or cpuset.
Topology discover_topology(const TopologyPaths &paths = {});
GpuLocality gpu_locality(std::string_view render_node, const Topology &topology, const TopologyPaths &paths = {});
std::vector<int> socket_ids(const std::vector<unsigned> &mask, const Topology &topology);
std::optional<int> single_numa_node(const std::vector<unsigned> &mask, const Topology &topology);

} // namespace wolf::numa
