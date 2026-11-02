#include "shared_video.hpp"
#include <helpers/logger.hpp>

namespace streaming {
bool EncodedVideoQueue::push(const EncodedSample &sample) {
  auto buffer = gst_sample_get_buffer(sample.get());
  if (!buffer)
    return true;
  std::lock_guard lock(mutex_);
  bool request = false;
  if (samples_.size() >= 4 || bytes_ >= 8 * 1024 * 1024) {
    samples_.clear();
    bytes_ = 0;
    waiting_for_keyframe_ = true;
    request = true;
  }
  if (waiting_for_keyframe_ && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
    return request;
  waiting_for_keyframe_ = false;
  bytes_ += gst_buffer_get_size(buffer);
  samples_.push_back(sample);
  ready_.notify_one();
  return request;
}
EncodedSample EncodedVideoQueue::pop(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  ready_.wait_for(lock, timeout, [&] { return !samples_.empty(); });
  if (samples_.empty())
    return {};
  auto result = std::move(samples_.front());
  samples_.pop_front();
  bytes_ -= gst_buffer_get_size(gst_sample_get_buffer(result.get()));
  return result;
}
void EncodedVideoQueue::reset() {
  std::lock_guard lock(mutex_);
  samples_.clear();
  bytes_ = 0;
  waiting_for_keyframe_ = true;
}
SharedVideoEncoder::SharedVideoEncoder(std::shared_ptr<const wolf::gpu::Runtime::Launch> reservation, Run run)
    : launch(std::move(reservation)), run_(std::move(run)) {}
SharedVideoEncoder::~SharedVideoEncoder() {
  worker_.request_stop();
  if (worker_.joinable())
    worker_.join();
  if (launch)
    logs::log(logs::info, "[LOBBY] Released unwatched encoder on {}", launch->device.render_node);
}
bool SharedVideoEncoder::healthy() const {
  return !ended_.load() && (!launch || launch->reservation->valid());
}
std::shared_ptr<EncodedVideoQueue> SharedVideoEncoder::subscribe() {
  std::lock_guard lock(mutex_);
  auto viewer = std::make_shared<EncodedVideoQueue>();
  viewers_.push_back(viewer);
  request_keyframe();
  if (!worker_.joinable()) {
    worker_ = std::jthread([this](std::stop_token stop) {
      try {
        run_(*this, stop);
      } catch (const std::exception &error) {
        logs::log(logs::error, "[LOBBY] Shared video encoder failed: {}", error.what());
      }
      ended_.store(true);
    });
  }
  return viewer;
}
void SharedVideoEncoder::publish(const EncodedSample &sample) {
  std::lock_guard lock(mutex_);
  std::erase_if(viewers_, [](const auto &viewer) { return viewer.expired(); });
  for (const auto &weak : viewers_)
    if (auto viewer = weak.lock(); viewer && viewer->push(sample))
      request_keyframe();
}
void SharedVideoEncoder::request_keyframe() {
  keyframe_.store(true);
}
bool SharedVideoEncoder::take_keyframe_request() {
  return keyframe_.exchange(false);
}
std::shared_ptr<SharedVideoEncoder>
SharedVideoEncoders::acquire(const std::string &key,
                             const std::function<std::shared_ptr<SharedVideoEncoder>()> &create) {
  std::lock_guard lock(mutex_);
  std::erase_if(encoders_, [](const auto &item) { return item.second.expired(); });
  if (auto existing = encoders_[key].lock(); existing && existing->healthy())
    return existing;
  auto encoder = create();
  if (encoder)
    encoders_[key] = encoder;
  return encoder;
}
} // namespace streaming
