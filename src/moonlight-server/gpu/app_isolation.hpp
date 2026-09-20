#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace wolf::gpu {
// PCI identity, rather than an enumeration index or vendor/device pair, also
// distinguishes two identical GPUs. The ! suffix limits Mesa Vulkan enumeration.
inline std::optional<std::string> mesa_prime_selector(std::string pci) {
  if (pci.size() != 12 || pci[4] != ':' || pci[7] != ':' || pci[10] != '.')
    return {};
  for (std::size_t i = 0; i < pci.size(); ++i) {
    if (i == 4 || i == 7 || i == 10)
      continue;
    if (!std::isxdigit(static_cast<unsigned char>(pci[i])))
      return {};
    pci[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(pci[i])));
  }
  if (std::stoul(pci.substr(8, 2), nullptr, 16) > 31 || pci[11] > '7')
    return {};
  pci[4] = pci[7] = pci[10] = '_';
  return "pci-" + pci + "!";
}
inline std::string resolved_path(std::string_view path) {
  std::error_code error;
  auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
  return error ? std::filesystem::path(path).lexically_normal().string() : resolved.string();
}
inline bool graphics_path(std::string_view path) {
  return path == "/dev/dri" || path.starts_with("/dev/dri/") || path.starts_with("/dev/nvidia");
}
inline bool graphics_mount(std::string_view path) {
  auto p = std::filesystem::path(path).lexically_normal().string();
  auto resolved = resolved_path(path);
  while (p.size() > 1 && p.back() == '/')
    p.pop_back();
  while (resolved.size() > 1 && resolved.back() == '/')
    resolved.pop_back();
  return p == "/" || p == "/dev" || resolved == "/" || resolved == "/dev" || graphics_path(p) ||
         graphics_path(resolved);
}
inline bool allowed_graphics_device(std::string_view source,
                                    std::string_view destination,
                                    const std::vector<std::string> &allowed) {
  auto canonical = resolved_path(source);
  if (!graphics_path(source) && !graphics_path(destination) && !graphics_path(canonical))
    return true;
  // Preserve the selected nodes' names in the container as well as their identities.
  return std::any_of(allowed.begin(), allowed.end(), [&](const auto &node) {
    return canonical == resolved_path(node) && destination == node;
  });
}
inline bool broad_graphics_rule(std::string_view rule) {
  std::istringstream stream{std::string(rule)};
  std::string type, device;
  stream >> type >> device;
  if (type == "a")
    return true;
  auto colon = device.find(':');
  auto major = device.substr(0, colon);
  return type == "c" && (major == "*" || major == "226" || major == "195");
}
inline bool mesa_selection_env(std::string_view item) {
  return item.starts_with("DRI_PRIME=") || item.starts_with("MESA_VK_DEVICE_SELECT=") ||
         item.starts_with("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=");
}
} // namespace wolf::gpu
