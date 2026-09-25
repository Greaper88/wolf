#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wolf::gpu {

// No driver, GStreamer or session-state dependencies: policy consumes one fresh snapshot.
struct Options {
  bool auto_select = true;
  bool auto_blacklist = true;
  std::vector<std::string> blacklist;
  std::vector<std::string> last_resort;
  double gpu_threshold = 90;
  double gpu_retry_threshold = 87;
  double gpu_variance = 5;
  double vram_threshold = 85;
  double vram_retry_threshold = 80;
  double encoder_threshold = 90;
  double encoder_retry_threshold = 85;
  double encoder_hard_limit = 98;
  double encoder_variance = 5;
  std::string encoder_unknown_policy = "session_count";
  bool manual_override = false;
  bool use_zero_copy = true;
  bool require_zero_copy = false;

  bool enabled() const {
    return auto_select && !manual_override;
  }
};

using Environment = std::function<std::optional<std::string>(const std::string &)>;
Options read_options(const Environment &environment);
Options read_options();

struct Device {
  std::string id; // PCI BDF, otherwise canonical render node
  std::string render_node;
  std::string by_path;
  std::string driver;
  std::string name;
  bool last_resort = false; // Shared-memory or unknown device class, never a software pool.
  bool accessible = false;
  bool hardware_encoder = false; // Must be established by a device-specific capability probe.
  std::optional<std::uint64_t> vram_bytes;
  std::optional<double> vram_percent;
  std::optional<double> gpu_percent;
  std::optional<double> encoder_percent;
  unsigned int active_sessions = 0;
  unsigned int pending_sessions = 0;
  unsigned int retained_sessions = 0; // Apps still resident; no encoder demand while disconnected.
};

// Additive /sessions response field. Null telemetry means unavailable, never zero.
struct SessionGpu {
  std::string id;
  std::string name;
  std::string render_node;
  std::string encoder_node;
  std::optional<std::uint64_t> vram_bytes;
  std::optional<double> encoder_percent;
  std::optional<double> projected_encoder_percent;
  unsigned int session_count_on_gpu = 0;
  std::optional<std::string> stream_error;
  std::optional<double> gpu_percent;
  std::optional<std::string> codec;
};

struct Rejection {
  std::string id;
  std::string reason;
};
struct Selection {
  std::optional<Device> device;
  std::optional<double> projected_encoder_percent;
  std::vector<Rejection> rejected;
  std::string error() const;
};

std::string normalize_identifier(std::string value);
bool blacklisted(const Device &device, const Options &options);
bool matches(const Device &device, const std::vector<std::string> &identifiers);
std::optional<double> projected_encoder(const Device &device);
Selection
select(const std::vector<Device> &snapshot, const Options &options, bool retry = false, bool existing_app = false);
SessionGpu metadata(const Device &device, std::optional<double> projected, unsigned int count);

} // namespace wolf::gpu
