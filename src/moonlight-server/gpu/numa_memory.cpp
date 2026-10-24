#include "numa_memory.hpp"
#include "numa.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wolf::numa {
namespace {
template <class T> std::optional<T> number(std::string_view text) {
  T value{};
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::optional<int> node_id(std::string_view name) {
  if (!name.starts_with("node"))
    return std::nullopt;
  auto id = number<int>(name.substr(4));
  if (!id || *id < 0 || name != "node" + std::to_string(*id))
    return std::nullopt;
  return id;
}

std::set<int> node_ids(const Topology &topology, const std::filesystem::path &nodes) {
  std::set<int> ids;
  for (const auto &[node, cpus] : topology.node_cpus)
    if (node <= static_cast<unsigned>(std::numeric_limits<int>::max()))
      ids.insert(static_cast<int>(node));
  // CPU-less nodes can hold memory even though they have no package owner.
  std::ifstream memory_list(nodes / "has_memory");
  std::string list;
  if (std::getline(memory_list, list)) {
    try {
      for (auto node : parse_list(list))
        if (node <= static_cast<unsigned>(std::numeric_limits<int>::max()))
          ids.insert(static_cast<int>(node));
    } catch (const std::invalid_argument &) {
      // Directory discovery still preserves nodes if this optional list is bad.
    }
  }
  std::error_code error;
  std::filesystem::directory_iterator it(nodes, error), end;
  for (; !error && it != end; it.increment(error))
    if (auto id = node_id(it->path().filename().string()))
      ids.insert(*id);
  return ids;
}

std::vector<int> node_owners(const std::vector<unsigned> &cpus, const Topology &topology) {
  // A partly missing package inventory must not turn shared/unknown memory into
  // an apparently exclusive package total in a dashboard consumer.
  for (auto cpu : cpus) {
    const bool mapped = std::any_of(topology.cpus.begin(), topology.cpus.end(), [cpu](const auto &socket) {
      return std::find(socket.cpus.begin(), socket.cpus.end(), cpu) != socket.cpus.end();
    });
    if (!mapped)
      return {};
  }
  return socket_ids(cpus, topology);
}

void read_node(MemoryNode &node, const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input)
    return;
  std::optional<std::uint64_t> total, free;
  bool invalid = false;
  std::string line;
  while (std::getline(input, line)) {
    // Ignore unrelated kernel fields, but validate the full line for either
    // required field, including its node prefix, units and duplicate entries.
    const bool total_line = line.find("MemTotal:") != std::string::npos;
    const bool free_line = line.find("MemFree:") != std::string::npos;
    if (!total_line && !free_line)
      continue;
    std::istringstream fields(line);
    std::string prefix, id_text, key, amount_text, unit, extra;
    if (!(fields >> prefix >> id_text >> key >> amount_text >> unit) || fields >> extra || prefix != "Node" ||
        unit != "kB") {
      invalid = true;
      continue;
    }
    auto id = number<int>(id_text);
    auto amount = number<std::uint64_t>(amount_text);
    if (!id || *id != node.id || !amount || *amount > std::numeric_limits<std::uint64_t>::max() / 1024) {
      invalid = true;
      continue;
    }
    auto *target = key == "MemTotal:" ? &total : key == "MemFree:" ? &free : nullptr;
    if (!target || target->has_value()) {
      invalid = true;
      continue;
    }
    *target = *amount * 1024;
  }
  if (input.bad() || invalid || !total || !free || *free > *total)
    return;
  node.total_bytes = *total;
  node.used_bytes = *total - *free;
  if (*total > 0)
    node.used_percent = 100.0 * static_cast<double>(*node.used_bytes) / static_cast<double>(*total);
}
} // namespace

std::vector<MemoryNode> read_memory(const Topology &topology, std::filesystem::path nodes) {
  std::vector<MemoryNode> result;
  for (auto id : node_ids(topology, nodes)) {
    MemoryNode node{id, {}, {}, {}, {}};
    if (auto cpus = topology.node_cpus.find(static_cast<unsigned>(id)); cpus != topology.node_cpus.end())
      node.cpu_ids = node_owners(cpus->second, topology);
    read_node(node, nodes / ("node" + std::to_string(id)) / "meminfo");
    result.push_back(std::move(node));
  }
  return result;
}

MemorySampler make_memory_sampler(Topology topology, std::filesystem::path nodes, MemoryClock clock) {
  struct State {
    std::mutex mutex;
    std::optional<std::chrono::steady_clock::time_point> sampled;
    std::vector<MemoryNode> sample;
  };
  auto state = std::make_shared<State>();
  for (const auto &[node, cpus] : topology.node_cpus)
    if (node <= static_cast<unsigned>(std::numeric_limits<int>::max()))
      state->sample.push_back({static_cast<int>(node), node_owners(cpus, topology), {}, {}, {}});
  if (!clock)
    clock = [] { return std::chrono::steady_clock::now(); };
  return [state, topology = std::move(topology), nodes = std::move(nodes), clock = std::move(clock)] {
    std::lock_guard lock(state->mutex);
    try {
      const auto now = clock();
      if (!state->sampled || now < *state->sampled || now - *state->sampled >= std::chrono::milliseconds(250)) {
        state->sample = read_memory(topology, nodes);
        state->sampled = now;
      }
    } catch (...) {
      // Optional monitoring must not fail the status endpoint or retain stale
      // usage. Preserve known node identities and retry on the next request.
      state->sampled.reset();
      for (auto &node : state->sample) {
        node.total_bytes.reset();
        node.used_bytes.reset();
        node.used_percent.reset();
      }
    }
    return state->sample;
  };
}

} // namespace wolf::numa
