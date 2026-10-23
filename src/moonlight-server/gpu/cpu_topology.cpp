#include "cpu_topology.hpp"
#include "numa.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace wolf::numa {
namespace {
std::string_view trim(std::string_view value) {
  auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<std::string> read_text(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input)
    return std::nullopt;
  std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return input.bad() ? std::nullopt : std::optional(std::move(value));
}

template <class T> std::optional<T> number(std::string_view value) {
  value = trim(value);
  if (value.empty())
    return std::nullopt;
  T result{};
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size())
    return std::nullopt;
  return result;
}

std::optional<std::vector<unsigned>> read_list(const std::filesystem::path &path) {
  auto text = read_text(path);
  if (!text)
    return std::nullopt;
  try {
    return parse_list(*text);
  } catch (const std::invalid_argument &) {
    return std::nullopt;
  }
}

std::optional<unsigned> entry_id(const std::filesystem::path &path, std::string_view prefix) {
  const auto name = path.filename().string();
  if (!name.starts_with(prefix) || name.size() == prefix.size())
    return std::nullopt;
  return number<unsigned>(std::string_view(name).substr(prefix.size()));
}

std::map<unsigned, std::string> cpu_names(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::map<unsigned, std::string> names;
  std::optional<unsigned> cpu;
  std::string name, line;
  auto flush = [&] {
    if (cpu && !name.empty())
      names[*cpu] = name;
    cpu.reset();
    name.clear();
  };
  while (std::getline(input, line)) {
    auto value = trim(line);
    if (value.empty()) {
      flush();
      continue;
    }
    auto separator = value.find(':');
    if (separator == std::string_view::npos)
      continue;
    auto key = trim(value.substr(0, separator));
    value = trim(value.substr(separator + 1));
    if (key == "processor")
      cpu = number<unsigned>(value);
    else if (key == "model name" || key == "Processor")
      name = value;
  }
  flush();
  return names;
}

std::vector<unsigned> memory_nodes(const TopologyPaths &paths) {
  if (auto nodes = read_list(paths.nodes / "has_memory"))
    return *nodes;
  std::vector<unsigned> nodes;
  std::error_code error;
  std::filesystem::directory_iterator it(paths.nodes, error), end;
  for (; !error && it != end; it.increment(error)) {
    auto node = entry_id(it->path(), "node");
    auto info = read_text(it->path() / "meminfo");
    if (!node || !info)
      continue;
    auto start = info->find("MemTotal:");
    if (start == std::string::npos)
      continue;
    auto value = trim(std::string_view(*info).substr(start + std::string_view("MemTotal:").size()));
    unsigned long long total = 0;
    auto [last, ec] = std::from_chars(value.data(), value.data() + value.size(), total);
    if (ec == std::errc{} && last != value.data() && total > 0)
      nodes.push_back(*node);
  }
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

std::optional<int> node_at(const std::filesystem::path &device) {
  auto text = read_text(device / "numa_node");
  auto node = text ? number<int>(*text) : std::nullopt;
  return node && *node >= 0 ? node : std::nullopt;
}
} // namespace

Topology discover_topology(const TopologyPaths &paths) {
  Topology result;
  auto online = read_list(paths.cpus / "online");
  const auto names = cpu_names(paths.cpuinfo);
  std::vector<unsigned> online_cpus;
  std::map<int, CpuSocket> sockets;
  std::error_code error;
  std::filesystem::directory_iterator it(paths.cpus, error), end;
  for (; !error && it != end; it.increment(error)) {
    auto cpu = entry_id(it->path(), "cpu");
    if (!cpu || (online && !std::binary_search(online->begin(), online->end(), *cpu)))
      continue;
    // CPU 0 and non-hotpluggable CPUs do not necessarily have an online file.
    if (auto flag = read_text(it->path() / "online"); flag && trim(*flag) != "1")
      continue;
    online_cpus.push_back(*cpu);
    auto text = read_text(it->path() / "topology/physical_package_id");
    auto package = text ? number<int>(*text) : std::nullopt;
    if (!package || *package < 0)
      continue;
    auto [socket, inserted] = sockets.try_emplace(*package, CpuSocket{*package, {}, {}, {}});
    socket->second.cpus.push_back(*cpu);
  }
  std::sort(online_cpus.begin(), online_cpus.end());
  error.clear();
  it = std::filesystem::directory_iterator(paths.nodes, error);
  for (; !error && it != end; it.increment(error)) {
    auto node = entry_id(it->path(), "node");
    if (!node)
      continue;
    auto cpus = read_list(it->path() / "cpulist");
    if (cpus)
      result.node_cpus.emplace(*node, intersect_lists(*cpus, online_cpus));
  }
  for (auto &[id, socket] : sockets) {
    std::sort(socket.cpus.begin(), socket.cpus.end());
    for (auto cpu : socket.cpus) {
      if (auto name = names.find(cpu); name != names.end()) {
        socket.name = name->second;
        break;
      }
    }
    if (socket.name.empty())
      socket.name = "CPU";
    for (const auto &[node, cpus] : result.node_cpus)
      if (!intersect_lists(socket.cpus, cpus).empty())
        socket.numa_nodes.push_back(node);
    result.cpus.push_back(std::move(socket));
  }
  return result;
}

std::vector<int> socket_ids(const std::vector<unsigned> &mask, const Topology &topology) {
  std::vector<int> result;
  for (const auto &socket : topology.cpus)
    if (!intersect_lists(mask, socket.cpus).empty())
      result.push_back(socket.id);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::optional<int> single_numa_node(const std::vector<unsigned> &mask, const Topology &topology) {
  if (mask.empty())
    return std::nullopt;
  const auto normalized = intersect_lists(mask, mask);
  std::optional<int> result;
  for (const auto &[node, cpus] : topology.node_cpus) {
    if (node > static_cast<unsigned>(std::numeric_limits<int>::max()))
      continue;
    if (intersect_lists(normalized, cpus) == normalized) {
      if (result)
        return std::nullopt;
      result = static_cast<int>(node);
    }
  }
  return result;
}

GpuLocality gpu_locality(std::string_view render_node, const Topology &topology, const TopologyPaths &paths) {
  std::error_code error;
  auto render = std::filesystem::canonical(std::filesystem::path(render_node), error);
  auto name = (error ? std::filesystem::path(render_node) : render).filename().string();
  if (!name.starts_with("renderD") || name.size() == 7 || name.find_first_not_of("0123456789", 7) != std::string::npos)
    return {};
  auto device = std::filesystem::canonical(paths.drm_class / name / "device", error);
  if (error)
    return {};
  std::optional<int> node;
  for (auto current = device; !current.empty(); current = current.parent_path()) {
    if ((node = node_at(current)))
      break;
    if (current == current.parent_path())
      break;
  }
  if (!node) {
    auto memory = memory_nodes(paths);
    if (memory.size() == 1 && memory.front() <= static_cast<unsigned>(std::numeric_limits<int>::max()))
      node = static_cast<int>(memory.front());
  }
  if (!node)
    return {};
  GpuLocality result{node, {}, {}};
  if (auto cpus = topology.node_cpus.find(*node); cpus != topology.node_cpus.end()) {
    result.cpus = cpus->second;
    result.cpu_ids = socket_ids(result.cpus, topology);
  }
  return result;
}

} // namespace wolf::numa
