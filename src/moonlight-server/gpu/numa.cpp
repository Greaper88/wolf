#include "numa.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef __linux__
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace wolf::numa {
namespace {
constexpr unsigned max_mask_bits = 1024 * 1024;
#ifdef __linux__
constexpr unsigned word_bits = sizeof(unsigned long) * CHAR_BIT;
#endif

std::string_view trim(std::string_view value) {
  auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<unsigned> sorted(std::vector<unsigned> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

#ifdef __linux__
std::optional<std::string> read_text(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input)
    return std::nullopt;
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad())
    return std::nullopt;
  return text;
}

std::optional<std::string> status_field(const std::string &status, std::string_view field) {
  std::string_view remaining(status);
  while (!remaining.empty()) {
    auto end = remaining.find('\n');
    auto line = remaining.substr(0, end);
    if (line.starts_with(field) && line.size() > field.size() && line[field.size()] == ':')
      return std::string(trim(line.substr(field.size() + 1)));
    if (end == std::string_view::npos)
      break;
    remaining.remove_prefix(end + 1);
  }
  return std::nullopt;
}

std::optional<int> node_at(const std::filesystem::path &device) {
  auto text = read_text(device / "numa_node");
  if (!text)
    return std::nullopt;
  auto value = trim(*text);
  if (value.empty())
    return std::nullopt;
  int node = -1;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), node);
  return error == std::errc{} && end == value.data() + value.size() && node >= 0 ? std::optional(node) : std::nullopt;
}

std::vector<unsigned> memory_nodes(const Paths &paths) {
  if (auto memory = read_text(paths.nodes / "has_memory"))
    return parse_list(*memory);
  // Older kernels may omit has_memory. Only count nodes with actual RAM, rather
  // than treating an arbitrary single CPU-only node as a memory locality domain.
  std::vector<unsigned> nodes;
  std::error_code error;
  std::filesystem::directory_iterator it(paths.nodes, error), end;
  for (; !error && it != end; it.increment(error)) {
    auto name = it->path().filename().string();
    if (!name.starts_with("node") || name.size() == 4)
      continue;
    unsigned node;
    auto [last, ec] = std::from_chars(name.data() + 4, name.data() + name.size(), node);
    if (ec != std::errc{} || last != name.data() + name.size())
      continue;
    auto info = read_text(it->path() / "meminfo");
    if (!info)
      continue;
    auto total = info->find("MemTotal:");
    if (total == std::string::npos)
      continue;
    auto value = trim(std::string_view(*info).substr(total + std::string_view("MemTotal:").size()));
    unsigned long long bytes = 0;
    auto [last_value, value_error] = std::from_chars(value.data(), value.data() + value.size(), bytes);
    if (value_error == std::errc{} && last_value != value.data() && bytes > 0)
      nodes.push_back(node);
  }
  return sorted(std::move(nodes));
}

std::string system_error(std::string_view operation, int error) {
  return std::string(operation) + ": " + std::error_code(error, std::generic_category()).message();
}

bool affinity(std::vector<unsigned long> &mask, std::string &error) {
  for (unsigned bits = 1024; bits <= max_mask_bits; bits *= 2) {
    mask.assign((bits + word_bits - 1) / word_bits, 0);
    if (syscall(SYS_sched_getaffinity, 0, mask.size() * sizeof(unsigned long), mask.data()) >= 0)
      return true;
    if (errno != EINVAL) {
      error = system_error("sched_getaffinity", errno);
      return false;
    }
  }
  error = "CPU affinity mask exceeds supported size";
  return false;
}

bool bit_set(const std::vector<unsigned long> &mask, unsigned bit) {
  return bit / word_bits < mask.size() && (mask[bit / word_bits] & (1UL << (bit % word_bits)));
}

std::vector<unsigned long> make_mask(const std::vector<unsigned> &values, std::size_t minimum_words = 0) {
  auto maximum = values.empty() ? 0 : *std::max_element(values.begin(), values.end());
  if (maximum >= max_mask_bits)
    throw std::invalid_argument("CPU or memory node ID exceeds supported mask size");
  std::vector<unsigned long> mask(std::max(minimum_words, static_cast<std::size_t>(maximum / word_bits + 1)), 0);
  for (auto value : values)
    mask[value / word_bits] |= 1UL << (value % word_bits);
  return mask;
}
#endif
} // namespace

std::vector<unsigned> parse_list(std::string_view input) {
  input = trim(input);
  std::vector<unsigned> values;
  if (input.empty())
    return values;
  while (true) {
    auto comma = input.find(',');
    auto entry = trim(input.substr(0, comma));
    auto dash = entry.find('-');
    auto number = [](std::string_view text) {
      text = trim(text);
      if (text.empty())
        throw std::invalid_argument("Invalid CPU or memory node list");
      unsigned value = 0;
      auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("Invalid CPU or memory node list");
      return value;
    };
    auto first = number(entry.substr(0, dash));
    auto last = dash == std::string_view::npos ? first : number(entry.substr(dash + 1));
    if (last < first || static_cast<unsigned long long>(last) - first + values.size() + 1 > max_mask_bits)
      throw std::invalid_argument("Invalid or excessively large CPU or memory node range");
    for (auto value = first;; ++value) {
      values.push_back(value);
      if (value == last)
        break;
    }
    if (comma == std::string_view::npos)
      return sorted(std::move(values));
    input.remove_prefix(comma + 1);
  }
}

std::string format_list(const std::vector<unsigned> &input) {
  auto values = sorted(input);
  std::string result;
  for (std::size_t i = 0; i < values.size(); ++i) {
    auto first = values[i], last = first;
    while (i + 1 < values.size() && last < std::numeric_limits<unsigned>::max() && values[i + 1] == last + 1)
      last = values[++i];
    if (!result.empty())
      result += ',';
    result += std::to_string(first);
    if (last != first)
      result += '-' + std::to_string(last);
  }
  return result;
}

std::vector<unsigned> intersect_lists(const std::vector<unsigned> &left, const std::vector<unsigned> &right) {
  auto a = sorted(left), b = sorted(right);
  std::vector<unsigned> result;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
  return result;
}

bool enabled() {
  auto value = std::getenv("WOLF_NUMA_AWARE");
  if (!value)
    return true;
  std::string flag(trim(value));
  std::transform(flag.begin(), flag.end(), flag.begin(), [](unsigned char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
  });
  if (flag == "FALSE" || flag == "0" || flag == "OFF" || flag == "NO")
    return false;
  if (flag == "TRUE" || flag == "1" || flag == "ON" || flag == "YES")
    return true;
  throw std::invalid_argument("WOLF_NUMA_AWARE must be a boolean");
}

Mode read_mode() {
  auto value = std::getenv("WOLF_NUMA_MODE");
  if (!value)
    return Mode::loose;
  std::string flag(trim(value));
  std::transform(flag.begin(), flag.end(), flag.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
  });
  if (flag == "loose")
    return Mode::loose;
  if (flag == "strict")
    return Mode::strict;
  throw std::invalid_argument("WOLF_NUMA_MODE must be loose or strict");
}

const char *mode_name(Mode mode) {
  return mode == Mode::strict ? "strict" : "loose";
}

Resolution resolve(std::string_view render_node, const Paths &paths) {
#ifdef __linux__
  try {
    std::error_code error;
    auto render = std::filesystem::canonical(std::filesystem::path(render_node), error);
    auto name = (error ? std::filesystem::path(render_node) : render).filename().string();
    if (!name.starts_with("renderD") || name.size() == 7 ||
        name.find_first_not_of("0123456789", 7) != std::string::npos)
      return {{}, "Selected GPU has no canonical DRM render node", false};
    auto device = std::filesystem::canonical(paths.drm_class / name / "device", error);
    if (error)
      return {{}, "Selected GPU PCI topology is unavailable", false};
    std::optional<int> node;
    for (auto current = device; !current.empty(); current = current.parent_path()) {
      if ((node = node_at(current)))
        break;
      if (current == current.parent_path())
        break;
    }
    auto memory = memory_nodes(paths);
    if (!node && memory.size() == 1 && memory.front() <= static_cast<unsigned>(std::numeric_limits<int>::max()))
      node = static_cast<int>(memory.front());
    if (!node)
      return {{}, "Selected GPU NUMA node is unknown on this topology", false};
    auto text = read_text(paths.status);
    if (!text)
      return {{}, "Process CPU and memory allowances are unavailable", true};
    auto allowed_cpus = status_field(*text, "Cpus_allowed_list");
    auto allowed_memory = status_field(*text, "Mems_allowed_list");
    if (!allowed_cpus || !allowed_memory)
      return {{}, "Process CPU and memory allowances are incomplete", true};
    auto local_text = read_text(paths.nodes / ("node" + std::to_string(*node)) / "cpulist");
    if (!local_text)
      return {{}, "Selected GPU NUMA node has no CPU topology", true};
    auto eligible_cpus = parse_list(*allowed_cpus);
    auto eligible_memory = parse_list(*allowed_memory);
    auto cpus = intersect_lists(parse_list(*local_text), eligible_cpus);
    auto local_memory = intersect_lists({static_cast<unsigned>(*node)}, eligible_memory);
    if (cpus.empty())
      return {{}, "Selected GPU NUMA node has no allowed local CPUs", true};
    if (local_memory.empty() || (!memory.empty() && !std::binary_search(memory.begin(), memory.end(), *node)))
      return {{}, "Selected GPU NUMA node has no allowed local RAM", true};
    return {Placement{*node,
                      std::move(cpus),
                      std::move(local_memory),
                      std::move(eligible_cpus),
                      std::move(eligible_memory)},
            {},
            false};
  } catch (const std::invalid_argument &error) {
    return {{}, std::string("Invalid NUMA topology or process allowance: ") + error.what(), true};
  }
#else
  (void)render_node;
  (void)paths;
  return {{}, "NUMA placement is unavailable on this platform", false};
#endif
}

struct ThreadScope::State {
  bool cpu_applied = false, memory_applied = false;
  std::string cpu_error, memory_error;
#ifdef __linux__
  std::vector<unsigned long> original_cpu, original_memory;
  int original_mode = MPOL_DEFAULT;
  unsigned memory_bits = 0;
  bool restore_cpu = false;
#endif
  ~State() noexcept {
#ifdef __linux__
#if defined(SYS_set_mempolicy)
    if (memory_applied) {
      auto *mask = original_mode == MPOL_DEFAULT ? nullptr : original_memory.data();
      syscall(SYS_set_mempolicy, original_mode, mask, mask ? memory_bits : 0UL);
    }
#endif
    if (restore_cpu)
      syscall(SYS_sched_setaffinity, 0, original_cpu.size() * sizeof(unsigned long), original_cpu.data());
#endif
  }
};

ThreadScope::ThreadScope(const Placement &placement, Mode mode, bool respect_current_affinity)
    : ThreadScope(std::optional(placement), mode, respect_current_affinity) {}

ThreadScope::ThreadScope(const std::optional<Placement> &placement, Mode mode, bool respect_current_affinity)
    : state_(std::make_unique<State>()) {
  if (!placement)
    return;
#ifdef __linux__
  if (affinity(state_->original_cpu, state_->cpu_error)) {
    std::vector<unsigned> cpus;
    for (auto cpu : placement->cpus)
      if (!respect_current_affinity || bit_set(state_->original_cpu, cpu))
        cpus.push_back(cpu);
    if (cpus.empty()) {
      state_->cpu_error = "Placement has no CPUs allowed by the current thread";
    } else {
      auto mask = make_mask(cpus, state_->original_cpu.size());
      if (syscall(SYS_sched_setaffinity, 0, mask.size() * sizeof(unsigned long), mask.data()) == 0) {
        state_->restore_cpu = true;
        if (mode == Mode::strict) {
          state_->cpu_applied = true;
        } else if (syscall(SYS_sched_setaffinity,
                           0,
                           state_->original_cpu.size() * sizeof(unsigned long),
                           state_->original_cpu.data()) == 0) {
          state_->cpu_applied = true;
          state_->restore_cpu = false;
        } else {
          state_->cpu_error = system_error("Restore CPU eligibility after local placement", errno);
          // Returning would leave a loose worker temporarily hard-bound for its
          // lifetime. Abort construction; State retries the original mask while
          // unwinding instead of allowing the caller to continue in that state.
          throw std::runtime_error(state_->cpu_error);
        }
      } else {
        state_->cpu_error = system_error("sched_setaffinity", errno);
      }
    }
  }
  if (!state_->cpu_applied) {
    state_->memory_error = "Memory placement was not applied because CPU placement failed";
    return;
  }
#if defined(SYS_get_mempolicy) && defined(SYS_set_mempolicy)
  if (placement->memory_nodes.empty()) {
    state_->memory_error = "Placement has no memory nodes";
    return;
  }
  for (unsigned bits = 1024; bits <= max_mask_bits; bits *= 2) {
    state_->original_memory.assign((bits + word_bits - 1) / word_bits, 0);
    // Linux counts the nodemask's upper bound after subtracting one; include
    // that sentinel so a node at the last bit of a word is not truncated.
    if (syscall(SYS_get_mempolicy, &state_->original_mode, state_->original_memory.data(), bits + 1UL, nullptr, 0UL) ==
        0) {
      state_->memory_bits = bits + 1;
      break;
    }
    if (errno != EINVAL) {
      state_->memory_error = system_error("get_mempolicy", errno);
      return;
    }
  }
  if (!state_->memory_bits) {
    state_->memory_error = "Memory policy mask exceeds supported size";
    return;
  }
  auto mask = make_mask(placement->memory_nodes, state_->original_memory.size());
  auto bits = mask.size() * word_bits + 1;
  auto policy = mode == Mode::strict ? MPOL_BIND : MPOL_PREFERRED;
  if (syscall(SYS_set_mempolicy, policy | MPOL_F_STATIC_NODES, mask.data(), bits) == 0)
    state_->memory_applied = true;
  else
    state_->memory_error = system_error("set_mempolicy", errno);
#else
  state_->memory_error = "Memory policy syscalls are unavailable on this platform";
#endif
#else
  (void)mode;
  (void)respect_current_affinity;
  state_->cpu_error = "CPU placement is unavailable on this platform";
  state_->memory_error = "Memory placement is unavailable on this platform";
#endif
}

ThreadScope::~ThreadScope() noexcept = default;

bool ThreadScope::cpu_applied() const {
  return state_->cpu_applied;
}
bool ThreadScope::memory_applied() const {
  return state_->memory_applied;
}
const std::string &ThreadScope::cpu_error() const {
  return state_->cpu_error;
}
const std::string &ThreadScope::memory_error() const {
  return state_->memory_error;
}

} // namespace wolf::numa
