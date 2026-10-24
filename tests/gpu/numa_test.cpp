#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <gpu/numa.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef __linux__
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace wolf::numa;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
#ifdef __linux__
void write(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << value;
}
std::string status_field(const fs::path &status, const std::string &key) {
  std::ifstream input(status);
  std::string line;
  while (std::getline(input, line))
    if (line.starts_with(key + ':'))
      return line.substr(key.size() + 1);
  throw std::runtime_error("Missing test process status field");
}
#endif

void list_tests() {
  check(parse_list(" 0-2,8,32-34 \n") == std::vector<unsigned>({0, 1, 2, 8, 32, 33, 34}), "sparse list parsing");
  check(parse_list("8,2-4,3,0") == std::vector<unsigned>({0, 2, 3, 4, 8}), "list normalization");
  check(parse_list(" \n").empty(), "empty kernel list is valid");
  check(format_list({8, 3, 2, 0, 1, 33, 32, 3}) == "0-3,8,32-33", "canonical sparse list formatting");
  check(format_list({}).empty(), "empty list formatting");
  check(format_list({std::numeric_limits<unsigned>::max()}) == std::to_string(std::numeric_limits<unsigned>::max()),
        "largest unsigned ID formats without overflow");
  check(intersect_lists({7, 1, 1, 33}, {33, 7, 9}) == std::vector<unsigned>({7, 33}), "sparse intersection");
  for (const auto *text : {"-1", "1-", "1,", ",1", "4-2", "1-2-3", "1;2", "4294967296", "0-4294967295"}) {
    bool rejected = false;
    try {
      parse_list(text);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    check(rejected, "malformed or unbounded list rejected");
  }
}

#ifdef __linux__
struct Fixture {
  fs::path root;
  Paths paths;
  fs::path render, bridge, device;
  Fixture() {
    auto pattern = (fs::temp_directory_path() / "wolf-numa-test-XXXXXX").string();
    auto made = mkdtemp(pattern.data());
    if (!made)
      throw std::runtime_error("Cannot create NUMA fixture");
    root = made;
    paths = {root / "sys/class/drm", root / "sys/devices/system/node", root / "proc/self/status"};
    render = root / "dev/dri/renderD128";
    bridge = root / "sys/devices/pci0000:80/0000:80:03.0";
    device = bridge / "0000:81:00.0";
    write(render, "");
    fs::create_directories(device);
    fs::create_directories(paths.drm_class / "renderD128");
    fs::create_directory_symlink(device, paths.drm_class / "renderD128/device");
    write(paths.nodes / "has_memory", "2,8\n");
    write(paths.nodes / "node2/cpulist", "0-3,32-35\n");
    write(paths.nodes / "node8/cpulist", "4-7,36-39\n");
    write(paths.status, "Name:\twolf\nCpus_allowed_list:\t1-2,33,36-37\nMems_allowed_list:\t2,8\n");
    write(device / "numa_node", "2\n");
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  Resolution get(Mode mode = Mode::on) const {
    return resolve(render.string(), paths, mode);
  }
};

void discovery_tests() {
  Fixture f;
  auto local = f.get();
  check(local.placement && local.placement->node == 2 && !local.conflict, "GPU node resolved from PCI device");
  check(local.placement->cpus == std::vector<unsigned>({1, 2, 33}), "process cpuset limits local CPUs and SMT IDs");
  check(local.placement->memory_nodes == std::vector<unsigned>({2}), "RAM constrained to GPU-local node");
  check(local.placement->allowed_cpus == std::vector<unsigned>({1, 2, 33, 36, 37}),
        "guest allowance preserves the leader's full CPU restrictions");
  check(local.placement->allowed_memory_nodes == std::vector<unsigned>({2, 8}),
        "guest allowance retains remote RAM within the leader's restrictions");
  write(f.device / "numa_node", "8\n");
  local = f.get();
  check(local.placement && local.placement->cpus == std::vector<unsigned>({36, 37}), "second GPU maps independently");
  write(f.paths.status, "Cpus_allowed_list:\t0-3\nMems_allowed_list:\t2,8\n");
  local = f.get();
  check(!local.placement && local.conflict, "known node with excluded CPUs is a conflict");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\nMems_allowed_list:\t2\n");
  local = f.get();
  check(!local.placement && local.conflict, "known node with excluded RAM is a conflict");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\nMems_allowed_list:\t2,8\n");
  write(f.device / "numa_node", "-1\n");
  local = f.get();
  check(!local.placement && !local.conflict, "unknown node is not coerced to zero on multi-node host");
  write(f.bridge / "numa_node", "8\n");
  local = f.get();
  check(local.placement && local.placement->node == 8, "nearest PCI ancestor supplies missing locality");
  write(f.device / "numa_node", "2\n");
  check(f.get().placement->node == 2, "endpoint locality takes priority over ancestor");
  fs::remove(f.device / "numa_node");
  fs::remove(f.bridge / "numa_node");
  write(f.paths.nodes / "has_memory", "8\n");
  local = f.get();
  check(local.placement && local.placement->node == 8, "single memory node safely supplies fallback");
  fs::remove(f.paths.nodes / "has_memory");
  write(f.paths.nodes / "node2/meminfo", "Node 2 MemTotal: 0 kB\n");
  write(f.paths.nodes / "node8/meminfo", "Node 8 MemTotal: 65536 kB\n");
  local = f.get();
  check(local.placement && local.placement->node == 8, "memoryless CPU node does not prevent single-memory fallback");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\n");
  check(f.get().conflict, "missing process memory allowance is not silently ignored");
  write(f.paths.status, "Cpus_allowed_list:\tbogus\nMems_allowed_list:\t8\n");
  check(f.get().conflict, "malformed process allowance is reported");
  auto missing = resolve("/dev/dri/renderD900", f.paths, Mode::on);
  check(!missing.placement && !missing.conflict, "missing GPU topology stays unavailable");
  check(!resolve("/dev/nvidia0", f.paths, Mode::on).placement,
        "arbitrary device path is not interpreted as a DRM node");
  fs::create_directories(f.root / "dev/dri/by-path");
  fs::create_symlink(f.render, f.root / "dev/dri/by-path/pci-0000:81:00.0-render");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\nMems_allowed_list:\t8\n");
  check(resolve((f.root / "dev/dri/by-path/pci-0000:81:00.0-render").string(), f.paths, Mode::on).placement.has_value(),
        "stable by-path GPU links resolve to the canonical render node");
  write(f.paths.nodes / "has_memory", "63,127\n");
  write(f.paths.status, "Cpus_allowed_list:\t1023-1025,4095,8191\nMems_allowed_list:\t63,127\n");
  for (const auto node : {63, 127}) {
    write(f.device / "numa_node", std::to_string(node));
    write(f.paths.nodes / ("node" + std::to_string(node)) / "cpulist", "1023-1025,4095\n");
    const auto boundary = f.get();
    check(boundary.placement && boundary.placement->node == node &&
              boundary.placement->memory_nodes == std::vector<unsigned>{static_cast<unsigned>(node)},
          "NUMA IDs at nodemask word boundaries remain intact");
    check(boundary.placement->cpus == std::vector<unsigned>({1023, 1024, 1025, 4095}),
          "CPU IDs beyond fixed cpu_set_t capacity remain available");
  }
}

void automatic_tests() {
  Fixture f;
  auto result = f.get(Mode::automatic);
  check(result.placement && result.placement->node == 2, "auto enables usable GPU locality on a multi-node host");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\nMems_allowed_list:\t2\n");
  result = f.get(Mode::automatic);
  check(result.placement.has_value(), "auto counts physical memory nodes rather than only permitted memory nodes");
  write(f.paths.status, "Cpus_allowed_list:\t4-7\nMems_allowed_list:\t2,8\n");
  result = f.get(Mode::automatic);
  check(!result.placement && !result.conflict && !result.reason.empty(),
        "auto falls back normally when local CPU resources are excluded");
  check(f.get(Mode::on).conflict, "explicit on retains known CPU conflicts");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\nMems_allowed_list:\t8\n");
  result = f.get(Mode::automatic);
  check(!result.placement && !result.conflict, "auto falls back normally when local RAM is excluded");
  check(f.get(Mode::on).conflict, "explicit on retains known memory conflicts");
  write(f.paths.status, "Cpus_allowed_list:\t0-39\nMems_allowed_list:\t2,8\n");
  write(f.paths.nodes / "has_memory", "2\n");
  result = f.get(Mode::automatic);
  check(!result.placement && !result.conflict && result.reason.find("one memory node") != std::string::npos,
        "auto leaves a single memory node host unchanged");
  check(f.get(Mode::on).placement.has_value(), "explicit on can apply placement on a single-node host");
  fs::remove(f.paths.nodes / "has_memory");
  write(f.paths.nodes / "node2/meminfo", "Node 2 MemTotal: 65536 kB\n");
  write(f.paths.nodes / "node8/meminfo", "Node 8 MemTotal: 0 kB\n");
  check(!f.get(Mode::automatic).placement, "auto ignores CPU-only nodes when detecting non-uniform memory");
  write(f.paths.nodes / "node8/meminfo", "Node 8 MemTotal: 65536 kB\n");
  check(f.get(Mode::automatic).placement.has_value(), "auto detects memory nodes through meminfo fallback");
  write(f.device / "numa_node", "-1\n");
  result = f.get(Mode::automatic);
  check(!result.placement && !result.conflict && !result.reason.empty(), "auto does not guess an unknown GPU node");
  write(f.paths.nodes / "has_memory", "broken topology\n");
  result = f.get(Mode::automatic);
  check(!result.placement && !result.conflict && !result.reason.empty(), "auto safely skips invalid topology");
  check(f.get(Mode::on).conflict, "explicit on reports invalid topology");
  result = resolve("invalid GPU", {f.root / "missing", f.root / "missing", f.root / "missing"}, Mode::off);
  check(!result.placement && !result.conflict && result.reason == "NUMA placement is disabled",
        "off bypasses topology discovery entirely");
}

void scope_tests() {
  auto cpus = parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list"));
  auto memory = parse_list(status_field("/proc/thread-self/status", "Mems_allowed_list"));
  check(!cpus.empty() && !memory.empty(), "test process has CPU and memory allowances");
  Placement chosen{static_cast<int>(memory.front()), {cpus.front()}, {memory.front()}};
#if defined(SYS_get_mempolicy)
  constexpr unsigned bits = 32768;
  std::vector<unsigned long> before_mask(bits / (sizeof(unsigned long) * CHAR_BIT), 0);
  int before_mode = 0;
  const bool readable = syscall(SYS_get_mempolicy, &before_mode, before_mask.data(), bits, nullptr, 0UL) == 0;
#endif
  {
    ThreadScope scope(chosen);
    check(scope.cpu_applied() && scope.cpu_error().empty(), "current thread affinity applied");
    check(parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list")) == chosen.cpus,
          "scoped thread stays on selected local CPU");
    check(scope.memory_applied() == scope.memory_error().empty(), "memory policy result explicitly reports failure");
#if defined(SYS_get_mempolicy)
    if (scope.memory_applied()) {
      std::vector<unsigned long> bound_mask(before_mask.size(), 0);
      int bound_mode = -1;
      check(syscall(SYS_get_mempolicy, &bound_mode, bound_mask.data(), bits, nullptr, 0UL) == 0 &&
                (bound_mode & ~MPOL_MODE_FLAGS) == MPOL_PREFERRED,
            "scoped allocation policy prefers local RAM with remote fallback");
      std::vector<unsigned long> expected(bound_mask.size(), 0);
      auto word_bits = sizeof(unsigned long) * CHAR_BIT;
      expected[memory.front() / word_bits] = 1UL << (memory.front() % word_bits);
      check(bound_mask == expected, "scoped RAM policy selects only the intended memory node");
      {
        ThreadScope nested(chosen);
        check(nested.cpu_applied() && nested.memory_applied(), "nested explicit scope can save a non-default policy");
      }
      int nested_mode = -1;
      std::vector<unsigned long> nested_mask(bound_mask.size(), 0);
      check(syscall(SYS_get_mempolicy, &nested_mode, nested_mask.data(), bits, nullptr, 0UL) == 0 &&
                nested_mode == bound_mode && nested_mask == bound_mask,
            "nested scope restores previous non-default memory policy");
      auto child = std::async(std::launch::async, [&] {
        int mode = -1;
        std::vector<unsigned long> mask(before_mask.size(), 0);
        const auto result = syscall(SYS_get_mempolicy, &mode, mask.data(), bits, nullptr, 0UL);
        return result == 0 && mode == bound_mode && mask == bound_mask &&
               parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list")) == chosen.cpus;
      });
      check(child.get(), "child inherits local CPU affinity and preferred RAM policy");
    }
#endif
    if (!scope.memory_applied())
      std::cout << "Memory policy unavailable in this environment: " << scope.memory_error() << '\n';
    {
      ThreadScope nested(std::optional<Placement>{});
      check(!nested.cpu_applied() && !nested.memory_applied(), "unknown placement scope leaves policy unchanged");
    }
    Placement excluded = chosen;
    excluded.cpus = {cpus.back()};
    if (cpus.back() != cpus.front()) {
      {
        ThreadScope nested(excluded, true);
        check(!nested.cpu_applied() && !nested.cpu_error().empty(), "explicit nested CPU restriction is respected");
        check(!nested.memory_applied(), "CPU placement failure does not silently change RAM placement");
      }
      {
        ThreadScope moved(excluded);
        check(moved.cpu_applied(), "reused worker can expand its inherited affinity to another resolved GPU");
        check(parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list")) == excluded.cpus,
              "reused worker runs on new GPU's selected CPUs");
      }
      check(parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list")) == chosen.cpus,
            "reused worker scope restores its original narrower affinity");
    }
  }
  check(parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list")) == cpus,
        "original thread affinity restored exactly");
#if defined(SYS_get_mempolicy)
  if (readable) {
    std::vector<unsigned long> after_mask(before_mask.size(), 0);
    int after_mode = -1;
    check(syscall(SYS_get_mempolicy, &after_mode, after_mask.data(), bits, nullptr, 0UL) == 0 &&
              after_mode == before_mode && after_mask == before_mask,
          "original memory policy restored exactly");
  }
#endif
  // A helper must roll back CPU placement if later construction throws as well.
  bool failed = false;
  try {
    auto invalid = chosen;
    invalid.memory_nodes = {std::numeric_limits<unsigned>::max()};
    ThreadScope scope(invalid);
  } catch (const std::invalid_argument &) {
    failed = true;
  }
#if defined(SYS_get_mempolicy)
  if (readable)
    check(failed, "invalid memory node fails after CPU placement without escaping rollback");
#else
  (void)failed;
#endif
  check(parse_list(status_field("/proc/thread-self/status", "Cpus_allowed_list")) == cpus,
        "CPU affinity restored even if memory setup cannot finish");
}

void environment_tests() {
  const auto original = std::getenv("WOLF_NUMA_MODE");
  const auto saved = original ? std::optional<std::string>(original) : std::nullopt;
  unsetenv("WOLF_NUMA_MODE");
  check(read_mode() == Mode::automatic && enabled(), "NUMA mode defaults to automatic detection");
  for (const auto &entry :
       {std::pair{"AuTo", Mode::automatic}, std::pair{"ON", Mode::on}, std::pair{"oFf", Mode::off}}) {
    setenv("WOLF_NUMA_MODE", entry.first, 1);
    check(read_mode() == entry.second, "NUMA modes are parsed case-insensitively");
    check(enabled() == (entry.second != Mode::off), "only off disables placement before topology discovery");
  }
  check(std::string(mode_name(Mode::automatic)) == "auto" && std::string(mode_name(Mode::on)) == "on" &&
            std::string(mode_name(Mode::off)) == "off",
        "mode names match configuration values");
  for (const auto *value : {"", "true", "preferred", "maybe", "loose", "strict"}) {
    setenv("WOLF_NUMA_MODE", value, 1);
    bool invalid = false;
    try {
      read_mode();
    } catch (const std::invalid_argument &) {
      invalid = true;
    }
    check(invalid, "unrecognized mode values are rejected");
  }
  if (saved)
    setenv("WOLF_NUMA_MODE", saved->c_str(), 1);
  else
    unsetenv("WOLF_NUMA_MODE");
}
#endif
} // namespace

int main() {
  try {
    list_tests();
#ifdef __linux__
    discovery_tests();
    automatic_tests();
    scope_tests();
    environment_tests();
#else
    check(!resolve("/dev/dri/renderD128").placement, "unsupported platforms preserve no-op placement");
#endif
    std::cout << checks << " NUMA checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "NUMA test failure: " << error.what() << '\n';
    return 1;
  }
}
