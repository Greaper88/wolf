#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Grimm Display v1 is an explicitly negotiated GameStream extension. None of
// these fields changes legacy RTP or input until the RTSP agreement completes.
namespace moonlight::dual_display {
constexpr std::uint32_t ssrc_base = 0x47440000;
constexpr std::uint32_t input_magic = 0x55000010;
constexpr std::string_view transport = "rtp-ssrc-v1";

struct Display {
  int id = 0;
  int width = 1280;
  int height = 720;
  int fps = 60;
  int x = 0;
  int y = 0;
};

struct Config {
  int version = 0;
  std::string mode = "off";
  std::string layout = "horizontal";
  int primary = 0;
  std::vector<Display> displays;

  bool dual() const {
    return mode == "dual_monitor" || mode == "dual_app";
  }
  int canvas_width() const {
    int result = 0;
    for (const auto &d : displays)
      result = std::max(result, d.x + d.width);
    return result;
  }
  int canvas_height() const {
    int result = 0;
    for (const auto &d : displays)
      result = std::max(result, d.y + d.height);
    return result;
  }
};

inline int integer(std::string_view value, int low, int high, std::string_view name) {
  int parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed < low || parsed > high)
    throw std::invalid_argument("Invalid " + std::string(name));
  return parsed;
}

inline Config parse(const std::map<std::string, std::string> &query, int width, int height, int fps) {
  Config result;
  auto get = [&](const std::string &key, std::string fallback = {}) {
    auto it = query.find(key);
    return it == query.end() ? fallback : it->second;
  };
  const bool present = query.find("gd_version") != query.end();
  if (!present) {
    for (const auto &[key, value] : query)
      if (key.starts_with("gd_"))
        throw std::invalid_argument("Display options require gd_version=1");
    return result;
  }
  result.version = integer(get("gd_version"), 1, 1, "display extension version");
  result.mode = get("gd_mode", "off");
  if (result.mode != "off" && result.mode != "controls" && !result.dual())
    throw std::invalid_argument("Unknown second-display mode");
  result.layout = get("gd_layout", "horizontal");
  if (result.layout != "horizontal" && result.layout != "vertical")
    throw std::invalid_argument("Unknown monitor arrangement");
  result.primary = integer(get("gd_primary", "0"), 0, 1, "primary display");
  if (!result.dual())
    return result;
  auto dimension = [&](const std::string &key) {
    const auto value = integer(get(key), 128, 8192, key);
    if (value % 2)
      throw std::invalid_argument("Display dimensions must be even");
    return value;
  };
  if (width < 128 || width > 8192 || width % 2 || height < 128 || height > 8192 || height % 2 || fps < 1 || fps > 240)
    throw std::invalid_argument("Unsupported primary display dimensions or frame rate");
  result.displays = {
      {0, width, height, fps},
      {1, dimension("gd_width2"), dimension("gd_height2"), integer(get("gd_fps2"), 1, 240, "secondary frame rate")}};
  if (result.mode == "dual_monitor") {
    result.displays[1].x = result.layout == "horizontal" ? width : 0;
    result.displays[1].y = result.layout == "vertical" ? height : 0;
    if (std::int64_t(result.canvas_width()) * result.canvas_height() > 67108864)
      throw std::invalid_argument("Combined desktop exceeds the virtual compositor allocation limit");
  }
  return result;
}

inline std::uint32_t ssrc(unsigned display) {
  if (display > 1)
    throw std::invalid_argument("Display ID must be 0 or 1");
  return ssrc_base | display;
}

struct InputView {
  unsigned display;
  std::span<const std::uint8_t> packet;
};

// Operates on NV_INPUT_HEADER + payload, after decrypting the existing control
// channel. The wrapper never contains another ControlPacket or nested wrapper.
inline std::optional<InputView>
unwrap_input(std::span<const std::uint8_t> bytes, bool negotiated, unsigned focused = 0) {
  const auto be32 = [](const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
  };
  const auto magic = [](const std::uint8_t *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
  };
  if (bytes.size() < 8 || bytes.size() > 65531 || be32(bytes.data()) != bytes.size() - 4)
    return {};
  if (magic(bytes.data() + 4) != input_magic)
    return InputView{focused, bytes};
  if (!negotiated || bytes.size() < 20 || bytes[8] > 1 || bytes[9] || bytes[10] || bytes[11])
    return {};
  const auto inner = bytes.subspan(12);
  if (be32(inner.data()) != inner.size() - 4 || magic(inner.data() + 4) == input_magic)
    return {};
  return InputView{bytes[8], inner};
}
} // namespace moonlight::dual_display
