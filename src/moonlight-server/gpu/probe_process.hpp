#pragma once
#include "encoder_types.hpp"
#include <chrono>
#include <stop_token>

namespace wolf::gpu {
// Each phase has its own deadline: basic encoding first, then optional DMA-BUF/VA import.
// Failure of the latter preserves a verified hardware encoder for non-strict policy.
EncoderProbeResult isolated_probe(const std::string &executable,
                                  const Device &device,
                                  Codec codec,
                                  std::stop_token stop = {},
                                  std::chrono::milliseconds timeout = std::chrono::seconds(5));
// Internal child-process command. Writes a small result to descriptor 3; never starts the server.
int probe_child(int argc, char **argv);
} // namespace wolf::gpu
