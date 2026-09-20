#pragma once
#include "encoder_types.hpp"
#include <chrono>
#include <gst/gst.h>

namespace wolf::gpu {
// Resolve with the CUDA driver, never PCI sorting or NVML ordinals. No fallback to device zero.
std::optional<unsigned int> cuda_device_for_pci(const std::string &pci_id);
// Requires existing canonicalizable paths; missing paths never compare equal.
bool same_render_device(const std::string &left, const std::string &right);
// Validates plugin, hardware class, codec and read-only device identity on this instance.
std::optional<EncoderBinding> encoder_binding(GstElement *element, const Device &device, Codec codec);
// GStreamer must be initialized. Invoke on a probe worker, never the HTTP/event loop.
// The bus wait is bounded; driver initialization/teardown still require process isolation before launch wiring.
EncoderProbeResult probe_encoder(const Device &device,
                                 Codec codec = Codec::h264,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(3));
} // namespace wolf::gpu
