#include "discovery.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#ifdef __linux__
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wolf::gpu {
namespace {
std::string trim(std::string value) {
  auto first = value.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<unsigned int> hex_number(std::string value) {
  value = trim(std::move(value));
  if (value.starts_with("0x") || value.starts_with("0X"))
    value.erase(0, 2);
  unsigned int result;
  auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
  if (ec != std::errc{} || end != value.data() + value.size())
    return std::nullopt;
  return result;
}

struct DeviceNames {
  std::map<std::pair<unsigned int, unsigned int>, std::string> amd, pci;

  explicit DeviceNames(const DiscoveryPaths &paths) {
    std::ifstream amd_file(paths.amdgpu_ids);
    std::string line;
    while (std::getline(amd_file, line)) {
      auto first = line.find(','), second = line.find(',', first == std::string::npos ? first : first + 1);
      if (first == std::string::npos || second == std::string::npos)
        continue;
      auto device = hex_number(line.substr(0, first));
      auto revision = hex_number(line.substr(first + 1, second - first - 1));
      auto name = trim(line.substr(second + 1));
      if (device && revision && !name.empty())
        amd[{*device, *revision}] = name;
    }
    std::ifstream pci_file(paths.pci_ids);
    if (!pci_file && paths.pci_ids == DiscoveryPaths{}.pci_ids)
      pci_file.open("/usr/share/misc/pci.ids");
    std::optional<unsigned int> vendor;
    while (std::getline(pci_file, line)) {
      if (line.empty() || line.front() == '#')
        continue;
      if (line.front() != '\t') {
        vendor = line.size() > 4 && line[4] == ' ' ? hex_number(line.substr(0, 4)) : std::nullopt;
      } else if (vendor && line.size() > 5 && line[1] != '\t' && line[5] == ' ') {
        if (auto device = hex_number(line.substr(1, 4)))
          pci[{*vendor, *device}] = trim(line.substr(5));
      }
    }
  }

  std::string lookup(unsigned int vendor, unsigned int device, std::optional<unsigned int> revision) const {
    if (vendor == 0x1002 && revision) {
      if (auto it = amd.find({device, *revision}); it != amd.end())
        return it->second;
    }
    if (auto it = pci.find({vendor, device}); it != pci.end())
      return it->second;
    return vendor == 0x1002 ? "AMD GPU" : vendor == 0x8086 ? "Intel GPU" : vendor == 0x10de ? "NVIDIA GPU" : "GPU";
  }
};

const DeviceNames &system_device_names() {
  // Immutable name databases are loaded once; telemetry refreshes do not reread them.
  static const DeviceNames names(DiscoveryPaths{});
  return names;
}

std::optional<std::uint64_t> read_number(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string value;
  if (!(file >> value) || value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
    return std::nullopt;
  try {
    return std::stoull(value);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}
std::string read_text(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string value;
  std::getline(file, value);
  return value;
}
std::string resolved_path(const std::filesystem::path &path) {
  std::error_code ec;
  auto resolved = std::filesystem::canonical(path, ec);
  return ec ? "" : resolved.string();
}
} // namespace

void sample_nvidia(Device &d) {
#ifdef __linux__
  if (d.driver != "nvidia")
    return;
  // Do not retain a previously successful reading when the next NVML query fails.
  d.encoder_percent.reset();
  d.gpu_percent.reset();
  d.vram_percent.reset();
  d.vram_bytes.reset();
  // Stable public NVML ABI. Runtime loading keeps NVIDIA headers and libraries optional on AMD/Intel hosts.
  struct Memory {
    unsigned long long total, free, used;
  };
  struct Utilization {
    unsigned int gpu, memory;
  };
  struct NvmlDevice;
  using Handle = NvmlDevice *;
  void *library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library)
    return;
  struct Library {
    void *value;
    ~Library() {
      dlclose(value);
    }
  } owner{library};
  auto init = reinterpret_cast<int (*)()>(dlsym(library, "nvmlInit_v2"));
  auto shutdown = reinterpret_cast<int (*)()>(dlsym(library, "nvmlShutdown"));
  auto get_handle =
      reinterpret_cast<int (*)(const char *, Handle *)>(dlsym(library, "nvmlDeviceGetHandleByPciBusId_v2"));
  if (!init || !shutdown || !get_handle || init() != 0)
    return;
  struct Shutdown {
    int (*fn)();
    ~Shutdown() {
      fn();
    }
  } initialized{shutdown};
  Handle handle{};
  if (get_handle(d.id.c_str(), &handle) != 0)
    return;
  if (auto name = reinterpret_cast<int (*)(Handle, char *, unsigned int)>(dlsym(library, "nvmlDeviceGetName"))) {
    char buffer[128]{};
    if (name(handle, buffer, sizeof(buffer)) == 0)
      d.name = buffer;
  }
  if (auto memory = reinterpret_cast<int (*)(Handle, Memory *)>(dlsym(library, "nvmlDeviceGetMemoryInfo"))) {
    Memory value{};
    if (memory(handle, &value) == 0 && value.total > 0 && value.used <= value.total) {
      d.vram_bytes = value.total;
      d.vram_percent = 100.0 * value.used / value.total;
    }
  }
  if (auto utilization =
          reinterpret_cast<int (*)(Handle, Utilization *)>(dlsym(library, "nvmlDeviceGetUtilizationRates"))) {
    Utilization value{};
    if (utilization(handle, &value) == 0 && value.gpu <= 100)
      d.gpu_percent = value.gpu;
  }
  if (auto encoder = reinterpret_cast<int (*)(Handle, unsigned int *, unsigned int *)>(
          dlsym(library, "nvmlDeviceGetEncoderUtilization"))) {
    unsigned int value{}, period{};
    if (encoder(handle, &value, &period) == 0 && value <= 100)
      d.encoder_percent = value;
  }
#endif
}

std::vector<Device> discover(const DiscoveryPaths &paths, const CapabilityProbe &probe) {
  std::vector<Device> devices;
#ifdef __linux__
  std::error_code ec;
  std::set<std::string> seen;
  std::optional<DeviceNames> custom_names;
  if (paths.amdgpu_ids != DiscoveryPaths{}.amdgpu_ids || paths.pci_ids != DiscoveryPaths{}.pci_ids)
    custom_names.emplace(paths);
  const auto &names = custom_names ? *custom_names : system_device_names();
  std::filesystem::directory_iterator it(paths.dri, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    auto node_name = it->path().filename().string();
    if (!std::regex_match(node_name, std::regex("renderD[0-9]+")))
      continue;
    Device d;
    d.render_node = resolved_path(it->path());
    if (d.render_node.empty() || !seen.insert(d.render_node).second)
      continue;
    auto sys_device = paths.drm_class / node_name / "device";
    auto device_path = std::filesystem::path(resolved_path(sys_device));
    auto bdf = device_path.filename().string();
    bool pci = std::regex_match(bdf, std::regex("[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]"));
    d.id = pci ? normalize_identifier(bdf) : d.render_node;
    if (pci) {
      auto by_path = paths.dri / "by-path" / ("pci-" + d.id + "-render");
      if (resolved_path(by_path) == d.render_node)
        d.by_path = by_path.string();
    }
    d.driver = std::filesystem::path(resolved_path(sys_device / "driver")).filename().string();
    auto vendor = read_text(sys_device / "vendor");
    auto product = read_text(sys_device / "device");
    d.name = trim(read_text(sys_device / "product_name"));
    if (d.name.empty()) {
      auto vendor_id = hex_number(vendor), device_id = hex_number(product);
      d.name = vendor_id && device_id
                   ? names.lookup(*vendor_id, *device_id, hex_number(read_text(sys_device / "revision")))
                   : "GPU";
    }
    struct stat info{};
    if (stat(d.render_node.c_str(), &info) == 0 && S_ISCHR(info.st_mode)) {
      int fd = open(d.render_node.c_str(), O_RDWR | O_CLOEXEC);
      d.accessible = fd >= 0;
      if (fd >= 0)
        close(fd);
    }
    d.vram_bytes = read_number(sys_device / "mem_info_vram_total");
    auto used = read_number(sys_device / "mem_info_vram_used");
    if (d.vram_bytes && *d.vram_bytes && used && *used <= *d.vram_bytes)
      d.vram_percent = 100.0 * *used / *d.vram_bytes;
    if (auto busy = read_number(sys_device / "gpu_busy_percent"); busy && *busy <= 100)
      d.gpu_percent = *busy;
    // Conservative heuristic, not a claim that vendor implies integrated/discrete.
    // Unknown/local-memory-unreported devices remain available in the last-resort pool.
    d.last_resort = !(d.driver == "nvidia" || (d.vram_bytes && *d.vram_bytes > 1024ULL * 1024 * 1024));
    sample_nvidia(d);
    if (probe && d.accessible)
      d.hardware_encoder = probe(d);
    devices.push_back(std::move(d));
  }
#endif
  std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  return devices;
}
} // namespace wolf::gpu
