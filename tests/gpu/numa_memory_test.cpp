#include <chrono>
#include <filesystem>
#include <fstream>
#include <gpu/numa_memory.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

using namespace wolf::numa;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

std::string meminfo(int node, std::uint64_t total, std::uint64_t free) {
  const auto prefix = "Node " + std::to_string(node) + ' ';
  return prefix + "MemTotal: " + std::to_string(total) + " kB\n" + prefix + "MemFree: " + std::to_string(free) +
         " kB\n" + prefix + "FilePages: 100 kB\n" + prefix + "Active: 512 kB\n";
}

bool unknown(const MemoryNode &node) {
  return !node.total_bytes && !node.used_bytes && !node.used_percent;
}

struct Fixture {
  fs::path root;
  Topology topology{{{3, "First CPU", {0, 1}, {0, 2, 9}}, {8, "Second CPU", {2, 3}, {4, 9}}},
                    {{0, {0, 1}}, {2, {0}}, {4, {2, 3}}, {9, {0, 2}}}};
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      root = fs::temp_directory_path() / ("wolf-numa-memory-" + std::to_string(stamp) + '-' + std::to_string(attempt));
      if (fs::create_directory(root))
        break;
      if (attempt == 99)
        throw std::runtime_error("Cannot create NUMA memory fixture");
    }
    write(root / "node0/meminfo", meminfo(0, 8192, 2048));
    write(root / "node2/meminfo", meminfo(2, 4096, 4096));
    write(root / "node4/meminfo", meminfo(4, 16384, 0));
    write(root / "node9/meminfo", meminfo(9, 1024, 512));
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  std::vector<MemoryNode> read() const {
    return read_memory(topology, root);
  }
};

void value_and_ownership_tests() {
  Fixture f;
  auto result = f.read();
  check(result.size() == 4, "cached topology nodes are read once each");
  check(result[0].id == 0 && result[1].id == 2 && result[2].id == 4 && result[3].id == 9,
        "non-contiguous NUMA IDs remain sorted");
  check(result[0].total_bytes == 8192 * 1024 && result[0].used_bytes == 6144 * 1024 && result[0].used_percent == 75.0,
        "KiB becomes bytes and used is total minus free, including cache");
  check(result[0].cpu_ids == std::vector<int>{3} && result[1].cpu_ids == std::vector<int>{3},
        "one CPU package may own multiple separate memory nodes");
  check(result[2].cpu_ids == std::vector<int>{8}, "node IDs are not confused with package IDs");
  check(result[3].cpu_ids == std::vector<int>({3, 8}), "shared node ownership is not guessed or split");
  check(result[1].used_bytes == 0 && result[1].used_percent == 0.0, "idle zero remains a valid measurement");
  check(result[2].used_bytes == 16384 * 1024 && result[2].used_percent == 100.0,
        "fully used memory remains a valid measurement");

  write(f.root / "node7/meminfo", meminfo(7, 4096, 1024));
  write(f.root / "node7/cpulist", "99\n");
  write(f.root / "has_memory", "0,2,4,7,9,12\n");
  result = f.read();
  check(result.size() == 6 && result[3].id == 7 && result[5].id == 12,
        "directory and has_memory discovery preserve nodes absent from CPU topology");
  check(result[3].cpu_ids.empty() && result[3].used_percent == 75.0,
        "CPU-less memory remains visible without invented ownership");
  check(result[5].cpu_ids.empty() && unknown(result[5]), "a listed but unreadable node remains unknown");
  write(f.root / "node7/meminfo", meminfo(7, 0, 0));
  result = f.read();
  check(result[3].total_bytes == 0 && result[3].used_bytes == 0 && !result[3].used_percent,
        "memoryless nodes report zero bytes without a fabricated percentage");

  write(f.root / "has_memory", "invalid list\n");
  check(f.read().size() == 5, "bad has_memory data does not hide discoverable nodes");
  write(f.root / "node01/meminfo", meminfo(1, 4096, 1024));
  write(f.root / "node-1/meminfo", meminfo(-1, 4096, 1024));
  write(f.root / "node2147483648/meminfo", meminfo(0, 4096, 1024));
  write(f.root / "nodes/meminfo", meminfo(0, 4096, 1024));
  check(f.read().size() == 5, "malformed, negative and overflowing directory IDs are not normalized to real nodes");

  f.topology.node_cpus.at(0).push_back(9999);
  result = f.read();
  check(result[0].cpu_ids.empty() && result[0].used_percent == 75.0,
        "partially unknown package ownership never becomes an exclusive CPU memory total");
  check(result[1].cpu_ids == std::vector<int>{3}, "an unmapped node does not discard another node's known owner");
}

void invalid_input_tests() {
  Fixture f;
  const std::string valid = meminfo(0, 8192, 2048);
  const auto overflow = std::to_string(std::numeric_limits<std::uint64_t>::max() / 1024 + 1);
  const std::vector<std::string> invalid{"",
                                         "Node 0 MemTotal: 8192 kB\n",
                                         "Node 0 MemFree: 2048 kB\n",
                                         valid + "Node 0 MemTotal: 8192 kB\n",
                                         valid + "Node 0 MemFree: 2048 kB\n",
                                         "Node 1 MemTotal: 8192 kB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 1 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: 8193 kB\n",
                                         "Node 0 MemTotal: 0 kB\nNode 0 MemFree: 1 kB\n",
                                         "Node 0 MemTotal: -8192 kB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: -1 kB\n",
                                         "Node 0 MemTotal: invalid kB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: " + overflow + " kB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 18446744073709551616 kB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: " + overflow + " kB\n",
                                         "Node 0 MemTotal: 8192 MB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: 2048 KB\n",
                                         "Node 0 MemTotal: 8192 kB extra\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: 2048\n",
                                         "MemTotal: 8192 kB\nMemFree: 2048 kB\n",
                                         "Node broken MemTotal: 8192 kB\nNode 0 MemFree: 2048 kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: 2048x kB\n",
                                         "Node 0 MemTotal: 8192 kB\nNode 0 MemFree: +2048 kB\n",
                                         valid + "Node 0 PrefixMemFree: 2048 kB\n"};
  for (const auto &text : invalid) {
    write(f.root / "node0/meminfo", text);
    const auto result = f.read();
    check(unknown(result[0]), "incomplete or malformed measurements do not become partial or idle values");
    check(result[2].used_percent == 100.0, "one invalid node does not discard another node's measurement");
  }
  fs::remove(f.root / "node0/meminfo");
  check(unknown(f.read()[0]), "a missing meminfo file retains an unknown node");
  fs::create_directory(f.root / "node0/meminfo");
  check(unknown(f.read()[0]), "an unreadable non-file is unknown");
  fs::remove(f.root / "node0/meminfo");
  const auto maximum = std::numeric_limits<std::uint64_t>::max() / 1024;
  write(f.root / "node0/meminfo", meminfo(0, maximum, 0));
  check(f.read()[0].used_bytes == maximum * 1024 && f.read()[0].used_percent == 100.0,
        "largest nonoverflowing KiB count is accepted");
  check(read_memory({}, f.root / "missing").empty(), "missing topology and sysfs do not fabricate a node");
  const auto missing = read_memory(f.topology, f.root / "missing");
  check(missing.size() == 4 && unknown(missing[0]) && unknown(missing[3]),
        "cached topology survives unavailable sysfs with unknown measurements");
}

void cache_tests() {
  Fixture f;
  auto now = std::chrono::steady_clock::time_point{};
  auto sample = make_memory_sampler(f.topology, f.root, [&] { return now; });
  write(f.root / "node0/meminfo", meminfo(0, 8192, 4096));
  check(sample()[0].used_percent == 50.0, "factory creation performs no I/O and first request samples immediately");
  write(f.root / "node0/meminfo", meminfo(0, 8192, 2048));
  auto copy = sample;
  now += std::chrono::milliseconds(249);
  check(copy()[0].used_percent == 50.0, "sampler copies share the short request cache");
  now += std::chrono::milliseconds(1);
  check(sample()[0].used_percent == 75.0, "cache refreshes at 250 milliseconds");
  write(f.root / "node0/meminfo", meminfo(0, 8192, 0));
  std::vector<std::thread> readers;
  std::vector<std::optional<double>> values(12);
  for (unsigned i = 0; i < values.size(); ++i)
    readers.emplace_back([&, i] { values[i] = copy()[0].used_percent; });
  for (auto &reader : readers)
    reader.join();
  for (auto value : values)
    check(value == 75.0, "concurrent readers share the same cached snapshot");
  now -= std::chrono::seconds(1);
  check(copy()[0].used_percent == 100.0, "a backwards test clock cannot retain an old cache indefinitely");
  fs::remove(f.root / "node0/meminfo");
  now += std::chrono::hours(1);
  check(unknown(sample()[0]), "new missing data replaces stale known values after inactivity");

  bool fail_clock = true;
  auto guarded = make_memory_sampler(f.topology, f.root, [&] {
    if (fail_clock)
      throw std::runtime_error("injected clock error");
    return now;
  });
  const auto first_error = guarded();
  check(first_error.size() == 4 && first_error[0].cpu_ids == std::vector<int>{3} && unknown(first_error[0]),
        "a first monitoring exception preserves cached topology with unknown usage");
  write(f.root / "node0/meminfo", meminfo(0, 8192, 2048));
  fail_clock = false;
  check(guarded()[0].used_percent == 75.0, "sampling recovers immediately after an optional monitoring exception");
  fail_clock = true;
  check(unknown(guarded()[0]), "a later monitoring exception clears earlier known values");
  fail_clock = false;
  check(guarded()[0].used_percent == 75.0, "exception recovery is not held behind the old refresh interval");
}
} // namespace

int main() {
  try {
    value_and_ownership_tests();
    invalid_input_tests();
    cache_tests();
    std::cout << checks << " NUMA memory checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "NUMA memory test failure: " << error.what() << '\n';
    return 1;
  }
}
