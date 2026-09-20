#include "discovery.hpp"
#include <algorithm>
#include <fstream>
#include <platforms/hw.hpp>
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
  d.vram_used_bytes.reset();
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
      d.vram_used_bytes = value.used;
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

std::vector<Device> discover(const DiscoveryPaths &paths) {
  std::vector<Device> devices;
#ifdef __linux__
  std::error_code ec;
  std::set<std::string> seen;
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
    d.name = read_text(sys_device / "product_name");
    if (d.name.empty() && !vendor.empty() && !product.empty()) {
      try {
        d.name = get_gpu_name(std::stoul(vendor, nullptr, 16), std::stoul(product, nullptr, 16));
      } catch (const std::exception &) {
      } // Malformed sysfs data retains the fallback below.
    }
    if (d.name.empty())
      d.name = d.driver + " " + vendor + ":" + product + " (" + d.id + ")";
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
      d.vram_used_bytes = *used;
    if (auto busy = read_number(sys_device / "gpu_busy_percent"); busy && *busy <= 100)
      d.gpu_percent = *busy;
    sample_nvidia(d);
    devices.push_back(std::move(d));
  }
#endif
  std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  return devices;
}
} // namespace wolf::gpu
