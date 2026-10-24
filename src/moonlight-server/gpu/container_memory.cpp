#include "container_memory.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
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

bool add(std::uint64_t &total, std::uint64_t amount) {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total)
    return false;
  total += amount;
  return true;
}

bool full_id(std::string_view id) {
  return id.size() == 64 &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool regular(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !error;
}

bool counter_file(const std::filesystem::path &root, const std::filesystem::path &directory) {
  // Recheck cached path components too: a replaced ancestor must not turn a
  // previously valid cgroup into a symlink outside the configured hierarchy.
  auto current = root;
  for (const auto &part : directory.lexically_relative(root)) {
    if (part == "..")
      return false;
    if (part == ".")
      continue;
    current /= part;
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(current, error)) || error)
      return false;
  }
  return regular(current / "memory.numa_stat");
}

using Paths = std::map<std::string, std::filesystem::path>;
Paths discover(const std::filesystem::path &root, const ContainerCpuSets &containers) {
  Paths found;
  if (!regular(root / "cgroup.controllers"))
    return found; // Refuse v1, whose memory.numa_stat has incompatible units/fields.
  std::map<std::string, std::string> names;
  for (const auto &[id, cpus] : containers)
    if (full_id(id)) {
      names.emplace(id, id);
      names.emplace("docker-" + id + ".scope", id);
    }
  if (names.empty())
    return found;
  std::set<std::string> ambiguous;
  std::vector<std::pair<std::filesystem::path, unsigned>> pending{{root, 0}};
  std::size_t visited = 0;
  while (!pending.empty()) {
    auto [parent, depth] = std::move(pending.back());
    pending.pop_back();
    std::error_code error;
    std::filesystem::directory_iterator it(parent, error), end;
    for (; !error && it != end; it.increment(error)) {
      const auto type = it->symlink_status(error);
      if (error)
        return {};
      if (!std::filesystem::is_directory(type))
        continue; // Never follow a symlink outside the mounted hierarchy.
      if (++visited > 16384 || depth >= 32)
        return {}; // A partial search must not appear authoritative.
      pending.emplace_back(it->path(), depth + 1);
      if (auto match = names.find(it->path().filename().string()); match != names.end()) {
        if (!found.emplace(match->second, it->path()).second)
          ambiguous.insert(match->second);
      }
    }
    if (error)
      return {};
  }
  for (const auto &id : ambiguous)
    found.erase(id);
  return found;
}
} // namespace

std::optional<std::vector<int>> container_memory_nodes(const std::vector<unsigned> &cpus, const Topology &topology) {
  if (cpus.empty())
    return {};
  std::set<int> nodes;
  for (auto cpu : cpus) {
    std::optional<int> owner;
    for (const auto &[node, members] : topology.node_cpus)
      if (std::find(members.begin(), members.end(), cpu) != members.end()) {
        if (owner || node > static_cast<unsigned>(std::numeric_limits<int>::max()))
          return {};
        owner = static_cast<int>(node);
      }
    if (!owner)
      return {};
    nodes.insert(*owner);
  }
  return std::vector<int>(nodes.begin(), nodes.end());
}

std::optional<ContainerMemory> parse_container_memory(std::istream &input, const std::vector<int> &local_nodes) {
  if (local_nodes.empty() || std::any_of(local_nodes.begin(), local_nodes.end(), [](int node) { return node < 0; }))
    return {};
  std::string text;
  char character;
  while (input.get(character)) {
    if (text.size() >= 1024 * 1024)
      return {};
    text += character;
  }
  if (input.bad())
    return {};
  std::istringstream lines(text);
  std::optional<std::map<int, std::uint64_t>> anon, file;
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string key;
    if (!(fields >> key))
      continue;
    auto *target = key == "anon" ? &anon : key == "file" ? &file : nullptr;
    if (!target)
      continue;
    if (target->has_value())
      return {};
    target->emplace();
    std::string item;
    while (fields >> item) {
      const auto equal = item.find('=');
      if (!item.starts_with('N') || equal == std::string::npos)
        return {};
      auto node = number<int>(std::string_view(item).substr(1, equal - 1));
      auto amount = number<std::uint64_t>(std::string_view(item).substr(equal + 1));
      if (!node || *node < 0 || item.substr(0, equal) != "N" + std::to_string(*node) || !amount ||
          !(*target)->emplace(*node, *amount).second)
        return {};
    }
    if ((*target)->empty() || (*target)->size() > 4096)
      return {};
  }
  if (!anon || !file || anon->size() != file->size())
    return {};
  ContainerMemory result{.local_nodes = local_nodes, .anon = {}, .file = {}};
  std::sort(result.local_nodes.begin(), result.local_nodes.end());
  result.local_nodes.erase(std::unique(result.local_nodes.begin(), result.local_nodes.end()), result.local_nodes.end());
  auto account = [&](const auto &counters, ContainerMemoryBytes &category) {
    for (const auto &[node, amount] : counters) {
      auto &total = std::binary_search(result.local_nodes.begin(), result.local_nodes.end(), node)
                        ? category.local_bytes
                        : category.remote_bytes;
      if (!add(total, amount))
        return false;
    }
    return true;
  };
  // The paired rows come from one read. Mismatched node inventories are partial
  // data, not proof that the missing node has no memory charged to it.
  for (const auto &[node, amount] : *anon)
    if (!file->contains(node))
      return {};
  if (!account(*anon, result.anon) || !account(*file, result.file))
    return {};
  std::uint64_t total = 0;
  if (!add(total, result.anon.local_bytes) || !add(total, result.anon.remote_bytes) ||
      !add(total, result.file.local_bytes) || !add(total, result.file.remote_bytes))
    return {};
  return result;
}

ContainerMemorySampler
make_container_memory_sampler(Topology topology, std::filesystem::path root, ContainerMemoryClock clock) {
  struct State {
    std::mutex mutex;
    ContainerCpuSets targets;
    std::optional<std::chrono::steady_clock::time_point> sampled, discovered;
    ContainerMemories samples;
    Paths paths;
  };
  auto state = std::make_shared<State>();
  if (!clock)
    clock = [] { return std::chrono::steady_clock::now(); };
  return [state, topology = std::move(topology), root = std::move(root), clock = std::move(clock)](
             const ContainerCpuSets &containers) {
    std::lock_guard lock(state->mutex);
    if (containers.empty()) {
      state->targets.clear();
      state->samples.clear();
      state->paths.clear();
      state->sampled.reset();
      state->discovered.reset();
      return state->samples;
    }
    try {
      const auto now = clock();
      const bool changed = containers != state->targets;
      if (!changed && state->sampled && now >= *state->sampled &&
          now - *state->sampled < std::chrono::milliseconds(250))
        return state->samples;
      // Stable containers reuse resolved paths indefinitely. Retry a missing or
      // moved cgroup at most every five seconds, only while viewers request it.
      bool missing = false;
      for (const auto &[id, cpus] : containers) {
        const auto path = state->paths.find(id);
        if (path == state->paths.end() || !counter_file(root, path->second))
          missing = true;
      }
      if (changed || !state->discovered || now < *state->discovered ||
          (missing && now - *state->discovered >= std::chrono::seconds(5))) {
        state->paths = discover(root, containers);
        state->discovered = now;
      }
      ContainerMemories next;
      for (const auto &[id, cpus] : containers) {
        const auto path = state->paths.find(id);
        const auto nodes = container_memory_nodes(cpus, topology);
        if (path == state->paths.end() || !nodes || !counter_file(root, path->second))
          continue;
        std::ifstream input(path->second / "memory.numa_stat");
        if (input)
          if (auto memory = parse_container_memory(input, *nodes))
            next.emplace(id, std::move(*memory));
      }
      state->samples = std::move(next);
      state->targets = containers;
      state->sampled = now;
    } catch (...) {
      // Optional status sampling must never fail the endpoint or leave stale
      // readings presented as current after a failed refresh.
      state->samples.clear();
      state->sampled.reset();
      state->discovered.reset();
    }
    return state->samples;
  };
}

} // namespace wolf::numa
