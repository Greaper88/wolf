#include <chrono>
#include <filesystem>
#include <fstream>
#include <gpu/cpu_topology.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace wolf::numa;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void write(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << value;
}

struct Fixture {
  fs::path root;
  TopologyPaths paths;
  fs::path render, bridge, device;
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      root = fs::temp_directory_path() / ("wolf-cpu-topology-" + std::to_string(stamp) + '-' + std::to_string(attempt));
      if (fs::create_directory(root))
        break;
      if (attempt == 99)
        throw std::runtime_error("Cannot create CPU topology fixture");
    }
    paths = {root / "sys/devices/system/cpu",
             root / "sys/devices/system/node",
             root / "proc/cpuinfo",
             root / "sys/class/drm"};
    render = root / "dev/dri/renderD128";
    bridge = root / "sys/devices/pci0000:80/0000:80:03.0";
    device = bridge / "0000:81:00.0";
    write(render, "");
    fs::create_directories(device);
    fs::create_directories(paths.drm_class / "renderD128");
    fs::create_directory_symlink(device, paths.drm_class / "renderD128/device");
    write(device / "numa_node", "4\n");
    write(paths.cpus / "online", "0-3,8-11\n");
    for (auto cpu : {0, 1, 8, 9})
      write(paths.cpus / ("cpu" + std::to_string(cpu)) / "topology/physical_package_id", "2\n");
    for (auto cpu : {2, 3, 10, 11, 12})
      write(paths.cpus / ("cpu" + std::to_string(cpu)) / "topology/physical_package_id", "7\n");
    write(paths.nodes / "has_memory", "0-1,4\n");
    write(paths.nodes / "node0/cpulist", "0,8\n");
    write(paths.nodes / "node1/cpulist", "1,9\n");
    write(paths.nodes / "node4/cpulist", "2-3,10-12\n");
    write(paths.cpuinfo,
          "processor : 2\nmodel name : Second package\n\n"
          "processor : 0\nmodel name : First package\n\n"
          "processor : 1\nmodel name : First package\n\n");
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  Topology topology() const {
    return discover_topology(paths);
  }
  GpuLocality gpu(const Topology &topology) const {
    return gpu_locality(render.string(), topology, paths);
  }
};

void discovery_tests() {
  Fixture f;
  const auto topology = f.topology();
  check(topology.cpus.size() == 2, "physical packages are detected without treating NUMA nodes as sockets");
  check(topology.cpus[0].id == 2 && topology.cpus[1].id == 7, "non-contiguous package IDs are sorted");
  check(topology.cpus[0].cpus == std::vector<unsigned>({0, 1, 8, 9}),
        "SMT logical CPU IDs remain attached to their package");
  check(topology.cpus[0].numa_nodes == std::vector<unsigned>({0, 1}),
        "a physical package may contain multiple NUMA nodes");
  check(topology.cpus[1].numa_nodes == std::vector<unsigned>({4}), "node ID is independent of physical package ID");
  check(topology.cpus[0].name == "First package" && topology.cpus[1].name == "Second package",
        "CPU models follow processor IDs rather than cpuinfo file order");
  check(topology.node_cpus.at(4) == std::vector<unsigned>({2, 3, 10, 11}), "offline CPUs are removed from NUMA masks");
  check(topology.cpus[1].cpus == std::vector<unsigned>({2, 3, 10, 11}), "offline CPUs are removed from package masks");

  write(f.paths.cpus / "cpu1/online", "0\n");
  write(f.paths.cpus / "cpu3/online", "invalid\n");
  const auto offline = f.topology();
  check(offline.cpus[0].cpus == std::vector<unsigned>({0, 8, 9}), "per-CPU offline flag excludes a CPU");
  check(offline.cpus[1].cpus == std::vector<unsigned>({2, 10, 11}), "malformed per-CPU online flag is conservative");
  check(offline.node_cpus.at(1) == std::vector<unsigned>({9}), "NUMA masks match the online package inventory");

  fs::remove(f.paths.cpuinfo);
  check(f.topology().cpus[0].name == "CPU", "missing model information retains a generic CPU label");
  fs::remove(f.paths.cpus / "cpu0/topology/physical_package_id");
  write(f.paths.cpus / "cpu2/topology/physical_package_id", "-1\n");
  write(f.paths.cpus / "cpu8/topology/physical_package_id", "broken\n");
  const auto malformed = f.topology();
  check(malformed.cpus[0].cpus == std::vector<unsigned>({9}), "missing or malformed package IDs are not guessed");
  check(malformed.cpus[1].cpus == std::vector<unsigned>({10, 11}), "unknown package IDs do not become package zero");
  write(f.paths.nodes / "node1/cpulist", "invalid\n");
  check(!f.topology().node_cpus.contains(1), "malformed node CPU list does not become an empty known placement");
  fs::remove(f.paths.cpus / "online");
  check(f.topology().cpus[1].cpus == std::vector<unsigned>({10, 11, 12}),
        "missing global online list uses available per-CPU information");
}

void mapping_tests() {
  Fixture f;
  const auto topology = f.topology();
  check(socket_ids({11, 0, 11, 9}, topology) == std::vector<int>({2, 7}), "CPU masks can span physical sockets");
  check(socket_ids({99999}, topology).empty() && socket_ids({}, topology).empty(),
        "unmapped or empty masks do not invent package assignments");
  check(single_numa_node({11, 2, 11}, topology) == 4, "a restricted CPU mask resolves to its unique NUMA node");
  check(!single_numa_node({0, 1}, topology), "two NUMA nodes on one socket are not reported as one node");
  check(!single_numa_node({0, 99999}, topology), "partially unknown masks do not claim a complete NUMA assignment");
  check(!single_numa_node({}, topology), "empty masks have no NUMA assignment");
  auto ambiguous = topology;
  ambiguous.node_cpus.emplace(99, std::vector<unsigned>{0, 8});
  check(!single_numa_node({0}, ambiguous), "overlapping malformed topology remains ambiguous");

  write(f.paths.cpus / "online", "1024,8192\n");
  write(f.paths.cpus / "cpu1024/topology/physical_package_id", "2\n");
  write(f.paths.cpus / "cpu8192/topology/physical_package_id", "7\n");
  write(f.paths.nodes / "node127/cpulist", "1024,8192\n");
  const auto large = f.topology();
  check(large.cpus.size() == 2 && large.cpus[1].cpus == std::vector<unsigned>({8192}),
        "logical CPU IDs are not limited by fixed cpu_set_t capacity");
  check(single_numa_node({1024, 8192}, large) == 127 && socket_ids({1024, 8192}, large) == std::vector<int>({2, 7}),
        "CPU and NUMA mappings retain large IDs");
}

void gpu_tests() {
  Fixture f;
  const auto topology = f.topology();
  auto gpu = f.gpu(topology);
  check(gpu.numa_node == 4 && gpu.cpu_ids == std::vector<int>({7}), "GPU endpoint NUMA node maps to its physical CPU");
  check(gpu.cpus == std::vector<unsigned>({2, 3, 10, 11}), "GPU locality describes the online host CPUs");
  write(f.device / "numa_node", "-1\n");
  check(!f.gpu(topology).numa_node, "unknown GPU locality remains unknown on a multi-node host");
  write(f.bridge / "numa_node", "1\n");
  gpu = f.gpu(topology);
  check(gpu.numa_node == 1 && gpu.cpu_ids == std::vector<int>({2}),
        "GPU uses nearest PCI ancestor when endpoint is unknown");
  write(f.device / "numa_node", "4\n");
  check(f.gpu(topology).numa_node == 4, "endpoint locality takes precedence over PCI ancestors");
  write(f.device / "numa_node", "99\n");
  gpu = f.gpu(topology);
  check(gpu.numa_node == 99 && gpu.cpus.empty() && gpu.cpu_ids.empty(),
        "known node without CPU topology has no guessed socket");
  check(!gpu_locality("/dev/dri/renderD900", topology, f.paths).numa_node,
        "missing DRM topology does not get a fallback node");
  check(!gpu_locality("/dev/nvidia0", topology, f.paths).numa_node, "non-DRM device paths are rejected");
  fs::create_directories(f.root / "dev/dri/by-path");
  const auto alias = f.root / "dev/dri/by-path/pci-0000:81:00.0-render";
  fs::create_symlink(f.render, alias);
  check(gpu_locality(alias.string(), topology, f.paths).numa_node == 99,
        "stable DRM by-path aliases resolve to their render node");

  fs::remove(f.device / "numa_node");
  fs::remove(f.bridge / "numa_node");
  write(f.paths.nodes / "has_memory", "4\n");
  check(f.gpu(topology).numa_node == 4, "one known memory node provides a safe fallback");
  fs::remove(f.paths.nodes / "has_memory");
  write(f.paths.nodes / "node0/meminfo", "Node 0 MemTotal: 0 kB\n");
  write(f.paths.nodes / "node1/meminfo", "Node 1 MemTotal: 0 kB\n");
  write(f.paths.nodes / "node4/meminfo", "Node 4 MemTotal: 65536 kB\n");
  check(f.gpu(topology).numa_node == 4, "memoryless nodes do not prevent the single-memory-node fallback");
  write(f.paths.nodes / "node1/meminfo", "Node 1 MemTotal: 65536 kB\n");
  check(!f.gpu(topology).numa_node, "multiple memory nodes never guess missing GPU locality");
}

void missing_paths_tests() {
  Fixture f;
  TopologyPaths paths{f.root / "missing/cpus",
                      f.root / "missing/nodes",
                      f.root / "missing/cpuinfo",
                      f.root / "missing/drm"};
  const auto topology = discover_topology(paths);
  check(topology.cpus.empty() && topology.node_cpus.empty(), "missing host files return an empty topology");
  check(!gpu_locality("/dev/dri/renderD128", topology, paths).numa_node,
        "missing host files leave GPU locality unknown");
}
} // namespace

int main() {
  try {
    discovery_tests();
    mapping_tests();
    gpu_tests();
    missing_paths_tests();
    std::cout << checks << " CPU topology checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "CPU topology test failure: " << error.what() << '\n';
    return 1;
  }
}
