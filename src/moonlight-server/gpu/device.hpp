#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace wolf::gpu {
struct Device {
  std::string id;
  std::string render_node;
  std::string by_path;
  std::string driver;
  std::string name;
  bool accessible = false;
  std::optional<std::uint64_t> vram_bytes;
  std::optional<std::uint64_t> vram_used_bytes;
  std::optional<double> gpu_percent;
  std::optional<double> encoder_percent;
};
inline std::string normalize_identifier(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}
} // namespace wolf::gpu
