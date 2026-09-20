#include "gst-video-context.hpp"

namespace gst_video_context {

struct GstVideoContext {};

bool supports_cuda() {
  return false;
}

bool init() {
  return true;
}

gst_context_ptr
need_context_for_device(const std::string &device_path, GstMessage *msg, std::optional<unsigned int> cuda_device) {
  return nullptr;
}

bool set_context(gst_context_ptr context, GstMessage *msg) {
  return false;
}

} // namespace gst_video_context