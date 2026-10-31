#include <chrono>
#include <filesystem>
#include <fstream>
#include <gpu/container_memory.hpp>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
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
void write(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << value;
}
std::optional<ContainerMemory> parse(const std::string &value, const std::vector<int> &local = {0}) {
  std::istringstream input(value);
  return parse_container_memory(input, local);
}
const std::string valid = "anon N0=8192 N2=4096 N7=0\nfile N0=1024 N2=0 N7=2048\nshmem N0=1024 N2=0 N7=0\n";
const std::string first(64, 'a'), second(64, 'b'), third(64, 'c');
Topology topology{{{3, "CPU A", {0, 1, 2}, {0, 2}}, {8, "CPU B", {3}, {7}}}, {{0, {0, 1}}, {2, {2}}, {7, {3}}}};
struct Fixture {
  fs::path root;
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      root = fs::temp_directory_path() /
             ("wolf-container-memory-" + std::to_string(stamp) + '-' + std::to_string(attempt));
      if (fs::create_directory(root))
        break;
      if (attempt == 99)
        throw std::runtime_error("Cannot create container memory fixture");
    }
    write(root / "cgroup.controllers", "cpuset memory cpu\n");
    write(root / "docker" / first / "memory.numa_stat", valid);
    write(root / "custom.slice/nested.slice" / ("docker-" + second + ".scope") / "memory.numa_stat", valid);
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};
void parser_tests() {
  auto memory = parse(valid);
  check(memory.has_value(), "valid cgroup v2 byte counters parse");
  check(memory->local_nodes == std::vector<int>{0}, "local NUMA IDs are explicit");
  check(memory->anon.local_bytes == 8192 && memory->anon.remote_bytes == 4096,
        "anonymous bytes are separated by actual residency");
  check(memory->file.local_bytes == 1024 && memory->file.remote_bytes == 2048,
        "file bytes include shmem exactly once, not added again");
  memory = parse(valid, {2, 0, 2});
  check(memory->local_nodes == std::vector<int>({0, 2}) && memory->anon.local_bytes == 12288 &&
            memory->anon.remote_bytes == 0 && memory->file.remote_bytes == 2048,
        "multi-node affinity treats only assigned CPU nodes as local");
  memory = parse("anon N0=0 N2=0\nfile N2=0 N0=0\n");
  check(memory && memory->anon.local_bytes == 0 && memory->anon.remote_bytes == 0 && memory->file.local_bytes == 0 &&
            memory->file.remote_bytes == 0,
        "all zero counters and reordered nodes remain valid");
  memory = parse("unknown future kernel fields are ignored\n" + valid + "file_mapped invalid overlap\n");
  check(memory.has_value(), "future and overlapping kernel rows are not counted");
  check(!parse(valid, {}), "unrestricted placement is unknown, not every node local");
  check(!parse(valid, {-1}), "negative local IDs are rejected");
  const std::vector<std::string> invalid{"",
                                         "anon N0=0\n",
                                         "file N0=0\n",
                                         "anon\nfile N0=0\n",
                                         "anon N0=0\nfile\n",
                                         valid + "anon N0=0\n",
                                         valid + "file N0=0\n",
                                         "anon N0=0 N0=1\nfile N0=0\n",
                                         "anon N0=0\nfile N0=0 N0=1\n",
                                         "anon N0=0\nfile N2=0\n",
                                         "anon N0=0 N2=1\nfile N0=0\n",
                                         "anon N0=0\nfile N0=0 N2=0\n",
                                         "anon N-1=0\nfile N-1=0\n",
                                         "anon N2147483648=0\nfile N0=0\n",
                                         "anon node0=0\nfile N0=0\n",
                                         "anon N0\nfile N0=0\n",
                                         "anon N0=\nfile N0=0\n",
                                         "anon N0=+1\nfile N0=0\n",
                                         "anon N00=0\nfile N00=0\n",
                                         "anon N0=-1\nfile N0=0\n",
                                         "anon N0=1x\nfile N0=0\n",
                                         "anon N0=18446744073709551616\nfile N0=0\n",
                                         "anon N0=1 kB\nfile N0=0\n",
                                         "total=1 N0=1\nfile=1 N0=1\n",
                                         "anon N0=18446744073709551615 N2=1\nfile N0=0 N2=0\n",
                                         "anon N0=0 N2=18446744073709551615 N7=1\nfile N0=0 N2=0 N7=0\n",
                                         "anon N0=18446744073709551615\nfile N0=1\n",
                                         "anon N0=18446744073709551615 N2=1\nfile N0=0 N2=0\n",
                                         std::string(1024 * 1024 + 1, 'x') + '\n' + valid};
  for (const auto &text : invalid)
    check(!parse(text), "malformed/incomplete/v1/overflowing input remains wholly unavailable");
  std::istringstream bad(valid);
  bad.setstate(std::ios::badbit);
  check(!parse_container_memory(bad, {0}), "I/O failure is not an idle sample");
  check(
      parse("anon N0=18446744073709551615\nfile N0=0\n")->anon.local_bytes == std::numeric_limits<std::uint64_t>::max(),
      "largest representable byte total is valid");
}
void topology_tests() {
  check(container_memory_nodes({0, 1}, topology) == std::vector<int>{0}, "CPU node follows applied CPU mask");
  check(container_memory_nodes({0, 2}, topology) == std::vector<int>({0, 2}),
        "one socket may assign CPUs on several distinct nodes");
  check(container_memory_nodes({0, 3}, topology) == std::vector<int>({0, 7}),
        "cross socket accepted mask retains all node IDs");
  check(!container_memory_nodes({}, topology), "missing applied mask has unknown locality");
  check(!container_memory_nodes({0, 99}, topology), "partially unmapped mask never reports partial locality");
  auto duplicate = topology;
  duplicate.node_cpus.at(7).push_back(0);
  check(!container_memory_nodes({0}, duplicate), "ambiguous CPU node mapping is unknown");
  duplicate = topology;
  duplicate.node_cpus[std::numeric_limits<unsigned>::max()] = {99};
  check(!container_memory_nodes({99}, duplicate), "unrepresentable node IDs remain unknown");
}
void sampler_tests() {
  Fixture f;
  auto now = std::chrono::steady_clock::time_point{};
  auto sample = make_container_memory_sampler(topology, f.root, [&] { return now; });
  ContainerCpuSets targets{{first, {0, 1}}, {second, {3}}};
  write(f.root / "docker" / first / "memory.numa_stat", "anon N0=0 N7=2\nfile N0=3 N7=4\n");
  auto values = sample(targets);
  check(values.size() == 2, "creation does no I/O; first request resolves both Docker cgroup drivers");
  check(values.at(first).anon.local_bytes == 0 && values.at(first).anon.remote_bytes == 2,
        "fixture values are bytes, not page counts");
  check(values.at(second).file.local_bytes == 2048 && values.at(second).file.remote_bytes == 1024,
        "noncontiguous NUMA ID follows actual assignment");
  auto copy = sample;
  write(f.root / "docker" / first / "memory.numa_stat", valid);
  now += std::chrono::milliseconds(249);
  check(copy(targets).at(first).anon.local_bytes == 0, "copies share 250ms request cache");
  now += std::chrono::milliseconds(1);
  check(sample(targets).at(first).anon.local_bytes == 8192, "cache expires at 250ms");
  targets[first] = {2};
  check(sample(targets).at(first).anon.local_bytes == 4096,
        "placement change cannot reuse a sample classified against previous nodes");
  targets[first] = {0, 99};
  check(!sample(targets).contains(first), "unknown placement hides data even when file readable");
  targets[first] = {};
  check(!sample(targets).contains(first), "unrestricted container placement is unknown");
  targets[first] = {0};
  check(sample(targets).contains(first), "known placement can immediately recover");
  fs::remove_all(f.root / "docker" / first);
  now += std::chrono::milliseconds(250);
  check(!sample(targets).contains(first), "vanished container cannot retain an old reading after refresh");
  targets.erase(first);
  check(sample(targets).size() == 1, "stopped container is removed immediately, inside cache interval");
  write(f.root / "newparent" / first / "memory.numa_stat", valid);
  targets[first] = {0, 2};
  check(sample(targets).at(first).anon.local_bytes == 12288, "new container resolves exact ID under custom parent");
  write(f.root / "duplicate" / ("docker-" + first + ".scope") / "memory.numa_stat", valid);
  targets[third] = {0}; // New target invalidates discovery cache.
  check(!sample(targets).contains(first) && sample(targets).contains(second),
        "ambiguous matching paths hide only the affected container");
  fs::remove_all(f.root / "duplicate");
  now += std::chrono::seconds(5);
  check(sample(targets).contains(first), "missing/ambiguous discovery retries on viewer requests");
  write(f.root / "newparent" / first / "memory.numa_stat", "malformed\n");
  now += std::chrono::milliseconds(250);
  check(!sample(targets).contains(first), "malformed refresh drops stale data");
  write(f.root / "newparent" / first / "memory.numa_stat", valid);
  now -= std::chrono::seconds(10);
  check(sample(targets).contains(first), "backwards clock refreshes cache");
  std::vector<std::thread> readers;
  std::vector<std::size_t> sizes(12);
  for (unsigned i = 0; i < sizes.size(); ++i)
    readers.emplace_back([&, i] { sizes[i] = copy(targets).size(); });
  for (auto &reader : readers)
    reader.join();
  for (auto size : sizes)
    check(size == 2, "concurrent readers share synchronized cache");
  check(sample({}).empty(), "no containers means no retained samples");
}
void path_tests() {
  Fixture f;
  auto now = std::chrono::steady_clock::time_point{};
  const ContainerCpuSets target{{first, {0}}};
  auto read = [&] { return make_container_memory_sampler(topology, f.root, [&] { return now; })(target); };
  fs::remove(f.root / "cgroup.controllers");
  check(read().empty(), "v1/unavailable cgroup hierarchy never receives v2 byte interpretation");
  write(f.root / "cgroup.controllers", "");
  check(read().size() == 1, "empty v2 controller delegation remains a valid hierarchy");
  fs::remove_all(f.root / "docker");
  const auto external = f.root.parent_path() / (f.root.filename().string() + "-external");
  write(external / first / "memory.numa_stat", valid);
  fs::create_directory_symlink(external, f.root / "escape");
  check(read().empty(), "directory symlinks cannot escape host cgroup root");
  fs::remove(f.root / "escape");
  fs::create_directories(f.root / "docker" / first);
  fs::create_symlink(external / first / "memory.numa_stat", f.root / "docker" / first / "memory.numa_stat");
  check(read().empty(), "counter symlinks are not read");
  fs::remove(f.root / "docker" / first / "memory.numa_stat");
  write(f.root / "docker" / first / "memory.numa_stat", valid);
  auto cached = make_container_memory_sampler(topology, f.root, [&] { return now; });
  check(cached(target).size() == 1, "real cgroup path is cached");
  fs::remove_all(f.root / "docker");
  fs::create_directory_symlink(external, f.root / "docker");
  now += std::chrono::milliseconds(250);
  check(cached(target).empty(), "cached ancestor replaced with symlink cannot escape hierarchy");
  fs::remove(f.root / "docker");
  fs::remove_all(external);
  write(f.root / "docker/short-id/memory.numa_stat", valid);
  check(make_container_memory_sampler(topology, f.root)({{"short-id", {0}}}).empty(),
        "short IDs are not matched by prefix");
  check(make_container_memory_sampler(topology, f.root)({{"../escape", {0}}}).empty(),
        "untrusted ID cannot become path traversal");
  fs::remove_all(f.root / "docker");
  auto deep = f.root;
  for (unsigned depth = 0; depth < 34; ++depth)
    deep /= "nested";
  write(deep / first / "memory.numa_stat", valid);
  check(read().empty(), "bounded directory traversal rejects partial discovery");
}
} // namespace
int main() {
  try {
    parser_tests();
    topology_tests();
    sampler_tests();
    path_tests();
    std::cout << checks << " container memory checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
